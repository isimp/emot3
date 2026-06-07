#pragma once
// =====================================================================
//  Dev-only tester for the update flow. A dev tool, registered into the
//  dev-tools framework (DevTools.h / RegisterBuiltinDevTools). Gated by
//  EMOT3_DEVTOOLS like the other tools - present in DevTools / PlusDevTools /
//  Debug, absent from the public emot3.dll / emot3_plus.dll.
//
//  Flavor-aware (see UpdateCheckDebug.cpp): in a +plus dev build
//  (PlusDevTools/Debug, EMOT3_PLUS) it drives the CUSTOM update check
//  (plus/UpdateCheck: run / force / reset, lighting the icon badge + Options
//  banner + shortcut item); in the base dev build (DevTools, no EMOT3_PLUS) it
//  exposes the NEXUS-native update path (RequestUpdate).
// =====================================================================

#ifdef EMOT3_DEVTOOLS

// Draw the update-flow tester CONTENT only - no window, no enable gate; hosted
// as a collapsing section inside the Simulators window. Implemented in
// UpdateCheckDebug.cpp.
void RenderUpdateCheckBody();

#endif  // EMOT3_DEVTOOLS
