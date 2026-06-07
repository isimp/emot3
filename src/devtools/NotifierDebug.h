#pragma once
// =====================================================================
//  Dev-only tester for the new-bundled-emote notifier. A dev tool,
//  registered into the dev-tools framework (DevTools.h /
//  RegisterBuiltinDevTools).
//
//  Same gating as Profiling.h / QuickbarDebug.h: present ONLY when
//  EMOT3_DEVTOOLS is defined (the Debug and Dev configs); compiled away
//  in the public emot3.dll (Distribution) and emot3_plus.dll (Plus)
//  builds.
//
//  The production notifier only fires on a real bundle-vs-snapshot diff
//  across a version update, so it's otherwise untestable without bumping
//  the addon version + shipping new emotes. This tool drives the exact
//  same helpers (AllBundledEmoteIds / ComputeNewBundledEmotes) and globals
//  (g_PromptNewBundledEmotes / g_NewBundledEmoteIds) the real path uses,
//  so it exercises the actual code rather than a parallel mock.
//
//  Touch points: #include "NotifierDebug.h"; the body is drawn as a section of
//  the merged Simulators window (Simulators.cpp). The render body lives in
//  NotifierDebug.cpp (it touches Settings/EmoteData/Globals).
// =====================================================================

#ifdef EMOT3_DEVTOOLS

// Draw the notifier-tester CONTENT only - no window, no enable gate; hosted as
// a collapsing section inside the Simulators window. Implemented in
// NotifierDebug.cpp.
void RenderNotifierBody();

#endif  // EMOT3_DEVTOOLS
