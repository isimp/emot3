#pragma once

// Submitting an emote to the game chat and toggling the user-managed
// unlock state. The Send path runs on a detached worker thread because
// it sleeps between Win32 message sends — calling it from the render
// thread would stall the addon for ~half a second per click.

#include <string>

struct Emote;
struct MeMote;
enum class EmoteBlock;   // core/CharacterState.h (PickQbBlockReason param)

// Where a refusal message goes when a send is gated. A click in an emot3 surface
// keeps the in-window ShowFeedback overlay (the window is open, the user is looking
// at it). A bind / RadialMenus-wheel Invoke uses Nexus' SendAlert instead: emot3's
// window is usually closed at that point, so the in-window overlay wouldn't show -
// and SendAlert is exactly what RadialMenus itself uses for its own cancels.
enum class EFeedbackSink { InWindow, Alert };

// Inject the slash command for `e` into the game's chat box.
//   useTarget — appends " @" when the emote is targetable (caller decides;
//               the function double-checks IsTargetable defensively).
//   useSync   — appends " *" unconditionally.
//   sink      — where a gated-send refusal is shown (in-window overlay for a click;
//               Nexus SendAlert for a keybind / radial Invoke, since the window is
//               usually closed then).
// The held-printable-key / movement guard ALWAYS applies (unless the +plus input
// swallow is active): a keybind or radial invoke fires only when no bare printable
// key is held, exactly like a panel click - holding the trigger key through the
// injection would garble the command, so we refuse instead.
// Whether the input is auto-submitted (Enter at the end) or left for the
// user to finish is controlled by g_Settings.SendOnClick.
void SendOrFillEmote(const Emote& e, bool useTarget, bool useSync,
                     EFeedbackSink sink = EFeedbackSink::InWindow);

// EMeMoteVariant (Default / You / All) is defined in data/MeMotes.h (its
// /me-mote domain home, now that MeMote stores a KeybindVariant of this type).
// Opaque forward-declaration here is enough for the SendOrFillMeMote
// declaration below; every TU that names an enumerator already includes
// MeMotes.h (it deals with MeMote anyway).
enum class EMeMoteVariant;

// Inject "/me <text>" into the game's chat box, where <text> is the body
// selected by `variant`. If the selected variant's text is empty, falls back
// to TextDefault rather than refusing (the right-click menu disables the
// entries with empty bodies, so this is a defensive backstop). Whether the
// input is auto-submitted is controlled by g_Settings.MeMoteSendOnClick
// (independent of the official-emote SendOnClick).
//
// GW2's @ token doesn't substitute inside /me text and its * sync token also
// doesn't work — verified in-game — so the /me-mote send path is targetless
// and unsync'd by construction. The held-key/movement guard applies as in
// SendOrFillEmote; `sink` routes a refusal to the overlay or SendAlert.
void SendOrFillMeMote(const MeMote& m, EMeMoteVariant variant,
                      EFeedbackSink sink = EFeedbackSink::InWindow);

// A *transient* reason the send would be refused RIGHT NOW that can clear on its
// own: a GW2 text box is focused (Typing) or a printable key is held (KeysHeld).
enum class SendBusy { None, Typing, KeysHeld };

// Detect the current transient busy state. checkHeldKeys gates the held-key probe
// ("send while moving" passes false; see EmoteSendSwallowActive). This single
// detector is shared by ShouldSkipEmoteSend (the click-time refusal) and the
// Quickbar's optional "grey while typing or moving" so the two can't diverge;
// each maps the result to its own wording (the gate's "Emote skipped ..." toast
// vs the greyed cell's present-tense "Can't send while ..."). Cheap: textbox is a
// MumbleLink bit and the held-key check is a single atomic read (see below).
// ignoreTextbox skips the textbox-focused refusal (the "close chat on send" path
// closes the box instead, but the movement / held-key refusal still applies).
SendBusy CurrentSendBusy(bool checkHeldKeys, bool ignoreTextbox = false);

// Compose the Quickbar's "why this emote is unusable" cell-explainer key from the
// current game-state block + transient send-busy state. Pure: the caller (Quickbar)
// owns the move-hold debounce and passes the resolved `heldLongEnough`. Priority
// mirrors ShouldSkipEmoteSend (definite game state > transient typing/moving >
// airborne), mapped to the present-tense cells.blocked_* keys. Airborne and the
// transient reasons only apply when `greying` is on; a definite block always wins.
// Returns nullptr when the emote is usable (no reason to grey).
const char* PickQbBlockReason(EmoteBlock block, bool greying, SendBusy busy,
                              bool heldLongEnough);

// --- Held printable-key tracking (the movement/garble half of the gate) -------
// Maintained event-driven from the WndProc instead of polling ~37 keys/frame.
// "Printable" = A-Z, 0-9, space (the keys whose WM_CHAR auto-repeat garbles an
// injected command); modifiers/F-keys/arrows are ignored. Observe-only - the
// WndProc never consumes these messages.
//   NoteKeyEvent   - feed a WM_KEY*/WM_SYSKEY* message (msg + VK) from the WndProc.
//   ClearHeldKeys  - drop all held state (focus loss: no key-ups will arrive).
//   ReseedHeldKeys - re-sync once from GetAsyncKeyState (focus gain).
//   AnyPrintableKeyHeld / HeldPrintableCount - the live state the gate/readout read.
void NoteKeyEvent(unsigned msg, unsigned vk);
void ClearHeldKeys();
void ReseedHeldKeys();
bool AnyPrintableKeyHeld();
int  HeldPrintableCount();

// True when the +plus "send while moving" mode is active (it consumes held
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
