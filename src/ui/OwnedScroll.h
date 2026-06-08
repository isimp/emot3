#pragma once

// The Quickbar's owned custom-scroll subsystem, extracted from Quickbar.cpp:
// the replacement scrollbar drawn in fit mode (ImGui's own scrolling is turned
// off so it can't spawn the glitchy auto-bar), the wheel/page scroll math, and
// the purely-visual scroll-edge hints. Quickbar still owns the geometry globals
// (g_Qb*) these read; this is only the scroll behaviour over them.

#include "imgui/imgui.h"   // ImVec2 / ImDrawList

struct ImGuiWindow;        // from imgui_internal.h; used here only by pointer

// One wheel notch -> new scroll offset on an axis (Cells / Off path): applies
// the per-mode step, optional cell snap, clamp, and edge-triggered wrap.
float WheelScroll(float cur, float wheel, float step, float snapStep,
                  float maxS, bool snap, bool wrap);

// One wheel notch -> new scroll offset, Pages mode: steps whole viewports and
// treats maxS as a snap target so the last partial page stays reachable.
float PageWheelScroll(float cur, float wheel, float pageStep,
                      float maxS, bool wrap);

// Draw + handle the replacement scrollbar in the reserved gutter. Hands back the
// gutter hit-rect in outMin/outMax (the caller registers it in g_QbIconRects for
// click-through). Only call when the content actually overflows.
void QbCustomScrollbar(ImGuiWindow* child, bool axisY,
                       float scroll, float scrollMax, float viewport,
                       ImVec2 childPos, ImVec2 childSize,
                       float gutter, bool flatten,
                       ImVec2& outMin, ImVec2& outMax);

// Purely visual start/end scroll-edge hints, drawn into the grid child's draw
// list after EndChild. No layout reservation, no input.
void QbScrollEdgeHints(ImDrawList* dl, bool axisY, float scroll, float scrollMax,
                       ImVec2 childPos, ImVec2 childSize,
                       float gutter, bool flatten, bool highContrast,
                       bool wrap, bool bordered);
