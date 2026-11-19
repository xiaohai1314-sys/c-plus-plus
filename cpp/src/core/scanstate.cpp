#include "core/scanstate.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct AtomicStatus {
    std::atomic<bool> active{false};
    std::atomic<bool> cancelled{false};
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> found{0};
    mutable std::mutex stageMutex;
    std::string stage;

    void SetStage(std::string s) {
        std::lock_guard<std::mutex> lock(stageMutex);
        stage = std::move(s);
    }

    std::string GetStage() const {
        std::lock_guard<std::mutex> lock(stageMutex);
        return stage;
    }

    scanstate::Status ToStatus() const {
        scanstate::Status s;
        s.active = active.load(std::memory_order_relaxed);
        s.cancelled = cancelled.load(std::memory_order_relaxed);
        s.done = done.load(std::memory_order_relaxed);
        s.total = total.load(std::memory_order_relaxed);
        s.found = found.load(std::memory_order_relaxed);
        s.stage = GetStage();
        return s;
    }
};

AtomicStatus g_bigStatus;
AtomicStatus g_dupStatus;

std::mutex g_registryMutex;
std::unordered_map<std::string, std::shared_ptr<AtomicStatus>> g_customStatus;
std::vector<std::string> g_registeredKinds;

AtomicStatus* GetFastSlot(const std::string& kind) {
    if (kind == "big") return &g_bigStatus;
    if (kind == "dup") return &g_dupStatus;
    return nullptr;
}

AtomicStatus* GetOrCreateEntry(const std::string& kind) {
    if (AtomicStatus* fast = GetFastSlot(kind)) {
        return fast;
    }
    std::lock_guard<std::mutex> lock(g_registryMutex);
    auto it = g_customStatus.find(kind);
    if (it != g_customStatus.end()) {
        return it->second.get();
    }
    auto entry = std::make_shared<AtomicStatus>();
    g_customStatus[kind] = entry;
    return entry.get();
}

AtomicStatus* FindExistingEntry(const std::string& kind) {
    if (AtomicStatus* fast = GetFastSlot(kind)) {
        return fast;
    }
    std::lock_guard<std::mutex> lock(g_registryMutex);
    auto it = g_customStatus.find(kind);
    return it != g_customStatus.end() ? it->second.get() : nullptr;
}

void EnsureRegistered(const std::string& kind) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (const std::string& existing : g_registeredKinds) {
        if (existing == kind) return;
    }
    g_registeredKinds.push_back(kind);
}

}  // namespace

namespace scanstate {

void Begin(const std::string& kind, const std::string& stage) {
    EnsureRegistered(kind);
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->cancelled.store(false, std::memory_order_relaxed);
    entry->done.store(0, std::memory_order_relaxed);
    entry->total.store(0, std::memory_order_relaxed);
    entry->found.store(0, std::memory_order_relaxed);
    entry->SetStage(stage);
    entry->active.store(true, std::memory_order_release);
}

void End(const std::string& kind) {
    AtomicStatus* entry = FindExistingEntry(kind);
    if (entry != nullptr) {
        entry->active.store(false, std::memory_order_release);
    }
}

bool WasCancelled(const std::string& kind) {
    if (kind == "dup") {
        return g_dupStatus.cancelled.load(std::memory_order_relaxed);
    }
    if (kind == "big") {
        return g_bigStatus.cancelled.load(std::memory_order_relaxed);
    }
    AtomicStatus* entry = FindExistingEntry(kind);
    return entry != nullptr && entry->cancelled.load(std::memory_order_relaxed);
}

void Cancel(const std::string& kind) {
    if (kind.empty()) {
        std::vector<std::string> kinds;
        {
            std::lock_guard<std::mutex> lock(g_registryMutex);
            kinds = g_registeredKinds;
        }
        for (const std::string& k : kinds) {
            AtomicStatus* entry = FindExistingEntry(k);
            if (entry != nullptr && entry->active.load(std::memory_order_acquire)) {
                entry->cancelled.store(true, std::memory_order_relaxed);
            }
        }
        return;
    }
    AtomicStatus* entry = FindExistingEntry(kind);
    if (entry != nullptr && entry->active.load(std::memory_order_acquire)) {
        entry->cancelled.store(true, std::memory_order_relaxed);
    }
}

void Stage(const std::string& kind, const std::string& stage) {
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->SetStage(stage);
}

void Progress(const std::string& kind, uint64_t done, uint64_t total) {
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->done.store(done, std::memory_order_relaxed);
    entry->total.store(total, std::memory_order_relaxed);
}

void SetFound(const std::string& kind, uint64_t found_count) {
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->found.store(found_count, std::memory_order_relaxed);
}

void Reset(const std::string& kind, uint64_t total) {
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->done.store(0, std::memory_order_relaxed);
    entry->total.store(total, std::memory_order_relaxed);
    entry->found.store(0, std::memory_order_relaxed);
}

void AddTotal(const std::string& kind, uint64_t delta) {
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->total.fetch_add(delta, std::memory_order_relaxed);
}

void Advance(const std::string& kind, uint64_t delta) {
    if (kind == "dup") {
        g_dupStatus.done.fetch_add(delta, std::memory_order_relaxed);
        return;
    }
    if (kind == "big") {
        g_bigStatus.done.fetch_add(delta, std::memory_order_relaxed);
        return;
    }
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->done.fetch_add(delta, std::memory_order_relaxed);
}

void AddFound(const std::string& kind, uint64_t delta) {
    if (kind == "dup") {
        g_dupStatus.found.fetch_add(delta, std::memory_order_relaxed);
        return;
    }
    if (kind == "big") {
        g_bigStatus.found.fetch_add(delta, std::memory_order_relaxed);
        return;
    }
    AtomicStatus* entry = GetOrCreateEntry(kind);
    entry->found.fetch_add(delta, std::memory_order_relaxed);
}

Json StatusJson() {
    std::vector<std::string> kinds;
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        kinds = g_registeredKinds;
    }
    Json result = Json::object();
    for (const std::string& kind : kinds) {
        AtomicStatus* entry = FindExistingEntry(kind);
        if (entry != nullptr) {
            Status s = entry->ToStatus();
            Json item;
            item["active"] = s.active;
            item["stage"] = std::move(s.stage);
            item["cancelled"] = s.cancelled;
            item["done"] = s.done;
            item["total"] = s.total;
            item["found"] = s.found;
            result[kind] = std::move(item);
        }
    }
    return result;
}

}  // namespace scanstate
