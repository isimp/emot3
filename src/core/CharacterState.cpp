#include "CharacterState.h"

#include "Globals.h"    // APIDefs, MumbleLink
#include "Settings.h"   // g_Settings
#include "Logging.h"
#include "Profiling.h"  // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)

#include <cstdio>       // snprintf (RTApiDebugInfo)
#include <cmath>        // std::fabs (falling-speed guard)
#include <chrono>       // steady_clock (monotonic time; replaces ImGui::GetTime)

#include "rtapi/RTAPI.h" // RealTimeData, ECharacterState, DL_RTAPI (vendored)

#ifdef EMOT3_DEVTOOLS
#include "AirborneTuner.h"  // airtuner::OnSample (per-tick callback)
#endif

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

// ---- Airborne + moving (MumbleLink-derived) ---------------------------
// GW2 exposes no airborne flag, so we infer it from the avatar's height velocity
// (a jump launches up, a fall accelerates down; flat ground ~0). We read the FULL
// position each tick, so horizontal speed is free too - and it catches "moving" by
// ANY input, including mouse-walk that the held-key gate can't see. TickCharacterState
// (once per gameplay frame) measures both; CurrentEmoteBlock / CurrentSendBusy / the
// dev readout read them. (Unmount is NOT special-cased: its up-then-down hop is a
// real airborne trajectory and greys like any jump - mounted -> airborne -> land.)
bool  s_airborne   = false;  // in the air this frame (jump or fall)
float s_vertVel    = 0.f;    // last vertical speed, m/s (+ up / - down)
bool  s_moving     = false;  // horizontal movement this frame (any input)
float s_horizSpeed = 0.f;    // last horizontal speed, m/s

// Tuning (watch "vert vel" / "horiz spd" in the dev Game-state readout). kAirSpeed:
// above the vertical speed slopes/stairs produce, below a jump launch / real fall.
// Engage is asymmetric - RISING fast (jump) engages immediately; FALLING fast (off a
// ledge) waits kFallEngageTicks so a single-tick stair/curb drop can't engage.
// Release is direction-aware: while still rising into the apex we HOLD, and only run
// kReleaseSec once descending/settled - so the apex never flashes the buttons usable,
// at any kAirSpeed. kReleaseSec must exceed the time to fall from 0 to kAirSpeed
// (~kAirSpeed / GW2 gravity), so raise it alongside kAirSpeed. kMoveSpeed: above
// standing jitter, below a walk; kMoveReleaseSec keeps a step-pause from flickering.
// Tunable thresholds moved to CharacterState.h's cs_constants namespace so the
// dev-only Airborne tuner (devtools/AirborneTuner) can mutate them live; in
// shipped builds the getters are constexpr inline returning literals (compiler
// folds them to immediate constants, codegen identical to the old constexpr
// block here). Defaults documented at the declaration.

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

void TickCharacterState() {
    PROFILE_SCOPE("cs.tick");  // dev perf overlay (should read ~0 - no syscalls)
    // Sample the avatar's height only when MumbleLink reports a fresh game tick
    // (UITick), so the speed reflects the game's update cadence rather than our
    // render rate. The dt / teleport guards drop loading-screen stalls and map-jump
    // spikes (either would fake an enormous velocity).
    static bool     have        = false;
    static unsigned lastUITick  = 0;
    static float    lastX = 0.f, lastY = 0.f, lastZ = 0.f;
    static double   lastT       = 0.0;
    static int      downRun     = 0;     // consecutive fast-descending ticks (legacy)
    static int      upRun       = 0;     // consecutive unexplained fast-rising ticks (legacy)
    static double   groundSince = -1.0;  // legacy airborne release timer
    static double   stillSince  = -1.0;  // when horizontal motion dropped (move release)
    // Two airborne detectors run in parallel; DetectorMode picks which drives
    // s_airborne. legacy* = threshold+slope+debounce; ball* = ballistic (aUp ~ -g).
    static bool     legacyAir   = false;
    static bool     ballAir     = false;
    static float    prevVUp     = 0.f;   // last tick's vUp, for aUp = d(vUp)/dt
    static int      ballFall    = 0;     // consecutive free-fall (aUp ~ -g) ticks
    static double   ballGround  = -1.0;  // ballistic airborne release timer

    if (!MumbleLink) {
        have = false; downRun = 0; upRun = 0; groundSince = -1.0; stillSince = -1.0;
        legacyAir = false; ballAir = false; prevVUp = 0.f; ballFall = 0; ballGround = -1.0;
        s_airborne = false; s_vertVel = 0.f; s_moving = false; s_horizSpeed = 0.f;
        return;
    }

    const unsigned tick = MumbleLink->UITick;
    if (tick == lastUITick) return;   // no fresh game data this frame

    // Monotonic seconds (steady_clock; was ImGui::GetTime - keeps core/CharacterState
    // imgui-free). Used only for self-consistent deltas, so the arbitrary epoch is fine.
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const float  x = MumbleLink->AvatarPosition.X;
    const float  y = MumbleLink->AvatarPosition.Y;
    const float  z = MumbleLink->AvatarPosition.Z;
    if (have) {
        const double dt        = now - lastT;
        const float  dy        = y - lastY;
        const float  horizDist = std::sqrt((x - lastX) * (x - lastX) +
                                           (z - lastZ) * (z - lastZ));
        // Guards: drop stalls (dt huge) and teleport / map-load spikes (a big
        // vertical or horizontal jump in one tick) - either would fake a velocity.
        if (dt > 0.0001 && dt < 0.5 && std::fabs(dy) < 50.f && horizDist < 100.f) {
            // ---- vertical: airborne (jumps + falls) ----
            const float vUp   = dy / (float)dt;
            const float horiz = horizDist / (float)dt;   // also the moving signal below
            const float aUp   = (vUp - prevVUp) / (float)dt;  // vertical acceleration
            s_vertVel = vUp;

            // -- Detector A (legacy): velocity threshold + slope-gated rise + debounce.
            // A rise only counts toward a LAUNCH when it's fast AND not "explained" by
            // climbing the surface (vUp > horiz * kClimbSlopeMax); a fall debounces.
            {
                const bool launchTick = vUp > cs_constants::AirSpeed() &&
                                        vUp > horiz * cs_constants::ClimbSlopeMax();
                if (launchTick) {
                    if (++upRun >= cs_constants::RiseEngageTicks()) {
                        legacyAir = true; groundSince = -1.0;
                    }
                    downRun = 0;
                } else if (vUp < -cs_constants::AirSpeed()) {
                    if (++downRun >= cs_constants::FallEngageTicks()) legacyAir = true;
                    upRun = 0; groundSince = -1.0;
                } else {
                    upRun = 0; downRun = 0;
                    if (legacyAir) {
                        if (vUp > 0.f && vUp <= cs_constants::AirSpeed())
                            groundSince = -1.0;                  // genuine apex rise: hold
                        else {
                            if (groundSince < 0.0) groundSince = now;
                            if (now - groundSince >= cs_constants::ReleaseSec()) legacyAir = false;
                        }
                    }
                }
            }

            // -- Detector B (ballistic): a jump is free flight, so vertical accel ~ -g
            // for the whole arc (rise->apex->fall), decoupled from terrain. Stairs/ramps
            // never sustain -g, and their per-step accel is far below a jump's takeoff
            // impulse. Engage on the takeoff impulse OR sustained free-fall; release once
            // grounded. Knobs measured/tuned live (kGravity, kGravityTol, kLaunchAccel).
            {
                const float g   = cs_constants::Gravity();
                const float tol = cs_constants::GravityTol() * g;
                const bool freeFalling = std::fabs(aUp + g) <= tol;     // aUp ~ -g
                if (aUp > cs_constants::LaunchAccel() && vUp > 0.f) {
                    // Takeoff impulse (sudden upward accel, now moving up). vUp>0
                    // excludes the mirror landing spike (big +aUp while vUp <= 0).
                    ballAir = true; ballFall = 0; ballGround = -1.0;
                } else if (freeFalling) {
                    if (++ballFall >= cs_constants::FallEngageTicks()) ballAir = true;
                    ballGround = -1.0;
                } else {
                    ballFall = 0;
                    if (ballAir) {
                        if (vUp > 0.f) ballGround = -1.0;              // still ascending: hold
                        else {
                            if (ballGround < 0.0) ballGround = now;
                            if (now - ballGround >= cs_constants::ReleaseSec()) ballAir = false;
                        }
                    }
                }
            }

            // Publish the selected detector (dev-switchable; shipped = legacy).
            s_airborne = (cs_constants::DetectorMode() == 1) ? ballAir : legacyAir;
            prevVUp = vUp;

            // ---- horizontal: moving (any input, incl. mouse-walk) ----
            s_horizSpeed = horiz;
            if (s_horizSpeed > cs_constants::MoveSpeed()) { s_moving = true; stillSince = -1.0; }
            else if (s_moving) {
                if (stillSince < 0.0) stillSince = now;
                if (now - stillSince >= cs_constants::MoveReleaseSec()) s_moving = false;
            }
#ifdef EMOT3_DEVTOOLS
            // Feed the Airborne tuner overlay (dev-only) - rings, live readouts,
            // event-log edge detection. Free in non-dev builds (whole block + the
            // include compile out).
            airtuner::OnSample({
                s_vertVel, s_horizSpeed, y, (float)dt,
                tick - lastUITick,
                s_airborne, s_moving,
                downRun, upRun, groundSince, stillSince, now,
                aUp, legacyAir, ballAir, ballFall
            });
#endif
        } else {
            // Stall (loading / alt-tab) or teleport spike: re-baseline.
            s_airborne = false; s_vertVel = 0.f; downRun = 0; upRun = 0; groundSince = -1.0;
            s_moving = false; s_horizSpeed = 0.f; stillSince = -1.0;
            legacyAir = false; ballAir = false; prevVUp = 0.f; ballFall = 0; ballGround = -1.0;
        }
    }
    lastUITick = tick; lastX = x; lastY = y; lastZ = z; lastT = now; have = true;
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

    // Airborne (jump or fall): precise-detection opt-in, but MumbleLink-derived -
    // works without RTAPI. Checked after the RTAPI states so a controlled descent
    // (gliding / flying) or a water state keeps its own, more specific reason.
    if (g_Settings.QuickbarPreciseStateDetection && s_airborne)
        return EmoteBlock::Airborne;
    return EmoteBlock::None;
}

bool InCombatNow() {
    return MumbleLink && MumbleLink->Context.IsInCombat;
}

bool MovementActive() { return s_moving; }

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
    // Airborne (vertical) + moving (horizontal), from MumbleLink position velocity
    // - tune kAirSpeed / kMoveSpeed against these.
    DevStateRow("vert vel m/s",      "%+.2f", s_vertVel);
    DevStateRow("airborne",          "%s", s_airborne ? "yes" : "no");
    DevStateRow("horiz spd m/s",     "%.2f", s_horizSpeed);
    DevStateRow("moving",            "%s", s_moving ? "yes" : "no");
    DevStateRow("held printable",    "%d", HeldPrintableCount());
    DevStateRow("CurrentEmoteBlock", "%s", BlockName(CurrentEmoteBlock()));
    DevStateRow("g_QbBlockReason",   "%s", BlockName(g_QbBlockReason));
    DevStateRow("g_QbUnusableKey",   "%s", g_QbUnusableKey ? g_QbUnusableKey : "(usable)");
});
#endif  // EMOT3_DEVTOOLS
