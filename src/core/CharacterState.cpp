#include "CharacterState.h"

#include "Globals.h"        // APIDefs, MumbleLink
#include "Settings.h"       // g_Settings
#include "Logging.h"
#include "Profiling.h"      // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)
#include "AirborneDetect.h" // air::Tick + air::IsAirborne / IsMoving / VertSpeed / HorizSpeed

#include <cstdio>           // snprintf (RTApiDebugInfo)

#include "rtapi/RTAPI.h"    // RealTimeData, ECharacterState, DL_RTAPI (vendored)

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

// Per-frame update: delegate the airborne + moving detection to core/AirborneDetect.
// (The PROFILE scope stays here so the perf overlay's "cs.tick" still reflects the cost.)
void TickCharacterState() {
    PROFILE_SCOPE("cs.tick");  // dev perf overlay (should read ~0 - no syscalls)
    air::Tick();
}

bool RTApiConnected() {
    return LiveRTApi() != nullptr;
}

uint32_t RTApiCharacterStateBits() {
    RealTimeData* rt = LiveRTApi();
    return rt ? rt->CharacterState : 0u;
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

    // Airborne (jump or fall): its OWN opt-in (QuickbarAirborneDetection), separate from
    // the RTAPI states above - it's MumbleLink-derived (core/AirborneDetect) and needs no
    // addon. Checked after the RTAPI states so a controlled descent (gliding / flying) or
    // a water state keeps its own, more specific reason.
    if (g_Settings.QuickbarAirborneDetection && air::IsAirborne())
        return EmoteBlock::Airborne;
    return EmoteBlock::None;
}

bool InCombatNow() {
    return MumbleLink && MumbleLink->Context.IsInCombat;
}

bool MovementActive() { return air::IsMoving(); }

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
        case EmoteBlock::Airborne:   return "cells.blocked_airborne";
        case EmoteBlock::Mounted:
        default:                     return "cells.blocked_mounted";
    }
}

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
#include "EmoteAction.h"   // HeldPrintableCount (dev readout for the gate)
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
        case EmoteBlock::Airborne:   return "Airborne";
        default:                     return "?";
    }
}
}  // namespace
// Runtime state inspector section: the raw signals this module bridges, so a
// "why is my emote refused / greyed?" report can be diagnosed live. Self-
// registers via the Layer-2 standard (DevStateInspector.h).
static DevStateRegistrar s_gameStateSection(DevStateCat::GameSignals, "Game state", [] {
    const bool haveMumble = (MumbleLink != nullptr);
    const bool mounted = haveMumble &&
        MumbleLink->Context.MountIndex != Mumble::EMountIndex::None;
    DevStateRow("MumbleLink",        "%s", haveMumble ? "present" : "NULL");
    DevStateRow("mounted",           "%s (idx %d)", mounted ? "yes" : "no",
                haveMumble ? (int)MumbleLink->Context.MountIndex : -1);
    DevStateRow("in combat",         "%s", InCombatNow() ? "yes" : "no");
    DevStateRow("textbox focused",   "%s",
                (haveMumble && MumbleLink->Context.IsTextboxFocused) ? "yes" : "no");
    // UI-gate inputs the addon itself acts on: both already drive whether the
    // bar/panel render (MainPanel/Quickbar early-return on these), so surfacing
    // them makes "why did the UI hide?" explainable.
    DevStateRow("IsGameplay (Nexus)", "%s",
                (NexusLink && NexusLink->IsGameplay) ? "yes" : "no");
    DevStateRow("IsMapOpen (Mumble)", "%s",
                (haveMumble && MumbleLink->Context.IsMapOpen) ? "yes" : "no");
    DevStateRow("RTAPI connected",   "%s", RTApiConnected() ? "yes" : "no");
    // Raw RTAPI signals, so the precise-state blocks (downed/swim/underwater/
    // glide/fly) can be verified flag-by-flag once a working RTAPI is installed.
    if (RealTimeData* rt = LiveRTApi()) {
        uint32_t s = rt->CharacterState;
        auto gsName = [](uint32_t g) -> const char* {
            switch (g) {
                case GS_CharacterSelection: return "char-select";
                case GS_CharacterCreation:  return "char-create";
                case GS_Cinematic:          return "cinematic";
                case GS_LoadingScreen:      return "loading";
                case GS_Gameplay:           return "gameplay";
                default:                    return "?";
            }
        };
        DevStateRow("RTAPI GameBuild",  "%u", rt->GameBuild);
        // The master "should emotes work" context (CharSelect/Loading/Gameplay/...).
        DevStateRow("RTAPI GameState",  "%u (%s)", rt->GameState, gsName(rt->GameState));
        DevStateRow("char state bits",  "0x%02X", s);
        DevStateRow("  downed/sw/uw",   "%d/%d/%d", !!(s & CS_IsDowned),
                    !!(s & CS_IsSwimming), !!(s & CS_IsUnderwater));
        DevStateRow("  glide/fly/cmbt", "%d/%d/%d", !!(s & CS_IsGliding),
                    !!(s & CS_IsFlying), !!(s & CS_IsInCombat));
    }
    // Airborne (vertical) + moving (horizontal), from the airborne detector (core/
    // AirborneDetect) - the "[debug] Airborne tuner" overlay graphs/tunes against these.
    DevStateRow("vert vel m/s",      "%+.2f", air::VertSpeed());
    DevStateRow("airborne",          "%s", air::IsAirborne() ? "yes" : "no");
    DevStateRow("horiz spd m/s",     "%.2f", air::HorizSpeed());
    DevStateRow("moving",            "%s", air::IsMoving() ? "yes" : "no");
    DevStateRow("held printable",    "%d", HeldPrintableCount());
    DevStateRow("CurrentEmoteBlock", "%s", BlockName(CurrentEmoteBlock()));
    DevStateRow("g_QbBlockReason",   "%s", BlockName(g_QbBlockReason));
    DevStateRow("g_QbUnusableKey",   "%s", g_QbUnusableKey ? g_QbUnusableKey : "(usable)");
});
#endif  // EMOT3_DEVTOOLS
