#include "MemoryMonitor.h"
#include "QbHitRects.h"   // g_QbIconRects (capacity row)

#ifdef EMOT3_DEVTOOLS

// =====================================================================
//  MemoryMonitor: global operator new/delete overrides + the overlay
//  rendering. See MemoryMonitor.h for the design overview.
//
//  Everything in this TU is gated on EMOT3_DEVTOOLS. In the public
//  Distribution + Plus DLLs the file compiles to an empty object and the
//  CRT's default operator new/delete resolves, byte-identical to today.
// =====================================================================

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>     // _msize (MSVC heap-block size for malloc'd pointers)

// Heap-block size of a malloc'd pointer. MSVC and MinGW-w64 both spell it
// _msize (in <malloc.h>); a non-Windows libc would use malloc_usable_size.
// Devtools-only TU, so this never affects a shipped build either way.
#if defined(_MSC_VER) || defined(__MINGW32__)
#define EMOT3_MSIZE(p) _msize(p)
#else
#define EMOT3_MSIZE(p) malloc_usable_size(p)
#endif
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"

#include "DevToolsUI.h"   // devui::FormatBytesAuto / NumCell
#include "Globals.h"
#include "EmoteData.h"
#include "MeMotes.h"
#include "CatalogView.h"   // CatalogViewStats (cached-index footprint row)
#include "Settings.h"
#include "TextCache.h"
#include "Usage.h"      // usage log row (Recently/Frequently used)
#include "RadialExports.h" // radial-export row (wheel count + bytes)
#include "I18n.h"
#include "Icons.h"         // IconPoolStats / IconPoolUsage - deduped icon-texture pool sizing
#include "IconPicker.h"    // IconPickerListStats - the picker's transient lists
#include "Resources.h"     // kMeMoteAIIcons / kMeMoteAIIconsCount + BundledIcon (bundled-AI manifest row)
#include "Profiling.h"     // prof::Ring, prof::kHistLen, prof::displayMap

#pragma comment(lib, "psapi.lib")
#include <windows.h>
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
    memmon::g_DllBytesAllocated.fetch_add(EMOT3_MSIZE(p), std::memory_order_relaxed);
    memmon::g_DllAllocCount    .fetch_add(1,             std::memory_order_relaxed);
    return p;
}

void* operator new[](size_t n) {
    // The standard allows scalar/array new to differ; we route them through
    // the same path so the counter doesn't have to distinguish.
    return ::operator new(n);
}

void operator delete(void* p) noexcept {
    if (!p) return;
    memmon::g_DllBytesAllocated.fetch_sub(EMOT3_MSIZE(p), std::memory_order_relaxed);
    memmon::g_DllAllocCount    .fetch_sub(1,             std::memory_order_relaxed);
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

// Which heap a row's bytes live in. DllHeap = our operator new (counted in the DLL
// heap total, so those rows + "unaccounted" reconcile to it). External = a pool we
// don't own - Nexus's texture heap, .rdata statics - shown for context but NEVER
// summed into the DLL heap total (that was the apples+oranges in the old TOTAL).
enum class MemKind { DllHeap, External };

// One row's per-frame value, named by a stable string literal so we can use
// it as a map key without allocating.
struct Snapshot {
    const char* name;
    size_t      count;
    size_t      bytes;
    MemKind     kind = MemKind::DllHeap;
};

// Promoted, displayed per-row stats with rolling history.
struct RowStat {
    size_t       count = 0;
    size_t       bytes = 0;
    MemKind      kind  = MemKind::DllHeap;
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

    // Catalog (mutex-guarded - workers can mutate g_Emotes via UnlockScan). Sums
    // the struct + every owned std::string heap allocation. Icon textures are NOT
    // tallied per-entry here: they're a deduped pool shared with /me-motes and
    // the picker, counted once in the "icon textures (content pool)" row below
    // (Nexus-owned heap, est. Width*Height*4 per distinct texture).
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        size_t bytes = g_Emotes.capacity() * sizeof(Emote);
        for (const auto& e : g_Emotes) {
            bytes += string_heap(e.Id);
            bytes += string_heap(e.Command);
            bytes += string_heap(e.Name);
            bytes += string_heap(e.IconPath);
            bytes += e.Aliases.capacity() * sizeof(std::string);
            for (const auto& a : e.Aliases) bytes += string_heap(a);
        }
        out.push_back({ "catalog (g_Emotes)", g_Emotes.size(), bytes });
    }

    // /me-mote catalog. Symmetric to the g_Emotes row above — sums struct +
    // every owned std::string heap allocation (textures are in the shared
    // content-pool row below, not per catalog). /me-motes carry three text
    // bodies plus the standard Id/Name/Icon/Aliases, so the per-entry byte
    // count runs higher than an Emote on average.
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

    // Shared cached catalog index (ui/CatalogView): byId + meMotesById + the favorited
    // sets, rebuilt only on catalog/favorites/unlock change. OUR heap - hash nodes +
    // bucket arrays + COPIED string keys (distinct from the catalog strings above), so
    // it's its own row. Previously uncounted -> it inflated "unaccounted" until now.
    {
        size_t c = 0, b = 0; CatalogViewStats(c, b);
        out.push_back({ "catalog view (cached index)", c, b });
    }

    // Bundled emote i18n table (s_table from emotes_i18n.json): every bundled emote x
    // every language's command/name/aliases. Loaded once, resident for the addon's life
    // (seeding, language switch, search, unlock matching). Our heap; was uncounted.
    {
        size_t c = 0, b = 0; BundledEmoteTableStats(c, b);
        out.push_back({ "bundled emote i18n (s_table)", c, b });
    }
    // Unlock match index (GetBundledUnlockInfo.normKeyToId): the cross-language
    // normalized command/alias -> id map the GW2-API unlock sync resolves against.
    {
        size_t c = 0, b = 0; BundledUnlockIndexStats(c, b);
        out.push_back({ "unlock match index", c, b });
    }

    // /me-mote bundled-seed table. Lazily parsed file-static behind
    // MeMotesBundledSeedCount/Bytes accessors so we don't reach into the
    // anonymous-namespace globals (same accessor pattern TextCache +
    // I18n use). Count is the SeedEntry-count, bytes are out-of-line
    // string heap across the bundle's by-language entries.
    out.push_back({ "/me-mote bundled (s_seed)",
                    MeMotesBundledSeedCount(),
                    MeMotesBundledSeedBytes() });

    // Bundled AI-icon manifest overhead. The RCDATA bytes themselves live
    // in the DLL's .rdata section and don't move on the heap (so they're
    // not "DLL-attributable" in the alloc-counter sense MemoryMonitor's
    // global row tracks), but the manifest array exists in process memory
    // and is worth surfacing so the row is visible alongside its sibling
    // texture row above.
    {
        size_t bytes = (size_t)kMeMoteAIIconsCount * sizeof(BundledIcon);
        for (int i = 0; i < kMeMoteAIIconsCount; ++i) {
            if (kMeMoteAIIcons[i].command)
                bytes += std::strlen(kMeMoteAIIcons[i].command) + 1;
        }
        out.push_back({ "/me-mote bundled AI icons (manifest)",
                        (size_t)kMeMoteAIIconsCount, bytes, MemKind::External });
    }

    // Icon textures: ONE deduped content pool shared by cells AND the picker
    // (each distinct image loads once; W*H*4 per distinct key). The dev-state
    // inspector splits this into in-use vs idle; here we surface the reserved
    // total so it's visible against the process working set.
    {
        IconPoolUsage u; IconPoolStats(u);
        out.push_back({ "icon textures (content pool, est)", u.totalCount, u.totalBytes,
                        MemKind::External });
    }

    // Icon-cache BOOKKEEPING: the id->key memos + the permanent attempted-keys
    // set (the string-key maps/sets behind the lazy content cache). Distinct from
    // the texture pool row above - those bytes are Nexus-owned; these are our heap
    // and s_attemptedKeys grows monotonically, so it's a leak-watch row.
    {
        size_t c = 0, b = 0; IconCacheKeyStats(c, b);
        out.push_back({ "icon cache keys/memo", c, b });
    }

    // Icon picker's transient lists (folder filenames + the 4 PickItem vectors).
    // Non-zero only while the picker modal is open (rebuilt each open).
    {
        size_t c = 0, b = 0; IconPickerListStats(c, b);
        out.push_back({ "icon picker lists", c, b });
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

    // Usage log (Recently/Frequently used; bounded at kUsageLog).
    out.push_back({ "usage log", usage::LogSize(), usage::ApproxBytes() });

    // Radial exports (in-memory wheel record; scanned at load, not accumulating).
    out.push_back({ "radial exports",
                    (size_t)RadialExportWheelCount(),
                    RadialExportApproxBytes() });

    // I18n L() cache (the lazily-grown subset that's been looked up).
    out.push_back({ "I18n cache",
                    TranslationCacheSize(),
                    TranslationCacheApproxBytes() });

    // I18n ground-truth tables (the always-resident bundled English table +
    // display names + codes) - distinct from the cache above, and the larger of
    // the two. Loaded once at init; flat thereafter.
    out.push_back({ "I18n table (en)",
                    TranslationTableSize(),
                    TranslationTableApproxBytes() });

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

    // MemoryMonitor measuring ITSELF: the rows() map - each RowStat carries two
    // kHistLen-float history rings (~488 B each) - plus the DLL/WS history rings and any
    // baseline copy. Sampled from last frame's rows() (one-frame lag, fine for a dev tool).
    {
        size_t c = rows().size();
        size_t b = rows().size() * (sizeof(std::string) + sizeof(RowStat) + 32)
                 + 2 * sizeof(prof::Ring);
        if (baseline().active)
            b += baseline().rows.size() *
                 (sizeof(std::string) + sizeof(std::pair<size_t, size_t>) + 32);
        out.push_back({ "memmon (self)", c, b });
    }
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
        r.kind  = s.kind;
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

        // Auto-unit (KB below 10 MB, else MB): our addon allocates ~50-200 KB of
        // heap in practice (most "feel-like-addon" memory - icon textures, ImGui
        // buffers - lives in Nexus's heap, not ours), so MB precision at that
        // scale would make view-mode swaps and small leaks invisible.
        ImGui::Text("DLL heap: %s   (%zu live allocs)",
                    devui::FormatBytesAuto(dllBytes).c_str(), dllAllocs);
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
            int n = prof::FlattenRing(memmon::dllHistory(), buf);
            if (n > 0) {
                ImGui::PlotLines("##dll_spark", buf, n, 0,
                                  "DLL heap (KB) over time",
                                  FLT_MAX, FLT_MAX, ImVec2(0, 40));
            }
        }
        ImGui::Separator();

        // ---- Per-subsystem table --------------------------------------
        // Columns are click-sortable; numerics right-aligned. Rows are copied
        // out of the map into a vector so they can be reordered per the sort
        // spec (default: bytes desc - biggest consumer on top). A pinned TOTAL
        // row stays at the bottom regardless of sort.
        const bool baseAct = memmon::baseline().active;
        struct MRow { const std::string* name; size_t count; size_t bytes;
                      int64_t delta; double peakKB; bool growing; };

        // Partition by heap kind: DLL-heap rows reconcile to the DLL heap total above;
        // External rows (Nexus texture heap / .rdata statics) are shown apart and never
        // summed into it - that mix was the old TOTAL's apples+oranges.
        std::vector<MRow> dllRows, extRows;
        size_t dllCount = 0, dllBytesSum = 0;
        for (const auto& kv : memmon::rows()) {
            const memmon::RowStat& s = kv.second;
            int64_t deltaBytes = 0; bool growing = false;
            if (baseAct) {
                auto it = memmon::baseline().rows.find(kv.first);
                if (it != memmon::baseline().rows.end()) {
                    deltaBytes = (int64_t)s.bytes - (int64_t)it->second.second;
                    if (deltaBytes > 0) growing = memmon::IsMonotonicGrowth(s.bytesHist, 30);
                }
            }
            MRow row{ &kv.first, s.count, s.bytes, deltaBytes, s.bytesHist.peak(), growing };
            if (s.kind == memmon::MemKind::External) extRows.push_back(row);
            else { dllRows.push_back(row); dllCount += s.count; dllBytesSum += s.bytes; }
        }

        const int cols = baseAct ? 5 : 4;
        constexpr ImGuiTableFlags kMemFlags =
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_NoSavedSettings;

        // Shared column setup / per-spec sort / row draw, so both sections look alike.
        auto setupCols = [&] {
            ImGui::TableSetupColumn("subsystem", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("count",     ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("bytes (KB)",
                ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultSort);
            if (baseAct) ImGui::TableSetupColumn("Δ KB", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("peak KB",   ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableHeadersRow();
        };
        auto sortBySpec = [&](std::vector<MRow>& rs) {
            ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs();
            if (!sp || sp->SpecsCount == 0) return;
            const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
            bool asc = c.SortDirection == ImGuiSortDirection_Ascending;
            int  ci  = c.ColumnIndex;
            std::sort(rs.begin(), rs.end(), [&](const MRow& a, const MRow& b) {
                if (ci == 0) { int r = a.name->compare(*b.name); return asc ? r < 0 : r > 0; }
                double x, y;
                if      (ci == 1)            { x = (double)a.count; y = (double)b.count; }
                else if (ci == 2)            { x = (double)a.bytes; y = (double)b.bytes; }
                else if (baseAct && ci == 3) { x = (double)a.delta; y = (double)b.delta; }
                else                         { x = a.peakKB;        y = b.peakKB; }
                return asc ? (x < y) : (x > y);
            });
        };
        auto drawDataRow = [&](const MRow& r) {
            ImGui::TableNextRow();
            if (r.growing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name->c_str());
            ImGui::TableSetColumnIndex(1); devui::NumCell("%zu", r.count);
            ImGui::TableSetColumnIndex(2); devui::NumCell("%.1f", r.bytes / 1024.0);
            int col = 3;
            if (baseAct) { ImGui::TableSetColumnIndex(col++);
                           devui::NumCell("%+lld", (long long)(r.delta / 1024)); }
            ImGui::TableSetColumnIndex(col); devui::NumCell("%.1f", r.peakKB);
            if (r.growing) ImGui::PopStyleColor();
        };

        // ---- DLL heap section: tracked rows + unaccounted = the DLL heap total ----
        ImGui::TextDisabled("DLL heap breakdown (rows + unaccounted = the total above):");
        if (ImGui::BeginTable("##memmon_dll", cols, kMemFlags)) {
            setupCols();
            sortBySpec(dllRows);
            for (const MRow& r : dllRows) drawDataRow(r);

            // Unaccounted = the precise DLL heap counter minus everything we categorized
            // (CRT scratch, json temps, the monitor's own maps, any container without a
            // row). Signed: a negative value means our ~2x row estimates overshot.
            int64_t unacc = (int64_t)dllBytes - (int64_t)dllBytesSum;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("unaccounted (CRT / untracked)");
            ImGui::TableSetColumnIndex(2); devui::NumCell("%+.1f", unacc / 1024.0);

            // Pinned root: the true DLL heap total (= the rows + unaccounted, exactly).
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImVec4(0.26f, 0.26f, 0.32f, 0.55f)));
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("TOTAL (DLL heap)");
            ImGui::TableSetColumnIndex(1); devui::NumCell("%zu", dllCount);
            ImGui::TableSetColumnIndex(2); devui::NumCell("%.1f", dllBytes / 1024.0);
            if (baseAct) { ImGui::TableSetColumnIndex(3); devui::NumCell("%+lld",
                (long long)(((int64_t)dllBytes - (int64_t)memmon::baseline().dllBytes) / 1024)); }
            ImGui::EndTable();
        }

        // ---- External section: pools we don't own (NOT part of the DLL heap) ----
        if (!extRows.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("external pools (Nexus / .rdata - not our heap):");
            if (ImGui::BeginTable("##memmon_ext", cols, kMemFlags)) {
                setupCols();
                sortBySpec(extRows);
                for (const MRow& r : extRows) drawDataRow(r);
                ImGui::EndTable();
            }
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
