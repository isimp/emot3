#pragma once

// Small, generic UI primitives reused across the main panel, the Quickbar,
// and the Options tabs. Pure ImGui — no project-specific state.

#include <string>
#include <utility>

#include "imgui/imgui.h"

// Truncate `name` to a single line that fits within `maxW`, appending ".."
// when truncation happens.
std::string Ellipsize(const std::string& name, float maxW);

// Fit `name` into `maxW` pixels of horizontal space:
//   - returns (name, "")       if it fits on one line
//   - returns (first, second)  split at the last space that keeps both lines under maxW
//   - returns (truncated, "")  with ".." otherwise
std::pair<std::string, std::string> FitName(const std::string& name, float maxW);

// Square button drawn with the addon's "no texture" look — used in cells
// when an emote PNG isn't loaded. Returns true when clicked. `alphaMul`
// dims everything to match cell state.
bool RenderStyledFallback(const char* uniqueId, const char* displayName,
                          float size, float alphaMul);

// Two-state toggle button — looks like an ImGui button but highlights in
// the addon's blue when `*state` is true. Returns true on click (after the
// state has already been flipped).
bool ToggleButton(const char* label, bool* state);

// High-contrast button styling for HUD surfaces whose background is hidden:
// darkens the resting Button (and optionally FrameBg) colours and keeps the
// hover/active states bright, so a control reads against the game world instead
// of dissolving into it (see nexus-addon-dev.md "High-contrast button styling").
// Differentiates by RGB, not alpha — an alpha-only delta over the translucent
// world barely registers. Pushes N style colours and returns N; the caller
// balances with ImGui::PopStyleColor(N). `includeFrameBg` adds the three
// FrameBg* variants (for surfaces carrying combos/inputs, e.g. the Quickbar
// category bar); button-only callers pass false.
int PushHighContrastButtonStyles(bool includeFrameBg);

// Warm-red styling for destructive buttons (Clear catalog, per-row Delete).
// Pushes the three Button* colours; with includeText, also a light-red Text.
// Returns the count pushed; balance with ImGui::PopStyleColor(N).
int PushDestructiveButtonStyles(bool includeText);

// --- Validation styling (red FrameBg + drawn border + caller tooltip) ---
// Used by every input where we want to flag a duplicate value, in both
// the Options emote editor and the main/Options category text boxes.
void PushInvalidInputStyle();
void PopInvalidInputStyle();
// Call right after the InputText (before any SameLine) — reads the last
// item's rect to draw a 2px red border that survives FrameBorderSize=0 themes.
void DrawInvalidInputBorder();
