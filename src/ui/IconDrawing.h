#pragma once

// Small UI-decoration drawing helpers (section-header glyphs, disclosure arrow,
// trash glyph, cell overlays), extracted from Icons.cpp. Pure ImGui drawing
// against the bundled EMOT3_UI_* textures - no dependency on the icon
// resolution / texture-cache machinery that Icons.cpp owns.

#include "imgui/imgui.h"   // ImVec2 / ImU32 / ImDrawList

// Section-header glyphs. Each one prefers the corresponding PNG under
// addons/emot3/icons/ui/ when present; otherwise falls back to the
// hand-drawn version unchanged. Caller-supplied alpha (via `col`) is
// preserved in the texture path so dimming still works.
void DrawStarIcon(ImVec2 c, float r, ImU32 col);
void DrawPaperclipIcon(ImVec2 c, float r, ImU32 col);

// Disclosure triangle for collapsible section headers: right-pointing when
// collapsed, down-pointing when expanded. Drawn directly (no texture) since the
// bundled font has no disclosure glyph. `r` is the half-extent around center `c`.
void DrawCollapseArrow(ImVec2 c, float r, bool collapsed, ImU32 col);

// Trash-can glyph for the "drop to remove from favorites" zone. Drawn directly
// (line art, no texture / no font glyph). `r` is the half-extent around `c`. Pass
// a draw list to target it (e.g. the foreground list for an always-on-top
// overlay); null uses the current window's draw list.
void DrawTrashIcon(ImVec2 c, float r, ImU32 col, ImDrawList* dl = nullptr);

// Cell overlays.
//   DrawLockOverlay: centered over the last-rendered item; opaque enough
//     to stay readable when the cell itself is dimmed.
//   DrawTargetableDot: small upper-right corner indicator. dotSz is the
//     caller-computed diameter (kept consistent across view modes); alpha is
//     caller-supplied so the dot dims with the cell.
//   DrawMeMoteIndicator: small upper-right corner accent marking a /me-mote
//     cell. Sources the bundled (or icons/ui/-overridden) "me_mote_dot.png"
//     UI texture. Shares the same anchor as DrawTargetableDot — they never
//     co-occur (/me-mote cells are never IsTargetable). indSz is the
//     diameter in pixels (sized like the target dot so it tracks the
//     icon-scale slider).
void DrawLockOverlay();
void DrawTargetableDot(float dotSz, float alphaMul);
void DrawMeMoteIndicator(float indSz, float alphaMul);
