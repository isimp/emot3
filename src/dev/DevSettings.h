#pragma once
// =====================================================================
//  Dev-only settings, persisted to addons/emot3/dev.json.
//
//  Deliberately separate from settings.json: these toggle dev-only
//  behaviour that is compiled in only for the +plus flavor (EMOT3_PLUS),
//  so keeping them in their own file means a base build never reads
//  or writes them. settings.json stays clean for end users, and a dev's
//  values are never dropped by running a base build (which doesn't
//  touch dev.json at all). The whole header is empty in base builds.
// =====================================================================
#ifdef EMOT3_PLUS

#include <string>

struct DevSettings {
    // Route the mouse wheel to the Quickbar under click-through (consume
    // WM_MOUSEWHEEL in the WndProc so the game camera doesn't also zoom).
    // The input-swallow this enables is the dist-stripped bit - see
    // QuickbarWheel.h. Default on in dev builds.
    bool QbClickThroughWheel = true;

    // Swallow the user's keyboard during emote injection (the old "click
    // whenever" mode) instead of refusing the send when a printable key is
    // held. Consumes WM_KEY*/WM_CHAR in the WndProc - the AV-flagged bit -
    // so it's dist-stripped (see SendSuppress.h). Default OFF: the safe
    // refuse-when-unsafe gate (ShouldSkipEmoteSend) that ships in releases.
    bool SwallowInputOnSend = false;
};

extern DevSettings g_DevSettings;

// Parse dev.json (tolerant: missing file / keys -> struct defaults) and stash
// the path for SaveDevSettings. Call once at AddonLoad.
void LoadDevSettings(const std::string& path);
// Write the current g_DevSettings back to the stashed path (on toggle change).
void SaveDevSettings();

#endif  // EMOT3_PLUS
