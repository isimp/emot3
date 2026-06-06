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

#include "imgui/imgui.h"
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
extern std::string g_IconsDir;        // icons/ (local PNG overrides)
extern std::string g_PresetsDir;      // presets/ (one JSON per Quickbar preset)

// --- Cross-module flags ------------------------------------------------

// True when the unlockable-emote catalog mutated this frame, so the
// renderer re-kicks the texture loader without a full reload. Consumed and
// cleared at the top of every render call by LoadEmoteTextures (MainPanel.cpp)
// - it's a one-frame edge signal, not a multi-frame epoch. Anything that needs
// to detect mutations LATER in the frame (e.g. TextCache, which looks at the
// catalog from RenderEmoteCell after LoadEmoteTextures has already cleared the
// flag) must read g_EmoteCatalogVersion below instead.
extern bool g_EmotesDirty;

// Monotonically-incrementing catalog epoch. Bumped (paired with g_EmotesDirty)
// by every mutation site via MarkEmotesDirty() below. Read by the per-frame
// text-shaping cache (ui/TextCache) so it can invalidate when a name changes,
// an emote is added/removed, etc. Atomic because UnlockScan bumps it from a
// background scan thread (relaxed ordering is enough - the actual catalog
// mutation already happens under g_EmotesMutex, which carries the
// happens-before).
extern std::atomic<uint64_t> g_EmoteCatalogVersion;

// Single helper for "the catalog (or anything that affects icon eligibility)
// just changed." Sets the one-frame texture-reload edge AND bumps the
// monotonic version that the text cache observes. Inline so any TU touching
// g_EmotesDirty can call it without an extra include - paired 1:1 with what
// used to be `g_EmotesDirty = true;` at every mutation site. Slightly
// over-invalidates a few non-catalog cases (e.g. the AI-icon-fallback toggle),
// but the cost is a single-frame text-cache repop (invisible), and the 1:1
// pairing prevents the two flags from drifting if a future site forgets one.
inline void MarkEmotesDirty() {
    g_EmotesDirty = true;
    g_EmoteCatalogVersion.fetch_add(1, std::memory_order_relaxed);
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

// Screen-space rects of every interactive widget rendered in the Quickbar
// last frame (icon buttons, title bar, category selector). The QB's
// click-through logic hit-tests the cursor against this list before
// deciding whether to add ImGuiWindowFlags_NoInputs to the window — over
// any rect → keep input; off → pass through to the game.
extern std::vector<std::pair<ImVec2, ImVec2>> g_QbIconRects;

// --- Quickbar window geometry ------------------------------------------
//
// The Quickbar's size/position normally live only in ImGui's .ini. Presets
// need to read the live geometry (to capture) and write it back (to apply),
// so the Quickbar render mirrors it into these globals.

// Live geometry, refreshed each frame the Quickbar actually renders.
// g_QbGeometryValid stays false until the QB has drawn at least once, so a
// preset captured before the QB has ever opened simply stores no geometry.
extern float g_QbWinX, g_QbWinY, g_QbWinW, g_QbWinH;
extern bool  g_QbGeometryValid;

// One-shot apply: set when a preset is applied. The Quickbar consumes it on
// its next render — clamps to the viewport work area (so an off-screen or
// oversized saved geometry still lands usable), positions/sizes the window
// with ImGuiCond_Always, then clears the flag.
extern bool  g_QbApplyGeometry;
extern float g_QbApplyX, g_QbApplyY, g_QbApplyW, g_QbApplyH;

// Cell step (cellW+spacingX, cellH+spacingY) of the LAST Quickbar grid
// render, written by RenderEmoteSection on the isQuickbar pass. The window
// drag-snap (Quickbar.cpp) reads these one frame later to round the window
// to whole cells; 0 until the QB has drawn once.
extern float g_QbStepX, g_QbStepY;

// Column/row count of the LAST Quickbar grid render (RenderEmoteSection,
// isQuickbar pass). Used by the dev sizing readout (QuickbarDebug.h).
extern int   g_QbCols, g_QbRows;

// True when the last Quickbar grid had more cells than the viewport showed
// (integer test: total rows/cols > visible). This drives the custom
// scrollbar / gutter decision - immune to ImGui's sub-pixel ContentSize
// rounding, unlike a pixel scrollMax threshold.
extern bool  g_QbOverflow;

// The Quickbar's per-frame "can't emote right now" reason (mounted, swimming,
// downed, ...) lives in core/CharacterState.h as g_QbBlockReason - set by
// QuickbarRender, consumed by RenderEmoteCell to dim + block + explain cells.

// Cell-aligned max scroll of the last Quickbar grid = (total - visible)*step
// on the scroll axis (0 when it fits). Quickbar clamps owned scrolling to
// this so ImGui's ~1px ContentSize overhead is never scrollable (no sub-pixel
// scroll, no leftover notch at the end). The other axis is 0.
extern float g_QbMaxScrollX, g_QbMaxScrollY;
