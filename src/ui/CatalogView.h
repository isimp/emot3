#pragma once
// =====================================================================
//  Shared, cross-frame-cached catalog lookup tables for the render
//  surfaces (Library / Quickbar / Palette / Options unlock count).
//
//  Rebuilt ONLY when the emote catalog, the /me-mote catalog, or the
//  UI-view revision (favorites membership + the manual-unlock set)
//  changes - so idle frames do ZERO index work, where every surface
//  used to rebuild these string-keyed maps from scratch every frame
//  (the qb.frame / mp.frame allocation peaks).
//
//  Why this cross-frame cache is SAFE (unlike the rolled-back FilterCache,
//  see emot3.md "Things we tried and rolled back"):
//   - Every input has a version. Catalog CONTENT bumps
//     g_EmoteCatalogVersion / g_MeMotesVersion; that also covers vector
//     REALLOCATION, so the cached Emote*/MeMote* can never dangle - a
//     realloc forces a rebuild before the next read.
//   - Favorites membership + manual-unlock bump g_UiViewRevision, whose
//     mutation sites are UNIFIED in Favorites.cpp + EmoteAction.cpp (one
//     bump per chokepoint) - not the 17 scattered sites that sank the
//     output-list FilterCache. This caches the shared INDEX, not the
//     per-surface output, so search / filter / category / reorder stay
//     LIVE in each surface's (now cheap) build and need no invalidation.
// =====================================================================

#include <cstddef>       // size_t
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "EmoteData.h"   // CatalogIndex (byId + unlockedIds)

struct MeMote;

struct CatalogView {
    CatalogIndex idx;                                          // Emotes: byId + unlockedIds
    std::unordered_map<std::string, const MeMote*> meMotesById;
    std::unordered_set<std::string> favoritedIds;             // Emote ids sitting in any category
    std::unordered_set<std::string> favoritedMeMoteIds;       // /me-mote ids sitting in any category
};

// Render-thread only. Returns a reference to the cached view, rebuilding it
// (under g_EmotesMutex + g_MeMotesMutex) iff a version changed - otherwise an
// O(1) version compare and the same reference. NEVER store the reference across
// frames; call this each frame. Consumed lock-free on the render thread (the only
// writer to the catalogs / favorites / unlock set is also the render thread).
const CatalogView& GetCatalogView();

// Dev-tool (MemoryMonitor) accessor: the cached view's total entry count + an estimated
// heap footprint (nodes + bucket arrays + out-of-SSO key strings; within ~2x). Render-
// thread only. Defined unconditionally; only called under EMOT3_DEVTOOLS.
void CatalogViewStats(size_t& count, size_t& bytes);
