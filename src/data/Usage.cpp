#include "Usage.h"

#include "AtomicFile.h"      // AtomicWriteFile (crash-safe temp+rename)
#include "JsonUtil.h"        // never-throw typed getters
#include "Logging.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;
using namespace jsonutil;

namespace usage {
namespace {

std::deque<FavoriteRef> g_log;     // newest first; capped to kUsageLog
std::mutex              g_mutex;   // guards g_log (Record may run off the render thread)
std::atomic<uint64_t>   g_version{0};
std::string             g_path;    // set by Load; the unload Flush() writes here
std::atomic<bool>       g_dirty{false};  // a change happened this session (unwritten)

// Memo caches for the two derived views, keyed on the version epoch (+ the cap,
// defensively). Render-thread only.
std::vector<FavoriteRef> g_recentMemo; uint64_t g_recentVer = (uint64_t)-1; size_t g_recentMax = (size_t)-1;
std::vector<FavoriteRef> g_freqMemo;   uint64_t g_freqVer   = (uint64_t)-1; size_t g_freqMax   = (size_t)-1;

// A log change: bump the version epoch (invalidates the derive memos) AND flag the
// log dirty so the unload Flush() persists it. No per-change disk I/O — usage is
// low-stakes convenience data that rides to disk once at unload. Safe to call from
// any thread Record() reaches (atomics only).
void MarkChanged() {
    g_version.fetch_add(1, std::memory_order_relaxed);
    g_dirty.store(true, std::memory_order_relaxed);
}

std::string Serialize(const std::vector<FavoriteRef>& snap) {
    std::ostringstream f;
    f << "{\n";
    f << "  \"version\": 1,\n";
    f << "  \"log\": [";
    for (size_t i = 0; i < snap.size(); ++i) {
        if (i) f << ",";
        f << "\n    {\"type\": " << (int)snap[i].Type
          << ", \"id\": " << json(snap[i].Id).dump() << "}";
    }
    if (!snap.empty()) f << "\n  ";
    f << "]\n}\n";
    return f.str();
}

// Snapshot the log under the lock, write OUTSIDE it (never hold the mutex across
// disk I/O), then clear the dirty edge. binary=true writes the bytes verbatim (LF).
void DoSave() {
    if (g_path.empty()) return;
    std::vector<FavoriteRef> snap;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        snap.assign(g_log.begin(), g_log.end());
    }
    // Clear the dirty edge only on a successful write - otherwise a failed write
    // would both lose the data AND stop Flush() (writes iff dirty) from retrying.
    if (AtomicWriteFile(g_path, Serialize(snap), /*binary=*/true))
        g_dirty.store(false, std::memory_order_relaxed);
    else
        LOG_WARNING("usage: failed to write %s (kept dirty; will retry)", g_path.c_str());
}

}  // namespace

void Load(const std::string& path) {
    g_path = path;
    g_dirty.store(false, std::memory_order_relaxed);

    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_DEBUG("usage.json not present; usage log starts empty");
        return;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        LOG_WARNING("usage.json parse error (%s); usage log starts empty", e.what());
        return;
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    g_log.clear();
    int dropped = 0;
    const json& arr = GetArray(j, "log");
    for (const auto& e : arr) {
        if (!e.is_object()) { ++dropped; continue; }
        int t = GetInt(e, "type", -1);
        std::string id = GetString(e, "id", "");
        if (id.empty() || (t != (int)EFavoriteRefType::Emote &&
                            t != (int)EFavoriteRefType::MeMote)) {
            ++dropped;
            continue;
        }
        g_log.push_back(FavoriteRef{(EFavoriteRefType)t, id});
        if (g_log.size() >= kUsageLog) break;
    }
    if (dropped > 0)
        LOG_WARNING("usage: dropped %d malformed usage.json entr%s on load",
                    dropped, dropped == 1 ? "y" : "ies");
    g_version.fetch_add(1, std::memory_order_relaxed);  // invalidate the derive memos
}

void Record(EFavoriteRefType type, const std::string& id) {
    if (id.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        // Consecutive coalesce: a repeat of the current front is already the most
        // recent — nothing to change, so mashing one emote can't flush the window.
        if (!g_log.empty() && g_log.front().Type == type && g_log.front().Id == id)
            return;
        g_log.push_front(FavoriteRef{type, id});
        if (g_log.size() > kUsageLog) g_log.resize(kUsageLog);  // drops the oldest
    }
    MarkChanged();
}

const std::vector<FavoriteRef>& RecentlyUsed(size_t max) {
    uint64_t v = g_version.load(std::memory_order_relaxed);
    if (g_recentVer == v && g_recentMax == max) return g_recentMemo;

    g_recentMemo.clear();
    std::unordered_set<std::string> seen;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (const auto& ref : g_log) {  // front->back == newest->oldest
            if (g_recentMemo.size() >= max) break;
            if (seen.insert(FavoriteRefKey(ref)).second) g_recentMemo.push_back(ref);
        }
    }
    g_recentVer = v;
    g_recentMax = max;
    return g_recentMemo;
}

const std::vector<FavoriteRef>& Frequent(size_t max) {
    uint64_t v = g_version.load(std::memory_order_relaxed);
    if (g_freqVer == v && g_freqMax == max) return g_freqMemo;

    struct Agg { FavoriteRef ref; int count; size_t firstIdx; };
    std::unordered_map<std::string, Agg> agg;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        size_t idx = 0;
        for (const auto& ref : g_log) {
            auto it = agg.find(FavoriteRefKey(ref));
            if (it == agg.end()) agg.emplace(FavoriteRefKey(ref), Agg{ref, 1, idx});
            else                 ++it->second.count;
            ++idx;
        }
    }
    std::vector<Agg> v2;
    v2.reserve(agg.size());
    for (auto& kv : agg) v2.push_back(kv.second);
    std::sort(v2.begin(), v2.end(), [](const Agg& a, const Agg& b) {
        if (a.count != b.count) return a.count > b.count;   // most frequent first
        return a.firstIdx < b.firstIdx;                     // tiebreak: most recent
    });
    if (v2.size() > max) v2.resize(max);

    g_freqMemo.clear();
    g_freqMemo.reserve(v2.size());
    for (auto& a : v2) g_freqMemo.push_back(a.ref);
    g_freqVer = v;
    g_freqMax = max;
    return g_freqMemo;
}

void RemoveRef(EFavoriteRefType type, const std::string& id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto it = g_log.begin(); it != g_log.end();) {
            if (it->Type == type && it->Id == id) { it = g_log.erase(it); changed = true; }
            else ++it;
        }
    }
    if (changed) MarkChanged();
}

void RemoveAllOfType(EFavoriteRefType type) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto it = g_log.begin(); it != g_log.end();) {
            if (it->Type == type) { it = g_log.erase(it); changed = true; }
            else ++it;
        }
    }
    if (changed) MarkChanged();
}

void PruneDead(bool (*isLive)(EFavoriteRefType, const std::string&)) {
    if (!isLive) return;
    // Evaluate isLive WITHOUT g_mutex held: it locks the catalog mutexes, and the
    // Quickbar build locks catalog -> usage, so holding usage -> catalog here
    // would invert the order. Snapshot distinct refs under the lock, classify
    // outside it, then erase the dead set under the lock.
    std::vector<FavoriteRef> distinct;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        std::unordered_set<std::string> seen;
        for (const auto& r : g_log)
            if (seen.insert(FavoriteRefKey(r)).second) distinct.push_back(r);
    }
    std::unordered_set<std::string> dead;
    for (const auto& r : distinct)
        if (!isLive(r.Type, r.Id)) dead.insert(FavoriteRefKey(r));
    if (dead.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto it = g_log.begin(); it != g_log.end();) {
            if (dead.count(FavoriteRefKey(*it))) it = g_log.erase(it);
            else ++it;
        }
    }
    MarkChanged();
}

void Reset() {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_log.empty()) return;
        g_log.clear();
    }
    MarkChanged();
    DoSave();  // eager: a manual wipe should hit disk now, not wait for unload
    LOG_DEBUG("usage: reset (log cleared)");
}

void Flush() {
    if (g_dirty.load(std::memory_order_relaxed)) DoSave();
}

uint64_t Version() { return g_version.load(std::memory_order_relaxed); }

size_t LogSize() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_log.size();
}

size_t ApproxBytes() {
    std::lock_guard<std::mutex> lk(g_mutex);
    size_t b = 0;
    for (const auto& r : g_log) b += sizeof(FavoriteRef) + r.Id.capacity();
    return b;
}

}  // namespace usage

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
static DevStateRegistrar s_usageState(DevStateCat::Content, "Usage (recently/frequently used)", [] {
    DevStateRow("log entries", "%d / %d", (int)usage::LogSize(), (int)usage::kUsageLog);
    DevStateRow("approx bytes", "%d", (int)usage::ApproxBytes());
    DevStateRow("version epoch", "%u", (unsigned)usage::Version());
});
#endif
