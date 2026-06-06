#include "MemoryMonitor.h"

#ifdef EMOT3_DEVTOOLS

// =====================================================================
//  MemoryMonitor: global operator new/delete overrides + the overlay
//  rendering. See MemoryMonitor.h for the design overview.
//
//  Everything in this TU is gated on EMOT3_DEVTOOLS. In the public
//  Distribution + Plus DLLs the file compiles to an empty object and the
//  CRT's default operator new/delete resolves, byte-identical to today.
// =====================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>     // _msize (MSVC heap-block size for malloc'd pointers)
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"

#include "Globals.h"
#include "EmoteData.h"
#include "MeMotes.h"
#include "Settings.h"
#include "TextCache.h"
#include "I18n.h"
#include "Icons.h"         // GetEmoteTexture - to size Nexus-owned icon textures
#include "Profiling.h"     // prof::Ring, prof::kHistLen, prof::displayMap

#pragma comment(lib, "psapi.lib")
#include <Windows.h>
#include <psapi.h>

// =====================================================================
//  Global operator new/delete overrides.
//
//  These replace the CRT defaults at link time for every TU compiled
//  into this DLL. Every alloc / dealloc routed through `::operator new`
//  or `::operator delete` (the path used by std::allocator and any
//  unqualified `new T(...)` we write) bumps the counters defined below.
//
//  What's caught:
//    - All our STL containers (std::vector, std::string, std::map,
//      std::unordered_map, ...) - they go through std::allocator -> the
//      global operator new.
//    - nlohmann_json's allocations - uses std::allocator.
//    - Anywhere we write `new T(...)`.
//
//  What's NOT caught:
//    - ImGui. Nexus installs its own ImGui allocator at entry.cpp:142-145
//      (`APIDefs->ImguiMalloc/Free`), which routes ImGui's per-frame
//      vertex / draw-list / atlas churn through Nexus, not through us.
//      That's deliberate and correct: ImGui is shared Nexus infrastructure
//      across all addons, not addon-specific memory.
//    - Direct `malloc` / `_aligned_malloc` calls. We don't write any.
//    - CRT internal scratch (printf format temps, std::ofstream's file
//      buffer, locale conversion buffers). Bounded, transient, not a leak
//      surface.
//    - C++17 aligned-new (`operator new(size_t, std::align_val_t)`). We
//      have no overaligned types in our codebase, so these are never
//      called. If a future type needs alignment, add overrides here.
//
//  Cost: one LOCK XADD per alloc / dealloc, ~1-2 ns each. On top of
//  malloc's own hundreds of ns. ~1% on alloc-heavy code paths, zero on
//  steady render frames.
//
//  _msize: we ask the CRT for the actual heap-block size on every alloc
//  AND every dealloc, so the counter is always perfectly symmetric. C++14
//  sized-delete passes the C++ object size, which isn't the heap-block
//  size; using _msize uniformly avoids the mismatch. The extra microns
//  on every dealloc are negligible for a dev tool.
//
//  Threading: operator new/delete can fire from any thread (workers +
//  main). Atomic counters with relaxed ordering are sufficient - we want
//  monotonic counting, not synchronization with other data.
// =====================================================================

namespace memmon {
    // Const-initialized to 0 before any dynamic init, so allocations from
    // other TUs' static constructors get counted without an init-order hazard.
    std::atomic<size_t> g_DllBytesAllocated { 0 };
    std::atomic<size_t> g_DllAllocCount     { 0 };
}

void* operator new(size_t n) {
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc{};
    memmon::g_DllBytesAllocated.fetch_add(_msize(p), std::memory_order_relaxed);
    memmon::g_DllAllocCount    .fetch_add(1,         std::memory_order_relaxed);
    return p;
}

void* operator new[](size_t n) {
    // The standard allows scalar/array new to differ; we route them through
    // the same path so the counter doesn't have to distinguish.
    return ::operator new(n);
}

void operator delete(void* p) noexcept {
    if (!p) return;
    memmon::g_DllBytesAllocated.fetch_sub(_msize(p), std::memory_order_relaxed);
    memmon::g_DllAllocCount    .fetch_sub(1,         std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p) noexcept {
    ::operator delete(p);
}

// C++14 sized-delete. STL containers call these when the type's size is
// known statically. We use _msize anyway (see header note on symmetry), so
// the size parameter is informational only.
void operator delete (void* p, size_t /*sz*/) noexcept { ::operator delete (p); }
void operator delete[](void* p, size_t /*sz*/) noexcept { ::operator delete[](p); }

// =====================================================================
//  Per-subsystem sampling + the overlay window.
// =====================================================================

namespace memmon {

// Byte-estimate helpers. Within ~2x; the shape over time matters, not the
// absolute number.
namespace mem_est {
    // MSVC SSO holds 15 chars in-struct. Beyond that, std::string holds at
    // least capacity+1 bytes on the heap (capacity chars + null terminator).
    inline size_t string_heap(const std::string& s) {
        return s.capacity() > 15 ? s.capacity() + 1 : 0;
    }
}  // namespace mem_est

// One row's per-frame value, named by a stable string literal so we can use
// it as a map key without allocating.
struct Snapshot {
    const char* name;
    size_t      count;
    size_t      bytes;
};

// Promoted, displayed per-row stats with rolling history.
struct RowStat {
    size_t       count = 0;
    size_t       bytes = 0;
    prof::Ring   countHist;
    prof::Ring   bytesHist;  // stored as KB so the float Ring covers the
                             // range we care about without precision loss
};

// Baseline snapshot captured by [Set baseline] for delta + leak heuristic.
struct Baseline {
    bool                                              active   = false;
    size_t                                            dllBytes = 0;
    size_t                                            procWS   = 0;
    std::map<std::string, std::pair<size_t, size_t>>  rows;    // name -> (count, bytes)
};

// Function-local statics: stable addresses, init-order-safe.
inline std::map<std::string, RowStat>& rows()       { static std::map<std::string, RowStat> m; return m; }
inline Baseline&                       baseline()   { static Baseline b; return b; }
inline prof::Ring&                     dllHistory() { static prof::Ring r; return r; }
inline prof::Ring&                     wsHistory()  { static prof::Ring r; return r; }
inline int&                            lastFrame()  { static int f = -1; return f; }

// Fill `out` with one snapshot per tracked subsystem. Names are string
// literals - cheap to use as map keys.
void Sample(std::vector<Snapshot>& out) {
    using mem_est::string_heap;
    out.clear();
    out.reserve(10);

    // Catalog (mutex-guarded - workers can mutate g_Emotes via UnlockScan).
    // While we hold the lock, also tally the Nexus-owned icon textures: each
    // emote has a Texture in Nexus' cache once its icon has loaded. They live
    // in NEXUS's heap (not ours) so they don't appear in the DLL-heap counter
    // above - but the user thinks of them as "addon memory" because our addon
    // is what made them exist, so the second row puts them back on screen.
    // Decoded RGBA bytes = Width * Height * 4 (a best-effort heap-side
    // estimate; the GPU resource may carry mipmaps or a different format).
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        size_t bytes = g_Emotes.capacity() * sizeof(Emote);
        size_t texBytes = 0, texCount = 0;
        for (const auto& e : g_Emotes) {
            bytes += string_heap(e.Id);
            bytes += string_heap(e.Command);
            bytes += string_heap(e.Name);
            bytes += string_heap(e.IconPath);
            bytes += e.Aliases.capacity() * sizeof(std::string);
            for (const auto& a : e.Aliases) bytes += string_heap(a);

            if (Texture* t = GetEmoteTexture(e.Id)) {
                if (t->Resource) {  // skip placeholder / pending entries
                    texBytes += (size_t)t->Width * (size_t)t->Height * 4u;
                    ++texCount;
                }
            }
        }
        out.push_back({ "catalog (g_Emotes)",          g_Emotes.size(), bytes });
        out.push_back({ "icon textures (Nexus, est)",  texCount,        texBytes });
    }

    // /me-mote catalog. Symmetric to the g_Emotes row above — sums struct +
    // every owned std::string heap allocation. /me-motes carry three text
    // bodies plus the standard Id/Name/Icon/Aliases, so the per-entry byte
    // count runs higher than an Emote on average. Texture sums are folded into
    // the "icon textures (Nexus, est)" row above (GetEmoteTexture is keyed on
    // Id and the loader walks both catalogs once it's wired up).
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        size_t bytes = g_MeMotes.capacity() * sizeof(MeMote);
        for (const auto& m : g_MeMotes) {
            bytes += string_heap(m.Id);
            bytes += string_heap(m.Name);
            bytes += string_heap(m.IconPath);
            bytes += string_heap(m.TextDefault);
            bytes += string_heap(m.TextYou);
            bytes += string_heap(m.TextAll);
            bytes += m.Aliases.capacity() * sizeof(std::string);
            for (const auto& a : m.Aliases) bytes += string_heap(a);
        }
        out.push_back({ "catalog (g_MeMotes)", g_MeMotes.size(), bytes });
    }

    // Notifier pending list.
    {
        size_t bytes = g_NewBundledEmoteIds.capacity() * sizeof(std::string);
        for (const auto& s : g_NewBundledEmoteIds) bytes += string_heap(s);
        out.push_back({ "notifier pending", g_NewBundledEmoteIds.size(), bytes });
    }

    // QB icon rects - .capacity() so we see the high-water mark even after
    // the per-frame clear (vector never shrinks unless shrink_to_fit is
    // called, which nothing does).
    out.push_back({ "QB icon rects (cap)",
                    g_QbIconRects.capacity(),
                    g_QbIconRects.capacity() * sizeof(std::pair<ImVec2, ImVec2>) });

    // Detached workers in flight (RAII counter). Should be 0 at rest, 1
    // briefly during /emote send or unlock sync.
    out.push_back({ "inflight workers",
                    (size_t)g_InflightWorkers.load(),
                    0 });

    // TextCache: count + estimated bytes via the accessors in TextCache.cpp.
    out.push_back({ "TextCache (combined)",
                    TextCache::EllipsizeMapSize() + TextCache::FitMapSize(),
                    TextCache::ApproxBytes() });

    // I18n L() cache.
    out.push_back({ "I18n cache",
                    TranslationCacheSize(),
                    TranslationCacheApproxBytes() });

    // Favorites: total emote IDs across all user categories.
    {
        size_t total = 0;
        size_t bytes = g_Settings.FavoriteCategories.capacity() * sizeof(FavoriteCategory);
        for (const auto& c : g_Settings.FavoriteCategories) {
            total += c.Refs.size();
            bytes += string_heap(c.Name);
            bytes += c.Refs.capacity() * sizeof(FavoriteRef);
            for (const auto& r : c.Refs) bytes += string_heap(r.Id);
        }
        out.push_back({ "favorites (total refs)", total, bytes });
    }

    // ManuallyUnlocked - one row per non-core unlocked emote.
    {
        size_t bytes = g_Settings.ManuallyUnlocked.capacity() * sizeof(std::string);
        for (const auto& s : g_Settings.ManuallyUnlocked) bytes += string_heap(s);
        out.push_back({ "manually unlocked",
                        g_Settings.ManuallyUnlocked.size(),
                        bytes });
    }

    // Profiler displayMap - only nonzero when the perf overlay has been
    // enabled (it populates as PROFILE_SCOPEs close). Worth showing as a
    // sanity row.
    out.push_back({ "profiler labels",
                    prof::displayMap().size(),
                    prof::displayMap().size() *
                        (sizeof(std::string) + sizeof(prof::SectionStat) + 32) });
}

// True when the last `tail` samples in `r` are monotonically non-decreasing
// AND the ring has at least `tail` valid samples. Cheap leak heuristic that
// ignores expected one-time fills (e.g. TextCache plateauing after a scroll
// burst) - those break monotonicity as soon as growth stops.
bool IsMonotonicGrowth(const prof::Ring& r, int tail) {
    if (r.count < tail) return false;
    int start = (r.head - tail + prof::kHistLen) % prof::kHistLen;
    for (int i = 1; i < tail; ++i) {
        int prev = (start + i - 1) % prof::kHistLen;
        int cur  = (start + i    ) % prof::kHistLen;
        if (r.v[cur] < r.v[prev]) return false;
    }
    return true;
}

// Promote a fresh sample into the per-row history rings exactly once per
// ImGui frame, mirroring prof::NewFrameIfNeeded's pattern. While Paused()
// the rings hold still (the live counter shown at the top still ticks).
void NewFrameIfNeeded() {
    int f = ImGui::GetFrameCount();
    if (f == lastFrame()) return;
    lastFrame() = f;
    if (Paused()) return;

    std::vector<Snapshot> snaps;
    Sample(snaps);

    auto& rs = rows();
    for (const auto& s : snaps) {
        RowStat& r = rs[s.name];
        r.count = s.count;
        r.bytes = s.bytes;
        r.countHist.push((float)s.count);
        r.bytesHist.push((float)s.bytes / 1024.0f);  // KB
    }

    // DLL total - the precise primary signal. Track in KB for the ring.
    dllHistory().push((float)(g_DllBytesAllocated.load(std::memory_order_relaxed)
                              / 1024.0));

    // Process WS for the third-tier context row + sparkline. Track in MB.
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc))) {
        wsHistory().push((float)(pmc.WorkingSetSize / (1024.0 * 1024.0)));
    }
}

// Flatten a ring's circular buffer into a contiguous stack buffer in chrono
// order (oldest -> newest), suitable for ImGui::PlotLines. The buffer is
// returned via out-pointer + count.
void FlattenRing(const prof::Ring& r, float* out, int& count) {
    count = r.count;
    int start = (r.head - count + prof::kHistLen) % prof::kHistLen;
    for (int i = 0; i < count; ++i)
        out[i] = r.v[(start + i) % prof::kHistLen];
}

}  // namespace memmon

void RenderMemoryMonitor() {
    if (!memmon::Enabled()) return;
    memmon::NewFrameIfNeeded();

    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("emot3 memory##memmon", &memmon::Enabled(),
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {

        // ---- Top controls ---------------------------------------------
        ImGui::Checkbox("Freeze", &memmon::Paused());
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            memmon::rows().clear();
            memmon::dllHistory().clear();
            memmon::wsHistory().clear();
            memmon::baseline() = {};
        }
        ImGui::SameLine();
        if (memmon::baseline().active) {
            if (ImGui::Button("Clear baseline")) memmon::baseline() = {};
        } else if (ImGui::Button("Set baseline")) {
            memmon::Baseline& b = memmon::baseline();
            b.active   = true;
            b.dllBytes = memmon::g_DllBytesAllocated.load(std::memory_order_relaxed);
            for (const auto& kv : memmon::rows())
                b.rows[kv.first] = { kv.second.count, kv.second.bytes };
            PROCESS_MEMORY_COUNTERS_EX pmc {};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                      sizeof(pmc))) {
                b.procWS = pmc.WorkingSetSize;
            }
        }
        ImGui::Separator();

        // ---- DLL heap (the primary number) ----------------------------
        const size_t dllBytes  = memmon::g_DllBytesAllocated.load(std::memory_order_relaxed);
        const size_t dllAllocs = memmon::g_DllAllocCount    .load(std::memory_order_relaxed);

        bool dllGrowing = memmon::baseline().active
                        && dllBytes > memmon::baseline().dllBytes
                        && memmon::IsMonotonicGrowth(memmon::dllHistory(), 30);
        if (dllGrowing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));

        // Auto-unit: our addon allocates ~50-200 KB of heap in practice (most
        // "feel-like-addon" memory - icon textures, ImGui buffers - lives in
        // Nexus's heap, not ours). MB precision at that scale makes view-mode
        // swaps and small leaks invisible. Show KB below 10 MB; cross the
        // threshold (a real growth spurt) before reading MB.
        const bool dllInKB = dllBytes < 10ull * 1024 * 1024;
        if (dllInKB) {
            ImGui::Text("DLL heap: %.1f KB   (%zu live allocs)",
                        dllBytes / 1024.0, dllAllocs);
        } else {
            ImGui::Text("DLL heap: %.2f MB   (%zu live allocs)",
                        dllBytes / (1024.0 * 1024.0), dllAllocs);
        }
        if (memmon::baseline().active) {
            int64_t delta = (int64_t)dllBytes - (int64_t)memmon::baseline().dllBytes;
            ImGui::SameLine();
            // Δ always in KB - sub-KB jitter isn't useful, and KB precision
            // covers everything up to ~9 GB before the long long overflows.
            ImGui::TextDisabled("(Δ %+lld KB)", (long long)(delta / 1024));
        }
        if (dllGrowing) ImGui::PopStyleColor();

        // Sparkline of DLL heap over time (KB).
        {
            float buf[prof::kHistLen];
            int n = 0;
            memmon::FlattenRing(memmon::dllHistory(), buf, n);
            if (n > 0) {
                ImGui::PlotLines("##dll_spark", buf, n, 0,
                                  "DLL heap (KB) over time",
                                  FLT_MAX, FLT_MAX, ImVec2(0, 40));
            }
        }
        ImGui::Separator();

        // ---- Per-subsystem table --------------------------------------
        const int cols = memmon::baseline().active ? 5 : 4;
        if (ImGui::BeginTable("##memmon_subsystems", cols,
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("subsystem");
            ImGui::TableSetupColumn("count");
            ImGui::TableSetupColumn("bytes (KB)");
            if (memmon::baseline().active) ImGui::TableSetupColumn("Δ KB");
            ImGui::TableSetupColumn("peak KB");
            ImGui::TableHeadersRow();

            for (const auto& kv : memmon::rows()) {
                const memmon::RowStat& s = kv.second;
                ImGui::TableNextRow();

                bool growing = false;
                int64_t deltaBytes = 0;
                if (memmon::baseline().active) {
                    auto it = memmon::baseline().rows.find(kv.first);
                    if (it != memmon::baseline().rows.end()) {
                        deltaBytes = (int64_t)s.bytes - (int64_t)it->second.second;
                        if (deltaBytes > 0)
                            growing = memmon::IsMonotonicGrowth(s.bytesHist, 30);
                    }
                }

                if (growing) ImGui::PushStyleColor(ImGuiCol_Text,
                                                    ImVec4(1.0f, 0.55f, 0.55f, 1.0f));

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(kv.first.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu", s.count);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.1f", s.bytes / 1024.0);
                int col = 3;
                if (memmon::baseline().active) {
                    ImGui::TableSetColumnIndex(col++);
                    ImGui::Text("%+lld", (long long)(deltaBytes / 1024));
                }
                ImGui::TableSetColumnIndex(col);
                ImGui::Text("%.1f", s.bytesHist.peak());

                if (growing) ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        ImGui::Separator();

        // ---- Process context (the disclaimer-bearing third tier) ------
        ImGui::TextDisabled("process context (includes GW2):");
        PROCESS_MEMORY_COUNTERS_EX pmc {};
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                  sizeof(pmc))) {
            const double toMB = 1.0 / (1024.0 * 1024.0);
            ImGui::Text("  working set: %.1f MB   (peak %.1f MB)",
                        pmc.WorkingSetSize     * toMB,
                        pmc.PeakWorkingSetSize * toMB);
            ImGui::Text("  private:     %.1f MB   (peak %.1f MB)",
                        pmc.PagefileUsage     * toMB,
                        pmc.PeakPagefileUsage * toMB);
            if (memmon::baseline().active && memmon::baseline().procWS > 0) {
                int64_t delta = (int64_t)pmc.WorkingSetSize -
                                 (int64_t)memmon::baseline().procWS;
                ImGui::Text("  Δ working set since baseline: %+lld KB",
                            (long long)(delta / 1024));
            }
        } else {
            ImGui::TextDisabled("  (GetProcessMemoryInfo failed)");
        }
    }
    ImGui::End();
}

#endif  // EMOT3_DEVTOOLS
