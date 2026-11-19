#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "blake3.h"
#include "core/digest.h"
#include "util/win32.h"

namespace fastio {

struct OpenedFile {
    UniqueHandle handle;
    std::optional<std::pair<uint32_t, uint64_t>> fileId;
    uint64_t size = 0;
};

/// 只读卷根（`X:\`）的目录句柄，用作 OpenFileById 的卷提示。
UniqueHandle OpenDriveHint(char driveLetter);

/// 按 FRN 优先 OpenFileById，失败再退回路径打开；并检查跳过属性、硬链接标识。
std::optional<OpenedFile> OpenCheckedFileFast(
    HANDLE hint, uint64_t frn, const std::string& path, DWORD flags);

/// 仅查询文件元数据标识（不读内容），用于热缓存路径兜底识别硬链接。
std::optional<std::pair<uint32_t, uint64_t>> QueryFileIdentity(
    const std::string& path);

struct RawHeadTarget {
    size_t idx = 0;
    uint64_t offset = 0;
    size_t want = 0;
    bool captureStrongHead = false;
};

struct RawHeadResult {
    size_t idx = 0;
    Digest headDigest{};
    std::unique_ptr<blake3_hasher> strongHeadState;
};

/// 按物理 LCN 合并异步重叠直读同一卷上邻近文件的头部（直接读 `\\.\X:`）。
std::vector<RawHeadResult> CoalescedRawVolumeHeads(
    char driveLetter, std::vector<RawHeadTarget> targets, bool isSsd,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick);

struct RawTripleTarget {
    size_t idx = 0;
    uint64_t headOffset = 0;
    uint64_t midOffset = 0;
    uint64_t tailOffset = 0;
    size_t chunkLen = 0;
    std::unique_ptr<blake3_hasher> preSeededHead;
};

/// Stage 2 原生卷三段（头/中/尾）异步重叠 LBA 升序直读（支持跳过已融合头）。
std::vector<std::pair<size_t, Digest>> CoalescedRawVolumeTriples(
    char driveLetter, std::vector<RawTripleTarget> targets, bool isSsd,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick);

struct IocpHeadRequest {
    size_t idx = 0;
    std::string path;
    uint64_t frn = 0;
    size_t want = 0;
};

struct IocpHeadResult {
    size_t idx = 0;
    Digest digest{};
    std::optional<std::pair<uint32_t, uint64_t>> fileId;
};

/// OpenFileById + IOCP 异步重叠 I/O 批量读取文件前 4KB（SSD 32 路、HDD 16 路）。
std::vector<IocpHeadResult> IocpReadHeads(
    std::vector<IocpHeadRequest> requests, size_t concurrency,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick);

struct IocpTripleRequest {
    size_t idx = 0;
    std::string path;
    uint64_t frn = 0;
    uint64_t size = 0;
    size_t chunkLen = 0;
    bool hasFileId = false;
    std::unique_ptr<blake3_hasher> preSeededHead;
};

struct IocpTripleResult {
    size_t idx = 0;
    Digest digest{};
    std::optional<std::pair<uint32_t, uint64_t>> fileId;
};

/// OpenFileById + IOCP 异步重叠 I/O 批量读取三段采样/强校验（头/中/尾）。
std::vector<IocpTripleResult> IocpReadTriples(
    std::vector<IocpTripleRequest> requests, size_t concurrency, bool isSsd,
    const std::atomic<bool>& cancel, const std::function<void()>& onTick);

}  // namespace fastio
