#include "EmoteAction.h"
#include "Globals.h"
#include "I18n.h"
#include "Logging.h"
#include "Settings.h"
#include "SaveScheduler.h"  // RequestSave (debounced, off-thread settings writes)
#include "EmoteData.h"
#include "MeMotes.h"        // /me-mote struct (SendOrFillMeMote)
#include "SendSuppress.h"   // keyboard swallow during emote injection (stub in base builds)
#include "PlusSettings.h"   // g_PlusSettings (whole header empty in base builds)
#include "CharacterState.h" // CurrentEmoteBlock / EmoteBlockKey (mounted + RTAPI states)
#include "AirborneDetect.h" // cs_constants::MoveSpeed (the calibrated moving threshold)
#include "Feedback.h"       // ShowFeedback - in-window refusal line (replaces SendAlert)
#include "Usage.h"          // usage::Record - feeds the Recently/Frequently used categories
#include "Profiling.h"      // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS) - "send" path

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>

namespace {

// ---- Held-key tracking (movement / garble gate) -----------------------
// A printable key held down auto-repeats WM_CHAR, which interleaves with our
// injected command and garbles it - so the send is refused (and the Quickbar
// greys) while one is held. Rather than poll ~37 keys every frame with
// GetAsyncKeyState (a server round-trip each under Wine/Proton), we keep a live
// count fed from the WndProc. s_vkHeld is written only on the WndProc thread;
// s_heldPrintable is atomic for the render/gate thread to read.
inline bool IsPrintableVk(unsigned vk) {
    // Matches the old scan exactly: A-Z, 0-9, space. Modifiers, F-keys, arrows,
    // etc. are intentionally excluded (they don't type into the command).
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == VK_SPACE;
}
bool             s_vkHeld[256] = {};
std::atomic<int> s_heldPrintable{ 0 };

// Click-time guard: refuses an emote send when it can't play right now,
// when the pipeline would garble the injected command, or when it's
// misconfigured. Returns the i18n key to alert with via *outKey when it
// returns true; caller's responsibility to surface it. checkHeldKeys gates
// the held-printable-key check (4): the +plus "send while moving" mode passes
// false because it consumes held keys during injection instead of refusing.
//
// Priority: can't-emote state first (mounted/swimming/downed/... - the send is
// a guaranteed no-op, gated on the "grey out unusable" setting; see
// CharacterState). Then the unbound-chat config check (the rest are moot if
// Press(UiChatCommand) is a no-op). Then the in-game-textbox check (would dump
// the emote into the user's half-typed chat message). Then held printable keys
// (would interleave with the WM_CHAR stream and garble the command).
//
// We don't gate on our own ImGui InputText having focus - Press(
// UiChatCommand) immediately steals focus to GW2's chat box, so a
// concurrently-edited search field can't actually receive the
// injection.
bool ShouldSkipEmoteSend(const char** outKey, bool checkHeldKeys, bool ignoreTextbox = false) {
    // 0. Competitive modes (PvP / WvW): a hard compliance lockout. emot3 injects input
    //    to send emotes, so that stays fully disabled in competitive with NO user
    //    override - this sits ABOVE CurrentEmoteBlock()/QuickbarGreyUnusable. GW2's own
    //    MumbleLink IsCompetitive bit; covers every surface (clicks, keybinds, radial).
    if (InCompetitiveMode()) { *outKey = "cells.blocked_competitive"; return true; }

    // 1. Can't-emote game state: GW2 plays no emote while mounted/downed/swimming/
    //    underwater/gliding/flying, so the send is a silent no-op - refuse with a
    //    toast naming the reason. CurrentEmoteBlock() is the single gate shared with
    //    the Quickbar (core/CharacterState), gated on QuickbarGreyUnusable, covering
    //    mounted via MumbleLink and the rest via the optional RealTime API. Extends
    //    the block to *every* send surface - the main panel + the right-click
    //    Send/@/* variants, not just the Quickbar cells. AIRBORNE is held back to
    //    step 5 so the transient "moving" reason outranks it (matches Quickbar.cpp -
    //    "running and clipping airborne" reads clearer as moving).
    const EmoteBlock block = CurrentEmoteBlock();
    if (block != EmoteBlock::None && block != EmoteBlock::Airborne) {
        *outKey = EmoteBlockKey(block);
        return true;
    }

    // 2. Config: GW2's command-chat keybind must be set. Without
    //    it Press(UiChatCommand) is a no-op, chat never opens with
    //    '/', and our WM_CHAR injection appends garbage to whatever
    //    holds focus.
    if (APIDefs && !APIDefs->GameBinds.IsBound(EGameBinds_UiChatCommand)) {
        *outKey = "send.skip.chat_unbound";
        return true;
    }

    // 3 & 4. Transient refusals that can clear on their own: a GW2 textbox is
    //         focused (sending would append /bow to the half-typed message), or a
    //         printable key is held (key-repeat interleaves with our WM_CHAR
    //         stream and garbles the command). Detection is CurrentSendBusy so the
    //         Quickbar's "grey while typing or moving" uses the exact same test -
    //         greying and refusal can't drift apart. The toast uses the action-
    //         tense "Emote skipped ..." wording (the greyed cell uses present
    //         tense - see Quickbar.cpp).
    switch (CurrentSendBusy(checkHeldKeys, ignoreTextbox)) {
        case SendBusy::Typing:   *outKey = "send.skip.typing";    return true;
        case SendBusy::KeysHeld: *outKey = "send.skip.keys_held"; return true;
        case SendBusy::None:     break;
    }

    // 5. Airborne (jump / fall) - the fallback, only when no definite state, chat
    //    config issue, or transient (moving / typing) reason applied, so "moving"
    //    wins over it.
    if (block == EmoteBlock::Airborne) {
        *outKey = EmoteBlockKey(block);
        return true;
    }

    return false;
}

} // namespace

void NoteKeyEvent(unsigned msg, unsigned vk) {
    if (vk >= 256 || !IsPrintableVk(vk)) return;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (!s_vkHeld[vk]) {            // ignore auto-repeat (already held)
            s_vkHeld[vk] = true;
            s_heldPrintable.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        if (s_vkHeld[vk]) {
            s_vkHeld[vk] = false;
            s_heldPrintable.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void ClearHeldKeys() {
    for (bool& h : s_vkHeld) h = false;
    s_heldPrintable.store(0, std::memory_order_relaxed);
}

void ReseedHeldKeys() {
    // One-shot re-sync from the OS physical state, for focus gain: an alt-tab made
    // with a movement key held would otherwise leave us out of sync (we missed the
    // key-down). ~37 GetAsyncKeyState calls, but only on focus changes, not frames.
    int count = 0;
    for (unsigned vk = 0; vk < 256; ++vk) {
        bool held = IsPrintableVk(vk) && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
        s_vkHeld[vk] = held;
        if (held) ++count;
    }
    s_heldPrintable.store(count, std::memory_order_relaxed);
}

bool AnyPrintableKeyHeld() { return s_heldPrintable.load(std::memory_order_relaxed) > 0; }
int  HeldPrintableCount()  { return s_heldPrintable.load(std::memory_order_relaxed); }

bool EmoteSendSwallowActive() {
#ifdef EMOT3_PLUS
    return g_PlusSettings.SwallowInputOnSend;
#else
    return false;  // swallow mode is compiled out of base builds
#endif
}

SendBusy CurrentSendBusy(bool checkHeldKeys, bool ignoreTextbox) {
    // GW2 textbox focused (chat half-typed, mail, TP search, ...). Single bit GW2
    //    maintains in the Mumble Link. ignoreTextbox skips it for the "close chat
    //    on send" path, which will close the box rather than refuse - so the
    //    movement / held-key check below still applies.
    if (!ignoreTextbox && MumbleLink && MumbleLink->Context.IsTextboxFocused)
        return SendBusy::Typing;

    // Moving / a printable key held - both cancel or garble an emote, so both gate
    //    it. A held printable key (WASD, number row, ...) auto-repeats WM_CHAR that
    //    would interleave with our injected command; its count is maintained event-
    //    driven from the WndProc (NoteKeyEvent), a single atomic read - no per-frame
    //    polling, and (unlike a key scan) it covers garble from a key held while
    //    standing still. MovementActive() is the horizontal-velocity signal (from
    //    MumbleLink), which additionally catches MOUSE-walk - movement with no key
    //    down at all - which the game still cancels emotes for. Held-key state only
    //    reflects keys held while GW2 had focus (WndProc clears on focus loss /
    //    reseeds on gain), so no foreground guard is needed. Skipped when
    //    checkHeldKeys is false ("send while moving" mode handles it via the swallow).
    if (checkHeldKeys && (AnyPrintableKeyHeld() || MovementActive()))
        return SendBusy::KeysHeld;

    // Swallow mode (checkHeldKeys = false): held keys and autorun are both
    // HANDLED there (the swallow consumes key-repeats and the chat focus
    // pauses key-movement; the injection worker cancels autorun) - but
    // MOUSE-walk is not interruptible: chat focus doesn't pause it and we
    // can't release the user's physical mouse buttons, so the game would
    // silently eat the emote. Refuse/grey it like refuse mode does. Cost:
    // two GetAsyncKeyState calls, short-circuited behind "actually moving"
    // (so steady frames never pay them - see the no-per-frame-polling rule).
    if (!checkHeldKeys && MovementActive() &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
        return SendBusy::KeysHeld;
    return SendBusy::None;
}

const char* PickQbBlockReason(EmoteBlock block, bool greying, SendBusy busy,
                              bool heldLongEnough) {
    // A DEFINITE game state (mounted / downed / swimming / gliding / ...) wins,
    // regardless of the grey setting. Then the transient refusals - which outrank
    // AIRBORNE on purpose (running and briefly clipping "airborne" reads clearer as
    // "moving"). Airborne is the fallback. The transient + airborne reasons only
    // grey when `greying` is on; a definite block always explains itself.
    if (block != EmoteBlock::None && block != EmoteBlock::Airborne)
        return EmoteBlockKey(block);
    if (!greying) return nullptr;
    if (busy == SendBusy::Typing)                     return "cells.blocked_typing";
    if (busy == SendBusy::KeysHeld && heldLongEnough) return "cells.blocked_moving";
    if (block == EmoteBlock::Airborne)                return "cells.blocked_airborne";
    return nullptr;
}

namespace {

// Poll GW2's "textbox focused" bit (the same MumbleLink signal CurrentSendBusy
// trusts) until it reaches `wantFocused`, or `timeoutMs` elapses. This turns the
// chat open/close transitions from a blind Sleep into an ADAPTIVE wait: correct
// regardless of the platform's Sleep granularity. Windows rounds Sleep up to the
// ~15.6 ms scheduler tick, which gave the blind waits accidental slack; Wine's
// finer Linux timer removes that slack, so a fixed Sleep tuned on Windows can
// under-wait under Proton. Polling the actual signal sidesteps the whole issue
// (and is usually faster than the old fixed wait). Bails immediately on unload.
// If MumbleLink is unavailable, degrades to one blind Sleep(fallbackMs) so the
// behavior matches the pre-adaptive code.
void WaitForTextbox(bool wantFocused, int timeoutMs, int fallbackMs) {
    if (!MumbleLink) { Sleep(fallbackMs); return; }
    for (int waited = 0; waited < timeoutMs; waited += 5) {
        if (g_Unloading.load()) return;
        if ((bool)MumbleLink->Context.IsTextboxFocused == wantFocused) return;
        Sleep(5);
    }
}

// Spawn the detached worker thread that types `cmd` (a full slash command,
// leading '/') into GW2's chat box and optionally presses Enter at the end.
// Both SendOrFillEmote and SendOrFillMeMote feed this — gating + cmd build
// stays at the call site because the rules differ (target/sync suffixes for
// official emotes; "/me " prefix for /me-motes).
//
// Preconditions: caller already ran ShouldSkipEmoteSend, resolved `closeChat`
// (textbox focused + CloseChatOnSend), and decided `swallowMode` from the
// +plus setting. `autoSend` controls whether we press Enter at the end (the
// SendOnClick / MeMoteSendOnClick choice — caller picks the right one).
void InjectChatCommand(std::string cmd, bool autoSend, bool closeChat,
                       bool swallowMode) {
    std::thread([cmd, autoSend, swallowMode, closeChat]() {
        // Bump the inflight-workers counter so AddonUnload can wait
        // briefly for us to drain. Drop on every early return via RAII.
        InflightWorkerScope scope;

        // g_GameHwnd is captured by our WndProc on the first message (entry.cpp),
        // which always fires before any send is possible, so the FindWindowA
        // fallback effectively never runs. It's Wine-safe regardless: GW2 keeps
        // the "ArenaNet_Dx_Window_Class" window class under Proton/Wine.
        HWND hGame = g_GameHwnd
            ? g_GameHwnd
            : FindWindowA("ArenaNet_Dx_Window_Class", nullptr);
        if (!hGame) {
            LOG_WARNING("Could not find GW2 window - emote %s not sent",
                        cmd.c_str());
            return;
        }

        // +plus: swallow the user's keyboard for the whole injection window
        // (RAII - cleaned up on every g_Unloading early-return below) so held
        // keys can't interleave with our WM_CHAR stream. Inert when swallowMode
        // is false (refuse mode) and a no-op stub in base builds. The
        // window is exactly the injection's existing trimmed timings - it adds
        // no time, and SendToGameOnly bypasses our WndProc so our own injected
        // chars are never consumed.
        SendSuppressScope suppress(swallowMode);

        // ---- Autorun interruption (swallow mode only) -----------------------
        // Autorun is a game-side TOGGLE, not a held key: the swallow can't stop
        // it and the chat focus doesn't pause it, so the character keeps moving
        // and the game eats the emote. Cancel it via the remap-proof game bind
        // (same Press/Release pattern as UiChatCommand below). The toggle state
        // is unreadable, so NEVER toggle blindly - only when the movement can't
        // be explained any other way: horizontally moving with no printable key
        // held and no mouse-walk (both mouse buttons would mean the toggle
        // turns autorun ON instead - that case is refused at the gate). Not
        // re-enabled afterwards: resuming movement would cancel the emote.
        // Only reachable in +plus (swallowMode is constant false in base);
        // without swallow mode a moving send never gets this far (gate).
        if (swallowMode && MumbleLink && !g_Unloading.load() &&
            APIDefs->GameBinds.IsBound(EGameBinds_MoveAutoRun)) {
            // Horizontal speed over a 50 ms window, read straight from the
            // MumbleLink shared memory (worker thread - the render-thread
            // detector isn't safely readable from here). Y is up; X/Z ground.
            auto horizSpeed = []() -> float {
                if (!MumbleLink) return 0.f;
                const float x0 = MumbleLink->AvatarPosition.X;
                const float z0 = MumbleLink->AvatarPosition.Z;
                Sleep(50);
                const float dx = MumbleLink->AvatarPosition.X - x0;
                const float dz = MumbleLink->AvatarPosition.Z - z0;
                return std::sqrt(dx * dx + dz * dz) / 0.05f;
            };
            const bool keysHeld  = AnyPrintableKeyHeld();
            const bool mouseWalk = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
                                   (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
            if (!keysHeld && !mouseWalk) {
                const float v0 = horizSpeed();
                if (v0 > cs_constants::MoveSpeed()) {
                    LOG_DEBUG("autorun-cancel: %.2f m/s with no keys/buttons -> MoveAutoRun toggle", v0);
                    APIDefs->GameBinds.Press(EGameBinds_MoveAutoRun);
                    Sleep(60);
                    if (g_Unloading.load()) return;
                    APIDefs->GameBinds.Release(EGameBinds_MoveAutoRun);
                    // Let the character settle before chat opens (each probe
                    // sleeps 50 ms; <=400 ms total, proceed on timeout - the
                    // emote may still land if the game stops us in time).
                    float v = v0;
                    for (int i = 0; i < 8 && v > cs_constants::MoveSpeed(); ++i) {
                        if (g_Unloading.load()) return;
                        v = horizSpeed();
                    }
                    LOG_DEBUG("autorun-cancel: settled at %.2f m/s", v);
                }
            }
        }

        // Bail before each APIDefs deref if Nexus is tearing the addon
        // down (g_Unloading set in AddonUnload). Without this, a worker
        // mid-Sleep here would deref a dangling APIDefs after unload
        // and crash the host.
        //
        // Sleep timings: trimmed from the original conservative
        // 150/100/5/80/15 to 60/40/0/30/10. Two facts drive the trim:
        // (1) Windows' default Sleep granularity is ~15.6ms, so the old
        // per-char Sleep(5) actually cost ~16ms each (dominant for
        // long commands). (2) APIDefs->WndProc.SendToGameOnly is a
        // SYNCHRONOUS WndProc dispatch - Nexus runs the game's window
        // procedure inline and SendToGameOnly only returns once the
        // message has been processed - so no inter-char Sleep is
        // needed for ordering. The chat OPEN and CLOSE transitions are now
        // waited on adaptively (WaitForTextbox polls MumbleLink's textbox-focused
        // bit) instead of slept blindly, so they're correct regardless of Sleep
        // granularity (Windows ~15.6ms vs Wine's finer timer). The remaining fixed
        // Sleeps are the synthetic key holds, a small post-open readiness margin,
        // and the Enter-dispatch settle.
        // "Close chat on send": a text box was focused at click time - close it
        // first (Escape clears the half-typed line and unfocuses) so the command we
        // open + type below lands in a fresh chat instead of the user's message.
        // Injected the same way as the command keys (synchronous SendToGameOnly).
        if (closeChat) {
            if (g_Unloading.load()) return;
            const DWORD  escScan = MapVirtualKey(VK_ESCAPE, MAPVK_VK_TO_VSC);
            const LPARAM escDown = (LPARAM)((escScan << 16) | 1);
            const LPARAM escUp   = escDown | (LPARAM)0xC0000000;
            APIDefs->WndProc.SendToGameOnly(hGame, WM_KEYDOWN, VK_ESCAPE, escDown);
            Sleep(10);
            APIDefs->WndProc.SendToGameOnly(hGame, WM_KEYUP,   VK_ESCAPE, escUp);
            // Wait for the box to actually close before re-opening chat (adaptive;
            // 40 ms blind fallback only if MumbleLink is unavailable).
            WaitForTextbox(/*wantFocused=*/false, /*timeoutMs=*/300, /*fallbackMs=*/40);
        }

        if (g_Unloading.load()) return;
        APIDefs->GameBinds.Press(EGameBinds_UiChatCommand);
        Sleep(60);
        if (g_Unloading.load()) return;
        APIDefs->GameBinds.Release(EGameBinds_UiChatCommand);
        // Wait for chat to actually open (adaptive) instead of a blind Sleep, then
        // a small fixed margin for the box to be ready to accept characters.
        WaitForTextbox(/*wantFocused=*/true, /*timeoutMs=*/300, /*fallbackMs=*/40);
        if (g_Unloading.load()) return;
        Sleep(15);

        // Convert the UTF-8 command to UTF-16 before feeding it to the
        // chat box. The old code sent each UTF-8 *byte* as its own WM_CHAR,
        // which mangled any non-ASCII character - some emote command
        // variants the user may pick carry accents/umlauts (e.g.
        // /héroïque, /grübeln, /reír), whose multi-byte UTF-8 encoding
        // would otherwise arrive as two garbage glyphs. GW2's window is
        // Unicode, so WM_CHAR expects a UTF-16 code unit.
        // We skip index 0 (the leading '/') - chat is already opened with
        // it via the UiChatCommand game bind.
        std::wstring wcmd;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), (int)cmd.size(),
                                       nullptr, 0);
        if (wlen > 0) {
            wcmd.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), (int)cmd.size(),
                                &wcmd[0], wlen);
        }
        for (size_t i = 1; i < wcmd.size(); ++i) {
            if (g_Unloading.load()) return;
            APIDefs->WndProc.SendToGameOnly(hGame, WM_CHAR,
                (WPARAM)wcmd[i], 0);
            // No Sleep here - SendToGameOnly is synchronous (see the
            // comment block above), so the game has processed this
            // char by the time we send the next.
        }

        if (autoSend) {
            Sleep(30);
            if (g_Unloading.load()) return;
            DWORD  scan   = MapVirtualKey(VK_RETURN, MAPVK_VK_TO_VSC);
            LPARAM downLP = (LPARAM)((scan << 16) | 1);
            LPARAM upLP   = downLP | (LPARAM)0xC0000000;
            APIDefs->WndProc.SendToGameOnly(hGame, WM_KEYDOWN, VK_RETURN, downLP);
            Sleep(10);
            if (g_Unloading.load()) return;
            APIDefs->WndProc.SendToGameOnly(hGame, WM_CHAR, (WPARAM)'\r', downLP);
            Sleep(10);
            if (g_Unloading.load()) return;
            APIDefs->WndProc.SendToGameOnly(hGame, WM_KEYUP, VK_RETURN, upLP);
        }

        // NOTE - no held-key "resume" after the send, by design (tried and
        // REMOVED after in-game testing): a synthetic fresh WM_KEYDOWN posted
        // to the game does NOT resume movement - movement reads physical key
        // state (raw input / async state), not posted messages, so the game
        // can't be convinced a key is "still held". The shipped semantic is
        // therefore uniform with the autorun-cancel above: a send while
        // moving STOPS the character; the user resumes movement themselves
        // (release + re-press, or re-engage autorun).
    }).detach();
}

// Route a gated-send refusal to the right surface. InWindow keeps the existing
// in-window overlay (a click while the panel is open); Alert uses Nexus' SendAlert
// (a bind / RadialMenus wheel Invoke, where emot3's window is usually closed so the
// overlay wouldn't be seen). Falls back to ShowFeedback if SendAlert is unavailable.
static void EmitSendRefusal(const char* key, EFeedbackSink sink) {
    if (sink == EFeedbackSink::Alert && APIDefs && APIDefs->UI.SendAlert)
        APIDefs->UI.SendAlert(L(key));
    else
        ShowFeedback(L(key));
}

// Send-mode override state (see EmoteAction.h). Render-thread only; applied
// where the auto-submit settings are read, i.e. at SendOrFill* entry.
ESendModeOverride s_sendModeOverride = ESendModeOverride::None;

bool ApplySendModeOverride(bool settingValue) {
    if (s_sendModeOverride == ESendModeOverride::Send) return true;
    if (s_sendModeOverride == ESendModeOverride::Fill) return false;
    return settingValue;
}

} // namespace

void SetSendModeOverride(ESendModeOverride m) { s_sendModeOverride = m; }

bool SendOrFillEmote(const Emote& e, bool useTarget, bool useSync, EFeedbackSink sink) {
    PROFILE_SCOPE("send");  // dev perf overlay - render-thread send dispatch (inject runs async)
    if (!APIDefs) return false;
    std::string cmd = e.Command;
    if (useTarget && e.IsTargetable) cmd += " @";
    if (useSync)                     cmd += " *";
    const bool send        = ApplySendModeOverride(g_Settings.SendOnClick);
    const bool swallowMode = EmoteSendSwallowActive();
    // The held-printable-key / movement guard applies to clicks, keybinds, and radial
    // invokes alike (only the +plus swallow drops it): a keybind/wheel-hotkey that's a
    // bare printable key is still held during injection and would garble the command,
    // so we refuse rather than send garbage - behaving like a default click.
    const bool checkHeld   = !swallowMode;

    // "Close chat on send": if a GW2 text box is focused and the setting is on,
    // we'll close it (Escape) in the worker instead of refusing - so tell the gate
    // to ignore the textbox reason (the move / held-key refusal still applies).
    const bool closeChat = g_Settings.CloseChatOnSend &&
                           MumbleLink && MumbleLink->Context.IsTextboxFocused;

    // Refuse rather than garble. The check is at click time only - late typing
    // after the worker spawns still interleaves in refuse mode (swallow mode
    // covers that by consuming input). In swallow mode we drop the held-key
    // check - those are handled by the swallow - but keep the chat-unbound and
    // textbox-focused checks (the refined textbox guard stays).
    const char* skipKey = nullptr;
    if (ShouldSkipEmoteSend(&skipKey, /*checkHeldKeys=*/checkHeld,
                            /*ignoreTextbox=*/closeChat)) {
        LOG_DEBUG("Emote skipped (%s): %s", skipKey, cmd.c_str());
        EmitSendRefusal(skipKey, sink);
        return false;
    }

    // Real send/fill (gate passed) — record for Recently / Frequently used.
    // Locked emotes (non-core + not in ManuallyUnlocked) are excluded: the game
    // won't play them, so they shouldn't seed the usage categories. Checked
    // inline off the already-resolved `e` (no extra catalog scan).
    {
        const bool unlocked = e.IsCore ||
            std::find(g_Settings.ManuallyUnlocked.begin(),
                      g_Settings.ManuallyUnlocked.end(), e.Id)
                != g_Settings.ManuallyUnlocked.end();
        if (unlocked) usage::Record(EFavoriteRefType::Emote, e.Id);
    }

    LOG_DEBUG("%s emote: %s", send ? "Sending" : "Filling", cmd.c_str());
    InjectChatCommand(std::move(cmd), send, closeChat, swallowMode);
    return true;
}

bool SendOrFillMeMote(const MeMote& m, EMeMoteVariant variant, EFeedbackSink sink) {
    PROFILE_SCOPE("send");  // dev perf overlay - render-thread send dispatch (inject runs async)
    if (!APIDefs) return false;

    // Pick the variant body. Fall back to TextDefault when the requested
    // variant is empty (defensive backstop — the right-click menu disables
    // entries with empty bodies, so this path is unreachable through normal
    // UI). TextDefault itself can't be empty in a loaded /me-mote — sanitize
    // drops empty-default entries at load and the editor blocks save on it.
    const std::string* body = &m.TextDefault;
    switch (variant) {
        case EMeMoteVariant::You: if (!m.TextYou.empty()) body = &m.TextYou; break;
        case EMeMoteVariant::All: if (!m.TextAll.empty()) body = &m.TextAll; break;
        case EMeMoteVariant::Default: break;
    }
    if (body->empty()) {
        LOG_WARNING("/me-mote %s has empty body - send refused", m.Id.c_str());
        return false;
    }

    // Build "/me <text>". The "/me " prefix is owned exclusively here — never
    // stored in TextDefault / TextYou / TextAll (sanitize strips it at load),
    // so we can't ship a doubled prefix.
    std::string cmd = "/me " + *body;

    const bool send = ApplySendModeOverride(g_Settings.MeMoteSendOnClick);  // independent of SendOnClick
    const bool swallowMode = EmoteSendSwallowActive();
    const bool checkHeld   = !swallowMode;  // see SendOrFillEmote (gate applies to binds too)

    const bool closeChat = g_Settings.CloseChatOnSend &&
                           MumbleLink && MumbleLink->Context.IsTextboxFocused;

    // Same gating as official emotes — can't-emote state / chat unbound /
    // textbox focused / held printable keys / airborne. The gates aren't
    // specific to slash-command emotes; they protect ANY chat injection from
    // garbling.
    const char* skipKey = nullptr;
    if (ShouldSkipEmoteSend(&skipKey, /*checkHeldKeys=*/checkHeld,
                            /*ignoreTextbox=*/closeChat)) {
        LOG_DEBUG("/me-mote skipped (%s): %s", skipKey, cmd.c_str());
        EmitSendRefusal(skipKey, sink);
        return false;
    }

    // Real send/fill (gate passed) — record for Recently / Frequently used.
    usage::Record(EFavoriteRefType::MeMote, m.Id);

    LOG_DEBUG("%s /me-mote: %s", send ? "Sending" : "Filling", cmd.c_str());
    InjectChatCommand(std::move(cmd), send, closeChat, swallowMode);
    return true;
}

bool IsEmoteUnlocked(const std::string& id) {
    // Core emotes are always unlocked regardless of the manual list —
    // covers core entries the user couldn't mark locked anyway and saves
    // every caller from having to chain `e.IsCore || IsEmoteUnlocked(...)`.
    const Emote* e = FindEmote(id);
    if (e && e->IsCore) return true;
    return std::find(g_Settings.ManuallyUnlocked.begin(),
                     g_Settings.ManuallyUnlocked.end(),
                     id) != g_Settings.ManuallyUnlocked.end();
}

void MarkEmoteUnlocked(const std::string& id) {
    if (IsEmoteUnlocked(id)) return;
    g_Settings.ManuallyUnlocked.push_back(id);
    LOG_DEBUG("Marked %s as unlocked", id.c_str());
    RequestSave(SaveKind::Settings);
}

void MarkEmoteLocked(const std::string& id) {
    auto& u = g_Settings.ManuallyUnlocked;
    bool wasUnlocked = std::find(u.begin(), u.end(), id) != u.end();
    u.erase(std::remove(u.begin(), u.end(), id), u.end());
    // Evict only Emote-typed refs matching `id` — a /me-mote that happens to
    // share the Id stays put.
    int evicted = 0;
    for (auto& cat : g_Settings.FavoriteCategories) {
        size_t before = cat.Refs.size();
        cat.Refs.erase(std::remove_if(cat.Refs.begin(), cat.Refs.end(),
                                      [&](const FavoriteRef& r) {
                                          return r.Type == EFavoriteRefType::Emote && r.Id == id;
                                      }),
                       cat.Refs.end());
        evicted += (int)(before - cat.Refs.size());
    }
    if (wasUnlocked || evicted > 0) {
        LOG_DEBUG("Marked %s as locked (evicted from %d favorite slot(s))",
                  id.c_str(), evicted);
        RequestSave(SaveKind::Settings);
    }
}
