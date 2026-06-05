#include "EmoteAction.h"
#include "Globals.h"
#include "I18n.h"
#include "Logging.h"
#include "Settings.h"
#include "EmoteData.h"
#include "SendSuppress.h"   // keyboard swallow during emote injection (stub in base builds)
#include "DevSettings.h"    // g_DevSettings (whole header empty in base builds)
#include "CharacterState.h" // CurrentEmoteBlock / EmoteBlockKey (mounted + RTAPI states)
#include "Feedback.h"       // ShowFeedback - in-window refusal line (replaces SendAlert)

#include <Windows.h>
#include <algorithm>
#include <string>
#include <thread>

namespace {

// Click-time guard: refuses an emote send when it can't play right now,
// when the pipeline would garble the injected command, or when it's
// misconfigured. Returns the i18n key to alert with via *outKey when it
// returns true; caller's responsibility to surface it. checkHeldKeys gates
// the held-printable-key check (4): the dev-only swallow mode passes false
// because it consumes held keys during injection instead of refusing.
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
bool ShouldSkipEmoteSend(const char** outKey, bool checkHeldKeys) {
    // 1. Can't-emote state: GW2 plays no emote while mounted/downed/swimming/
    //    underwater/gliding/flying, so the send is a silent no-op - refuse with a
    //    toast naming the reason. CurrentEmoteBlock() is the single gate shared
    //    with the Quickbar (core/CharacterState): gated on QuickbarGreyUnusable,
    //    covers mounted via MumbleLink and the rest via the optional RealTime API.
    //    This extends the block to *every* send surface - the main panel and the
    //    right-click Send/@/* variants, not just the Quickbar's greyed cells.
    EmoteBlock block = CurrentEmoteBlock();
    if (block != EmoteBlock::None) {
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
    switch (CurrentSendBusy(checkHeldKeys)) {
        case SendBusy::Typing:   *outKey = "send.skip.typing";    return true;
        case SendBusy::KeysHeld: *outKey = "send.skip.keys_held"; return true;
        case SendBusy::None:     break;
    }

    return false;
}

} // namespace

bool EmoteSendSwallowActive() {
#ifdef EMOT3_PLUS
    return g_DevSettings.SwallowInputOnSend;
#else
    return false;  // swallow mode is compiled out of base builds
#endif
}

SendBusy CurrentSendBusy(bool checkHeldKeys) {
    // GW2 textbox focused (chat half-typed, mail, TP search, ...). Single bit GW2
    //    maintains in the Mumble Link.
    if (MumbleLink && MumbleLink->Context.IsTextboxFocused)
        return SendBusy::Typing;

    // A printable character key is physically held - held WASD during movement is
    //    the canonical case. GetAsyncKeyState (not io.KeysDown[]) per
    //    nexus-addon-dev.md "Detecting game / input state": Nexus only feeds keys
    //    to ImGui when ImGui wants the keyboard. Modifiers alone don't type, so
    //    they're not gated. Skipped when checkHeldKeys is false (dev swallow mode
    //    consumes held keys during injection instead of refusing - so a held key
    //    must NOT show the cell as unusable; you can still click it).
    //
    //    GUARD ON FOREGROUND: GetAsyncKeyState reads the OS-wide physical key
    //    state, ignoring focus. Used per-frame for the Quickbar greying that
    //    means keys typed in ANOTHER app (alt-tabbed to chat) would grey the bar.
    //    Only consider held keys while GW2 is the foreground window. At click time
    //    the game is always foreground (you clicked its overlay), so this doesn't
    //    weaken the send gate; it only stops the greying from reacting to
    //    background input.
    if (checkHeldKeys && g_GameHwnd && GetForegroundWindow() == g_GameHwnd) {
        auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        auto held_any = [&](int lo, int hi) {
            for (int vk = lo; vk <= hi; ++vk) if (held(vk)) return true;
            return false;
        };
        if (held_any('A', 'Z') || held_any('0', '9') || held(VK_SPACE))
            return SendBusy::KeysHeld;
    }
    return SendBusy::None;
}

void SendOrFillEmote(const Emote& e, bool useTarget, bool useSync) {
    if (!APIDefs) return;
    std::string cmd = e.Command;
    if (useTarget && e.IsTargetable) cmd += " @";
    if (useSync)                     cmd += " *";
    bool send = g_Settings.SendOnClick;

    // Send mode. Default (and the only mode in base builds): the refuse-when-
    // unsafe gate below. +plus builds can opt into the old "swallow keyboard
    // during injection" mode via g_DevSettings.SwallowInputOnSend - the AV-
    // flagged input-consume is compiled in only for the +plus flavor, so
    // swallowMode is always false elsewhere (the #ifdef leaves it default-false).
    bool swallowMode = false;
#ifdef EMOT3_PLUS
    swallowMode = g_DevSettings.SwallowInputOnSend;
#endif

    // Refuse rather than garble. The check is at click time only - late typing
    // after the worker spawns still interleaves in refuse mode (swallow mode
    // covers that by consuming input). In swallow mode we drop the held-key
    // check (check 3) - those are handled by the swallow - but keep the chat-
    // unbound and textbox-focused checks (the refined textbox guard stays).
    const char* skipKey = nullptr;
    if (ShouldSkipEmoteSend(&skipKey, /*checkHeldKeys=*/!swallowMode)) {
        LOG_DEBUG("Emote skipped (%s): %s", skipKey, cmd.c_str());
        ShowFeedback(L(skipKey));
        return;
    }

    LOG_DEBUG("%s emote: %s", send ? "Sending" : "Filling", cmd.c_str());

    std::thread([cmd, send, swallowMode]() {
        // Bump the inflight-workers counter so AddonUnload can wait
        // briefly for us to drain. Drop on every early return via RAII.
        InflightWorkerScope scope;

        HWND hGame = g_GameHwnd
            ? g_GameHwnd
            : FindWindowA("ArenaNet_Dx_Window_Class", nullptr);
        if (!hGame) {
            LOG_WARNING("Could not find GW2 window - emote %s not sent",
                        cmd.c_str());
            return;
        }

        // Dev-only: swallow the user's keyboard for the whole injection window
        // (RAII - cleaned up on every g_Unloading early-return below) so held
        // keys can't interleave with our WM_CHAR stream. Inert when swallowMode
        // is false (refuse mode) and a no-op stub in distribution builds. The
        // window is exactly the injection's existing trimmed timings - it adds
        // no time, and SendToGameOnly bypasses our WndProc so our own injected
        // chars are never consumed.
        SendSuppressScope suppress(swallowMode);

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
        // needed for ordering. The remaining Sleeps are for game-side
        // state transitions (chat opens in response to UiChatCommand,
        // chat box accepts text after opening, Enter dispatch settles)
        // which actually need time.
        if (g_Unloading.load()) return;
        APIDefs->GameBinds.Press(EGameBinds_UiChatCommand);
        Sleep(60);
        if (g_Unloading.load()) return;
        APIDefs->GameBinds.Release(EGameBinds_UiChatCommand);
        Sleep(40);

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

        if (send) {
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
    }).detach();
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
    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
}

void MarkEmoteLocked(const std::string& id) {
    auto& u = g_Settings.ManuallyUnlocked;
    bool wasUnlocked = std::find(u.begin(), u.end(), id) != u.end();
    u.erase(std::remove(u.begin(), u.end(), id), u.end());
    int evicted = 0;
    for (auto& cat : g_Settings.FavoriteCategories) {
        size_t before = cat.Emotes.size();
        cat.Emotes.erase(std::remove(cat.Emotes.begin(), cat.Emotes.end(), id),
                         cat.Emotes.end());
        evicted += (int)(before - cat.Emotes.size());
    }
    if (wasUnlocked || evicted > 0) {
        LOG_DEBUG("Marked %s as locked (evicted from %d favorite slot(s))",
                  id.c_str(), evicted);
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
}
