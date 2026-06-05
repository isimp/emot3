#pragma once

// Submitting an emote to the game chat and toggling the user-managed
// unlock state. The Send path runs on a detached worker thread because
// it sleeps between Win32 message sends — calling it from the render
// thread would stall the addon for ~half a second per click.

#include <string>

struct Emote;

// Inject the slash command for `e` into the game's chat box.
//   useTarget — appends " @" when the emote is targetable (caller decides;
//               the function double-checks IsTargetable defensively).
//   useSync   — appends " *" unconditionally.
// Whether the input is auto-submitted (Enter at the end) or left for the
// user to finish is controlled by g_Settings.SendOnClick.
void SendOrFillEmote(const Emote& e, bool useTarget, bool useSync);

// A *transient* reason the send would be refused RIGHT NOW that can clear on its
// own: a GW2 text box is focused (Typing) or a printable key is held (KeysHeld).
enum class SendBusy { None, Typing, KeysHeld };

// Detect the current transient busy state. checkHeldKeys gates the held-key probe
// (dev swallow mode passes false; see EmoteSendSwallowActive). This single
// detector is shared by ShouldSkipEmoteSend (the click-time refusal) and the
// Quickbar's optional "grey while typing or moving" so the two can't diverge;
// each maps the result to its own wording (the gate's "Emote skipped ..." toast
// vs the greyed cell's present-tense "Can't send while ..."). Cheap, but does a
// short GetAsyncKeyState scan when checkHeldKeys is true.
SendBusy CurrentSendBusy(bool checkHeldKeys);

// True when dev "swallow input on emote send" mode is active (it consumes held
// keys during injection instead of refusing). Always false in base builds.
// Callers gate the held-key checks on its negation.
bool EmoteSendSwallowActive();

// --- Manual unlock state -----------------------------------------------
// The GW2 API can't reliably tell us which emotes the user has unlocked
// so the unlock list is curated by the user via the right-click menu.
// Default (core) emotes are always considered unlocked. Keyed by the
// stable emote Id (ManuallyUnlocked stores Ids).
bool IsEmoteUnlocked(const std::string& id);
void MarkEmoteUnlocked(const std::string& id);
// Marking an emote locked also evicts it from any favorites category so
// the user doesn't end up with unusable buttons in their favorites lists.
void MarkEmoteLocked(const std::string& id);
