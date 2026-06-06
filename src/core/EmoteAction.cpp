#include "EmoteAction.h"
#include "Globals.h"
#include "I18n.h"
#include "Logging.h"
#include "Settings.h"
#include "EmoteData.h"
#include "MeMotes.h"        // /me-mote struct (SendOrFillMeMote)
#include "SendSuppress.h"   // keyboard swallow during emote injection (stub in base builds)
#include "PlusSettings.h"   // g_PlusSettings (whole header empty in base builds)
#include "CharacterState.h" // CurrentEmoteBlock / EmoteBlockKey (mounted + RTAPI states)
#include "Feedback.h"       // ShowFeedback - in-window refusal line (replaces SendAlert)

#include <Windows.h>
#include <algorithm>
#include <atomic>
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
    return SendBusy::None;
}

namespace {

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
            Sleep(40);   // let the box close before we re-open chat
        }

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
    }).detach();
}

// Resolve the +plus "swallow keyboard during injection" mode. Always false in
// base builds (the AV-flagged input-consume is compiled out via #ifdef so the
// branch leaves the local default-false). Shared by both send paths.
inline bool ResolveSwallowMode() {
#ifdef EMOT3_PLUS
    return g_PlusSettings.SwallowInputOnSend;
#else
    return false;
#endif
}

} // namespace

void SendOrFillEmote(const Emote& e, bool useTarget, bool useSync) {
    if (!APIDefs) return;
    std::string cmd = e.Command;
    if (useTarget && e.IsTargetable) cmd += " @";
    if (useSync)                     cmd += " *";
    const bool send        = g_Settings.SendOnClick;
    const bool swallowMode = ResolveSwallowMode();

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
    if (ShouldSkipEmoteSend(&skipKey, /*checkHeldKeys=*/!swallowMode,
                            /*ignoreTextbox=*/closeChat)) {
        LOG_DEBUG("Emote skipped (%s): %s", skipKey, cmd.c_str());
        ShowFeedback(L(skipKey));
        return;
    }

    LOG_DEBUG("%s emote: %s", send ? "Sending" : "Filling", cmd.c_str());
    InjectChatCommand(std::move(cmd), send, closeChat, swallowMode);
}

void SendOrFillMeMote(const MeMote& m, EMeMoteVariant variant) {
    if (!APIDefs) return;

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
        return;
    }

    // Build "/me <text>". The "/me " prefix is owned exclusively here — never
    // stored in TextDefault / TextYou / TextAll (sanitize strips it at load),
    // so we can't ship a doubled prefix.
    std::string cmd = "/me " + *body;

    const bool send        = g_Settings.MeMoteSendOnClick;  // independent of SendOnClick
    const bool swallowMode = ResolveSwallowMode();

    const bool closeChat = g_Settings.CloseChatOnSend &&
                           MumbleLink && MumbleLink->Context.IsTextboxFocused;

    // Same gating as official emotes — can't-emote state / chat unbound /
    // textbox focused / held printable keys / airborne. The gates aren't
    // specific to slash-command emotes; they protect ANY chat injection from
    // garbling.
    const char* skipKey = nullptr;
    if (ShouldSkipEmoteSend(&skipKey, /*checkHeldKeys=*/!swallowMode,
                            /*ignoreTextbox=*/closeChat)) {
        LOG_DEBUG("/me-mote skipped (%s): %s", skipKey, cmd.c_str());
        ShowFeedback(L(skipKey));
        return;
    }

    LOG_DEBUG("%s /me-mote: %s", send ? "Sending" : "Filling", cmd.c_str());
    InjectChatCommand(std::move(cmd), send, closeChat, swallowMode);
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
