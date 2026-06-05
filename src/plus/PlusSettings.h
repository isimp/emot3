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
};

extern PlusSettings g_PlusSettings;

// Parse plus.json (tolerant: missing file / keys -> struct defaults) and stash
// the path for SavePlusSettings. Call once at AddonLoad.
void LoadPlusSettings(const std::string& path);
// Write the current g_PlusSettings back to the stashed path (on toggle change).
void SavePlusSettings();

#endif  // EMOT3_PLUS
