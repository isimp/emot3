#pragma once

// Shared addon-wide state. Forward-declared by every module that needs to
// reach the Nexus API, the addon paths on disk, or any cross-module flag.
//
// Definitions live in Globals.cpp.

// Pin NOMINMAX here so every TU that pulls in Globals.h gets it before the
// Windows.h include, regardless of compilation order.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

#include "nexus/Nexus.h"
#include "mumble/Mumble.h"

// --- Nexus interface pointers ------------------------------------------
extern AddonAPI*      APIDefs;
extern NexusLinkData* NexusLink;
extern Mumble::Data*  MumbleLink;

// --- Game window handle, captured the first time WndProc fires ----------
extern HWND g_GameHwnd;

// --- On-disk paths under addons/emot3/ ---------------------------------
extern std::string g_SettingsPath;    // settings.json
extern std::string g_EmotesJsonPath;  // emotes.json (editable unlockables)
extern std::string g_MeMotesJsonPath; // me_motes.json (/me-motes; see data/MeMotes.h)
extern std::string g_IconsDir;        // icons/ (local PNG overrides)
extern std::string g_PresetsDir;      // presets/ (one JSON per Quickbar preset)

// --- Cross-module flags ------------------------------------------------

// Monotonically-incrementing emote-catalog epoch, bumped by every mutation site
// via MarkEmotesDirty() below. Read by the lazy texture cache (MaintainTexEpoch,
// ui/Icons.cpp) and the per-frame text-shaping cache (ui/TextCache) so they
// invalidate when a name changes, an emote is added/removed, etc. Atomic with
// relaxed ordering; the catalog mutation itself happens under g_EmotesMutex,
// which carries the happens-before.
extern std::atomic<uint64_t> g_EmoteCatalogVersion;

// Single helper for "the emote catalog (or anything that affects icon
// eligibility) just changed." Bumps the monotonic version the lazy texture cache
// (ui/Icons.cpp MaintainTexEpoch) and the text cache (ui/TextCache.cpp) observe,
// so each re-attempts / re-shapes the affected entries next frame. Inline so any
// TU can call it without an extra include. Slightly over-invalidates a few
// non-catalog cases (e.g. the AI-icon-fallback toggle), but the cost is a
// single-frame cache repop (invisible).
inline void MarkEmotesDirty() {
    g_EmoteCatalogVersion.fetch_add(1, std::memory_order_relaxed);
}

// /me-mote catalog epoch. Symmetric to g_EmoteCatalogVersion but tracks the
// separate /me-mote catalog (see data/MeMotes.h). Bumped via MarkMeMotesDirty
// from every mutation site (Options > /me-motes edits, JSON load). Read by the
// lazy texture cache (clears its /me-mote memo so changed icons re-load on next
// show) and the per-frame TextCache (so /me-mote label shaping invalidates on
// edit).
extern std::atomic<uint64_t> g_MeMotesVersion;

inline void MarkMeMotesDirty() {
    g_MeMotesVersion.fetch_add(1, std::memory_order_relaxed);
}

// --- New-bundled-emote notifier ----------------------------------------
//
// Set at AddonLoad when the bundled table gained emotes since the user's
// snapshot (g_Settings.KnownBundledEmotes) and the notify setting is on.
// MainPanel's AddonRender opens a first-run-style modal once while this is
// true; g_NewBundledEmoteIds holds the ids to offer. The dev "[debug]
// Notifier" tool also drives these for testing. See emot3.md.
extern bool                     g_PromptNewBundledEmotes;
extern std::vector<std::string> g_NewBundledEmoteIds;

// --- Detached-worker shutdown signal -----------------------------------
//
// Set true at the top of AddonUnload so detached worker threads
// (SendOrFillEmote's emote-injection thread, IconBrowse's file-picker
// thread) can bail before their next APIDefs deref. Nexus may call
// AddonUnload while a worker is mid-Sleep, after which APIDefs becomes
// invalid; without this check the worker crashes the host process on
// its next APIDefs->... deref. Workers also bump g_InflightWorkers on
// entry / decrement on exit (RAII), so AddonUnload can briefly wait for
// in-flight work to drain before returning.
extern std::atomic<bool> g_Unloading;
extern std::atomic<int>  g_InflightWorkers;

// RAII guard for a detached worker: bumps g_InflightWorkers on entry, drops it
// on exit so AddonUnload's drain loop can wait out in-flight work. Header-inline
// so the emote-send worker (EmoteAction) and the unlock own-key fetch
// (UnlockScan) share one definition instead of each re-rolling fetch_add/sub.
struct InflightWorkerScope {
    InflightWorkerScope()  { g_InflightWorkers.fetch_add(1); }
    ~InflightWorkerScope() { g_InflightWorkers.fetch_sub(1); }
};

// The Quickbar's screen-space hit-rects (g_QbIconRects, ImVec2-typed) moved to
// ui/QbHitRects.h; its window geometry + grid metrics (g_QbWin* / g_QbStep* /
// g_QbMaxScroll* / ...) moved to core/QuickbarGeometry.h. Keeping the lone
// ImVec2 global out of this header (which is included almost everywhere) is what
// lets the data/ and core/ layers compile without imgui.
//
// (CharacterState's g_QbBlockReason / g_QbUnusableKey are a separate concern -
// see core/CharacterState.h.)
