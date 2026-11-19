#include "core/bigfiles.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/disk.h"
#include "core/mft.h"
#include "core/scanstate.h"
#include "core/timeutil.h"
#include "util/path.h"
#include "util/utf.h"

namespace {

constexpr uint64_t kMb = 1024 * 1024;

const char* const kTopSkip[] = {
    "$recycle.bin",           "system volume information",
    "windows",                "program files",
    "program files (x86)",    "programdata",
    "recovery",               "perflogs",
    "msocache",               "config.msi",
    "$windows.~bt",           "$windows.~ws",
    "windows.old",
};

std::vector<std::string> SkipTop() {
    return std::vector<std::string>(std::begin(kTopSkip), std::end(kTopSkip));
}

std::string LowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string ExtensionOf(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const std::string name =
        slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return {};
    }
    return LowerAscii(name.substr(dot + 1));
}

/// 按物理磁盘用独立线程并发枚举：固态盘每个分区一个线程，同一块机械盘的分区共用一个串行线程。
std::vector<mft::FileEntry> ScanDrivesPerPhysicalDisk(
    const std::vector<std::string>& drives) {
    std::unordered_map<char, disk::DiskKey> cache;
    std::vector<std::string> ssd;
    std::unordered_map<disk::DiskKey, std::vector<std::string>, disk::DiskKeyHash>
        hdd;
    for (const std::string& drive : drives) {
        const disk::DiskKey key = disk::KeyForPath(drive, cache);
        if (key.kind == disk::DiskKeyKind::Ssd) {
            ssd.push_back(drive);
        } else {
            hdd[key].push_back(drive);
        }
    }

    auto worker = [](const std::string& drive) -> std::vector<mft::FileEntry> {
        if (scanstate::WasCancelled("big")) {
            return {};
        }
        // 谓词内一次完成系统文件过滤与分档阈值判断，避免主循环对每个结果
        // 再重复一次 Basename/ExtensionOf/LowerAscii。
        return mft::ScanFilesWhere(
            drive, 2 * kMb, SkipTop(),
            [](const std::string& name, uint64_t size) {
                if (size >= 100 * kMb) {
                    return !IsSystemFileName(name);
                }
                if (IsSystemFileName(name)) {
                    return false;
                }
                return size >= BigFileThresholdBytes(name);
            });
    };

    std::vector<std::vector<mft::FileEntry>> parts(ssd.size() + hdd.size());
    std::vector<std::thread> threads;
    size_t slot = 0;
    for (const std::string& drive : ssd) {
        const size_t index = slot++;
        threads.emplace_back([&worker, &parts, index, drive] {
            parts[index] = worker(drive);
        });
    }
    for (auto& [key, queue] : hdd) {
        (void)key;
        const size_t index = slot++;
        threads.emplace_back([&worker, &parts, index, queue = std::move(queue)] {
            for (const std::string& drive : queue) {
                std::vector<mft::FileEntry> found = worker(drive);
                parts[index].insert(parts[index].end(),
                                    std::make_move_iterator(found.begin()),
                                    std::make_move_iterator(found.end()));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::vector<mft::FileEntry> entries;
    for (std::vector<mft::FileEntry>& part : parts) {
        entries.insert(entries.end(), std::make_move_iterator(part.begin()),
                       std::make_move_iterator(part.end()));
    }
    return entries;
}

struct BigFile {
    std::string path;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

}  // namespace

bool IsSystemFileName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const std::string_view name =
        slash == std::string::npos
            ? std::string_view(path)
            : std::string_view(path).substr(slash + 1);
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    return ieq(name, "pagefile.sys") || ieq(name, "hiberfil.sys") ||
           ieq(name, "swapfile.sys") || ieq(name, "dumpstack.log.tmp") ||
           ieq(name, "memory.dmp");
}

uint64_t BigFileThresholdBytes(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot <= start) {
        return 50 * kMb;
    }
    char extBuf[16];
    const size_t extLen = path.size() - (dot + 1);
    if (extLen >= sizeof(extBuf) || extLen == 0) {
        return 50 * kMb;
    }
    for (size_t i = 0; i < extLen; ++i) {
        extBuf[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(path[dot + 1 + i])));
    }
    extBuf[extLen] = '\0';
    const std::string_view ext(extBuf, extLen);

    auto isOneOf = [&](std::initializer_list<const char*> values) {
        for (const char* value : values) {
            if (ext == value) {
                return true;
            }
        }
        return false;
    };
    if (isOneOf({"exe", "msi"})) {
        return 100 * kMb;
    }
    if (isOneOf({"mp4", "avi", "mov", "mkv", "wmv", "flv", "m4v", "3gp",
                 "mpeg", "mpg", "ts", "webm", "rmvb", "vob", "dv", "swf",
                 "tmp"})) {
        return 20 * kMb;
    }
    if (isOneOf({"mp3", "ape", "wav", "flac", "aac", "ogg", "m4a", "ac3",
                 "dts", "mka", "opus", "wma", "mid", "zip", "rar", "7z",
                 "gz", "iso", "img", "tar", "bz2", "xz", "cab", "fzip",
                 "rar4", "apk", "ipa", "ipsw", "pack", "wim", "pima", "lib",
                 "pdb", "rb", "torrent"})) {
        return 5 * kMb;
    }
    if (isOneOf({"jpg", "jpeg", "png", "gif", "bmp", "tiff", "tif", "webp",
                 "heic", "ico", "raw", "psd", "svg", "txt", "csv", "pdf",
                 "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odf", "wps",
                 "wpt", "dot", "dotx", "docm", "xlsm", "rtf", "odt", "dwg",
                 "dxf", "md", "et", "dps", "ofd", "xmind", "vsdx", "log",
                 "xlog", "dump", "dmp", "lg", "tlg", "ttd", "xltd"})) {
        return 2 * kMb;
    }
    return 50 * kMb;
}

Json ScanBig(const std::vector<std::string>& drives) {
    scanstate::Begin("big", "大文件扫描");
    const std::vector<std::string> list =
        drives.empty() ? disk::FixedDrives() : drives;
    std::vector<mft::FileEntry> found = ScanDrivesPerPhysicalDisk(list);

    std::vector<BigFile> rows;
    rows.reserve(found.size());
    for (mft::FileEntry& entry : found) {
        // 谓词已过滤系统文件并保证 size 达标，这里不再重复解析扩展名。
        BigFile row;
        row.size = entry.size;
        row.mtime =
            entry.mtime != 0 ? entry.mtime : FileMtimeSeconds(entry.path);
        row.path = std::move(entry.path);
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const BigFile& left, const BigFile& right) {
                  return left.size > right.size;
              });
    scanstate::End("big");

    Json out = Json::array();
    for (const BigFile& row : rows) {
        out.push_back(Json{{"path", row.path},
                           {"size", row.size},
                           {"mtime", row.mtime}});
    }
    return out;
}
