#pragma once
// =====================================================================
//  Custom "update available" check for the +plus flavor.
//
//  The public emot3.dll (base/Distribution) auto-updates via Nexus' GitHub
//  provider, so Nexus shows its own hint. The +plus builds are Provider=None
//  (manual, never auto-updated/clobbered - see entry.cpp GetAddonDef), so Nexus
//  never tells the user a newer version exists. This module does a lightweight
//  once-per-session version check against the GitHub releases API and, when a
//  newer release exists, badges the Nexus shortcut icon (QuickAccess.Notify) and
//  surfaces a context-menu item + Options banner pointing at the releases page.
//  It NEVER downloads or replaces anything - the update stays a manual opt-in.
//
//  GATE: active for the +plus flavor (EMOT3_PLUS = Plus / PlusDevTools / Debug).
//  Base builds - including the +devtools-only DevTools config - compile inert
//  stubs and rely on Nexus' native updater instead. The "[debug] Update check"
//  tool drives that native path in DevTools, and this real check in the +plus
//  dev builds (PlusDevTools/Debug).
//
//  Stubs keep the call sites (entry.cpp, NexusShortcut.cpp, OptionsGeneral.cpp)
//  unconditional, and no HTTP / shell code links into base builds.
// =====================================================================

#include <string>

#ifndef EMOT3_PLUS

inline void        InitUpdateCheck()    {}
inline void        DrainUpdateCheck()   {}
inline bool        PlusUpdateAvailable(){ return false; }
inline std::string PlusLatestVersion()  { return std::string(); }
inline const char* ReleasesUrl()        { return ""; }

#else

// Reset session state (reload-safe) + arm the once-per-session check. Call from
// AddonLoad.
void InitUpdateCheck();

// Render-thread pump: call from AddonRender + AddonOptions (like DrainUnlockSync).
// Launches the check worker once ~a few seconds after load and, when it reports a
// newer release, badges the shortcut icon once via QuickAccess.Notify.
void DrainUpdateCheck();

// True once a GitHub release newer than this build's AddonDef.Version was found.
bool PlusUpdateAvailable();

// The newer version string (e.g. "1.0.1"), or empty until/unless one is found.
// Returned by value (the worker writes it on another thread).
std::string PlusLatestVersion();

// The GitHub releases URL (for display + clipboard copy). Callers paste it via
// ImGui::SetClipboardText - we deliberately don't ShellExecute a browser, which
// would alt-tab (and can minimize an exclusive-fullscreen client).
const char* ReleasesUrl();

#ifdef EMOT3_DEVTOOLS
// Dev-tool hooks (only the "[debug] Update check" tool calls these; present in
// the +plus dev builds = PlusDevTools/Debug). Let the tool exercise the real
// pipeline.
void RunUpdateCheckNow();                          // launch the worker now (bypass delay/once)
void ForceUpdateAvailable(const std::string& ver); // set available + version, no network
void ResetUpdateCheck();                           // clear all state (= InitUpdateCheck)
#endif

#endif  // EMOT3_PLUS
