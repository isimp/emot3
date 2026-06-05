#pragma once
// =====================================================================
//  Plus "update available" check (Plus build only).
//
//  The public emot3.dll auto-updates via Nexus' GitHub provider, so Nexus
//  shows its own update hint. The Plus build is Provider=None (manual, never
//  auto-updated/clobbered - see entry.cpp GetAddonDef), so Nexus never tells
//  the user a newer Plus exists. This module does a lightweight once-per-
//  session version check against the GitHub releases API and, when a newer
//  release exists, badges the Nexus shortcut icon (QuickAccess.Notify) and
//  surfaces a context-menu item + Options banner pointing at the releases
//  page. It NEVER downloads or replaces anything - the update stays a manual
//  opt-in.
//
//  Everything compiles to inert stubs when EMOT3_PLUS is not defined, so the
//  call sites (entry.cpp, NexusShortcut.cpp, OptionsGeneral.cpp) stay
//  unconditional and no HTTP / shell code is linked into non-Plus builds.
// =====================================================================

#include <string>

#ifndef EMOT3_PLUS

inline void        InitUpdateCheck()    {}
inline void        DrainUpdateCheck()   {}
inline bool        PlusUpdateAvailable(){ return false; }
inline std::string PlusLatestVersion()  { return std::string(); }
inline void        OpenReleasesPage()   {}

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

// Open the GitHub releases page in the default browser (manual download).
void OpenReleasesPage();

#endif  // EMOT3_PLUS
