#pragma once
// =====================================================================
//  Dev-only click-through wheel routing.
//
//  Under click-through the Quickbar renders NoInputs (clicks fall to the
//  game), so ImGui won't capture the mouse - a wheel over the QB would
//  scroll the list AND zoom the game camera. To stop the double-action we
//  CONSUME WM_MOUSEWHEEL in the addon WndProc (return 0) and route the
//  delta to the render.
//
//  Returning 0 to swallow input is the pattern that has twice tripped
//  Microsoft Defender's ML heuristic (Wacatac.B!ml from a former
//  keystroke-swallow, C!ml from this wheel-swallow), so the whole thing
//  is compiled in only for the +plus flavor (EMOT3_PLUS) - exactly like the
//  dev overlays (Profiling.h / QuickbarDebug.h). In base builds these are
//  no-op stubs and the Quickbar falls back to its original grace-period
//  click-through scrolling (no input swallow). Gated at runtime by the
//  persisted dev setting g_DevSettings.QbClickThroughWheel (DevSettings.h).
//
//  Touch points: the guarded call in entry.cpp's WndProc; the
//  QbWheelPublish / QbWheelDrain calls in Quickbar.cpp; the dev toggle in
//  Options.cpp.
// =====================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// WndProc: returns true if it consumed this WM_MOUSEWHEEL (caller returns 0 to
// keep the game from also scrolling/zooming). Always false in base builds.
bool  QbWheelConsume(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Render (end of frame): publish the screen/client-space rect the wheel should
// be captured over. wantWheel=false - or the dev toggle off - disables capture.
// No-op in base builds.
void  QbWheelPublish(bool wantWheel, float x, float y, float w, float h);

// Render: drain the accumulated wheel delta in whole notches (sign matches
// io.MouseWheel; positive = up). Returns 0 in base builds / when nothing was captured.
float QbWheelDrain();
