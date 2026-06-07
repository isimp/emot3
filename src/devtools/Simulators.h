#pragma once
// =====================================================================
//  Dev-only "Simulators" window - hosts the prod-path testers (the
//  new-bundled-emote notifier + the update-check flow) as collapsing
//  sections in one window, instead of two separate one-off overlays.
//
//  Gated by EMOT3_DEVTOOLS like the rest of the dev tools; compiled away
//  in the public emot3.dll (Distribution) and emot3_plus.dll (Plus). The
//  section bodies live in NotifierDebug.cpp / UpdateCheckDebug.cpp and
//  drive the real production paths (not mocks).
//
//  Touch points: the RegisterDevTool entry in DevTools.cpp wires the
//  toggle + render; RenderSimulators is implemented in Simulators.cpp.
// =====================================================================

#ifndef EMOT3_DEVTOOLS

namespace simdbg { inline bool& Enabled() { static bool b = false; return b; } }
inline void RenderSimulators() {}

#else  // ---- dev build ----

namespace simdbg { inline bool& Enabled() { static bool b = false; return b; } }

// Draw the Simulators window when enabled. Implemented in Simulators.cpp.
void RenderSimulators();

#endif  // EMOT3_DEVTOOLS
