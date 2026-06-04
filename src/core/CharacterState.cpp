#include "CharacterState.h"

#include "Globals.h"    // APIDefs, MumbleLink
#include "Settings.h"   // g_Settings
#include "Logging.h"

#include <cstdio>       // snprintf (RTApiDebugInfo)

#include "rtapi/RTAPI.h" // RealTimeData, ECharacterState, DL_RTAPI (vendored)

EmoteBlock  g_QbBlockReason = EmoteBlock::None;
const char* g_QbUnusableKey  = nullptr;

namespace {

// The RTAPI DataLink, null until the RealTime API addon has created it. The
// region is Nexus-owned and persists for the process once made, so the pointer
// stays valid across an RTAPI unload - GameBuild going to 0 is the liveness
// signal (see LiveRTApi), not a dangling pointer.
RealTimeData* s_rtapi = nullptr;

// Resolve the RTAPI DataLink. Per the RealTime API docs the shared-memory block
// is registered under the key DL_RTAPI (which expands to the literal "RTAPI")
// and read with DataLink.Get - which returns null until the RTAPI addon has
// created it. (An earlier build also probed the literal "DL_RTAPI" out of
// uncertainty; the docs are unambiguous - the key is "RTAPI" only.)
RealTimeData* ResolveRTApi() {
    if (!APIDefs) return nullptr;
    return (RealTimeData*)APIDefs->DataLink.Get(DL_RTAPI);
}

// Last-known RTAPI liveness, so we can log the connect/disconnect transition
// (not every frame). Flips at most a couple of times per session.
bool s_rtapiLive = false;

// Returns the RTAPI pointer only when it's live. The pointer is set/cleared by
// the EV_ADDON_LOADED / EV_ADDON_UNLOADED handlers (plus the initial resolve at
// load); GameBuild is the staleness guard the docs call for - non-zero in-game,
// zeroed when RTAPI unloads (covers the brief window before the unload event,
// and the case where the Nexus-owned region lingers after a crash/unload).
RealTimeData* LiveRTApi() {
    bool live = (s_rtapi && s_rtapi->GameBuild != 0);
    if (live != s_rtapiLive) {
        // Precise can't-emote detection silently starts/stops working here, so
        // record the transition. Gated on the flip - called per frame, logs rarely.
        s_rtapiLive = live;
        LOG_INFO("RealTime API: %s", live ? "connected" : "disconnected");
    }
    return live ? s_rtapi : nullptr;
}

// Dev diagnostic: set once we observe an EV_ADDON_LOADED whose payload signature
// matches RTAPI - proof Nexus loaded RTAPI *after* us and our event wiring works.
// (Stays false if RTAPI loaded before us; not conclusive alone.)
bool s_sawRtapiEvent = false;

// Documented lifecycle (RealTime API README): EV_ADDON_LOADED / EV_ADDON_UNLOADED
// carry int* = the loading/unloading addon's signature. On RTAPI's load we
// (re)resolve the DataLink; on its unload we drop the pointer. We ignore every
// other addon's events - cheaper than the old re-resolve-on-any-change, and it
// matches the published example exactly.
void OnAddonLoaded(void* aEventArgs) {
    if (!aEventArgs || *(const int*)aEventArgs != RTAPI_SIG) return;
    s_sawRtapiEvent = true;
    s_rtapi = ResolveRTApi();
}

void OnAddonUnloaded(void* aEventArgs) {
    if (!aEventArgs || *(const int*)aEventArgs != RTAPI_SIG) return;
    s_rtapi = nullptr;
}

}  // namespace

void InitCharacterState() {
    if (!APIDefs) return;
    // Resolve once up front in case RTAPI loaded BEFORE us - its EV_ADDON_LOADED
    // already fired and Nexus doesn't replay it for a new subscriber. The event
    // handlers then keep the pointer correct if RTAPI (re)loads or unloads later.
    s_rtapi = ResolveRTApi();
    APIDefs->Events.Subscribe("EV_ADDON_LOADED",   OnAddonLoaded);
    APIDefs->Events.Subscribe("EV_ADDON_UNLOADED", OnAddonUnloaded);
    LOG_INFO("RealTime API: %s at load", s_rtapi ? "detected" : "not present");
}

void ShutdownCharacterState() {
    if (!APIDefs) return;
    APIDefs->Events.Unsubscribe("EV_ADDON_LOADED",   OnAddonLoaded);
    APIDefs->Events.Unsubscribe("EV_ADDON_UNLOADED", OnAddonUnloaded);
    s_rtapi = nullptr;
}

bool RTApiConnected() {
    return LiveRTApi() != nullptr;
}

EmoteBlock CurrentEmoteBlock() {
    if (!g_Settings.QuickbarGreyUnusable) return EmoteBlock::None;

    // Mounted: MumbleLink, always available (no RTAPI needed).
    if (MumbleLink && MumbleLink->Context.MountIndex != Mumble::EMountIndex::None)
        return EmoteBlock::Mounted;

    // Precise states: opt-in AND require a live RTAPI.
    RealTimeData* rt = g_Settings.QuickbarPreciseStateDetection ? LiveRTApi() : nullptr;
    if (rt) {
        uint32_t s = rt->CharacterState;
        // Underwater ("diving") is more specific than Swimming ("on surface"),
        // so report it first when both happen to be set.
        if (s & CS_IsDowned)     return EmoteBlock::Downed;
        if (s & CS_IsUnderwater) return EmoteBlock::Underwater;
        if (s & CS_IsSwimming)   return EmoteBlock::Swimming;
        if (s & CS_IsGliding)    return EmoteBlock::Gliding;
        if (s & CS_IsFlying)     return EmoteBlock::Flying;
    }
    return EmoteBlock::None;
}

bool InCombatNow() {
    return MumbleLink && MumbleLink->Context.IsInCombat;
}

const char* RTApiDebugInfo() {
    static char buf[160];
    void* p = APIDefs ? APIDefs->DataLink.Get(DL_RTAPI) : nullptr;  // "RTAPI"
    unsigned gb = p ? ((const RealTimeData*)p)->GameBuild : 0;
    std::snprintf(buf, sizeof(buf),
                  "[dev] DataLink \"%s\"=%p (GameBuild=%u)  RTAPI load-event seen: %s",
                  DL_RTAPI, p, gb, s_sawRtapiEvent ? "yes" : "no");
    return buf;
}

const char* EmoteBlockKey(EmoteBlock reason) {
    switch (reason) {
        case EmoteBlock::Downed:     return "cells.blocked_downed";
        case EmoteBlock::Swimming:   return "cells.blocked_swimming";
        case EmoteBlock::Underwater: return "cells.blocked_underwater";
        case EmoteBlock::Gliding:    return "cells.blocked_gliding";
        case EmoteBlock::Flying:     return "cells.blocked_flying";
        case EmoteBlock::Mounted:
        default:                     return "cells.blocked_mounted";
    }
}

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
namespace {
const char* BlockName(EmoteBlock r) {
    switch (r) {
        case EmoteBlock::None:       return "None";
        case EmoteBlock::Mounted:    return "Mounted";
        case EmoteBlock::Downed:     return "Downed";
        case EmoteBlock::Swimming:   return "Swimming";
        case EmoteBlock::Underwater: return "Underwater";
        case EmoteBlock::Gliding:    return "Gliding";
        case EmoteBlock::Flying:     return "Flying";
        default:                     return "?";
    }
}
}  // namespace
// Runtime state inspector section: the raw signals this module bridges, so a
// "why is my emote refused / greyed?" report can be diagnosed live. Self-
// registers via the Layer-2 standard (DevStateInspector.h).
static DevStateRegistrar s_gameStateSection("Game state", [] {
    const bool haveMumble = (MumbleLink != nullptr);
    const bool mounted = haveMumble &&
        MumbleLink->Context.MountIndex != Mumble::EMountIndex::None;
    DevStateRow("MumbleLink",        "%s", haveMumble ? "present" : "NULL");
    DevStateRow("mounted",           "%s (idx %d)", mounted ? "yes" : "no",
                haveMumble ? (int)MumbleLink->Context.MountIndex : -1);
    DevStateRow("in combat",         "%s", InCombatNow() ? "yes" : "no");
    DevStateRow("textbox focused",   "%s",
                (haveMumble && MumbleLink->Context.IsTextboxFocused) ? "yes" : "no");
    DevStateRow("RTAPI connected",   "%s", RTApiConnected() ? "yes" : "no");
    // Raw RTAPI signals, so the precise-state blocks (downed/swim/underwater/
    // glide/fly) can be verified flag-by-flag once a working RTAPI is installed.
    if (RealTimeData* rt = LiveRTApi()) {
        uint32_t s = rt->CharacterState;
        DevStateRow("RTAPI GameBuild",  "%u", rt->GameBuild);
        DevStateRow("char state bits",  "0x%02X", s);
        DevStateRow("  downed/sw/uw",   "%d/%d/%d", !!(s & CS_IsDowned),
                    !!(s & CS_IsSwimming), !!(s & CS_IsUnderwater));
        DevStateRow("  glide/fly/cmbt", "%d/%d/%d", !!(s & CS_IsGliding),
                    !!(s & CS_IsFlying), !!(s & CS_IsInCombat));
    }
    DevStateRow("CurrentEmoteBlock", "%s", BlockName(CurrentEmoteBlock()));
    DevStateRow("g_QbBlockReason",   "%s", BlockName(g_QbBlockReason));
    DevStateRow("g_QbUnusableKey",   "%s", g_QbUnusableKey ? g_QbUnusableKey : "(usable)");
});
#endif  // EMOT3_DEVTOOLS
