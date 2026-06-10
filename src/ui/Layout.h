#pragma once

// Small, generic UI primitives reused across the main panel, the Quickbar,
// and the Options tabs. Pure ImGui — no project-specific state.

#include <string>

#include "imgui/imgui.h"

// Right-align the cursor on the current line so that content of total width
// `width` ends flush with the content region's right edge. No-op when the row
// is narrower than `width`. RightAlignButtons is the common case: `count`
// equal-width buttons of width `w` with the standard ItemSpacing between them.
void RightAlignCursor(float width);
void RightAlignButtons(float w, int count);

// Disabled-control scope. This vendored ImGui predates ImGui::BeginDisabled(),
// so we use the PushItemFlag(Disabled) + alpha-dim pattern the addon standardised
// on. Push BEFORE the control and pop AFTER; never re-evaluate the disabled
// condition between push and pop (that unbalances the style/flag stack -> assert).
void BeginDisabledCompat();
void EndDisabledCompat();

// Truncate `name` to a single line that fits within `maxW`, appending ".."
// when truncation happens. Used by the lightweight non-per-cell callers
// (Quickbar category bar labels, the Options icon-source status line). The
// per-emote-cell label path goes through ui/TextCache - keyed memoization on
// emote id - because those run for every visible cell every frame.
std::string Ellipsize(const std::string& name, float maxW);

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
