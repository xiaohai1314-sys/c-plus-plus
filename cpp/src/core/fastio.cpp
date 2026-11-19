#include "core/fastio.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "blake3.h"
#include "core/mft.h"
#include "util/utf.h"

namespace {

constexpr DWORD kShareAll =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr size_t kHeadSize = 4096;

char UpperLetter(const std::string& path) {
    for (const char ch : path) {
        if (ch != '\\' && ch != '/') {
            return static_cast<char>(
                std::toupper(static_cast<unsigned char>(ch)));
        }
    }
    return 'C';
}

std::optional<std::pair<uint32_t, uint64_t>> IdentityOf(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(handle, &info) == 0) {
        return std::nullopt;
    }
    const uint64_t index =
        (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
        static_cast<uint64_t>(info.nFileIndexLow);
    if (info.nNumberOfLinks > 1 && index != 0) {
        return std::make_pair(static_cast<uint32_t>(info.dwVolumeSerialNumber),
                              index);
    }
    return std::nullopt;
}

UniqueHandle OpenRawHandleByIdOrPath(HANDLE hint, uint64_t frn,
                                     const std::string& path, DWORD flags) {
    if (frn != 0 && hint != nullptr) {
        FILE_ID_DESCRIPTOR descriptor = {};
        descriptor.dwSize = sizeof(descriptor);
        descriptor.Type = FileIdType;
        descriptor.FileId.QuadPart = static_cast<LONGLONG>(frn);
        HANDLE handle =
            OpenFileById(hint, &descriptor, GENERIC_READ, kShareAll, nullptr,
                         flags);
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            return UniqueHandle(handle);
        }
    }
    HANDLE handle = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ,
                                kShareAll, nullptr, OPEN_EXISTING, flags,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return UniqueHandle();
    }
    return UniqueHandle(handle);
}

struct IocpSlot {
    OVERLAPPED overlapped = {};
    HANDLE handle = nullptr;
    size_t itemIdx = 0;
    size_t want = 0;
    bool active = false;
    std::array<uint8_t, kHeadSize> buffer = {};
};

class VirtualBuffer {
public:
    VirtualBuffer() = default;
    ~VirtualBuffer() { Reset(); }
    VirtualBuffer(const VirtualBuffer&) = delete;
    VirtualBuffer& operator=(const VirtualBuffer&) = delete;

    bool Allocate(size_t bytes) {
        Reset();
        if (bytes == 0) {
            return true;
        }
        void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE);
        if (p == nullptr) {
            return false;
        }
        ptr_ = static_cast<uint8_t*>(p);
        size_ = bytes;
        return true;
    }

    void Reset() {
        if (ptr_ != nullptr) {
            VirtualFree(ptr_, 0, MEM_RELEASE);
            ptr_ = nullptr;
            size_ = 0;
        }
    }

    uint8_t* data() const { return ptr_; }
    size_t size() const { return size_; }

private:
    uint8_t* ptr_ = nullptr;
    size_t size_ = 0;
};

uint64_t QueryVolumeAlign(HANDLE volume) {
    DISK_GEOMETRY geom = {};
    DWORD returned = 0;
    if (DeviceIoControl(volume, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0,
                        &geom, sizeof(geom), &returned, nullptr) != 0 &&
        geom.BytesPerSector > 0) {
        return std::max<uint64_t>(geom.BytesPerSector, 4096);
    }
    return 4096;
}

uint64_t AlignDown(uint64_t value, uint64_t align) {
    return (value / align) * align;
}

uint64_t AlignUp(uint64_t value, uint64_t align) {
    return ((value + align - 1) / align) * align;
}

struct VolumeSpan {
    uint64_t startOffset = 0;
    size_t length = 0;
    size_t opBegin = 0;
    size_t opEnd = 0;
};

template <typename Op, typename GetOffset, typename GetLen>
std::vector<VolumeSpan> BuildVolumeSpans(const std::vector<Op>& ops,
                                         uint64_t align, uint64_t maxGap,
                                         uint64_t maxSpan, GetOffset getOffset,
                                         GetLen getLen) {
    std::vector<VolumeSpan> spans;
    spans.reserve(ops.size());
    size_t index = 0;
    while (index < ops.size()) {
        const uint64_t firstOff = getOffset(ops[index]);
        const uint64_t firstEnd = firstOff + getLen(ops[index]);
        const uint64_t spanStart = AlignDown(firstOff, align);
        uint64_t spanEnd =
            std::max(AlignUp(firstEnd, align), spanStart + align);
        size_t next = index + 1;
        while (next < ops.size()) {
            const uint64_t nOff = getOffset(ops[next]);
            const uint64_t nEnd = nOff + getLen(ops[next]);
            const uint64_t aStart = AlignDown(nOff, align);
            const uint64_t aEnd = std::max(AlignUp(nEnd, align), aStart + align);
            const uint64_t candEnd = std::max(spanEnd, aEnd);
            if (aStart <= spanEnd + maxGap && candEnd - spanStart <= maxSpan) {
                spanEnd = candEnd;
                ++next;
            } else {
                break;
            }
        }
        VolumeSpan span;
        span.startOffset = spanStart;
        span.length = static_cast<size_t>(spanEnd - spanStart);
        span.opBegin = index;
        span.opEnd = next;
        spans.push_back(span);
        index = next;
    }
    return spans;
}

template <typename OnDone, typename OnFail>
void RunRawVolumeSpansIocp(HANDLE volume, uint64_t align,
                           const std::vector<VolumeSpan>& spans, bool isSsd,
                           const std::atomic<bool>& cancel, OnDone onDone,
                           OnFail onFail) {
    if (spans.empty()) {
        return;
    }
    HANDLE portRaw =
        CreateIoCompletionPort(volume, nullptr, 0, 0);
    if (portRaw == nullptr) {
        for (const VolumeSpan& sp : spans) {
            onFail(sp);
        }
        return;
    }
    UniqueHandle port(portRaw);

    size_t maxLen = 0;
    for (const VolumeSpan& sp : spans) {
        if (sp.length > maxLen) {
            maxLen = sp.length;
        }
    }
    const size_t alignSz = static_cast<size_t>(align);
    const size_t slotStride = ((maxLen + alignSz - 1) / alignSz) * alignSz;
    const size_t qd =
        std::min<size_t>(spans.size(), isSsd ? 64 : 32);

    VirtualBuffer vbuf;
    if (!vbuf.Allocate(slotStride * qd)) {
        for (const VolumeSpan& sp : spans) {
            onFail(sp);
        }
        return;
    }

    std::vector<OVERLAPPED> ovs(qd);
    std::vector<size_t> slotSpan(qd, 0);
    size_t nextSpan = 0;
    size_t inFlight = 0;
    bool cancelIssued = false;

    auto submitSlot = [&](size_t slot) -> bool {
        while (nextSpan < spans.size() && !cancel.load()) {
            const size_t spIdx = nextSpan++;
            const VolumeSpan& sp = spans[spIdx];
            OVERLAPPED& ov = ovs[slot];
            ov = {};
            ov.Offset = static_cast<DWORD>(sp.startOffset & 0xFFFF'FFFFULL);
            ov.OffsetHigh = static_cast<DWORD>(sp.startOffset >> 32);
            slotSpan[slot] = spIdx;
            DWORD rd = 0;
            const BOOL ok = ReadFile(volume, vbuf.data() + slot * slotStride,
                                     static_cast<DWORD>(sp.length), &rd, &ov);
            if (ok != 0 || GetLastError() == ERROR_IO_PENDING) {
                ++inFlight;
                return true;
            }
            onFail(sp);
        }
        return false;
    };

    for (size_t s = 0; s < qd; ++s) {
        if (!submitSlot(s)) {
            break;
        }
    }

    while (inFlight > 0) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ovPtr = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(
            port.get(), &transferred, &key, &ovPtr, INFINITE);
        if (ovPtr == nullptr) {
            break;
        }
        const size_t slot = static_cast<size_t>(ovPtr - ovs.data());
        --inFlight;
        const VolumeSpan& sp = spans[slotSpan[slot]];
        if (ok != 0 && transferred >= sp.length && !cancel.load()) {
            onDone(sp, vbuf.data() + slot * slotStride, transferred);
        } else {
            onFail(sp);
        }
        if (cancel.load()) {
            if (!cancelIssued) {
                cancelIssued = true;
                CancelIoEx(volume, nullptr);
            }
        } else {
            submitSlot(slot);
        }
    }
}

}  // namespace

namespace fastio {

UniqueHandle OpenDriveHint(char driveLetter) {
    const std::wstring root =
        std::wstring(1, static_cast<wchar_t>(std::toupper(
                            static_cast<unsigned char>(driveLetter)))) +
        L":\\";
    HANDLE handle = CreateFileW(root.c_str(), 0, kShareAll, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return UniqueHandle();
    }
    return UniqueHandle(handle);
}

std::optional<OpenedFile> OpenCheckedFileFast(HANDLE hint, uint64_t frn,
                                              const std::string& path,
                                              DWORD flags) {
    UniqueHandle file = OpenRawHandleByIdOrPath(hint, frn, path, flags);
    if (!file) {
        return std::nullopt;
    }
    OpenedFile result;
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(file.get(), &info) != 0) {
        if ((info.dwFileAttributes & mft::kSkipFileAttrs) != 0) {
            return std::nullopt;
        }
        const uint64_t index =
            (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
            static_cast<uint64_t>(info.nFileIndexLow);
        if (info.nNumberOfLinks > 1 && index != 0) {
            result.fileId = std::make_pair(
                static_cast<uint32_t>(info.dwVolumeSerialNumber), index);
        }
        result.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                      static_cast<uint64_t>(info.nFileSizeLow);
    } else {
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(file.get(), &size) != 0) {
            result.size = static_cast<uint64_t>(size.QuadPart);
        }
    }
    result.handle = std::move(file);
    return result;
}

std::optional<std::pair<uint32_t, uint64_t>> QueryFileIdentity(
    const std::string& path) {
    HANDLE handle = CreateFileW(Utf8ToWide(path).c_str(), FILE_READ_ATTRIBUTES,
                                kShareAll, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    UniqueHandle guard(handle);
    return IdentityOf(guard.get());
}

std::vector<RawHeadResult> CoalescedRawVolumeHeads(
    char driveLetter, std::vector<RawHeadTarget> targets, bool isSsd,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick) {
    std::vector<RawHeadResult> out;
    if (targets.empty()) {
        return out;
    }
    const std::wstring volumePath =
        std::wstring(L"\\\\.\\") +
        static_cast<wchar_t>(
            std::toupper(static_cast<unsigned char>(driveLetter))) +
        L":";
    HANDLE raw = CreateFileW(volumePath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING,
                             nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return out;
    }
    UniqueHandle volume(raw);

    std::sort(targets.begin(), targets.end(),
              [](const RawHeadTarget& left, const RawHeadTarget& right) {
                  return left.offset < right.offset;
              });

    const uint64_t align = QueryVolumeAlign(volume.get());
    const uint64_t maxGap = isSsd ? 16 * 1024 : 64 * 1024;
    const uint64_t maxSpan = 512 * 1024;
    const std::vector<VolumeSpan> spans = BuildVolumeSpans(
        targets, align, maxGap, maxSpan,
        [](const RawHeadTarget& t) { return t.offset; },
        [](const RawHeadTarget& t) {
            return static_cast<uint64_t>(t.want);
        });

    out.reserve(targets.size());
    RunRawVolumeSpansIocp(
        volume.get(), align, spans, isSsd, cancel,
        [&](const VolumeSpan& sp, const uint8_t* data, DWORD transferred) {
            for (size_t i = sp.opBegin; i < sp.opEnd; ++i) {
                const RawHeadTarget& t = targets[i];
                const size_t rel = static_cast<size_t>(t.offset - sp.startOffset);
                if (rel + t.want > transferred) {
                    continue;
                }
                RawHeadResult res;
                res.idx = t.idx;
                const size_t headLen = std::min<size_t>(t.want, kHeadSize);
                res.headDigest = Blake3Hash(data + rel, headLen);
                if (t.captureStrongHead && t.want > kHeadSize) {
                    res.strongHeadState = std::make_unique<blake3_hasher>();
                    blake3_hasher_init(res.strongHeadState.get());
                    blake3_hasher_update(res.strongHeadState.get(), data + rel,
                                         t.want);
                }
                out.push_back(std::move(res));
                onTick();
            }
        },
        [](const VolumeSpan&) {});

    if (std::getenv("CDC_TRACE") != nullptr) {
        std::fprintf(stderr, "raw-head[%c]: %zu 个文件 -> %zu 次并发读(%s)\n",
                     driveLetter, targets.size(), spans.size(),
                     isSsd ? "固态" : "机械");
    }
    return out;
}

std::vector<std::pair<size_t, Digest>> CoalescedRawVolumeTriples(
    char driveLetter, std::vector<RawTripleTarget> targets, bool isSsd,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick) {
    std::vector<std::pair<size_t, Digest>> out;
    if (targets.empty()) {
        return out;
    }
    const std::wstring volumePath =
        std::wstring(L"\\\\.\\") +
        static_cast<wchar_t>(
            std::toupper(static_cast<unsigned char>(driveLetter))) +
        L":";
    HANDLE raw = CreateFileW(volumePath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING,
                             nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return out;
    }
    UniqueHandle volume(raw);

    struct FileAssembly {
        size_t userIdx = 0;
        uint8_t nextPart = 0;
        bool failed = false;
        std::unique_ptr<blake3_hasher> hasher;
        std::vector<uint8_t> pending[2];
    };
    struct ChunkOp {
        size_t fileSlot = 0;
        uint8_t part = 0;
        uint64_t rawOffset = 0;
        size_t len = 0;
    };

    std::vector<FileAssembly> assemblies(targets.size());
    std::vector<ChunkOp> ops;
    ops.reserve(targets.size() * 3);
    size_t maxChunk = 0;

    for (size_t i = 0; i < targets.size(); ++i) {
        RawTripleTarget& t = targets[i];
        FileAssembly& asmState = assemblies[i];
        asmState.userIdx = t.idx;
        if (t.chunkLen == 0) {
            asmState.failed = true;
            continue;
        }
        if (t.chunkLen > maxChunk) {
            maxChunk = t.chunkLen;
        }
        if (t.preSeededHead != nullptr) {
            asmState.nextPart = 1;
            asmState.hasher = std::move(t.preSeededHead);
        } else {
            asmState.nextPart = 0;
            ops.push_back(ChunkOp{i, 0, t.headOffset, t.chunkLen});
        }
        ops.push_back(ChunkOp{i, 1, t.midOffset, t.chunkLen});
        ops.push_back(ChunkOp{i, 2, t.tailOffset, t.chunkLen});
    }

    if (ops.empty()) {
        return out;
    }

    std::sort(ops.begin(), ops.end(),
              [](const ChunkOp& left, const ChunkOp& right) {
                  return left.rawOffset < right.rawOffset;
              });

    const uint64_t align = QueryVolumeAlign(volume.get());
    const uint64_t maxGap = isSsd ? 16 * 1024 : 64 * 1024;
    const uint64_t maxSpan =
        (maxChunk >= 256 * 1024) ? (544 * 1024) : (256 * 1024);
    const std::vector<VolumeSpan> spans = BuildVolumeSpans(
        ops, align, maxGap, maxSpan,
        [](const ChunkOp& op) { return op.rawOffset; },
        [](const ChunkOp& op) { return static_cast<uint64_t>(op.len); });

    auto failFile = [](FileAssembly& asmState) {
        asmState.failed = true;
        asmState.hasher.reset();
        std::vector<uint8_t>().swap(asmState.pending[0]);
        std::vector<uint8_t>().swap(asmState.pending[1]);
    };

    out.reserve(targets.size());
    RunRawVolumeSpansIocp(
        volume.get(), align, spans, isSsd, cancel,
        [&](const VolumeSpan& sp, const uint8_t* data, DWORD transferred) {
            for (size_t i = sp.opBegin; i < sp.opEnd; ++i) {
                const ChunkOp& op = ops[i];
                FileAssembly& asmState = assemblies[op.fileSlot];
                if (asmState.failed) {
                    continue;
                }
                const size_t rel =
                    static_cast<size_t>(op.rawOffset - sp.startOffset);
                if (rel + op.len > transferred) {
                    failFile(asmState);
                    continue;
                }
                const uint8_t* slice = data + rel;
                if (op.part == asmState.nextPart) {
                    if (asmState.hasher == nullptr) {
                        asmState.hasher = std::make_unique<blake3_hasher>();
                        blake3_hasher_init(asmState.hasher.get());
                    }
                    blake3_hasher_update(asmState.hasher.get(), slice, op.len);
                    ++asmState.nextPart;
                    while (asmState.nextPart < 3) {
                        std::vector<uint8_t>& buf =
                            asmState.pending[asmState.nextPart - 1];
                        if (buf.empty()) {
                            break;
                        }
                        blake3_hasher_update(asmState.hasher.get(), buf.data(),
                                             buf.size());
                        std::vector<uint8_t>().swap(buf);
                        ++asmState.nextPart;
                    }
                    if (asmState.nextPart == 3) {
                        Digest digest{};
                        blake3_hasher_finalize(asmState.hasher.get(),
                                               digest.data(), digest.size());
                        asmState.hasher.reset();
                        out.emplace_back(asmState.userIdx, digest);
                        onTick();
                    }
                } else if (op.part > asmState.nextPart && op.part <= 2) {
                    asmState.pending[op.part - 1].assign(slice, slice + op.len);
                }
            }
        },
        [&](const VolumeSpan& sp) {
            for (size_t i = sp.opBegin; i < sp.opEnd; ++i) {
                failFile(assemblies[ops[i].fileSlot]);
            }
        });

    if (std::getenv("CDC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "raw-triple[%c]: %zu 个文件 (%zu/%zu 成功, %zu 次并发读 %s)\n",
                     driveLetter, targets.size(), out.size(), targets.size(),
                     spans.size(), isSsd ? "固态" : "机械");
    }
    return out;
}

namespace {

template <typename Request>
std::unordered_map<char, UniqueHandle> BuildDriveHints(
    const std::vector<Request>& requests) {
    std::unordered_map<char, UniqueHandle> hints;
    for (const Request& request : requests) {
        if (request.path.empty() || request.frn == 0) {
            continue;
        }
        const char letter = UpperLetter(request.path);
        if (hints.find(letter) == hints.end()) {
            UniqueHandle hint = OpenDriveHint(letter);
            if (hint) {
                hints.emplace(letter, std::move(hint));
            }
        }
    }
    return hints;
}

template <typename Request>
class PrefetchOpenerPool {
public:
    PrefetchOpenerPool(std::vector<Request>& requests,
                       const std::unordered_map<char, UniqueHandle>& hints,
                       size_t poolSize, size_t openerCount,
                       const std::atomic<bool>& cancel)
        : requests_(requests),
          cancel_(cancel),
          maxQueue_(poolSize * 2),
          activeOpeners_(openerCount) {
        openers_.reserve(openerCount);
        for (size_t t = 0; t < openerCount; ++t) {
            openers_.emplace_back([this, &hints] {
                for (;;) {
                    const size_t index = openCursor_.fetch_add(1);
                    if (index >= requests_.size() || cancel_.load()) {
                        break;
                    }
                    const Request& request = requests_[index];
                    if (request.path.empty()) {
                        continue;
                    }
                    const char letter = UpperLetter(request.path);
                    const auto hint = hints.find(letter);
                    HANDLE hintHandle =
                        hint == hints.end() ? nullptr : hint->second.get();
                    UniqueHandle handle = OpenRawHandleByIdOrPath(
                        hintHandle, request.frn, request.path,
                        FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS);
                    if (!handle) {
                        continue;
                    }
                    if (request.frn == 0) {
                        BY_HANDLE_FILE_INFORMATION info = {};
                        if (GetFileInformationByHandle(handle.get(), &info) !=
                                0 &&
                            (info.dwFileAttributes & mft::kSkipFileAttrs) !=
                                0) {
                            continue;
                        }
                    }
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        spaceCv_.wait(lock, [this] {
                            return ready_.size() < maxQueue_ || cancel_.load();
                        });
                        if (cancel_.load()) {
                            break;
                        }
                        ready_.push_back(
                            PendingHandle{std::move(handle), index});
                    }
                    openCv_.notify_one();
                }
                activeOpeners_.fetch_sub(1);
                openCv_.notify_all();
            });
        }
    }

    ~PrefetchOpenerPool() { Shutdown(); }

    PrefetchOpenerPool(const PrefetchOpenerPool&) = delete;
    PrefetchOpenerPool& operator=(const PrefetchOpenerPool&) = delete;

    bool Pop(Request** outRequest, UniqueHandle* outHandle) {
        std::unique_lock<std::mutex> lock(mutex_);
        openCv_.wait(lock, [this] {
            return !ready_.empty() || activeOpeners_.load() == 0 ||
                   cancel_.load();
        });
        if (ready_.empty()) {
            return false;
        }
        PendingHandle pending = std::move(ready_.front());
        ready_.pop_front();
        spaceCv_.notify_one();
        *outRequest = &requests_[pending.reqPos];
        *outHandle = std::move(pending.handle);
        return true;
    }

    void Shutdown() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        openCursor_.store(requests_.size());
        spaceCv_.notify_all();
        openCv_.notify_all();
        for (std::thread& opener : openers_) {
            if (opener.joinable()) {
                opener.join();
            }
        }
        ready_.clear();
    }

private:
    struct PendingHandle {
        UniqueHandle handle;
        size_t reqPos = 0;
    };

    std::vector<Request>& requests_;
    const std::atomic<bool>& cancel_;
    size_t maxQueue_ = 1;
    std::atomic<size_t> openCursor_{0};
    std::atomic<size_t> activeOpeners_{0};
    std::mutex mutex_;
    std::deque<PendingHandle> ready_;
    std::condition_variable openCv_;
    std::condition_variable spaceCv_;
    std::vector<std::thread> openers_;
    bool stopped_ = false;
};

struct IocpTripleSlot {
    HANDLE handle = nullptr;
    size_t userIdx = 0;
    uint64_t fileSize = 0;
    size_t chunkLen = 0;
    bool hasFileId = false;
    bool isFullSeq = false;
    bool active = false;
    uint8_t part = 0;
    uint64_t offsets[3] = {};
    uint64_t currentOffset = 0;
    size_t partHave = 0;
    size_t partWant = 0;
    uint64_t seqDone = 0;
    blake3_hasher hasher{};
    std::vector<uint8_t> buffer;
};

}  // namespace

std::vector<IocpHeadResult> IocpReadHeads(
    std::vector<IocpHeadRequest> requests, size_t concurrency,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick) {
    std::vector<IocpHeadResult> results;
    if (requests.empty()) {
        return results;
    }
    HANDLE portRaw =
        CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (portRaw == nullptr) {
        return results;
    }
    UniqueHandle port(portRaw);

    const std::unordered_map<char, UniqueHandle> hints =
        BuildDriveHints(requests);

    const size_t poolSize =
        std::min<size_t>(std::min<size_t>(std::max<size_t>(concurrency, 1), 64),
                         requests.size());
    std::vector<IocpSlot> slots(poolSize);
    std::vector<size_t> freeSlots;
    freeSlots.reserve(poolSize);
    for (size_t index = poolSize; index > 0; --index) {
        freeSlots.push_back(index - 1);
    }
    results.reserve(requests.size());
    size_t inFlight = 0;

    // 句柄打开（CreateFileW / OpenFileById）会走内核 Minifilter 同步路径，
    // 若在完成端口调度线程里串行执行，异步读会长期饥饿；并发预开句柄与 I/O 重叠。
    const size_t openerCount =
        std::min<size_t>(4, std::max<size_t>(1, poolSize));
    PrefetchOpenerPool<IocpHeadRequest> openerPool(
        requests, hints, poolSize, openerCount, cancel);

    auto submitSlot = [&](size_t slotIndex, IocpHeadRequest& request,
                          UniqueHandle& handle) -> bool {
        IocpSlot& slot = slots[slotIndex];
        HANDLE bound = CreateIoCompletionPort(handle.get(), port.get(),
                                              slotIndex, 0);
        if (bound == nullptr) {
            onTick();
            return false;
        }

        const size_t want = std::min(request.want, kHeadSize);
        if (want == 0) {
            results.push_back(
                IocpHeadResult{request.idx, Blake3Hash(nullptr, 0),
                               std::nullopt});
            onTick();
            return false;
        }

        slot.overlapped = {};
        slot.handle = handle.release();
        slot.itemIdx = request.idx;
        slot.want = want;
        slot.active = true;

        DWORD transferred = 0;
        const BOOL ok =
            ReadFile(slot.handle, slot.buffer.data(),
                     static_cast<DWORD>(want), &transferred,
                     &slot.overlapped);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            slot.active = false;
            CloseHandle(slot.handle);
            slot.handle = nullptr;
            onTick();
            return false;
        }
        return true;
    };

    // 启动：先用预开线程产出的句柄填满槽位。
    while (!freeSlots.empty()) {
        IocpHeadRequest* request = nullptr;
        UniqueHandle handle;
        if (!openerPool.Pop(&request, &handle)) {
            break;
        }
        if (submitSlot(freeSlots.back(), *request, handle)) {
            freeSlots.pop_back();
            ++inFlight;
        }
    }

    while (inFlight > 0) {
        DWORD transferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(
            port.get(), &transferred, &completionKey, &overlapped, INFINITE);
        if (overlapped == nullptr) {
            break;
        }
        const size_t slotIndex = static_cast<size_t>(completionKey);
        IocpSlot& slot = slots[slotIndex];
        if (ok != 0 && transferred > 0) {
            const size_t length =
                std::min<size_t>(transferred, slot.want);
            IocpHeadResult result;
            result.idx = slot.itemIdx;
            result.digest = Blake3Hash(slot.buffer.data(), length);
            result.fileId = IdentityOf(slot.handle);
            results.push_back(std::move(result));
        }
        if (slot.handle != nullptr) {
            CloseHandle(slot.handle);
            slot.handle = nullptr;
        }
        slot.active = false;
        --inFlight;
        onTick();

        bool resubmitted = false;
        if (!cancel.load()) {
            IocpHeadRequest* request = nullptr;
            UniqueHandle handle;
            if (openerPool.Pop(&request, &handle)) {
                resubmitted = submitSlot(slotIndex, *request, handle);
            }
        }
        if (resubmitted) {
            ++inFlight;
        } else {
            freeSlots.push_back(slotIndex);
            if (cancel.load()) {
                for (IocpSlot& other : slots) {
                    if (other.active && other.handle != nullptr) {
                        CancelIoEx(other.handle, nullptr);
                    }
                }
            }
        }
    }

    openerPool.Shutdown();
    for (IocpSlot& slot : slots) {
        if (slot.handle != nullptr) {
            CloseHandle(slot.handle);
            slot.handle = nullptr;
        }
    }
    return results;
}

std::vector<IocpTripleResult> IocpReadTriples(
    std::vector<IocpTripleRequest> requests, size_t concurrency, bool isSsd,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick) {
    std::vector<IocpTripleResult> results;
    if (requests.empty()) {
        return results;
    }
    size_t maxChunk = 0;
    for (const IocpTripleRequest& request : requests) {
        if (request.chunkLen > maxChunk) {
            maxChunk = request.chunkLen;
        }
    }
    if (maxChunk == 0) {
        return results;
    }

    HANDLE portRaw =
        CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (portRaw == nullptr) {
        return results;
    }
    UniqueHandle port(portRaw);

    const std::unordered_map<char, UniqueHandle> hints =
        BuildDriveHints(requests);

    const size_t poolSize =
        std::min<size_t>(std::min<size_t>(std::max<size_t>(concurrency, 1), 64),
                         requests.size());
    const size_t openerCount =
        isSsd ? std::min<size_t>(4, std::max<size_t>(1, poolSize))
              : std::min<size_t>(2, std::max<size_t>(1, poolSize));

    std::vector<OVERLAPPED> ovs(poolSize);
    std::vector<IocpTripleSlot> slots(poolSize);
    for (IocpTripleSlot& slot : slots) {
        slot.buffer.resize(maxChunk);
    }
    results.reserve(requests.size());
    size_t inFlight = 0;
    bool cancelIssued = false;

    PrefetchOpenerPool<IocpTripleRequest> openerPool(
        requests, hints, poolSize, openerCount, cancel);

    auto issueChunk = [&](size_t slotIndex) -> bool {
        IocpTripleSlot& slot = slots[slotIndex];
        const size_t remaining = slot.partWant - slot.partHave;
        const uint64_t off = slot.currentOffset + slot.partHave;
        OVERLAPPED& ov = ovs[slotIndex];
        ov = {};
        ov.Offset = static_cast<DWORD>(off & 0xFFFF'FFFFULL);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        slot.active = true;
        DWORD rd = 0;
        const BOOL ok = ReadFile(slot.handle, slot.buffer.data(),
                                 static_cast<DWORD>(remaining), &rd, &ov);
        if (ok == 0 && GetLastError() != ERROR_IO_PENDING) {
            slot.active = false;
            CloseHandle(slot.handle);
            slot.handle = nullptr;
            onTick();
            return false;
        }
        return true;
    };

    auto startRequestOnSlot = [&](size_t slotIndex, IocpTripleRequest& request,
                                  UniqueHandle& handle) -> bool {
        if (request.size == 0) {
            IocpTripleResult res;
            res.idx = request.idx;
            res.digest = Blake3Hash(nullptr, 0);
            if (!request.hasFileId) {
                res.fileId = IdentityOf(handle.get());
            }
            results.push_back(std::move(res));
            onTick();
            return false;
        }
        if (request.chunkLen == 0) {
            onTick();
            return false;
        }
        HANDLE bound = CreateIoCompletionPort(handle.get(), port.get(),
                                              slotIndex, 0);
        if (bound == nullptr) {
            onTick();
            return false;
        }

        IocpTripleSlot& slot = slots[slotIndex];
        slot.userIdx = request.idx;
        slot.fileSize = request.size;
        slot.chunkLen = request.chunkLen;
        slot.hasFileId = request.hasFileId;
        slot.partHave = 0;
        if (request.size <= static_cast<uint64_t>(request.chunkLen) * 3) {
            slot.isFullSeq = true;
            slot.seqDone = 0;
            slot.currentOffset = 0;
            slot.partWant =
                static_cast<size_t>(std::min<uint64_t>(request.size, maxChunk));
            blake3_hasher_init(&slot.hasher);
        } else {
            slot.isFullSeq = false;
            slot.offsets[0] = 0;
            slot.offsets[1] = request.size / 2;
            slot.offsets[2] = request.size - request.chunkLen;
            if (request.preSeededHead != nullptr) {
                slot.hasher = *request.preSeededHead;
                request.preSeededHead.reset();
                slot.part = 1;
            } else {
                blake3_hasher_init(&slot.hasher);
                slot.part = 0;
            }
            slot.currentOffset = slot.offsets[slot.part];
            slot.partWant = request.chunkLen;
        }
        slot.handle = handle.release();
        return issueChunk(slotIndex);
    };

    auto fillSlot = [&](size_t slotIndex) -> bool {
        while (!cancel.load()) {
            IocpTripleRequest* request = nullptr;
            UniqueHandle handle;
            if (!openerPool.Pop(&request, &handle)) {
                return false;
            }
            if (startRequestOnSlot(slotIndex, *request, handle)) {
                return true;
            }
        }
        return false;
    };

    for (size_t s = 0; s < poolSize; ++s) {
        if (cancel.load()) {
            break;
        }
        if (fillSlot(s)) {
            ++inFlight;
        } else {
            break;
        }
    }

    while (inFlight > 0) {
        DWORD transferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(
            port.get(), &transferred, &completionKey, &overlapped, INFINITE);
        if (overlapped == nullptr) {
            break;
        }
        const size_t slotIndex = static_cast<size_t>(completionKey);
        IocpTripleSlot& slot = slots[slotIndex];
        slot.active = false;
        --inFlight;

        bool needNextFile = false;
        if (ok != 0 && transferred > 0 && !cancel.load()) {
            const size_t take =
                std::min<size_t>(transferred, slot.partWant - slot.partHave);
            blake3_hasher_update(&slot.hasher, slot.buffer.data(), take);
            slot.partHave += take;
            if (slot.partHave < slot.partWant) {
                if (issueChunk(slotIndex)) {
                    ++inFlight;
                } else {
                    needNextFile = true;
                }
            } else {
                bool finished = false;
                if (!slot.isFullSeq) {
                    ++slot.part;
                    if (slot.part < 3) {
                        slot.currentOffset = slot.offsets[slot.part];
                        slot.partHave = 0;
                        slot.partWant = slot.chunkLen;
                        if (issueChunk(slotIndex)) {
                            ++inFlight;
                        } else {
                            needNextFile = true;
                        }
                    } else {
                        finished = true;
                    }
                } else {
                    slot.seqDone += slot.partWant;
                    if (slot.seqDone < slot.fileSize) {
                        slot.currentOffset = slot.seqDone;
                        slot.partHave = 0;
                        slot.partWant = static_cast<size_t>(std::min<uint64_t>(
                            slot.fileSize - slot.seqDone, maxChunk));
                        if (issueChunk(slotIndex)) {
                            ++inFlight;
                        } else {
                            needNextFile = true;
                        }
                    } else {
                        finished = true;
                    }
                }
                if (finished) {
                    IocpTripleResult res;
                    res.idx = slot.userIdx;
                    blake3_hasher_finalize(&slot.hasher, res.digest.data(),
                                           res.digest.size());
                    if (!slot.hasFileId) {
                        res.fileId = IdentityOf(slot.handle);
                    }
                    results.push_back(std::move(res));
                    CloseHandle(slot.handle);
                    slot.handle = nullptr;
                    onTick();
                    needNextFile = true;
                }
            }
        } else {
            if (slot.handle != nullptr) {
                CloseHandle(slot.handle);
                slot.handle = nullptr;
            }
            if (!cancel.load()) {
                onTick();
            }
            needNextFile = true;
        }

        if (needNextFile) {
            if (cancel.load()) {
                if (!cancelIssued) {
                    cancelIssued = true;
                    for (IocpTripleSlot& other : slots) {
                        if (other.active && other.handle != nullptr) {
                            CancelIoEx(other.handle, nullptr);
                        }
                    }
                }
            } else if (fillSlot(slotIndex)) {
                ++inFlight;
            }
        }
    }

    openerPool.Shutdown();
    for (IocpTripleSlot& slot : slots) {
        if (slot.handle != nullptr) {
            CloseHandle(slot.handle);
            slot.handle = nullptr;
        }
    }
    return results;
}

}  // namespace fastio
