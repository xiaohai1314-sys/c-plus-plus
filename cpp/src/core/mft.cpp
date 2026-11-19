#include "core/mft.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include "core/disk.h"
#include "core/timeutil.h"
#include "util/path.h"
#include "util/utf.h"
#include "util/win32.h"

namespace {

constexpr size_t kSnapshotFloorMinSize = 1u << 20;
constexpr size_t kMaxUsnDeltaFrns = 25'000;
constexpr uint64_t kRawReadMinAgeSecs = 300;
constexpr size_t kChunkSize = 8u * 1024 * 1024;
constexpr size_t kDeltaParallelThreshold = 64;
constexpr size_t kDeltaParallelThreads = 8;
constexpr uint64_t kStrongChunkBytes = 256ULL * 1024;

struct Run {
    int64_t vcn = 0;
    int64_t lcn = 0;
    int64_t length = 0;
};

std::string ToLowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

uint16_t U16At(const uint8_t* data, size_t length, size_t offset) {
    uint16_t value = 0;
    if (offset + 2 <= length) {
        std::memcpy(&value, data + offset, 2);
    }
    return value;
}

uint32_t U32At(const uint8_t* data, size_t length, size_t offset) {
    uint32_t value = 0;
    if (offset + 4 <= length) {
        std::memcpy(&value, data + offset, 4);
    }
    return value;
}

uint64_t U64At(const uint8_t* data, size_t length, size_t offset) {
    uint64_t value = 0;
    if (offset + 8 <= length) {
        std::memcpy(&value, data + offset, 8);
    }
    return value;
}

int64_t ReadUintLe(const uint8_t* data, size_t length) {
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index) {
        value |= static_cast<uint64_t>(data[index]) << (8 * index);
    }
    return static_cast<int64_t>(value);
}

int64_t ReadIntLe(const uint8_t* data, size_t length) {
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index) {
        value |= static_cast<uint64_t>(data[index]) << (8 * index);
    }
    const size_t bits = length * 8;
    if (bits < 64 && (value & (1ULL << (bits - 1))) != 0) {
        value |= ~0ULL << bits;
    }
    return static_cast<int64_t>(value);
}

void ApplyFixups(uint8_t* record, size_t length, size_t bytesPerSector) {
    if (length < 8) {
        return;
    }
    const size_t usaOffset = U16At(record, length, 4);
    const size_t usaCount = U16At(record, length, 6);
    if (usaOffset == 0 || usaCount < 2 || usaOffset + usaCount * 2 > length) {
        return;
    }
    for (size_t index = 1; index < usaCount; ++index) {
        const size_t position = index * bytesPerSector;
        const size_t source = usaOffset + index * 2;
        if (position < 2 || position > length || source + 2 > length) {
            break;
        }
        record[position - 2] = record[source];
        record[position - 1] = record[source + 1];
    }
}

std::vector<Run> ParseRunlist(const uint8_t* data, size_t length) {
    std::vector<Run> runs;
    int64_t vcn = 0;
    int64_t previousLcn = 0;
    size_t position = 0;
    while (position < length) {
        const uint8_t header = data[position];
        ++position;
        if (header == 0) {
            break;
        }
        const size_t lengthSize = header & 0x0F;
        const size_t offsetSize = (header >> 4) & 0x0F;
        if (lengthSize == 0 || lengthSize > 8 || offsetSize > 8 ||
            position + lengthSize + offsetSize > length) {
            break;
        }
        const int64_t runLength = ReadUintLe(data + position, lengthSize);
        position += lengthSize;
        const int64_t offset =
            offsetSize == 0 ? 0 : ReadIntLe(data + position, offsetSize);
        position += offsetSize;
        Run run;
        run.vcn = vcn;
        run.length = runLength;
        if (offsetSize > 0) {
            previousLcn += offset;
            run.lcn = previousLcn;
        } else {
            run.lcn = -1;
        }
        runs.push_back(run);
        vcn += runLength;
    }
    return runs;
}

void ParseDataExtents(const uint8_t* data, size_t length,
                      uint64_t bytesPerCluster, uint64_t fileSize,
                      uint64_t& firstLcn, uint64_t& firstRunClusters,
                      bool& hasStrongExtents, uint64_t& midRawOffset,
                      uint64_t& tailRawOffset) {
    firstLcn = 0;
    firstRunClusters = 0;
    hasStrongExtents = false;
    midRawOffset = 0;
    tailRawOffset = 0;

    if (length == 0 || bytesPerCluster == 0) {
        return;
    }

    const bool needTriple = (fileSize >= kStrongChunkBytes * 3);
    const uint64_t midTargetStart = fileSize / 2;
    const uint64_t midTargetEnd = midTargetStart + kStrongChunkBytes;
    const uint64_t tailTargetStart =
        needTriple ? (fileSize - kStrongChunkBytes) : 0;
    const uint64_t tailTargetEnd = fileSize;
    bool foundMid = false;
    bool foundTail = false;

    int64_t vcn = 0;
    int64_t prevLcn = 0;
    size_t position = 0;

    while (position < length) {
        const uint8_t header = data[position];
        ++position;
        if (header == 0) {
            break;
        }
        const size_t lengthSize = header & 0x0F;
        const size_t offsetSize = (header >> 4) & 0x0F;
        if (lengthSize == 0 || lengthSize > 8 || offsetSize > 8 ||
            position + lengthSize + offsetSize > length) {
            break;
        }
        const int64_t runLength = ReadUintLe(data + position, lengthSize);
        position += lengthSize;
        const int64_t delta =
            offsetSize == 0 ? 0 : ReadIntLe(data + position, offsetSize);
        position += offsetSize;

        int64_t runLcn = -1;
        if (offsetSize > 0) {
            prevLcn += delta;
            runLcn = prevLcn;
        }

        if (runLength <= 0) {
            break;
        }

        if (vcn == 0) {
            if (runLcn <= 0) {
                return;
            }
            firstLcn = static_cast<uint64_t>(runLcn);
            firstRunClusters = static_cast<uint64_t>(runLength);
            const uint64_t firstBytes = firstRunClusters * bytesPerCluster;
            if (firstBytes >= fileSize) {
                if (needTriple) {
                    const uint64_t baseOffset = firstLcn * bytesPerCluster;
                    const uint64_t midOff = baseOffset + midTargetStart;
                    const uint64_t tailOff = baseOffset + tailTargetStart;
                    if (baseOffset < midOff && midOff < tailOff) {
                        midRawOffset = midOff;
                        tailRawOffset = tailOff;
                        hasStrongExtents = true;
                    }
                }
                return;
            }
            if (!needTriple || firstBytes < kStrongChunkBytes) {
                return;
            }
        }

        if (runLcn > 0) {
            const uint64_t runStartByte =
                static_cast<uint64_t>(vcn) * bytesPerCluster;
            const uint64_t runEndByte =
                (static_cast<uint64_t>(vcn) + static_cast<uint64_t>(runLength)) *
                bytesPerCluster;
            const uint64_t runPhysStart =
                static_cast<uint64_t>(runLcn) * bytesPerCluster;
            if (!foundMid && midTargetStart >= runStartByte &&
                midTargetEnd <= runEndByte) {
                midRawOffset = runPhysStart + (midTargetStart - runStartByte);
                foundMid = true;
            }
            if (!foundTail && tailTargetStart >= runStartByte &&
                tailTargetEnd <= runEndByte) {
                tailRawOffset = runPhysStart + (tailTargetStart - runStartByte);
                foundTail = true;
            }
            if (foundMid && foundTail) {
                break;
            }
        }

        vcn += runLength;
    }

    if (foundMid && foundTail) {
        const uint64_t headOffset = firstLcn * bytesPerCluster;
        if (headOffset < midRawOffset && midRawOffset < tailRawOffset) {
            hasStrongExtents = true;
            return;
        }
    }
    midRawOffset = 0;
    tailRawOffset = 0;
}

std::string Utf16Le(const uint8_t* data, size_t length) {
    const size_t charCount = length / 2;
    if (charCount == 0) {
        return {};
    }
    std::string s;
    s.resize(charCount);
    bool isAscii = true;
    for (size_t i = 0; i < charCount; ++i) {
        const uint8_t c0 = data[2 * i];
        const uint8_t c1 = data[2 * i + 1];
        if (c1 != 0 || c0 >= 0x80) {
            isAscii = false;
            break;
        }
        s[i] = static_cast<char>(c0);
    }
    if (isAscii) {
        return s;
    }
    const wchar_t* wptr = nullptr;
    wchar_t stackBuf[260];
    std::vector<wchar_t> heapBuf;
    if ((reinterpret_cast<uintptr_t>(data) & 1) == 0) {
        wptr = reinterpret_cast<const wchar_t*>(data);
    } else {
        if (charCount <= 260) {
            std::memcpy(stackBuf, data, charCount * 2);
            wptr = stackBuf;
        } else {
            heapBuf.resize(charCount);
            std::memcpy(heapBuf.data(), data, charCount * 2);
            wptr = heapBuf.data();
        }
    }
    const int utf8Len = WideCharToMultiByte(
        CP_UTF8, 0, wptr, static_cast<int>(charCount), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) {
        return {};
    }
    s.resize(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(charCount),
                        s.data(), utf8Len, nullptr, nullptr);
    return s;
}

struct ParsedRecord {
    enum class Kind { Skip, Dir, File, MftRoot };
    Kind kind = Kind::Skip;
    uint64_t index = 0;
    mft::DirNode node;
    mft::RawFile raw;
    std::vector<Run> runs;
};

ParsedRecord ParseRecordInPlace(uint8_t* record, size_t length, uint64_t index,
                                size_t bytesPerSector, size_t bytesPerCluster,
                                uint64_t minSize) {
    ParsedRecord result;
    if (length < 48 || std::memcmp(record, "FILE", 4) != 0) {
        return result;
    }
    const uint16_t flags = U16At(record, length, 22);
    if ((flags & 1) == 0) {
        return result;
    }
    if (index != 0 && U64At(record, length, 32) != 0) {
        return result;
    }
    ApplyFixups(record, length, bytesPerSector);

    const bool isDir = (flags & 2) != 0;
    const uint16_t sequence = U16At(record, length, 16);
    const uint64_t frn = (static_cast<uint64_t>(sequence) << 48) | index;
    const size_t firstAttribute = U16At(record, length, 20);

    bool hasName = false;
    bool nameIsDos = false;
    size_t nameOffset = 0;
    size_t nameBytes = 0;
    uint64_t parentFrn = 0;
    uint64_t size = 0;
    uint64_t initializedSize = 0;
    uint64_t mtime = 0;
    uint32_t attributes = 0;
    uint16_t dataFlags = 0;
    uint64_t firstLcn = 0;
    uint64_t firstRunClusters = 0;
    bool hasStrongExtents = false;
    uint64_t midRawOffset = 0;
    uint64_t tailRawOffset = 0;
    bool hasMftRuns = false;
    std::vector<Run> mftRuns;

    size_t position = firstAttribute;
    while (position + 8 <= length) {
        const uint32_t attributeType = U32At(record, length, position);
        if (attributeType == 0xFFFF'FFFF) {
            break;
        }
        const size_t attributeLength = U32At(record, length, position + 4);
        if (attributeLength < 16 || position + attributeLength > length) {
            break;
        }
        const uint8_t nonResident = record[position + 8];
        const uint8_t nameLength = record[position + 9];

        if (attributeType == 0x10 && nonResident == 0) {
            const size_t valueOffset =
                U16At(record, length, position + 20);
            const size_t value = position + valueOffset;
            if (value + 36 <= position + attributeLength) {
                mtime = mft::FileTimeToUnix(U64At(record, length, value + 8));
                attributes = U32At(record, length, value + 32);
                if (!isDir && (attributes & mft::kSkipFileAttrs) != 0) {
                    return result;
                }
            }
        } else if (attributeType == 0x30 && nonResident == 0) {
            const size_t valueOffset =
                U16At(record, length, position + 20);
            const size_t value = position + valueOffset;
            if (value + 66 <= position + attributeLength) {
                const uint64_t parentRef = U64At(record, length, value);
                const size_t fileNameLength = record[value + 64];
                const uint8_t nameSpace = record[value + 65];
                const size_t fileNameOffset = value + 66;
                const size_t fileNameBytes = fileNameLength * 2;
                if (fileNameOffset + fileNameBytes <=
                    position + attributeLength) {
                    const bool isDos = nameSpace == 2;
                    if (!hasName || (nameIsDos && !isDos) || nameSpace == 0 ||
                        nameSpace == 1) {
                        hasName = true;
                        nameOffset = fileNameOffset;
                        nameBytes = fileNameBytes;
                        parentFrn = parentRef;
                        nameIsDos = isDos;
                    }
                }
            }
        } else if (attributeType == 0x80 && nameLength == 0) {
            dataFlags = U16At(record, length, position + 12);
            if (nonResident != 0) {
                if (position + 56 <= length &&
                    U64At(record, length, position + 16) == 0) {
                    size = U64At(record, length, position + 48);
                    initializedSize =
                        (position + 64 <= position + attributeLength)
                            ? U64At(record, length, position + 56)
                            : 0;
                    const size_t runOffset =
                        U16At(record, length, position + 32);
                    const size_t start = position + runOffset;
                    if (start <= position + attributeLength) {
                        const size_t runBytes =
                            position + attributeLength - start;
                        if (index == 0) {
                            mftRuns = ParseRunlist(record + start, runBytes);
                            hasMftRuns = true;
                        } else if (!isDir && size >= minSize) {
                            ParseDataExtents(
                                record + start, runBytes, bytesPerCluster, size,
                                firstLcn, firstRunClusters, hasStrongExtents,
                                midRawOffset, tailRawOffset);
                        }
                    }
                }
            } else {
                size = U32At(record, length, position + 16);
                initializedSize = size;
            }
        }

        position += attributeLength;
    }

    if (index == 0) {
        if (hasMftRuns && !mftRuns.empty()) {
            result.kind = ParsedRecord::Kind::MftRoot;
            result.runs = std::move(mftRuns);
        }
        return result;
    }

    if (!hasName) {
        return result;
    }

    if (isDir) {
        result.kind = ParsedRecord::Kind::Dir;
        result.index = index;
        result.node.seq = sequence;
        result.node.parentFrn = parentFrn;
        result.node.name = Utf16Le(record + nameOffset, nameBytes);
        result.node.reparse =
            (attributes & mft::kFileAttributeReparsePoint) != 0;
        return result;
    }

    if (size < minSize || (attributes & mft::kSkipFileAttrs) != 0) {
        return result;
    }

    if (nameBytes >= 2 && record[nameOffset] == '$' && record[nameOffset + 1] == 0) {
        return result;
    }

    std::string name = Utf16Le(record + nameOffset, nameBytes);
    if (!name.empty() && name.front() == '$') {
        return result;
    }

    constexpr uint32_t kNoRawAttrs =
        mft::kSkipFileAttrs | 0x0000'0800 /* COMPRESSED */ |
        0x0000'4000 /* ENCRYPTED */ | mft::kFileAttributeSparseFile;
    const uint64_t firstExtentBytes =
        firstRunClusters * static_cast<uint64_t>(bytesPerCluster);
    const bool canRawRead =
        (attributes & kNoRawAttrs) == 0 &&
        (dataFlags & (0x0001 | 0x4000 | 0x8000)) == 0 && firstLcn > 0 &&
        firstExtentBytes >= std::min<uint64_t>(size, 4096) &&
        initializedSize >= std::min<uint64_t>(size, 4096);
    const bool canRawFull =
        canRawRead && initializedSize >= size && (firstExtentBytes >= size);
    const bool canRawStrong =
        canRawRead && initializedSize >= size && hasStrongExtents;

    result.kind = ParsedRecord::Kind::File;
    result.index = index;
    result.raw.frn = frn;
    result.raw.parentFrn = parentFrn;
    result.raw.name = std::move(name);
    result.raw.size = size;
    result.raw.mtime = mtime;
    result.raw.firstLcn = firstLcn;
    result.raw.sortKey = firstLcn > 0 ? firstLcn : index;
    result.raw.canRawRead = canRawRead;
    result.raw.canRawFull = canRawFull;
    result.raw.canRawStrong = canRawStrong;
    result.raw.midRawOffset = canRawStrong ? midRawOffset : 0;
    result.raw.tailRawOffset = canRawStrong ? tailRawOffset : 0;
    return result;
}

size_t ReadExactAt(HANDLE handle, int64_t offset, uint8_t* buffer,
                   size_t length) {
    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    if (SetFilePointerEx(handle, distance, nullptr, FILE_BEGIN) == 0) {
        return 0;
    }
    size_t total = 0;
    while (total < length) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<size_t>(length - total, 8u * 1024 * 1024));
        if (ReadFile(handle, buffer + total, want, &read, nullptr) == 0 ||
            read == 0) {
            break;
        }
        total += read;
    }
    return total;
}

struct UsnDelta {
    enum class Kind { Changed, TooMany, Failed };
    Kind kind = Kind::Failed;
    std::unordered_set<uint64_t> changed;
};

bool QueryUsnJournal(HANDLE handle, USN_JOURNAL_DATA_V0& journal) {
    DWORD returned = 0;
    return DeviceIoControl(handle, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                           &journal, sizeof(journal), &returned, nullptr) != 0;
}

UsnDelta CollectUsnChangedIndices(HANDLE handle,
                                  const USN_JOURNAL_DATA_V0& journal,
                                  int64_t startUsn) {
    UsnDelta result;
    READ_USN_JOURNAL_DATA_V0 input = {};
    input.StartUsn = startUsn;
    input.ReasonMask = 0xFFFF'FFFF;
    input.ReturnOnlyOnClose = 0;
    input.Timeout = 0;
    input.BytesToWaitFor = 0;
    input.UsnJournalID = journal.UsnJournalID;

    std::vector<uint8_t> buffer(1024 * 1024);
    std::unordered_set<uint64_t> changed;
    for (;;) {
        DWORD returned = 0;
        if (DeviceIoControl(handle, FSCTL_READ_USN_JOURNAL, &input,
                            sizeof(input), buffer.data(),
                            static_cast<DWORD>(buffer.size()), &returned,
                            nullptr) == 0 ||
            returned < 8) {
            result.kind = UsnDelta::Kind::Failed;
            return result;
        }
        const size_t length = returned;
        const int64_t nextCursor =
            static_cast<int64_t>(U64At(buffer.data(), length, 0));
        if (length == 8) {
            break;
        }
        size_t offset = 8;
        while (offset + 60 <= length) {
            const size_t recordLength = U32At(buffer.data(), length, offset);
            if (recordLength < 60 || offset + recordLength > length) {
                break;
            }
            const uint16_t major = U16At(buffer.data(), length, offset + 4);
            if (major == 2 || major == 3) {
                const uint64_t frn = U64At(buffer.data(), length, offset + 8);
                changed.insert(frn & 0xFFFF'FFFF'FFFFULL);
                if (changed.size() > kMaxUsnDeltaFrns) {
                    result.kind = UsnDelta::Kind::TooMany;
                    return result;
                }
            }
            offset += recordLength;
        }
        if (nextCursor <= input.StartUsn ||
            nextCursor >= journal.NextUsn) {
            break;
        }
        input.StartUsn = nextCursor;
    }
    result.kind = UsnDelta::Kind::Changed;
    result.changed = std::move(changed);
    return result;
}

bool ReadSingleMftRecord(HANDLE handle, uint64_t index, size_t recordSize,
                         std::vector<uint8_t>& out, uint64_t& actualFrn) {
    NTFS_FILE_RECORD_INPUT_BUFFER input = {};
    input.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(index);
    DWORD returned = 0;
    const BOOL ioOk = DeviceIoControl(handle, FSCTL_GET_NTFS_FILE_RECORD, &input,
                                      sizeof(input), out.data(),
                                      static_cast<DWORD>(out.size()), &returned,
                                      nullptr);
    if (ioOk == 0 || returned < 12 + recordSize) {
        return false;
    }
    actualFrn = U64At(out.data(), out.size(), 0);
    const size_t recordLength = U32At(out.data(), out.size(), 8);
    return recordLength >= recordSize;
}

UniqueHandle OpenVolumeHandle(const std::string& volumePath) {
    return UniqueHandle(CreateFileW(Utf8ToWide(volumePath).c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr));
}

struct DeltaUpdate {
    uint64_t index = 0;
    ParsedRecord record;
};

std::vector<DeltaUpdate> ApplyUsnDelta(
    HANDLE handle, const std::string& volumePath,
    const std::unordered_set<uint64_t>& changedSet, size_t recordSize,
    size_t bytesPerSector, size_t bytesPerCluster, uint64_t minSize) {
    std::vector<uint64_t> changed(changedSet.begin(), changedSet.end());
    std::sort(changed.begin(), changed.end());
    auto parseOne = [&](HANDLE source, std::vector<uint8_t>& buffer,
                        uint64_t index) -> std::optional<DeltaUpdate> {
        uint64_t actualFrn = 0;
        if (!ReadSingleMftRecord(source, index, recordSize, buffer, actualFrn)) {
            return std::nullopt;
        }
        if ((actualFrn & 0xFFFF'FFFF'FFFFULL) != index) {
            return std::nullopt;
        }
        DeltaUpdate update;
        update.index = index;
        update.record = ParseRecordInPlace(buffer.data() + 12, recordSize,
                                           index, bytesPerSector,
                                           bytesPerCluster, minSize);
        return update;
    };

    std::vector<DeltaUpdate> updates;
    if (changed.size() < kDeltaParallelThreshold) {
        std::vector<uint8_t> buffer(16 + recordSize);
        updates.reserve(changed.size());
        for (const uint64_t index : changed) {
            if (auto update = parseOne(handle, buffer, index)) {
                updates.push_back(std::move(*update));
            }
        }
        return updates;
    }

    const size_t threads =
        std::min(kDeltaParallelThreads, changed.size());
    const size_t chunk = (changed.size() + threads - 1) / threads;
    std::vector<std::vector<DeltaUpdate>> parts(threads);
    std::vector<std::thread> workers;
    std::atomic<bool> allOk{true};
    workers.reserve(threads);
    for (size_t part = 0; part < threads; ++part) {
        const size_t start = part * chunk;
        if (start >= changed.size()) {
            break;
        }
        const size_t end = std::min(start + chunk, changed.size());
        workers.emplace_back([&, start, end, part] {
            UniqueHandle volume = OpenVolumeHandle(volumePath);
            if (!volume) {
                allOk.store(false);
                return;
            }
            std::vector<uint8_t> buffer(16 + recordSize);
            std::vector<DeltaUpdate> local;
            local.reserve(end - start);
            for (size_t cursor = start; cursor < end; ++cursor) {
                if (auto update = parseOne(volume.get(), buffer, changed[cursor])) {
                    local.push_back(std::move(*update));
                }
            }
            parts[part] = std::move(local);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    if (allOk.load()) {
        for (std::vector<DeltaUpdate>& part : parts) {
            updates.insert(updates.end(),
                           std::make_move_iterator(part.begin()),
                           std::make_move_iterator(part.end()));
        }
        return updates;
    }

    // 有线程开卷失败：退回串行，避免把读不到的记录误判为已删除。
    updates.clear();
    std::vector<uint8_t> buffer(16 + recordSize);
    for (const uint64_t index : changed) {
        if (auto update = parseOne(handle, buffer, index)) {
            updates.push_back(std::move(*update));
        }
    }
    return updates;
}

using DirMap = std::unordered_map<uint64_t, mft::DirNode>;
using FileMap = std::unordered_map<uint64_t, mft::RawFile>;

size_t ParseThreadCount() {
    const unsigned int hardware = std::thread::hardware_concurrency();
    return std::max<size_t>(1, std::min<size_t>(hardware == 0 ? 4 : hardware, 16));
}

class MftWorkerPool {
public:
    struct WorkerResults {
        std::vector<std::pair<uint64_t, mft::DirNode>> dirs;
        std::vector<std::pair<uint64_t, mft::RawFile>> files;
    };

    explicit MftWorkerPool(size_t threadCount)
        : threadCount_(threadCount), doneCount_(0), running_(true), jobEpoch_(0) {
        results_.resize(threadCount);
        workers_.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this, i] { WorkerLoop(i); });
        }
    }

    ~MftWorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cvStart_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void ProcessChunk(uint8_t* data, size_t count, uint64_t baseIndex,
                      size_t recordSize, size_t bytesPerSector,
                      size_t bytesPerCluster, uint64_t minSize) {
        if (count == 0) {
            return;
        }
        chunkData_ = data;
        totalRecords_ = count;
        baseIndex_ = baseIndex;
        recordSize_ = recordSize;
        bytesPerSector_ = bytesPerSector;
        bytesPerCluster_ = bytesPerCluster;
        minSize_ = minSize;
        doneCount_.store(0, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++jobEpoch_;
        }
        cvStart_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        cvDone_.wait(lock, [this] {
            return doneCount_.load(std::memory_order_acquire) == threadCount_;
        });
    }

    std::vector<WorkerResults> results_;

private:
    void WorkerLoop(size_t workerId) {
        size_t lastEpoch = 0;
        WorkerResults& myResult = results_[workerId];
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cvStart_.wait(lock, [this, lastEpoch] {
                    return !running_ || jobEpoch_ > lastEpoch;
                });
                if (!running_) {
                    break;
                }
                lastEpoch = jobEpoch_;
            }

            const size_t recordsPerThread =
                (totalRecords_ + threadCount_ - 1) / threadCount_;
            const size_t startRec = workerId * recordsPerThread;
            if (startRec < totalRecords_) {
                const size_t endRec =
                    std::min(startRec + recordsPerThread, totalRecords_);
                for (size_t r = startRec; r < endRec; ++r) {
                    uint8_t* raw = chunkData_ + r * recordSize_;
                    ParsedRecord parsed = ParseRecordInPlace(
                        raw, recordSize_, baseIndex_ + r, bytesPerSector_,
                        bytesPerCluster_, minSize_);
                    if (parsed.kind == ParsedRecord::Kind::Dir) {
                        myResult.dirs.emplace_back(parsed.index,
                                                   std::move(parsed.node));
                    } else if (parsed.kind == ParsedRecord::Kind::File) {
                        myResult.files.emplace_back(parsed.index,
                                                    std::move(parsed.raw));
                    }
                }
            }

            if (doneCount_.fetch_add(1, std::memory_order_release) + 1 ==
                threadCount_) {
                std::lock_guard<std::mutex> lock(mutex_);
                cvDone_.notify_one();
            }
        }
    }

    size_t threadCount_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cvStart_;
    std::condition_variable cvDone_;
    std::atomic<size_t> doneCount_;
    size_t jobEpoch_ = 0;
    bool running_ = true;

    uint8_t* chunkData_ = nullptr;
    size_t totalRecords_ = 0;
    uint64_t baseIndex_ = 0;
    size_t recordSize_ = 0;
    size_t bytesPerSector_ = 0;
    size_t bytesPerCluster_ = 0;
    uint64_t minSize_ = 0;
};

bool FullMftReadParallel(HANDLE handle,
                         const NTFS_VOLUME_DATA_BUFFER& volume,
                         const std::vector<Run>& runs, uint64_t storeMinSize,
                         DirMap& dirs, FileMap& files) {
    const size_t recordSize = volume.BytesPerFileRecordSegment;
    const size_t bytesPerCluster = volume.BytesPerCluster;
    const size_t bytesPerSector = volume.BytesPerSector;
    const uint64_t maxMftBytes =
        volume.MftValidDataLength.QuadPart > 0
            ? static_cast<uint64_t>(volume.MftValidDataLength.QuadPart)
            : UINT64_MAX;

    constexpr size_t kLargeChunkSize = 16u * 1024 * 1024;
    std::vector<uint8_t> buffer(kLargeChunkSize);
    std::vector<uint8_t> carry;
    carry.reserve(recordSize);
    uint64_t totalOffset = 0;

    const size_t threadCount = std::min<size_t>(ParseThreadCount(), 8);
    MftWorkerPool pool(threadCount);

    for (const Run& run : runs) {
        if (totalOffset >= maxMftBytes) {
            break;
        }
        if (run.length <= 0) {
            continue;
        }
        const uint64_t runBytes =
            static_cast<uint64_t>(run.length) * bytesPerCluster;
        if (run.lcn < 0) {
            totalOffset += runBytes;
            carry.clear();
            continue;
        }
        const int64_t diskOffset =
            run.lcn * static_cast<int64_t>(bytesPerCluster);
        const uint64_t toProcess =
            std::min(runBytes, maxMftBytes - totalOffset);
        uint64_t doneInRun = 0;

        while (doneInRun < toProcess) {
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(toProcess - doneInRun, kLargeChunkSize));
            const size_t got = ReadExactAt(
                handle, diskOffset + static_cast<int64_t>(doneInRun),
                buffer.data(), want);
            if (got == 0) {
                break;
            }
            uint8_t* data = buffer.data();
            size_t cursor = 0;

            if (!carry.empty()) {
                const size_t need = recordSize - carry.size();
                if (got >= need) {
                    const uint64_t recordIndex =
                        (totalOffset - carry.size()) / recordSize;
                    carry.insert(carry.end(), data, data + need);
                    ParsedRecord parsed = ParseRecordInPlace(
                        carry.data(), recordSize, recordIndex, bytesPerSector,
                        bytesPerCluster, storeMinSize);
                    if (parsed.kind == ParsedRecord::Kind::Dir) {
                        dirs[parsed.index] = std::move(parsed.node);
                    } else if (parsed.kind == ParsedRecord::Kind::File) {
                        files[parsed.index] = std::move(parsed.raw);
                    }
                    carry.clear();
                    cursor = need;
                } else {
                    carry.insert(carry.end(), data, data + got);
                    doneInRun += got;
                    totalOffset += got;
                    continue;
                }
            }

            const size_t alignedLength =
                ((got - cursor) / recordSize) * recordSize;
            if (alignedLength > 0) {
                pool.ProcessChunk(data + cursor, alignedLength / recordSize,
                                  (totalOffset + cursor) / recordSize,
                                  recordSize, bytesPerSector, bytesPerCluster,
                                  storeMinSize);
                cursor += alignedLength;
            }

            if (cursor < got) {
                carry.insert(carry.end(), data + cursor, data + got);
            }

            doneInRun += got;
            totalOffset += got;
        }
    }

    size_t totalDirs = dirs.size();
    size_t totalFiles = files.size();
    for (const auto& res : pool.results_) {
        totalDirs += res.dirs.size();
        totalFiles += res.files.size();
    }
    dirs.reserve(totalDirs);
    files.reserve(totalFiles);
    for (auto& res : pool.results_) {
        for (auto& [idx, node] : res.dirs) {
            dirs[idx] = std::move(node);
        }
        for (auto& [idx, raw] : res.files) {
            files[idx] = std::move(raw);
        }
    }

    return !(dirs.empty() && files.empty());
}

std::string DriveRootOf(const std::string& root) {
    std::string trimmed = root;
    while (!trimmed.empty() && trimmed.back() == '\\') {
        trimmed.pop_back();
    }
    if (trimmed.size() < 2 || trimmed[1] != ':') {
        return {};
    }
    const char letter = static_cast<char>(
        std::toupper(static_cast<unsigned char>(trimmed[0])));
    return std::string(1, letter) + ":";
}

std::string SnapshotPath(uint64_t serial) {
    char name[64];
    std::snprintf(name, sizeof(name), "mft-v4-%016llx.bin",
                  static_cast<unsigned long long>(serial));
    return JoinPath(CacheDir(), name);
}

struct SnapshotStore {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<mft::Snapshot>> map;
};

SnapshotStore& MemSnapshots() {
    static SnapshotStore store;
    return store;
}

std::shared_ptr<mft::Snapshot> MemSnapshotGet(uint64_t serial) {
    SnapshotStore& store = MemSnapshots();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.map.find(serial);
    return found == store.map.end() ? nullptr : found->second;
}

void MemSnapshotPut(uint64_t serial, std::shared_ptr<mft::Snapshot> snapshot) {
    SnapshotStore& store = MemSnapshots();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.map[serial] = std::move(snapshot);
}

std::shared_ptr<mft::Snapshot> MemSnapshotTake(uint64_t serial) {
    SnapshotStore& store = MemSnapshots();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.map.find(serial);
    if (found == store.map.end()) {
        return nullptr;
    }
    std::shared_ptr<mft::Snapshot> snapshot = std::move(found->second);
    store.map.erase(found);
    return snapshot;
}

std::atomic<int>& BackgroundWrites() {
    static std::atomic<int> count{0};
    return count;
}

std::mutex& SaveLock() {
    static std::mutex mutex;
    return mutex;
}

void SaveSnapshotInBackground(std::string path,
                              std::shared_ptr<mft::Snapshot> snapshot) {
    BackgroundWrites().fetch_add(1);
    std::thread([path = std::move(path), snapshot = std::move(snapshot)] {
        {
            std::lock_guard<std::mutex> guard(SaveLock());
            mft::SaveSnapshot(path, *snapshot);
        }
        BackgroundWrites().fetch_sub(1);
    }).detach();
}

std::mutex& RebuildSetMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<uint64_t>& RebuildSet() {
    static std::unordered_set<uint64_t> set;
    return set;
}

void SpawnBackgroundRebuild(std::string volumePath, uint64_t serial,
                            const NTFS_VOLUME_DATA_BUFFER& volume,
                            uint64_t storeMinSize, std::string snapshotFile,
                            size_t recordSize) {
    {
        std::lock_guard<std::mutex> lock(RebuildSetMutex());
        if (!RebuildSet().insert(serial).second) {
            return;
        }
    }
    BackgroundWrites().fetch_add(1);
    std::thread([volumePath = std::move(volumePath), serial, volume,
                 storeMinSize, snapshotFile = std::move(snapshotFile),
                 recordSize] {
        std::shared_ptr<mft::Snapshot> rebuilt;
        UniqueHandle handle = OpenVolumeHandle(volumePath);
        if (handle) {
            USN_JOURNAL_DATA_V0 journal = {};
            const bool hasJournal = QueryUsnJournal(handle.get(), journal);
            std::vector<uint8_t> first(recordSize);
            const int64_t offset =
                volume.MftStartLcn.QuadPart *
                static_cast<int64_t>(volume.BytesPerCluster);
            if (ReadExactAt(handle.get(), offset, first.data(), recordSize) >=
                recordSize) {
                ParsedRecord root = ParseRecordInPlace(
                    first.data(), recordSize, 0, volume.BytesPerSector,
                    volume.BytesPerCluster, storeMinSize);
                if (root.kind == ParsedRecord::Kind::MftRoot) {
                    auto snapshot = std::make_shared<mft::Snapshot>();
                    snapshot->volumeSerial = serial;
                    snapshot->journalId = hasJournal ? journal.UsnJournalID : 0;
                    snapshot->nextUsn =
                        hasJournal ? static_cast<int64_t>(journal.NextUsn) : 0;
                    snapshot->minSize = storeMinSize;
                    snapshot->bytesPerCluster = volume.BytesPerCluster;
                    if (FullMftReadParallel(handle.get(), volume, root.runs,
                                            storeMinSize, snapshot->dirs,
                                            snapshot->files)) {
                        rebuilt = std::move(snapshot);
                    }
                }
            }
        }
        if (rebuilt) {
            MemSnapshotPut(serial, rebuilt);
            mft::SaveSnapshot(snapshotFile, *rebuilt);
        }
        {
            std::lock_guard<std::mutex> lock(RebuildSetMutex());
            RebuildSet().erase(serial);
        }
        BackgroundWrites().fetch_sub(1);
    }).detach();
}

std::optional<std::string> ResolveDir(
    uint64_t frn, const DirMap& dirs,
    std::unordered_map<uint64_t, std::optional<std::string>>& cache,
    const std::string& drive, bool skipTopAtRoot,
    const std::vector<std::string>& skipTop) {
    const auto cached = cache.find(frn);
    if (cached != cache.end()) {
        return cached->second;
    }
    const uint64_t rootIndex = frn & 0xFFFF'FFFF'FFFFULL;
    if (rootIndex == 5) {
        const std::string path = drive + "\\";
        cache[frn] = path;
        return path;
    }

    std::vector<uint64_t> chain;
    uint64_t currentFrn = frn;
    std::optional<std::string> base;

    for (;;) {
        const auto hit = cache.find(currentFrn);
        if (hit != cache.end()) {
            base = hit->second;
            break;
        }
        const uint64_t currentIndex = currentFrn & 0xFFFF'FFFF'FFFFULL;
        const uint16_t currentSeq =
            static_cast<uint16_t>(currentFrn >> 48);
        const auto nodeIt = dirs.find(currentIndex);
        if (nodeIt == dirs.end()) {
            break;
        }
        const mft::DirNode& node = nodeIt->second;
        if (currentSeq != 0 && node.seq != currentSeq) {
            break;
        }
        if (node.reparse || (!node.name.empty() && node.name.front() == '$')) {
            break;
        }
        const uint64_t parentIndex = node.parentFrn & 0xFFFF'FFFF'FFFFULL;
        if (node.parentFrn == 0 || parentIndex == 5) {
            if (skipTopAtRoot) {
                const std::string lower = ToLowerAscii(node.name);
                if (std::find(skipTop.begin(), skipTop.end(), lower) !=
                    skipTop.end()) {
                    break;
                }
            }
            const std::string path = drive + "\\" + node.name;
            cache[currentFrn] = path;
            base = path;
            break;
        }
        chain.push_back(currentFrn);
        currentFrn = node.parentFrn;
        if (chain.size() > 4096) {
            break;
        }
    }

    if (!base.has_value()) {
        for (const uint64_t id : chain) {
            cache[id] = std::nullopt;
        }
        cache[frn] = std::nullopt;
        return std::nullopt;
    }

    std::string path = *base;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const uint64_t index = *it & 0xFFFF'FFFF'FFFFULL;
        const auto nodeIt = dirs.find(index);
        if (nodeIt == dirs.end()) {
            cache[frn] = std::nullopt;
            return std::nullopt;
        }
        while (!path.empty() && path.back() == '\\') {
            path.pop_back();
        }
        path += "\\" + nodeIt->second.name;
        cache[*it] = path;
    }
    return path;
}

}  // namespace

namespace mft {

uint64_t FileTimeToUnix(uint64_t fileTime) {
    constexpr uint64_t kWindowsEpochDiffSecs = 11'644'473'600ULL;
    const uint64_t seconds = fileTime / 10'000'000ULL;
    return seconds > kWindowsEpochDiffSecs ? seconds - kWindowsEpochDiffSecs : 0;
}

std::pair<uint64_t, uint64_t> ParseFirstExtent(const uint8_t* data,
                                               size_t length) {
    if (length == 0 || data[0] == 0) {
        return {0, 0};
    }
    const uint8_t header = data[0];
    const size_t lengthSize = header & 0x0F;
    const size_t offsetSize = (header >> 4) & 0x0F;
    if (lengthSize == 0 || lengthSize > 8 || offsetSize == 0 ||
        offsetSize > 8 || 1 + lengthSize + offsetSize > length) {
        return {0, 0};
    }
    const int64_t runLength = ReadUintLe(data + 1, lengthSize);
    const int64_t lcn = ReadIntLe(data + 1 + lengthSize, offsetSize);
    if (lcn > 0 && runLength > 0) {
        return {static_cast<uint64_t>(lcn), static_cast<uint64_t>(runLength)};
    }
    return {0, 0};
}

/// 超过这个数量就认为目录遍历不划算，切回整卷 $MFT。
constexpr size_t kSubtreeWalkLimit = 10'000;

/// 长路径的 FindFirstFileW 需要 \\?\ 前缀，超过 MAX_PATH 才加，避免影响普通路径。
std::wstring LongPathForm(const std::wstring& path) {
    if (path.size() < 240 || path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::vector<FileEntry> FallbackWalkFiles(
    const std::string& root, uint64_t minSize,
    const std::vector<std::string>& skipTop, size_t limit, bool* overflow) {
    std::vector<FileEntry> out;
    std::vector<std::wstring> tops;
    std::atomic<size_t> produced{0};
    std::atomic<bool> capped{false};

    auto overLimit = [limit, &produced, &capped]() {
        if (limit == 0) {
            return false;
        }
        if (produced.fetch_add(1) + 1 <= limit) {
            return false;
        }
        capped.store(true, std::memory_order_relaxed);
        return true;
    };

    WIN32_FIND_DATAW data;
    const std::wstring wideRoot = Utf8ToWide(root);
    HANDLE handle =
        FindFirstFileExW(LongPathForm(wideRoot + L"\\*").c_str(),
                         FindExInfoBasic, &data, FindExSearchNameMatch,
                         nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE) {
        return out;
    }
    do {
        if (data.cFileName[0] == L'.' &&
            (data.cFileName[1] == L'\0' ||
             (data.cFileName[1] == L'.' && data.cFileName[2] == L'\0'))) {
            continue;
        }
        if (data.cFileName[0] == L'$') {
            continue;
        }
        if ((data.dwFileAttributes & kSkipFileAttrs) != 0) {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const std::string lower = ToLowerAscii(WideToUtf8(data.cFileName));
            if (std::find(skipTop.begin(), skipTop.end(), lower) !=
                skipTop.end()) {
                continue;
            }
            tops.push_back(wideRoot + L"\\" + data.cFileName);
        } else {
            const uint64_t size =
                (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                static_cast<uint64_t>(data.nFileSizeLow);
            if (size < minSize) {
                continue;
            }
            if (overLimit()) {
                break;
            }
            FileEntry entry;
            entry.path = WideToUtf8(wideRoot + L"\\" + data.cFileName);
            entry.size = size;
            entry.mtime = FileTimeToUnix(
                (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                static_cast<uint64_t>(data.ftLastWriteTime.dwLowDateTime));
            out.push_back(std::move(entry));
        }
    } while (FindNextFileW(handle, &data) != 0);
    FindClose(handle);

    auto walkDir = [minSize, &overLimit, &capped](const std::wstring& start,
                                                  std::vector<FileEntry>& sink) {
        std::vector<std::wstring> stack = {start};
        while (!stack.empty()) {
            if (capped.load(std::memory_order_relaxed)) {
                return;
            }
            const std::wstring current = stack.back();
            stack.pop_back();
            WIN32_FIND_DATAW child;
            HANDLE find =
                FindFirstFileExW(LongPathForm(current + L"\\*").c_str(),
                                 FindExInfoBasic, &child, FindExSearchNameMatch,
                                 nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) {
                continue;
            }
            do {
                if (child.cFileName[0] == L'.' &&
                    (child.cFileName[1] == L'\0' ||
                     (child.cFileName[1] == L'.' && child.cFileName[2] == L'\0'))) {
                    continue;
                }
                if (child.cFileName[0] == L'$') {
                    continue;
                }
                if ((child.dwFileAttributes & kSkipFileAttrs) != 0) {
                    continue;
                }
                if ((child.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    stack.push_back(current + L"\\" + child.cFileName);
                    continue;
                }
                const uint64_t size =
                    (static_cast<uint64_t>(child.nFileSizeHigh) << 32) |
                    static_cast<uint64_t>(child.nFileSizeLow);
                if (size < minSize) {
                    continue;
                }
                if (overLimit()) {
                    break;
                }
                FileEntry entry;
                entry.path = WideToUtf8(current + L"\\" + child.cFileName);
                entry.size = size;
                entry.mtime = FileTimeToUnix(
                    (static_cast<uint64_t>(child.ftLastWriteTime.dwHighDateTime)
                     << 32) |
                    static_cast<uint64_t>(child.ftLastWriteTime.dwLowDateTime));
                sink.push_back(std::move(entry));
            } while (FindNextFileW(find, &child) != 0);
            FindClose(find);
        }
    };

    const std::optional<bool> ssd = disk::IsSsd(root);
    if (!ssd.has_value() || *ssd) {
        std::vector<std::vector<FileEntry>> parts(tops.size());
        std::atomic<size_t> next{0};
        const size_t threads =
            std::min(ParseThreadCount(), std::max<size_t>(1, tops.size()));
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (size_t index = 0; index < threads; ++index) {
            workers.emplace_back([&] {
                for (;;) {
                    const size_t slot = next.fetch_add(1);
                    if (slot >= tops.size()) {
                        break;
                    }
                    walkDir(tops[slot], parts[slot]);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        for (std::vector<FileEntry>& part : parts) {
            out.insert(out.end(), std::make_move_iterator(part.begin()),
                       std::make_move_iterator(part.end()));
        }
    } else {
        for (const std::wstring& dir : tops) {
            walkDir(dir, out);
        }
    }
    if (overflow != nullptr) {
        *overflow = capped.load(std::memory_order_relaxed);
    }
    return out;
}

std::optional<uint64_t> FindSubdirectoryIndex(const DirMap& dirs,
                                              const std::string& trimmed) {
    size_t pos = trimmed.find('\\');
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    while (pos < trimmed.size()) {
        while (pos < trimmed.size() &&
               (trimmed[pos] == '\\' || trimmed[pos] == '/')) {
            ++pos;
        }
        if (pos >= trimmed.size()) {
            break;
        }
        size_t nextPos = trimmed.find_first_of("\\/", pos);
        if (nextPos == std::string::npos) {
            nextPos = trimmed.size();
        }
        parts.push_back(trimmed.substr(pos, nextPos - pos));
        pos = nextPos;
    }
    if (parts.empty()) {
        return 5;
    }

    uint64_t currIndex = 5;
    for (const std::string& part : parts) {
        bool found = false;
        for (const auto& [idx, node] : dirs) {
            if ((node.parentFrn & 0xFFFF'FFFF'FFFFULL) == currIndex) {
                if (_stricmp(node.name.c_str(), part.c_str()) == 0) {
                    currIndex = idx;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return currIndex;
}

std::vector<FileEntry> ScanFiles(const std::string& root, uint64_t minSize,
                                 const std::vector<std::string>& skipTop) {
    return ScanFilesWhere(root, minSize, skipTop,
                          [](const std::string&, uint64_t) { return true; });
}

std::vector<FileEntry> ScanFilesWhere(const std::string& root, uint64_t minSize,
                                      const std::vector<std::string>& skipTop,
                                      const KeepFn& keep) {
    // ---------- 优先直读 $MFT ----------
    std::string trimmed = root;
    while (!trimmed.empty() && trimmed.back() == '\\') {
        trimmed.pop_back();
    }
    const std::wstring wideTrimmed = Utf8ToWide(trimmed);
    wchar_t longBuf[32768];
    if (GetLongPathNameW(wideTrimmed.c_str(), longBuf, 32768) != 0) {
        trimmed = WideToUtf8(longBuf);
        while (!trimmed.empty() && trimmed.back() == '\\') {
            trimmed.pop_back();
        }
    }
    const bool isDriveRoot = trimmed.size() == 2;
    // 子目录优先检查是否已有该卷的 $MFT 缓存（内存或磁盘快照）；
    // 若已有快照直接在内存中做子树匹配，避免跑耗时数十秒的目录递归。
    // 若尚未生成快照且子目录文件较少（<= 10000），则走轻量遍历；超过阈值则建全卷快照。
    if (!isDriveRoot && trimmed.size() > 2 && trimmed[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(trimmed[0])) != 0) {
        const std::string drive = DriveRootOf(trimmed);
        const std::string volumePath = "\\\\.\\" + drive;
        UniqueHandle volHandle = OpenVolumeHandle(volumePath);
        if (volHandle) {
            NTFS_VOLUME_DATA_BUFFER volData = {};
            DWORD volRet = 0;
            if (DeviceIoControl(volHandle.get(), FSCTL_GET_NTFS_VOLUME_DATA,
                                nullptr, 0, &volData, sizeof(volData), &volRet,
                                nullptr) != 0) {
                const uint64_t serial =
                    static_cast<uint64_t>(volData.VolumeSerialNumber.QuadPart);
                std::shared_ptr<Snapshot> cached = MemSnapshotGet(serial);
                if (!cached) {
                    if (auto loaded = LoadSnapshot(SnapshotPath(serial))) {
                        cached = std::make_shared<Snapshot>(std::move(*loaded));
                    }
                }
                if (!cached) {
                    bool overflow = false;
                    std::vector<FileEntry> walked = FallbackWalkFiles(
                        trimmed, minSize, skipTop, kSubtreeWalkLimit, &overflow);
                    if (!overflow) {
                        std::vector<FileEntry> filtered;
                        filtered.reserve(walked.size());
                        for (FileEntry& entry : walked) {
                            if (keep(Basename(entry.path), entry.size)) {
                                filtered.push_back(std::move(entry));
                            }
                        }
                        return filtered;
                    }
                }
            }
        }
    }
    if (trimmed.size() >= 2 && trimmed[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(trimmed[0])) != 0) {
        const std::string drive = DriveRootOf(trimmed);
        const std::string volumePath = "\\\\.\\" + drive;
        UniqueHandle handle = OpenVolumeHandle(volumePath);
        if (handle) {
            NTFS_VOLUME_DATA_BUFFER volume = {};
            DWORD returned = 0;
            const bool volumeOk =
                DeviceIoControl(handle.get(), FSCTL_GET_NTFS_VOLUME_DATA,
                                nullptr, 0, &volume, sizeof(volume), &returned,
                                nullptr) != 0;
            const size_t recordSize = volume.BytesPerFileRecordSegment;
            const size_t bytesPerCluster = volume.BytesPerCluster;
            const size_t bytesPerSector = volume.BytesPerSector;
            if (volumeOk && (recordSize == 1024 || recordSize == 4096) &&
                bytesPerCluster != 0 && bytesPerSector != 0) {
                const uint64_t serial =
                    static_cast<uint64_t>(volume.VolumeSerialNumber.QuadPart);
                USN_JOURNAL_DATA_V0 journal = {};
                const bool hasJournal = QueryUsnJournal(handle.get(), journal);
                const std::string snapshotFile = SnapshotPath(serial);
                const uint64_t storeMinSize =
                    minSize >= kSnapshotFloorMinSize ? kSnapshotFloorMinSize
                                                     : minSize;

                std::shared_ptr<Snapshot> snapshot;
                if (hasJournal) {
                    std::shared_ptr<Snapshot> cached = MemSnapshotGet(serial);
                    if (!cached) {
                        if (auto loaded = LoadSnapshot(snapshotFile)) {
                            cached = std::make_shared<Snapshot>(std::move(*loaded));
                        }
                    }
                    if (cached && cached->volumeSerial == serial &&
                        cached->journalId == journal.UsnJournalID &&
                        cached->bytesPerCluster == bytesPerCluster &&
                        cached->minSize <= minSize) {
                        if (cached->nextUsn ==
                            static_cast<int64_t>(journal.NextUsn)) {
                            MemSnapshotPut(serial, cached);
                            snapshot = cached;
                        } else if (cached->nextUsn >=
                                       static_cast<int64_t>(journal.FirstUsn) &&
                                   cached->nextUsn <
                                       static_cast<int64_t>(journal.NextUsn)) {
                            UsnDelta delta = CollectUsnChangedIndices(
                                handle.get(), journal, cached->nextUsn);
                            if (delta.kind == UsnDelta::Kind::Changed) {
                                MemSnapshotTake(serial);
                                Snapshot working;
                                if (cached.use_count() == 1) {
                                    working = std::move(*cached);
                                } else {
                                    working = *cached;
                                }
                                const std::vector<DeltaUpdate> updates =
                                    ApplyUsnDelta(
                                        handle.get(), volumePath, delta.changed,
                                        recordSize, bytesPerSector,
                                        bytesPerCluster, working.minSize);
                                for (const DeltaUpdate& update : updates) {
                                    working.dirs.erase(update.index);
                                    working.files.erase(update.index);
                                    if (update.record.kind ==
                                        ParsedRecord::Kind::Dir) {
                                        working.dirs[update.record.index] =
                                            update.record.node;
                                    } else if (update.record.kind ==
                                               ParsedRecord::Kind::File) {
                                        working.files[update.record.index] =
                                            update.record.raw;
                                    }
                                }
                                working.nextUsn =
                                    static_cast<int64_t>(journal.NextUsn);
                                auto updated =
                                    std::make_shared<Snapshot>(std::move(working));
                                MemSnapshotPut(serial, updated);
                                SaveSnapshotInBackground(snapshotFile, updated);
                                snapshot = std::move(updated);
                            } else if (delta.kind == UsnDelta::Kind::TooMany) {
                                MemSnapshotPut(serial, cached);
                                SpawnBackgroundRebuild(
                                    volumePath, serial, volume, storeMinSize,
                                    snapshotFile, recordSize);
                                snapshot = cached;
                            }
                        }
                    }
                }

                if (!snapshot) {
                    std::vector<uint8_t> first(recordSize);
                    const int64_t offset =
                        volume.MftStartLcn.QuadPart *
                        static_cast<int64_t>(bytesPerCluster);
                    if (ReadExactAt(handle.get(), offset, first.data(),
                                    recordSize) < recordSize) {
                        return FallbackWalkFiles(root, minSize, skipTop);
                    }
                    ParsedRecord rootRecord = ParseRecordInPlace(
                        first.data(), recordSize, 0, bytesPerSector,
                        bytesPerCluster, storeMinSize);
                    if (rootRecord.kind != ParsedRecord::Kind::MftRoot) {
                        return FallbackWalkFiles(root, minSize, skipTop);
                    }
                    auto built = std::make_shared<Snapshot>();
                    built->volumeSerial = serial;
                    built->journalId = hasJournal ? journal.UsnJournalID : 0;
                    built->nextUsn =
                        hasJournal ? static_cast<int64_t>(journal.NextUsn) : 0;
                    built->minSize = storeMinSize;
                    built->bytesPerCluster = volume.BytesPerCluster;
                    if (!FullMftReadParallel(handle.get(), volume,
                                             rootRecord.runs, storeMinSize,
                                             built->dirs, built->files)) {
                        return FallbackWalkFiles(root, minSize, skipTop);
                    }
                    if (hasJournal && storeMinSize >= kSnapshotFloorMinSize) {
                        MemSnapshotPut(serial, built);
                        SaveSnapshotInBackground(snapshotFile, built);
                    }
                    snapshot = std::move(built);
                }

                const DirMap& dirs = snapshot->dirs;
                const FileMap& files = snapshot->files;
                const std::string rootLower = ToLowerAscii(trimmed);
                const std::string rootPrefix = rootLower + "\\";
                const size_t rootLen = rootLower.size() + 1;
                const uint64_t nowUnix = NowUnixSeconds();

                std::optional<uint64_t> targetDirIndex;
                if (!isDriveRoot) {
                    targetDirIndex = FindSubdirectoryIndex(dirs, trimmed);
                }

                std::unordered_map<uint64_t, bool> inSubtreeMemo;
                if (targetDirIndex.has_value()) {
                    inSubtreeMemo[*targetDirIndex] = true;
                    inSubtreeMemo[5] = false;
                }
                auto isInSubtree = [&](uint64_t parentFrn) -> bool {
                    uint64_t curr = parentFrn & 0xFFFF'FFFF'FFFFULL;
                    std::vector<uint64_t> chain;
                    while (curr != 0) {
                        auto it = inSubtreeMemo.find(curr);
                        if (it != inSubtreeMemo.end()) {
                            const bool ans = it->second;
                            for (const uint64_t c : chain) {
                                inSubtreeMemo[c] = ans;
                            }
                            return ans;
                        }
                        chain.push_back(curr);
                        auto dit = dirs.find(curr);
                        if (dit == dirs.end()) {
                            break;
                        }
                        curr = dit->second.parentFrn & 0xFFFF'FFFF'FFFFULL;
                    }
                    for (const uint64_t c : chain) {
                        inSubtreeMemo[c] = false;
                    }
                    return false;
                };

                std::unordered_map<uint64_t, std::optional<std::string>>
                    dirCache;
                dirCache.reserve(std::min<size_t>(dirs.size(), 65536));
                std::vector<FileEntry> entries;
                entries.reserve(files.size());

                for (const auto& [index, raw] : files) {
                    (void)index;
                    if (raw.size < minSize || !keep(raw.name, raw.size)) {
                        continue;
                    }
                    if (!isDriveRoot && targetDirIndex.has_value()) {
                        if (!isInSubtree(raw.parentFrn)) {
                            continue;
                        }
                    }
                    std::string dirPath;
                    if ((raw.parentFrn & 0xFFFF'FFFF'FFFFULL) == 5) {
                        dirPath = drive + "\\";
                    } else {
                        const std::optional<std::string> resolved =
                            ResolveDir(raw.parentFrn, dirs, dirCache, drive,
                                       isDriveRoot, skipTop);
                        if (!resolved.has_value()) {
                            continue;
                        }
                        dirPath = *resolved;
                    }

                    std::string fullPath;
                    fullPath.reserve(dirPath.size() + 1 + raw.name.size());
                    if (!dirPath.empty() && dirPath.back() == '\\') {
                        fullPath = dirPath + raw.name;
                    } else {
                        fullPath = dirPath + "\\" + raw.name;
                    }

                    if (!isDriveRoot && !targetDirIndex.has_value()) {
                        const std::string lower = ToLowerAscii(fullPath);
                        if (lower.compare(0, rootPrefix.size(), rootPrefix) !=
                            0) {
                            continue;
                        }
                        if (lower.size() > rootLen) {
                            const size_t end = lower.find('\\', rootLen);
                            const std::string first =
                                lower.substr(rootLen, end == std::string::npos
                                                          ? std::string::npos
                                                          : end - rootLen);
                            if (std::find(skipTop.begin(), skipTop.end(),
                                          first) != skipTop.end()) {
                                continue;
                            }
                        }
                    }

                    const bool recent =
                        raw.mtime == 0 ||
                        nowUnix - raw.mtime < kRawReadMinAgeSecs;
                    FileEntry entry;
                    entry.path = std::move(fullPath);
                    entry.size = raw.size;
                    entry.mtime = raw.mtime;
                    entry.sortKey = raw.sortKey;
                    entry.frn = raw.frn;
                    entry.firstLcn = raw.firstLcn;
                    entry.bytesPerCluster = static_cast<uint32_t>(bytesPerCluster);
                    entry.volumeSerial = serial;
                    entry.canRawRead = raw.canRawRead && !recent;
                    entry.canRawFull = raw.canRawFull && !recent;
                    entry.canRawStrong = raw.canRawStrong && !recent;
                    entry.midRawOffset =
                        entry.canRawStrong ? raw.midRawOffset : 0;
                    entry.tailRawOffset =
                        entry.canRawStrong ? raw.tailRawOffset : 0;
                    entries.push_back(std::move(entry));
                }
                return entries;
            }
        }
    }

    std::vector<FileEntry> entries = FallbackWalkFiles(root, minSize, skipTop);
    std::vector<FileEntry> filtered;
    filtered.reserve(entries.size());
    for (FileEntry& entry : entries) {
        const std::string name = Basename(entry.path);
        if (keep(name, entry.size)) {
            filtered.push_back(std::move(entry));
        }
    }
    return filtered;
}

void WaitForBackgroundWrites(int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (BackgroundWrites().load() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool SaveSnapshot(const std::string& path, const Snapshot& snapshot) {
    const std::string temp = path + ".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, Utf8ToWide(temp).c_str(), L"wb") != 0 ||
        file == nullptr) {
        return false;
    }
    const size_t estimated =
        128 + snapshot.dirs.size() * 48 + snapshot.files.size() * 96;
    std::vector<uint8_t> buffer;
    buffer.reserve(std::min<size_t>(estimated, 64 * 1024 * 1024));

    auto appendRaw = [&](const void* data, size_t len) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), b, b + len);
    };
    auto appendU8 = [&](uint8_t v) { buffer.push_back(v); };
    auto appendU16 = [&](uint16_t v) { appendRaw(&v, 2); };
    auto appendU32 = [&](uint32_t v) { appendRaw(&v, 4); };
    auto appendU64 = [&](uint64_t v) { appendRaw(&v, 8); };
    auto appendStr = [&](const std::string& text) {
        appendU32(static_cast<uint32_t>(text.size()));
        appendRaw(text.data(), text.size());
    };

    appendRaw("MFT4", 4);
    appendU64(snapshot.volumeSerial);
    appendU64(snapshot.journalId);
    appendU64(static_cast<uint64_t>(snapshot.nextUsn));
    appendU64(snapshot.minSize);
    appendU32(snapshot.bytesPerCluster);
    appendU32(static_cast<uint32_t>(snapshot.dirs.size()));
    for (const auto& [index, node] : snapshot.dirs) {
        appendU64(index);
        appendU16(node.seq);
        appendU8(node.reparse ? 1 : 0);
        appendU64(node.parentFrn);
        appendStr(node.name);
    }
    appendU32(static_cast<uint32_t>(snapshot.files.size()));
    for (const auto& [index, raw] : snapshot.files) {
        appendU64(index);
        appendU64(raw.frn);
        appendU64(raw.parentFrn);
        appendU64(raw.size);
        appendU64(raw.mtime);
        appendU64(raw.firstLcn);
        appendU64(raw.sortKey);
        const uint8_t rawFlags = (raw.canRawRead ? 1 : 0) |
                                 (raw.canRawFull ? 2 : 0) |
                                 (raw.canRawStrong ? 4 : 0);
        appendU8(rawFlags);
        if (raw.canRawStrong) {
            appendU64(raw.midRawOffset);
            appendU64(raw.tailRawOffset);
        }
        appendStr(raw.name);
    }

    const size_t written = fwrite(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    if (written != buffer.size()) {
        DeleteFileW(Utf8ToWide(temp).c_str());
        return false;
    }
    if (MoveFileExW(Utf8ToWide(temp).c_str(), Utf8ToWide(path).c_str(),
                    MOVEFILE_REPLACE_EXISTING) == 0) {
        DeleteFileW(Utf8ToWide(temp).c_str());
        return false;
    }
    return true;
}

std::optional<Snapshot> LoadSnapshot(const std::string& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, Utf8ToWide(path).c_str(), L"rb") != 0 ||
        file == nullptr) {
        return std::nullopt;
    }
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        fclose(file);
        return std::nullopt;
    }
    const int64_t fileSize = _ftelli64(file);
    if (fileSize < 40 || fileSize > 1024LL * 1024 * 1024) {
        fclose(file);
        return std::nullopt;
    }
    _fseeki64(file, 0, SEEK_SET);

    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    const size_t bytesRead = fread(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    if (bytesRead != buffer.size()) {
        return std::nullopt;
    }

    const uint8_t* ptr = buffer.data();
    const uint8_t* end = buffer.data() + buffer.size();

    auto readRaw = [&](void* dst, size_t n) -> bool {
        if (ptr + n > end) return false;
        std::memcpy(dst, ptr, n);
        ptr += n;
        return true;
    };
    auto readU8 = [&](uint8_t& val) -> bool { return readRaw(&val, 1); };
    auto readU16 = [&](uint16_t& val) -> bool { return readRaw(&val, 2); };
    auto readU32 = [&](uint32_t& val) -> bool { return readRaw(&val, 4); };
    auto readU64 = [&](uint64_t& val) -> bool { return readRaw(&val, 8); };
    auto readStr = [&](std::string& text) -> bool {
        uint32_t length = 0;
        if (!readU32(length) || length > 32 * 1024) return false;
        if (ptr + length > end) return false;
        text.assign(reinterpret_cast<const char*>(ptr), length);
        ptr += length;
        return true;
    };

    char magic[4] = {};
    if (!readRaw(magic, 4)) {
        return std::nullopt;
    }
    const bool isV3 = (std::memcmp(magic, "MFT3", 4) == 0);
    const bool isV4 = (std::memcmp(magic, "MFT4", 4) == 0);
    // MFT5 的 mid/tail 偏移按 64KB 计算，和当前 256KB 强采样不兼容。
    if (!isV3 && !isV4) {
        return std::nullopt;
    }
    Snapshot snapshot;
    uint64_t nextUsn = 0;
    bool ok = readU64(snapshot.volumeSerial) && readU64(snapshot.journalId) &&
              readU64(nextUsn) && readU64(snapshot.minSize) &&
              readU32(snapshot.bytesPerCluster);
    if (!ok) return std::nullopt;
    snapshot.nextUsn = static_cast<int64_t>(nextUsn);

    uint32_t dirCount = 0;
    if (!readU32(dirCount)) return std::nullopt;
    snapshot.dirs.reserve(dirCount);
    for (uint32_t i = 0; i < dirCount; ++i) {
        uint64_t id = 0;
        DirNode node;
        uint8_t reparse = 0;
        if (!readU64(id) || !readU16(node.seq) || !readU8(reparse) ||
            !readU64(node.parentFrn) || !readStr(node.name)) {
            return std::nullopt;
        }
        node.reparse = (reparse != 0);
        snapshot.dirs[id] = std::move(node);
    }

    uint32_t fileCount = 0;
    if (!readU32(fileCount)) return std::nullopt;
    snapshot.files.reserve(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i) {
        uint64_t id = 0;
        RawFile raw;
        uint8_t rawFlags = 0;
        if (!readU64(id) || !readU64(raw.frn) || !readU64(raw.parentFrn) ||
            !readU64(raw.size) || !readU64(raw.mtime) ||
            !readU64(raw.firstLcn) || !readU64(raw.sortKey) ||
            !readU8(rawFlags)) {
            return std::nullopt;
        }
        raw.canRawRead = (rawFlags & 1) != 0;
        raw.canRawFull = (rawFlags & 2) != 0;
        raw.canRawStrong = isV4 && ((rawFlags & 4) != 0);
        if (raw.canRawStrong) {
            if (!readU64(raw.midRawOffset) || !readU64(raw.tailRawOffset)) {
                return std::nullopt;
            }
        }
        if (!readStr(raw.name)) {
            return std::nullopt;
        }
        snapshot.files[id] = std::move(raw);
    }
    return snapshot;
}

}  // namespace mft
