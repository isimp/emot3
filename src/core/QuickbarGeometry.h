#pragma once

// Quickbar window geometry + last-grid metrics, mirrored from the Quickbar's
// live ImGui state so other modules can read them WITHOUT touching ImGui. All
// plain types (no imgui), so this is a neutral core/ header: data/QuickbarPresets
// reads it to capture/apply preset geometry, the dev tools read it for the
// sizing readout, and the owned-scroll math reads step/overflow/max-scroll.
// Written by the Quickbar render (Quickbar.cpp / RenderEmoteSection); defined in
// core/Globals.cpp.

// --- Quickbar window geometry ------------------------------------------
//
// The Quickbar's size/position normally live only in ImGui's .ini. Presets need
// to read the live geometry (to capture) and write it back (to apply), so the
// Quickbar render mirrors it into these globals.

// Live geometry, refreshed each frame the Quickbar actually renders.
// g_QbGeometryValid stays false until the QB has drawn at least once, so a
// preset captured before the QB has ever opened simply stores no geometry.
extern float g_QbWinX, g_QbWinY, g_QbWinW, g_QbWinH;
extern bool  g_QbGeometryValid;

// One-shot apply: set when a preset is applied. The Quickbar consumes it on its
// next render - clamps to the viewport work area (so an off-screen or oversized
// saved geometry still lands usable), positions/sizes the window with
// ImGuiCond_Always, then clears the flag.
extern bool  g_QbApplyGeometry;
extern float g_QbApplyX, g_QbApplyY, g_QbApplyW, g_QbApplyH;

// Cell step (cellW+spacingX, cellH+spacingY) of the LAST Quickbar grid render,
// written by RenderEmoteSection on the isQuickbar pass. The window drag-snap
// (Quickbar.cpp) reads these one frame later to round the window to whole cells;
// 0 until the QB has drawn once.
extern float g_QbStepX, g_QbStepY;

// Column/row count of the LAST Quickbar grid render (RenderEmoteSection,
// isQuickbar pass). Used by the dev sizing readout (QuickbarDebug.h).
extern int   g_QbCols, g_QbRows;

// True when the last Quickbar grid had more cells than the viewport showed
// (integer test: total rows/cols > visible). This drives the custom scrollbar /
// gutter decision - immune to ImGui's sub-pixel ContentSize rounding.
extern bool  g_QbOverflow;

// Cell-aligned max scroll of the last Quickbar grid = (total - visible)*step on
// the scroll axis (0 when it fits). Quickbar clamps owned scrolling to this so
// ImGui's ~1px ContentSize overhead is never scrollable. The other axis is 0.
extern float g_QbMaxScrollX, g_QbMaxScrollY;
