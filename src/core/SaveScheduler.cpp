#include "SaveScheduler.h"

#include "AtomicFile.h"
#include "Globals.h"      // g_SettingsPath, g_EmotesJsonPath, g_MeMotesJsonPath
#include "Settings.h"   // SerializeSettings / SaveSettings
#include "EmoteData.h"  // SerializeEmotesJson / SaveEmotesJson
#include "MeMotes.h"    // SerializeMeMotesJson / SaveMeMotesJson
#include "DevStateInspector.h"  // dev-only "Save scheduler" section (empty in non-dev)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace {
using SteadyClock = std::chrono::steady_clock;

// Write a config this long after its LAST change (so a burst coalesces to one
// write once the user stops), but never defer longer than kMaxDefer even if it
// keeps being dirtied (a safety net against any residual per-frame dirtier).
constexpr auto kDebounce = std::chrono::milliseconds(1000);
constexpr auto kMaxDefer = std::chrono::milliseconds(5000);

constexpr int kNumKinds = 3;  // keep in sync with SaveKind

struct DirtySlot {
    bool                   dirty = false;
    SteadyClock::time_point firstChange{};
    SteadyClock::time_point lastChange{};
};
DirtySlot  s_slots[kNumKinds];  // indexed by (int)SaveKind
std::mutex s_slotMu;            // guards s_slots — RequestSave may be called from
                                // any thread (most UI is render-thread, but the
                                // guard removes all doubt); PumpSaves reads here.

// --- background writer thread ------------------------------------------
// Single thread + a last-wins queue keyed by path: a newer enqueue for a path
// overwrites the pending content, so a stale snapshot is never written after a
// fresh one. Serialization stays on the render thread (callers pass finished
// bytes here), so the writer touches only its queue and the filesystem.
std::thread             s_writer;
std::mutex              s_qmu;
std::condition_variable s_qcv;
std::map<std::string, std::string> s_pending;  // path -> latest content
bool                    s_stop    = false;
bool                    s_started = false;

// Writer-thread telemetry — written by the worker after each AtomicWriteFile,
// read lock-free by the dev-tools "Save scheduler" section. Makes the off-thread
// disk cost VISIBLE (each write is sub-ms and infrequent) rather than merely
// trusted. Unconditional (a couple of relaxed atomic stores per write is free),
// only the display is dev-gated.
std::atomic<unsigned long long> s_writeCount{0};
std::atomic<unsigned long long> s_writeBytes{0};
std::atomic<double>             s_lastWriteMs{0.0};
std::atomic<double>             s_peakWriteMs{0.0};

void WriterLoop() {
    for (;;) {
        std::map<std::string, std::string> batch;
        {
            std::unique_lock<std::mutex> lk(s_qmu);
            s_qcv.wait(lk, [] { return s_stop || !s_pending.empty(); });
            batch.swap(s_pending);                 // take everything queued so far
            if (batch.empty() && s_stop) return;   // stop only once fully drained
        }
        for (auto& kv : batch) {
            const auto t0 = SteadyClock::now();
            AtomicWriteFile(kv.first, kv.second);
            const double ms = std::chrono::duration<double, std::milli>(
                SteadyClock::now() - t0).count();
            s_lastWriteMs.store(ms, std::memory_order_relaxed);
            s_writeCount.fetch_add(1, std::memory_order_relaxed);
            s_writeBytes.fetch_add(kv.second.size(), std::memory_order_relaxed);
            for (double pk = s_peakWriteMs.load(std::memory_order_relaxed);
                 ms > pk && !s_peakWriteMs.compare_exchange_weak(
                                pk, ms, std::memory_order_relaxed); ) {}
        }
    }
}

void Enqueue(const std::string& path, std::string content) {
    if (path.empty()) return;
    {
        std::lock_guard<std::mutex> lk(s_qmu);
        s_pending[path] = std::move(content);  // last-wins
    }
    s_qcv.notify_one();
}

std::string Serialize(SaveKind k) {
    switch (k) {
        case SaveKind::Settings: return SerializeSettings();
        case SaveKind::Emotes:   return SerializeEmotesJson();
        case SaveKind::MeMotes:  return SerializeMeMotesJson();
    }
    return {};
}

const std::string& PathFor(SaveKind k) {
    switch (k) {
        case SaveKind::Settings: return g_SettingsPath;
        case SaveKind::Emotes:   return g_EmotesJsonPath;
        case SaveKind::MeMotes:  return g_MeMotesJsonPath;
    }
    static const std::string none;
    return none;
}

void StopWriter() {
    if (!s_started) return;
    { std::lock_guard<std::mutex> lk(s_qmu); s_stop = true; }
    s_qcv.notify_one();
    if (s_writer.joinable()) s_writer.join();
    s_started = false;
}
}  // namespace

void StartSaveWriter() {
    if (s_started) return;
    s_stop    = false;
    s_writer  = std::thread(WriterLoop);
    s_started = true;
}

void RequestSave(SaveKind k) {
    const auto now = SteadyClock::now();
    std::lock_guard<std::mutex> lk(s_slotMu);
    DirtySlot& s = s_slots[(int)k];
    if (!s.dirty) { s.dirty = true; s.firstChange = now; }
    s.lastChange = now;
}

void PumpSaves() {
    const auto now = SteadyClock::now();
    // Under the lock, decide which configs are due and clear their dirty bits;
    // serialize + enqueue OUTSIDE the lock so a concurrent RequestSave never waits
    // on serialization. A change arriving between clear and serialize simply
    // re-dirties the slot and is picked up next frame.
    SaveKind due[kNumKinds];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(s_slotMu);
        for (int i = 0; i < kNumKinds; ++i) {
            DirtySlot& s = s_slots[i];
            if (!s.dirty) continue;
            if (now - s.lastChange >= kDebounce || now - s.firstChange >= kMaxDefer) {
                s.dirty = false;
                due[n++] = (SaveKind)i;
            }
        }
    }
    for (int j = 0; j < n; ++j) Enqueue(PathFor(due[j]), Serialize(due[j]));
}

void FlushSavesBlocking() {
    // Called from AddonUnload, after the render callbacks are deregistered, so no
    // PumpSaves can run concurrently. Capture which catalogs are still dirty, then
    // stop+join the writer (draining anything it had queued) before our final
    // synchronous writes so the writes can't race.
    bool emotesDirty, meMotesDirty;
    {
        std::lock_guard<std::mutex> lk(s_slotMu);
        emotesDirty  = s_slots[(int)SaveKind::Emotes].dirty;
        meMotesDirty = s_slots[(int)SaveKind::MeMotes].dirty;
    }

    StopWriter();

    // Always write settings: pure navigation state (active category, collapse
    // flags) marks nothing dirty, so this unload flush is its guaranteed persist
    // point. Catalogs are written only if a real edit left them dirty.
    if (!g_SettingsPath.empty())                   SaveSettings(g_SettingsPath);
    if (emotesDirty  && !g_EmotesJsonPath.empty())  SaveEmotesJson(g_EmotesJsonPath);
    if (meMotesDirty && !g_MeMotesJsonPath.empty()) SaveMeMotesJson(g_MeMotesJsonPath);

    {
        std::lock_guard<std::mutex> lk(s_slotMu);
        for (auto& s : s_slots) s.dirty = false;
    }
}

#ifdef EMOT3_DEVTOOLS
// Self-registering runtime-state section (Layer 2 — see DevStateInspector.h).
// Read-only snapshot of the save scheduler so the writer-thread cost is something
// you can WATCH, not just trust: total writes + bytes, last/peak write duration
// (the actual off-thread disk cost), the pending-queue depth, and the per-kind
// dirty bits. Toggle a setting and watch a dirty bit flip to 1, then ~1s later a
// write land (count++, duration sub-ms) and the bit clear — the debounce, live.
// The anon-namespace statics above are visible here (same translation unit).
static DevStateRegistrar s_saveSchedulerState(DevStateCat::Config, "Save scheduler", [] {
    DevStateRow("writes (total)", "%llu", s_writeCount.load(std::memory_order_relaxed));
    DevStateRow("bytes written",  "%llu", s_writeBytes.load(std::memory_order_relaxed));
    DevStateRow("last write",     "%.3f ms", s_lastWriteMs.load(std::memory_order_relaxed));
    DevStateRow("peak write",     "%.3f ms", s_peakWriteMs.load(std::memory_order_relaxed));
    int pending;
    { std::lock_guard<std::mutex> lk(s_qmu);   pending = (int)s_pending.size(); }
    DevStateRow("pending queue", "%d", pending);
    bool d[kNumKinds];
    { std::lock_guard<std::mutex> lk(s_slotMu); for (int i = 0; i < kNumKinds; ++i) d[i] = s_slots[i].dirty; }
    DevStateRow("dirty (set/emo/me)", "%d / %d / %d", (int)d[0], (int)d[1], (int)d[2]);
});
#endif  // EMOT3_DEVTOOLS
