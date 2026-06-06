#pragma once

// Memoized text shaping for emote cell labels. Replaces Ellipsize / FitName
// (formerly in Layout.cpp) for the per-frame cell-render hot path.
//
// Why a cache: each label needs ImGui::CalcTextSize to fit it into the cell.
// Ellipsize pops characters in a loop and re-measures after each; FitName tries
// every space-split position with two measures per try. On a steady display the
// same (emote name, view mode, max width) tuple reproduces the same string
// every frame, so we memoize and skip the shaping. Each entry also stores the
// measured size, so the caller doesn't re-measure the returned string for
// centering (the original Ellipsize/FitName threw their last measurement away).
//
// Composes with the off-screen cell cull in RenderEmoteSection - only visible
// cells reach this cache, so first-visit and scroll-burst frames pay the
// shaping cost while steady-state frames hit a small hash lookup.
//
// Invalidation (lazy, checked on every lookup):
//   - Catalog mutation (rename / add / delete) bumps g_EmoteCatalogVersion;
//     a counter mismatch clears the cache. g_EmotesDirty alone would not work
//     because LoadEmoteTextures consumes it at the top of every frame, well
//     before RenderEmoteCell reads it.
//   - Font scale change: when Nexus rebuilds the font atlas at a different
//     size, ImGui::GetFontSize() differs between frames and the cache clears.
//     This covers the Nexus UI-scaling control without any explicit hook.
//   - Both invalidations clear wholesale; no per-entry eviction.
//
// Memory ceiling: catalog size (~68 bundled + user-added) x 4 view modes x a
// small set of widths in practice = a few hundred entries, well under 100 KB.
// Stale entries from a one-time scale change drift here until the next catalog
// mutation; the ceiling stays small enough that no LRU is justified. Document
// it here so a future contributor doesn't add eviction logic thinking the cache
// is unbounded.
//
// Threading: main-thread only. Called from RenderEmoteCell inside mp.draw /
// qb.draw, which both run on the render thread after BuildCatalogIndex
// completes under g_EmotesMutex. The cache itself takes no lock - any worker
// that mutates the catalog must bump g_EmoteCatalogVersion (atomic) but never
// touches the maps directly.

#include <string>

#include "imgui/imgui.h"
#include "Settings.h"   // EViewMode

namespace TextCache {

// Single-line ellipsized label + its measured size. The size lets callers skip
// a second CalcTextSize on the returned string (the original Ellipsize did the
// final measurement implicitly and discarded the value).
struct EllipsizedEntry {
    std::string label;
    ImVec2      size;
};

// Two-line label-fit result + each line's measured size. line2 is empty when
// the name fit on one line; size2 is then unused (left zero-initialized).
struct FitEntry {
    std::string line1;
    std::string line2;
    ImVec2      size1;
    ImVec2      size2;
};

// Ellipsize `name` to fit within `maxW`. emoteId + mode + quantized maxW key
// the cache slot; `name` populates the entry on miss. Returned reference stays
// valid only until the next invalidation event (catalog version bump or font
// size change) - do not hold it across frames.
const EllipsizedEntry& EllipsizeCached(const std::string& emoteId,
                                       const std::string& name,
                                       EViewMode mode, float maxW);

// Fit `name` into `maxW` across one or two lines (the Full-mode two-line
// label). emoteId + quantized maxW key the cache slot; view mode is implicit
// (Full is the only caller, since the other modes use Ellipsize).
const FitEntry& FitNameCached(const std::string& emoteId,
                              const std::string& name,
                              float maxW);

// --- Dev-tool introspection (used by devtools/MemoryMonitor) -----------
// Ungated (not behind EMOT3_DEVTOOLS) so the Distribution + Plus DLLs
// still expose them — the implementation is a 1-line .size() / a small sum
// so the cost in shipped builds is a single uncalled function (linker
// drops it when unreferenced).
//
// EllipsizeMapSize / FitMapSize  - entry counts of the two caches.
// ApproxBytes                    - estimated heap footprint of BOTH caches:
//   bucket array + per-node overhead + per-entry string-capacity bytes.
//   The estimate is within ~2x; what matters for leak detection is the
//   shape over time, not the absolute number.
size_t EllipsizeMapSize();
size_t FitMapSize();
size_t ApproxBytes();

}  // namespace TextCache
