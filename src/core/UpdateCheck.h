#pragma once
// =====================================================================
//  Custom "update available" check for the plus-flavored builds.
//
//  The public emot3.dll (Distribution) auto-updates via Nexus' GitHub provider,
//  so Nexus shows its own hint. The plus-flavored builds are Provider=None
//  (manual, never auto-updated/clobbered - see entry.cpp GetAddonDef), so Nexus
//  never tells the user a newer version exists. This module does a lightweight
//  once-per-session version check against the GitHub releases API and, when a
//  newer release exists, badges the Nexus shortcut icon (QuickAccess.Notify) and
//  surfaces a context-menu item + Options banner pointing at the releases page.
//  It NEVER downloads or replaces anything - the update stays a manual opt-in.
//
//  GATE (provisional): active for "plus-flavored" builds = the input-swallow ones
//  (Plus + Dev + Debug), expressed here as `!defined(EMOT3_DIST)`. The public
//  Distribution build (EMOT3_DIST) compiles inert stubs and relies on Nexus'
//  native updater instead. Re-gated off the swallow axis (was EMOT3_PLUS) so the
//  plus-flavored *dev* build (Dev) gets it too and the "[debug] Update check"
//  tool can drive it. The `!EMOT3_DIST` double-negative + the `Plus*` function
//  names are intentionally provisional - the dedicated config/macro naming-cleanup
//  PR replaces them with a positive flavor macro + neutral names.
//
//  Stubs keep the call sites (entry.cpp, NexusShortcut.cpp, OptionsGeneral.cpp)
//  unconditional, and no HTTP / shell code links into the Distribution build.
// =====================================================================

#include <string>

#ifdef EMOT3_DIST

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
// plus-flavored dev builds = Dev/Debug). Let the tool exercise the real pipeline.
void RunUpdateCheckNow();                          // launch the worker now (bypass delay/once)
void ForceUpdateAvailable(const std::string& ver); // set available + version, no network
void ResetUpdateCheck();                           // clear all state (= InitUpdateCheck)
#endif

#endif  // EMOT3_DIST
