#pragma once
// =====================================================================
//  Plus settings, persisted to addons/emot3/plus.json.
//
//  The two input-swallow conveniences added by the +plus flavor have
//  user-facing toggles (Quickbar > wheel routing, General > Sending). They
//  live in their own plus.json, deliberately separate from settings.json:
//  the +plus features are compiled in only for EMOT3_PLUS builds, so keeping
//  their toggles in a separate file means a base build never reads or writes
//  them - settings.json stays clean for everyone, and a +plus user's values
//  are never dropped by running a base build (which doesn't touch plus.json).
//  The whole header is empty in base builds.
// =====================================================================
#ifdef EMOT3_PLUS

#include <string>

struct PlusSettings {
    // Route the mouse wheel to the Quickbar under click-through (consume
    // WM_MOUSEWHEEL in the WndProc so the game camera doesn't also zoom).
    // The input-swallow this enables is a +plus feature - see QuickbarWheel.h.
    // Default on.
    bool QbClickThroughWheel = true;

    // Swallow the user's keyboard during emote injection ("send while moving")
    // instead of refusing the send when a printable key is held. Consumes
    // WM_KEY*/WM_CHAR in the WndProc - the AV-sensitive bit - so it's a +plus
    // feature (see SendSuppress.h). Default OFF: the safe refuse-when-unsafe
    // gate (ShouldSkipEmoteSend) that ships in the base build.
    bool SwallowInputOnSend = false;

    // Let the Plus update notifier consider preview / beta builds (prereleases),
    // not just stable releases. The base build gets this from Nexus' own per-addon
    // "AllowPrereleases" toggle; Plus is Provider=None, which Nexus neither
    // auto-updates nor exposes that toggle for, so this is Plus' equivalent. When
    // on, UpdateCheck queries /releases (incl. prereleases) instead of
    // /releases/latest, so a beta tester is badged when a newer beta ships.
    // Default OFF - stable users are never nudged toward a beta. See UpdateCheck.h.
    bool NotifyPrereleases = false;
};

extern PlusSettings g_PlusSettings;

// Parse plus.json (tolerant: missing file / keys -> struct defaults) and stash
// the path for SavePlusSettings. Call once at AddonLoad.
void LoadPlusSettings(const std::string& path);
// Write the current g_PlusSettings back to the stashed path (on toggle change).
void SavePlusSettings();

#endif  // EMOT3_PLUS
