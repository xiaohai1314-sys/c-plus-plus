#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/digest.h"
#include "core/fastio.h"
#include "core/filehash.h"
#include "harness.hpp"
#include "util/env.h"
#include "util/path.h"
#include "util/utf.h"

namespace {

void RemoveTree(const std::string& path) {
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(Utf8ToWide(path)), error);
}

std::string TempDir(const char* name) {
    std::string base = GetEnv("TEMP");
    if (base.empty()) {
        base = "C:\\Windows\\Temp";
    }
    wchar_t longPath[32768] = {};
    if (GetLongPathNameW(Utf8ToWide(base).c_str(), longPath, 32768) != 0) {
        base = WideToUtf8(longPath);
    }
    const std::string dir = JoinPath(base, name);
    RemoveTree(dir);
    CreateDirectoryW(Utf8ToWide(dir).c_str(), nullptr);
    return dir;
}

void WriteBytes(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* file = nullptr;
    _wfopen_s(&file, Utf8ToWide(path).c_str(), L"wb");
    fwrite(data.data(), 1, data.size(), file);
    fclose(file);
}

}  // namespace

TEST(fastio_open_by_id_and_iocp_match_direct_read) {
    const std::string dir = TempDir("cdc-fastio-test");
    const std::string path1 = JoinPath(dir, "a.bin");
    const std::string path2 = JoinPath(dir, "b.bin");

    std::vector<uint8_t> data1(8192);
    std::vector<uint8_t> data2(8192);
    for (size_t index = 0; index < 8192; ++index) {
        data1[index] = static_cast<uint8_t>(index % 251);
        data2[index] = static_cast<uint8_t>((index * 7) % 241);
    }
    WriteBytes(path1, data1);
    WriteBytes(path2, data2);

    const std::optional<fastio::OpenedFile> direct = fastio::OpenCheckedFileFast(
        nullptr, 0, path1, FILE_FLAG_RANDOM_ACCESS);
    CHECK(direct.has_value());
    CHECK_EQ(direct->size, static_cast<uint64_t>(8192));

    uint64_t frn = 0;
    {
        HANDLE handle =
            CreateFileW(Utf8ToWide(path1).c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(handle != INVALID_HANDLE_VALUE);
        BY_HANDLE_FILE_INFORMATION info = {};
        CHECK(GetFileInformationByHandle(handle, &info) != 0);
        frn = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
              static_cast<uint64_t>(info.nFileIndexLow);
        CloseHandle(handle);
    }
    CHECK(frn != 0);

    const char drive = path1.empty() ? 'C' : path1[0];
    UniqueHandle hint = fastio::OpenDriveHint(drive);
    CHECK(hint.get() != nullptr && hint.get() != INVALID_HANDLE_VALUE);

    const std::optional<fastio::OpenedFile> byId = fastio::OpenCheckedFileFast(
        hint.get(), frn, path1, FILE_FLAG_RANDOM_ACCESS);
    CHECK(byId.has_value());
    std::vector<uint8_t> head(4096);
    DWORD read = 0;
    CHECK(ReadFile(byId->handle.get(), head.data(), 4096, &read, nullptr) != 0);
    CHECK_EQ(read, static_cast<DWORD>(4096));
    CHECK(std::memcmp(head.data(), data1.data(), 4096) == 0);

    std::atomic<bool> cancel{false};
    std::atomic<size_t> ticks{0};
    std::vector<fastio::IocpHeadRequest> requests;
    requests.push_back({0, path1, frn, 4096});
    requests.push_back({1, path2, 0, 4096});
    const std::vector<fastio::IocpHeadResult> results = fastio::IocpReadHeads(
        std::move(requests), 4, cancel, [&ticks] { ticks.fetch_add(1); });

    CHECK_EQ(results.size(), static_cast<size_t>(2));
    CHECK_EQ(ticks.load(), static_cast<size_t>(2));
    for (const fastio::IocpHeadResult& result : results) {
        const std::vector<uint8_t>& expected = result.idx == 0 ? data1 : data2;
        CHECK(result.digest == Blake3Hash(expected.data(), 4096));
    }

    RemoveTree(dir);
}

TEST(fastio_iocp_read_triples_matches_filehash) {
    const std::string dir = TempDir("cdc-fastio-triples-test");
    const std::string path1 = JoinPath(dir, "unaligned_a.bin");
    const std::string path2 = JoinPath(dir, "unaligned_b.bin");

    // 刻意选用非 4KB 对齐的文件长度（1,000,123 字节 > 768KB）。
    constexpr size_t kSize = 1'000'123;
    std::vector<uint8_t> data1(kSize);
    std::vector<uint8_t> data2(kSize);
    for (size_t i = 0; i < kSize; ++i) {
        data1[i] = static_cast<uint8_t>((i * 13 + 7) % 251);
        data2[i] = static_cast<uint8_t>((i * 31 + 19) % 241);
    }
    WriteBytes(path1, data1);
    WriteBytes(path2, data2);

    const std::optional<filehash::Hashed> expectedStrong1 =
        filehash::StrongHash(path1, 0, kSize);
    const std::optional<filehash::Hashed> expectedStrong2 =
        filehash::StrongHash(path2, 0, kSize);
    const std::optional<filehash::Hashed> expectedSample1 =
        filehash::SampleHash(path1, 0, kSize);
    CHECK(expectedStrong1.has_value());
    CHECK(expectedStrong2.has_value());
    CHECK(expectedSample1.has_value());

    constexpr size_t kStrongChunk = 256 * 1024;
    constexpr size_t kSampleChunk = 16 * 1024;

    // 256KB 三段：同时测试无 preSeededHead 与带 preSeededHead 两种路径。
    auto seeded = std::make_unique<blake3_hasher>();
    blake3_hasher_init(seeded.get());
    blake3_hasher_update(seeded.get(), data2.data(), kStrongChunk);

    std::vector<fastio::IocpTripleRequest> strongReqs;
    strongReqs.push_back(
        {0, path1, 0, kSize, kStrongChunk, false, nullptr});
    strongReqs.push_back(
        {1, path2, 0, kSize, kStrongChunk, false, std::move(seeded)});

    std::atomic<bool> cancel{false};
    std::atomic<size_t> ticks{0};
    const std::vector<fastio::IocpTripleResult> strongResults =
        fastio::IocpReadTriples(std::move(strongReqs), 4, true, cancel,
                                [&ticks] { ticks.fetch_add(1); });
    CHECK_EQ(strongResults.size(), static_cast<size_t>(2));
    CHECK_EQ(ticks.load(), static_cast<size_t>(2));
    for (const fastio::IocpTripleResult& res : strongResults) {
        const Digest& expected =
            res.idx == 0 ? expectedStrong1->digest : expectedStrong2->digest;
        CHECK(res.digest == expected);
    }

    // 16KB 三段采样对比 SampleHash。
    std::vector<fastio::IocpTripleRequest> sampleReqs;
    sampleReqs.push_back(
        {7, path1, 0, kSize, kSampleChunk, false, nullptr});
    ticks.store(0);
    const std::vector<fastio::IocpTripleResult> sampleResults =
        fastio::IocpReadTriples(std::move(sampleReqs), 2, false, cancel,
                                [&ticks] { ticks.fetch_add(1); });
    CHECK_EQ(sampleResults.size(), static_cast<size_t>(1));
    CHECK_EQ(ticks.load(), static_cast<size_t>(1));
    CHECK_EQ(sampleResults[0].idx, static_cast<size_t>(7));
    CHECK(sampleResults[0].digest == expectedSample1->digest);

    RemoveTree(dir);
}

TEST(fastio_raw_volume_heads_and_triples_match_filehash) {
    const std::string dir = TempDir("cdc-fastio-raw-test");
    const std::string path = JoinPath(dir, "raw_test.bin");
    const char drive =
        dir.empty() ? 'C'
                    : static_cast<char>(
                          std::toupper(static_cast<unsigned char>(dir[0])));

    const std::wstring volPath =
        std::wstring(L"\\\\.\\") + static_cast<wchar_t>(drive) + L":";
    HANDLE volProbe = CreateFileW(
        volPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING,
        nullptr);
    if (volProbe == INVALID_HANDLE_VALUE) {
        // 非管理员环境无法打开原生卷时直接跳过。
        RemoveTree(dir);
        return;
    }
    CloseHandle(volProbe);

    constexpr size_t kSize = 1'000'123;
    std::vector<uint8_t> data(kSize);
    for (size_t i = 0; i < kSize; ++i) {
        data[i] = static_cast<uint8_t>((i * 29 + 3) % 251);
    }

    {
        HANDLE h = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        CHECK(WriteFile(h, data.data(), static_cast<DWORD>(data.size()),
                        &written, nullptr) != 0);
        CHECK_EQ(written, static_cast<DWORD>(data.size()));
        CHECK(FlushFileBuffers(h) != 0);
        CloseHandle(h);
    }

    const std::wstring rootW =
        std::wstring(1, static_cast<wchar_t>(drive)) + L":\\";
    DWORD spc = 0;
    DWORD bps = 0;
    DWORD fc = 0;
    DWORD tc = 0;
    CHECK(GetDiskFreeSpaceW(rootW.c_str(), &spc, &bps, &fc, &tc) != 0);
    const uint64_t bpc =
        static_cast<uint64_t>(spc) * static_cast<uint64_t>(bps);
    CHECK(bpc > 0);

    HANDLE hRead =
        CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(hRead != INVALID_HANDLE_VALUE);
    STARTING_VCN_INPUT_BUFFER startVcn = {};
    startVcn.StartingVcn.QuadPart = 0;
    std::vector<uint8_t> rpBuf(8192);
    DWORD retBytes = 0;
    const BOOL gotExtents = DeviceIoControl(
        hRead, FSCTL_GET_RETRIEVAL_POINTERS, &startVcn, sizeof(startVcn),
        rpBuf.data(), static_cast<DWORD>(rpBuf.size()), &retBytes, nullptr);
    CloseHandle(hRead);

    if (gotExtents == 0) {
        RemoveTree(dir);
        return;
    }

    const auto* rp = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(rpBuf.data());
    constexpr size_t kStrongChunk = 256 * 1024;
    constexpr size_t kSampleChunk = 16 * 1024;

    auto findSpanOffset = [&](uint64_t startByte, uint64_t len)
        -> std::optional<uint64_t> {
        int64_t vcn = rp->StartingVcn.QuadPart;
        for (DWORD i = 0; i < rp->ExtentCount; ++i) {
            const int64_t nextVcn = rp->Extents[i].NextVcn.QuadPart;
            const int64_t lcn = rp->Extents[i].Lcn.QuadPart;
            const uint64_t rStart = static_cast<uint64_t>(vcn) * bpc;
            const uint64_t rEnd = static_cast<uint64_t>(nextVcn) * bpc;
            if (lcn > 0 && startByte >= rStart && startByte + len <= rEnd) {
                return static_cast<uint64_t>(lcn) * bpc + (startByte - rStart);
            }
            vcn = nextVcn;
        }
        return std::nullopt;
    };

    const auto headOff = findSpanOffset(0, kStrongChunk);
    const auto midOff = findSpanOffset(kSize / 2, kStrongChunk);
    const auto tailOff = findSpanOffset(kSize - kStrongChunk, kStrongChunk);
    if (!headOff.has_value() || !midOff.has_value() || !tailOff.has_value() ||
        !(*headOff < *midOff && *midOff < *tailOff)) {
        // 若临时测试文件落盘高度碎片化或物理重排非递增，跳过直读断言。
        RemoveTree(dir);
        return;
    }

    const std::optional<filehash::Hashed> expectedStrong =
        filehash::StrongHash(path, 0, kSize);
    const std::optional<filehash::Hashed> expectedSample =
        filehash::SampleHash(path, 0, kSize);
    CHECK(expectedStrong.has_value());
    CHECK(expectedSample.has_value());

    std::atomic<bool> cancel{false};
    std::atomic<size_t> ticks{0};

    // 1. CoalescedRawVolumeHeads: 4KB 模式 + 融合 256KB captureStrongHead 模式
    std::vector<fastio::RawHeadTarget> headTargets = {
        {10, *headOff, 4096, false},
        {20, *headOff, kStrongChunk, true},
    };
    std::vector<fastio::RawHeadResult> headRes = fastio::CoalescedRawVolumeHeads(
        drive, std::move(headTargets), false, cancel,
        [&ticks] { ticks.fetch_add(1); });
    CHECK_EQ(headRes.size(), static_cast<size_t>(2));
    CHECK_EQ(ticks.load(), static_cast<size_t>(2));
    std::unique_ptr<blake3_hasher> capturedState;
    const Digest expected4k = Blake3Hash(data.data(), 4096);
    for (fastio::RawHeadResult& r : headRes) {
        CHECK(r.headDigest == expected4k);
        if (r.idx == 10) {
            CHECK(r.strongHeadState == nullptr);
        } else if (r.idx == 20) {
            CHECK(r.strongHeadState != nullptr);
            capturedState = std::move(r.strongHeadState);
        }
    }
    CHECK(capturedState != nullptr);

    // 2. CoalescedRawVolumeTriples:
    //    - 256KB 2-part (preSeededHead from CoalescedRawVolumeHeads)
    //    - 256KB 3-part (fresh)
    //    - 16KB 3-part Sample
    std::vector<fastio::RawTripleTarget> tripleTargets;
    tripleTargets.push_back({100, *headOff, *midOff, *tailOff, kStrongChunk,
                             std::move(capturedState)});
    tripleTargets.push_back(
        {200, *headOff, *midOff, *tailOff, kStrongChunk, nullptr});
    tripleTargets.push_back({300, *headOff, *midOff,
                             *tailOff + (kStrongChunk - kSampleChunk),
                             kSampleChunk, nullptr});
    ticks.store(0);
    const std::vector<std::pair<size_t, Digest>> tripleRes =
        fastio::CoalescedRawVolumeTriples(drive, std::move(tripleTargets), false,
                                          cancel,
                                          [&ticks] { ticks.fetch_add(1); });
    CHECK_EQ(tripleRes.size(), static_cast<size_t>(3));
    CHECK_EQ(ticks.load(), static_cast<size_t>(3));
    for (const auto& [idx, digest] : tripleRes) {
        if (idx == 100 || idx == 200) {
            CHECK(digest == expectedStrong->digest);
        } else {
            CHECK_EQ(idx, static_cast<size_t>(300));
            CHECK(digest == expectedSample->digest);
        }
    }

    RemoveTree(dir);
}
