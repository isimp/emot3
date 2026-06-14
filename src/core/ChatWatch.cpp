#include "ChatWatch.h"

#include "Globals.h"        // APIDefs, MumbleLink (+ <windows.h> for WideCharToMultiByte/GetTickCount64)
#include "Settings.h"       // g_Settings (master enable + watched-channel toggles + cooldown)
#include "CharacterState.h" // InCompetitiveMode (defence-in-depth; the send gate also blocks)
#include "EmoteAction.h"    // SendOrFillEmote, EFeedbackSink
#include "EmoteData.h"      // Emote, g_Emotes, g_EmotesMutex, FindEmote (the catalog + its AutoKeywords)
#include "StringUtil.h"     // ToLower
#include "Logging.h"
#include "Profiling.h"      // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)

#include "gw2chat/ChatApi.h"  // mirrored Events:Chat ABI (vendor/, no upstream license)

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot / Module32* — Events:Chat module
                        // probe (kernel32; psapi is devtools-only, ChatWatch ships)

using json = nlohmann::json;

namespace {

// "Events: Chat" presence. The addon exposes no DataLink to probe (unlike RTAPI),
// so we triangulate three ways: (1) an authoritative module-signature probe at
// our load (InitChatWatch -> ProbeChatAddonLoaded), which is the ONLY thing that
// catches "Events: Chat loaded before us" — the case Nexus' EV_ADDON_LOADED won't
// replay, and the same case that made an emot3 reload look broken; (2) its
// load/unload events, keeping us live afterwards; (3) the first chat line we see,
// a last-resort backstop. Atomic: written from the load + event threads, read by
// the render thread (Options banner).
std::atomic<bool> s_available{ false };

// The single pending auto-fire slot: the matched emote's catalog Id, set by
// OnChat (any thread) and drained by DrainAutoMotes (render thread). Last-wins —
// auto-fire is best-effort, so a burst collapses to the most recent match rather
// than queueing a backlog of emotes. Spam limiting lives at the shared send gate
// now (the global min-interval, core/EmoteAction), so OnChat keeps no cooldown
// state of its own — it just finds a match and drops it here.
std::mutex  s_pendingMu;
std::string s_pending;

// Whole-word, case-insensitive test: does `keyword` (already lowercased) appear
// in `lowered` (the message, already lowercased ONCE by the caller) bounded by
// non-alphanumeric edges? "lol" matches "thats crazy lol" and "LOL!" but not
// "lollipop". ASCII word chars only — non-ASCII bytes count as boundaries, which
// is fine for typical triggers ("lol", "gg", "brb"). The caller lowercases the
// message a single time and reuses it across every keyword/emote, so this never
// allocates per keyword.
bool KeywordMatchesLowered(const std::string& lowered, const std::string& keyword) {
    if (keyword.empty()) return false;
    auto isWord = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    size_t pos = 0;
    while ((pos = lowered.find(keyword, pos)) != std::string::npos) {
        const bool leftOk  = (pos == 0) || !isWord((unsigned char)lowered[pos - 1]);
        const size_t end   = pos + keyword.size();
        const bool rightOk = (end >= lowered.size()) || !isWord((unsigned char)lowered[end]);
        if (leftOk && rightOk) return true;
        ++pos;
    }
    return false;
}

// The local character's name from MumbleLink's Identity JSON (a UTF-16 blob,
// e.g. {"name":"Foo","profession":1,...}). Used for own-message detection on
// every channel except whispers (which carry their own IsFromMe flag).
//
// CACHED: the Identity only changes on a character / map swap, but OnChat fires
// once per RECEIVED chat line (Events:Chat re-broadcasts everyone's chat), so
// JSON-parsing it per message would be needless allocation churn in busy chat.
// We re-parse only when the raw blob actually changes (a cheap wstring compare).
// thread_local so the cache is lock-free no matter which thread the chat event
// lands on. Returns "" when MumbleLink / the field / the JSON is unavailable.
const std::string& LocalCharacterName() {
    thread_local std::wstring s_lastId;
    thread_local std::string  s_name;
    static const std::string  kEmpty;

    if (!MumbleLink) return kEmpty;
    const wchar_t* wid = MumbleLink->Identity;
    if (!wid || !wid[0]) return kEmpty;
    if (s_lastId == wid) return s_name;   // unchanged → reuse the parsed name

    s_lastId = wid;                        // remember the blob we're parsing
    s_name.clear();
    char utf8[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, wid, -1, utf8, (int)sizeof(utf8),
                                nullptr, nullptr);
    if (n > 0) {
        try {
            json j = json::parse(utf8);
            if (j.is_object()) {
                auto it = j.find("name");
                if (it != j.end() && it->is_string()) s_name = it->get<std::string>();
            }
        } catch (...) {
            // Malformed / partial Identity — cache the empty result (won't retry
            // until the blob changes); own-message check fails closed for now.
        }
    }
    return s_name;
}

// Copy a publisher-owned, null-terminated C string into a fixed buffer, bounded
// and always null-terminated; null source -> empty. POD/no-throw. The src[i]
// read is the one that could fault if the (beta, untrusted) ABI moved this field
// — that fault is caught by the SEH frame in SafeExtractChat below.
void CopyBounded(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// POD snapshot of the one chat line we care about, copied out of the publisher's
// payload so the rest of OnChat works on our own memory (the char* fields are
// valid only during the synchronous callback anyway).
struct ChatExtract {
    int  channelType = 0;   // gw2chat_MessageType, for the watched-channel gate
    bool isWhisper   = false;
    bool whisperFromMe = false;
    char author[64]  = {0};
    char content[512] = {0};
};

// Raw read of the Events:Chat payload (an OPTIONAL, BETA dependency whose ABI we
// mirror but don't own) into our POD snapshot. The pointer derefs + union index
// here are the ONLY untrusted-ABI operations in the feature, so a changed struct
// layout can only fault inside this function — which is why SafeExtractChat wraps
// it in SEH on MSVC. POD-only (no std::string / objects needing unwinding) so the
// MSVC __try caller is C2712-clean and an SEH unwind has nothing to destruct.
// Returns false for any channel we don't watch (the loop guard: Emote/EmoteCustom/
// MotD/SquadMessage/Team*/Error all fall through default).
bool ExtractChatRaw(const void* args, ChatExtract& out) {
    const gw2chat_Message* m = (const gw2chat_Message*)args;
    const gw2chat_MessageType type = m->Type;
    out.channelType = (int)type;
    const gw2chat_GenericMessage* gm = nullptr;
    switch (type) {
        case GW2CHAT_Local:   gm = &m->Local;      break;
        case GW2CHAT_Map:     gm = &m->Map;        break;
        case GW2CHAT_Party:   gm = &m->Party;      break;
        case GW2CHAT_Squad:   gm = &m->Squad;      break;
        case GW2CHAT_Guild:   gm = &m->Guild.Base; break;
        case GW2CHAT_Whisper: gm = &m->Whisper;    break;
        default: return false;  // Emote/EmoteCustom/MotD/SquadMessage/Team*/Error
    }
    if (!gm) return false;
    out.isWhisper     = (type == GW2CHAT_Whisper);
    out.whisperFromMe = (m->Flags & GW2CHAT_Whisper_IsFromMe) != 0;
    CopyBounded(out.author,  sizeof(out.author),  gm->CharacterName);
    CopyBounded(out.content, sizeof(out.content), gm->Content);
    return true;
}

// SEH-guard the raw read so a beta ABI/struct-layout change faults into "ignore
// this line" instead of crashing the whole GW2 process.
//
// Only MSVC has the __try/__except STATEMENT; mingw-w64 GCC does not (it parses
// __try as C++ `try`). That's fine: the SHIPPED DLL is always the MSVC build —
// and Wine loads that same MSVC binary — so every binary that reaches a user IS
// guarded. The MinGW build is the Linux portability GATE only (never shipped), so
// there we call straight through; it still has to compile + stay Wine-clean, which
// this does. A faulting read => the mirrored ABI no longer matches the live addon;
// drop the line (no LOG in the handler — it must stay trivial).
bool SafeExtractChat(const void* args, ChatExtract& out) {
#if defined(_MSC_VER)
    __try {
        return ExtractChatRaw(args, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return ExtractChatRaw(args, out);
#endif
}

// Whether auto-motes may fire on the CURRENT map, by GW2 map type (reused from
// MumbleLink Context.MapType - no custom map-ID lists). Open world (Public +
// Public_Mini: zones, cities, Dry Top / Silverwastes / Mistlock Sanctuary; plus
// WvW_Lounge, the non-combat Armistice Bastion social space) and instanced content
// (Instance: dungeons, fractals, raids, strikes, story, guild/home) are the two
// user toggles. Everything else fails closed: competitive PvP/WvW (already refused
// by the send gate), the unused BigBattle, and transient maps
// (redirect/charcreate/tutorial). A missing MumbleLink doesn't add a failure mode
// (treat as allowed).
static bool AutoMoteMapAllowed() {
    if (!MumbleLink) return true;
    using MT = Mumble::EMapType;
    switch (MumbleLink->Context.MapType) {
        case MT::Public:
        case MT::Public_Mini:
        case MT::WvW_Lounge:  return g_Settings.AutoMoteInOpenWorld;
        case MT::Instance:    return g_Settings.AutoMoteInInstances;
        default:              return false;
    }
}

// The chat event. Fires (on some publisher thread) once per chat line. We keep
// ONLY the user's own, sent, non-competitive lines on a watched channel, match
// them against the catalog's auto-keywords, and queue the first hit. NEVER sends
// inline and NEVER touches ImGui/APIDefs.
void OnChat(void* aEventArgs) {
    if (!aEventArgs) return;
    // Receiving any chat line proves the addon is present (covers loaded-before-us).
    s_available.store(true, std::memory_order_relaxed);

    if (!g_Settings.AutoMotesEnabled) return;

    // Snapshot the untrusted payload behind the SEH guard FIRST; everything below
    // works on our own POD copy. A handled channel we don't watch, or an ABI
    // fault, both come back false here.
    ChatExtract ex;
    if (!SafeExtractChat(aEventArgs, ex)) return;

    // Resolve the watched toggle for this channel (settings; outside the SEH frame).
    // Emote/EmoteCustom and Team*/MotD never reach here (SafeExtractChat dropped
    // them) — that's the loop guard.
    bool watched = false;
    switch ((gw2chat_MessageType)ex.channelType) {
        case GW2CHAT_Local:   watched = g_Settings.AutoMoteWatchLocal;   break;
        case GW2CHAT_Map:     watched = g_Settings.AutoMoteWatchMap;     break;
        case GW2CHAT_Party:   watched = g_Settings.AutoMoteWatchParty;   break;
        case GW2CHAT_Squad:   watched = g_Settings.AutoMoteWatchSquad;   break;
        case GW2CHAT_Guild:   watched = g_Settings.AutoMoteWatchGuild;   break;
        case GW2CHAT_Whisper: watched = g_Settings.AutoMoteWatchWhisper; break;
        default: return;
    }
    if (!watched) return;

    // Defence-in-depth: never auto-act in PvP/WvW (the send gate also refuses).
    if (InCompetitiveMode()) return;

    // Map-type gate: only fire where the user allowed it (open world / instances).
    if (!AutoMoteMapAllowed()) return;

    // Own-message only. Whispers carry an explicit IsFromMe flag; every other
    // channel compares the author's character name to the local player.
    bool fromMe = false;
    if (ex.isWhisper) {
        fromMe = ex.whisperFromMe;
    } else {
        const std::string& me = LocalCharacterName();   // cached; no per-message parse
        if (!me.empty() && me == ex.author) fromMe = true;
    }
    if (!fromMe) return;

    if (ex.content[0] == '\0') return;

    // First catalog emote whose chat triggers match this line wins. No cooldown
    // bookkeeping here — the shared send gate enforces the global min-interval at
    // fire time (core/EmoteAction), and the last-wins slot already collapses a
    // burst, so OnChat just finds the match and enqueues. Scan under g_EmotesMutex.
    // We're here ONLY for the user's OWN message (rare vs. received chat), and
    // lowercase it ONCE for every keyword/emote rather than per comparison.
    const std::string lowered = ToLower(std::string(ex.content));
    std::string fireEmoteId;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (const auto& e : g_Emotes) {
            if (e.AutoKeywords.empty()) continue;
            for (const auto& kw : e.AutoKeywords)
                if (KeywordMatchesLowered(lowered, kw)) { fireEmoteId = e.Id; break; }
            if (!fireEmoteId.empty()) break;  // one fire per line
        }
    }
    if (fireEmoteId.empty()) return;

    {
        std::lock_guard<std::mutex> lk(s_pendingMu);
        s_pending = std::move(fireEmoteId);  // last-wins; the gate throttles at fire
    }
    LOG_DEBUG("auto-mote: queued fire (channel=%d)", ex.channelType);
}

// Does this loaded module export the Nexus addon entry point AND report the
// Events:Chat signature? Every Nexus addon exports `GetAddonDef` returning an
// AddonDefinition* whose Signature identifies it (same value Nexus hands us in
// EV_ADDON_LOADED). GetAddonDef is a third-party export, so the call + deref sit
// behind SEH on MSVC (the shipped + Wine binary): a malformed addon faults into
// "not it" rather than crashing GW2. POD-only (HMODULE / pointer), so the __try
// caller is C2712-clean. MinGW (portability gate, never shipped) calls through.
bool ModuleIsChatAddon(HMODULE mod) {
    typedef AddonDefinition* (*GetAddonDef_t)();
    GetAddonDef_t getDef = (GetAddonDef_t)GetProcAddress(mod, "GetAddonDef");
    if (!getDef) return false;
#if defined(_MSC_VER)
    __try {
        AddonDefinition* def = getDef();
        return def && def->Signature == EMOT3_GW2CHAT_SIGNATURE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    AddonDefinition* def = getDef();
    return def && def->Signature == EMOT3_GW2CHAT_SIGNATURE;
#endif
}

// One-time active probe for "Events: Chat is already loaded". Walks the GW2
// process' loaded modules (Toolhelp; kernel32, no psapi link) and matches any
// addon carrying the Events:Chat signature. This is what makes detection correct
// at our load — including across an emot3 disable/re-enable, where the addon was
// up the whole time and Nexus won't re-raise EV_ADDON_LOADED. Cheap + one-shot.
bool ProbeChatAddonLoaded() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32First(snap, &me)) {
        do {
            if (ModuleIsChatAddon(me.hModule)) { found = true; break; }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// EV_ADDON_LOADED / EV_ADDON_UNLOADED both carry int* = the (un)loading addon's
// signature. We only care about Events:Chat's; ignore everyone else's.
void OnAddonLoaded(void* aEventArgs) {
    if (!aEventArgs || *(const int*)aEventArgs != EMOT3_GW2CHAT_SIGNATURE) return;
    s_available.store(true, std::memory_order_relaxed);
    LOG_INFO("Events: Chat addon loaded — auto-motes active");
}

void OnAddonUnloaded(void* aEventArgs) {
    if (!aEventArgs || *(const int*)aEventArgs != EMOT3_GW2CHAT_SIGNATURE) return;
    s_available.store(false, std::memory_order_relaxed);
    LOG_INFO("Events: Chat addon unloaded — auto-motes inert");
}

}  // namespace

void InitChatWatch() {
    if (!APIDefs) return;
    APIDefs->Events.Subscribe(EMOT3_GW2CHAT_EVENT, OnChat);
    APIDefs->Events.Subscribe("EV_ADDON_LOADED",   OnAddonLoaded);
    APIDefs->Events.Subscribe("EV_ADDON_UNLOADED", OnAddonUnloaded);
    LOG_INFO("Auto-motes chat watch subscribed (Events: Chat optional dependency)");

    // Seed availability authoritatively from a module probe. Order matters: we
    // subscribe FIRST so a load racing this window still reaches OnAddonLoaded,
    // then the probe catches anything already up (the loaded-before-us case Nexus
    // won't replay — and the re-enable-after-disable case). Store the result
    // UNCONDITIONALLY so a stale `true` (DLL stays resident across reload) is
    // cleared if Events:Chat was removed while we were disabled.
    const bool present = ProbeChatAddonLoaded();
    s_available.store(present, std::memory_order_relaxed);
    LOG_INFO("Events: Chat load-time probe: %s", present ? "detected" : "not found");
}

void ShutdownChatWatch() {
    if (!APIDefs) return;
    APIDefs->Events.Unsubscribe(EMOT3_GW2CHAT_EVENT, OnChat);
    APIDefs->Events.Unsubscribe("EV_ADDON_LOADED",   OnAddonLoaded);
    APIDefs->Events.Unsubscribe("EV_ADDON_UNLOADED", OnAddonUnloaded);
    // Drop any pending fire so a Nexus reload can't resurrect a stale queue.
    std::lock_guard<std::mutex> lk(s_pendingMu);
    s_pending.clear();
}

bool ChatWatchAvailable() {
    return s_available.load(std::memory_order_relaxed);
}

void DrainAutoMotes() {
    PROFILE_SCOPE("automotes");  // dev perf overlay — render-thread drain (≈0 when idle)
    std::string id;
    {
        std::lock_guard<std::mutex> lk(s_pendingMu);
        if (s_pending.empty()) return;
        id.swap(s_pending);
    }

    // Auto-only minimum interval, ADDITIONAL to the global send-gate throttle:
    // auto-fires get their own (typically longer) floor so passive chat triggers
    // don't fire too often. Render-thread only (DrainAutoMotes is the sole user),
    // so a plain static needs no lock. Stamped ONLY on an actual dispatch below —
    // a drop here, or a gate refusal in SendOrFillEmote, must not burn the clock.
    static uint64_t s_lastAutoFireMs = 0;
    const uint64_t now = GetTickCount64();
    if (s_lastAutoFireMs != 0 &&
        now - s_lastAutoFireMs < (uint64_t)g_Settings.AutoMoteMinIntervalMs)
        return;  // too soon for another auto-fire; drop silently

    // Resolve + copy the Emote under the catalog lock (SendOrFillEmote only needs
    // it for the synchronous command build; its detached worker copies the string
    // it types). A trigger whose emote no longer resolves is inert, logged once.
    Emote e;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        const Emote* p = FindEmote(id);
        if (!p) {
            LOG_DEBUG("auto-mote: emote '%s' not in catalog — fire skipped", id.c_str());
            return;
        }
        e = *p;
    }
    // Don't auto-fire an emote the user hasn't unlocked — GW2 would just reject
    // the command, so it'd be a silent dead send. A manual click is the user's
    // call; an automatic one should only ever fire something that'll play. Core
    // emotes are always unlocked.
    if (!e.IsCore && !IsEmoteUnlocked(e.Id)) {
        LOG_DEBUG("auto-mote: emote '%s' is locked — fire skipped", e.Id.c_str());
        return;
    }
    // Auto-fire is a click with no human: never on-target, never sync'd, and
    // SILENT on refusal — there's no cursor to anchor an in-window cue to and a
    // toast for an action the user didn't knowingly trigger reads as noise (this
    // is also what keeps a gate throttle or a "you're typing" refusal right after
    // you hit Enter from popping a confusing alert). The send gate
    // (ShouldSkipEmoteSend + the global min-interval throttle) applies for free.
    // Stamp the auto clock only when it actually dispatched — if the global gate
    // refused (throttle / typing / competitive / ...), the auto floor isn't spent.
    if (SendOrFillEmote(e, /*useTarget=*/false, /*useSync=*/false, EFeedbackSink::Silent))
        s_lastAutoFireMs = now;
}

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
// Runtime-state section (Layer 2): the live auto-mote signals, so a "why didn't
// my emote auto-fire?" can be diagnosed — is Events:Chat connected, is the
// feature on, which channels, did the last line queue, is a send throttled, and
// does the name-match have a local name. Self-registers (see DevStateInspector.h).
static DevStateRegistrar s_autoMoteState(DevStateCat::GameSignals, "Auto-motes", [] {
    DevStateRow("Events: Chat",  "%s", ChatWatchAvailable() ? "detected" : "not detected");
    DevStateRow("enabled",       "%s", g_Settings.AutoMotesEnabled ? "yes" : "no");

    std::string chans;
    auto add = [&](bool on, const char* n) {
        if (on) { if (!chans.empty()) chans += ' '; chans += n; }
    };
    add(g_Settings.AutoMoteWatchLocal,   "Local");
    add(g_Settings.AutoMoteWatchParty,   "Party");
    add(g_Settings.AutoMoteWatchMap,     "Map");
    add(g_Settings.AutoMoteWatchSquad,   "Squad");
    add(g_Settings.AutoMoteWatchGuild,   "Guild");
    add(g_Settings.AutoMoteWatchWhisper, "Whisper");
    DevStateRow("watched",       "%s", chans.empty() ? "(none)" : chans.c_str());

    {   // live map type + whether the map-type gate currently passes
        int mt = MumbleLink ? (int)MumbleLink->Context.MapType : -1;
        DevStateRow("map type",  "%d %s", mt,
                    AutoMoteMapAllowed() ? "(allowed)" : "(blocked)");
    }

    const std::string& me = LocalCharacterName();
    DevStateRow("local name",    "%s", me.empty() ? "(unknown)" : me.c_str());

    {
        std::lock_guard<std::mutex> lk(s_pendingMu);
        DevStateRow("pending fire", "%s", s_pending.empty() ? "(none)" : s_pending.c_str());
    }

    int triggerEmotes = 0;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (const auto& e : g_Emotes) if (!e.AutoKeywords.empty()) ++triggerEmotes;
    }
    DevStateRow("trigger emotes", "%d", triggerEmotes);
    DevStateRow("send throttle",  "%u ms", SendThrottleRemainingMs());
});
#endif  // EMOT3_DEVTOOLS
