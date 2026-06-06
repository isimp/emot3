#pragma once
// =====================================================================
//  Character-state gating: the single source of truth for "can the
//  player emote right now?" and "is the player in combat?".
//
//  Bridges two signals:
//   - MumbleLink (always present): mounted + in-combat, and - derived from the
//     avatar's height over time - airborne (jumps + falls; GW2 exposes no
//     "in the air" flag, so we measure vertical speed; see TickCharacterState).
//   - The optional GW2 RealTime API addon (RTAPI, a DataLink shared-memory
//     block): the states MumbleLink can't see - downed, swimming,
//     underwater, gliding, flying.
//
//  Centralising it here keeps the send refusal (EmoteAction) and the
//  Quickbar's grey/hide visual in agreement, and degrades gracefully when
//  RTAPI isn't installed (mounted-only, exactly as before).
// =====================================================================

// Why the player currently can't emote. None = usable. Mounted comes from
// MumbleLink; Airborne (jumps + falls) is MumbleLink-derived (height velocity)
// under the precise-detection opt-in; the rest require RTAPI + that opt-in.
enum class EmoteBlock {
    None = 0,
    Mounted,
    Downed,
    Swimming,
    Underwater,
    Gliding,
    Flying,
    Airborne,
};

// The Quickbar's game-state block reason for the current frame, written by
// QuickbarRender (= CurrentEmoteBlock()). Kept ONLY to drive the hide decision
// (game states can hide the bar). None until the QB has evaluated once.
extern EmoteBlock g_QbBlockReason;

// The unified "why this Quickbar emote can't be used this frame" i18n key, or
// nullptr when usable. Written by QuickbarRender, read by RenderEmoteCell for the
// grey + dim + right-click hint + click feedback. Covers all sources when greying
// is active (QuickbarGreyUnusable): the game-state blocks (mounted / airborne /
// RTAPI states) plus the transient send refusals (text box focused, or moving / a
// key held, via CurrentSendBusy). The chosen interaction (QuickbarUnusableBehavior grey/hide) applies to whichever
// fired. One key for every consumer = no scattered reason logic.
extern const char* g_QbUnusableKey;

// Resolve the RTAPI DataLink + subscribe to addon load/unload so the pointer
// stays correct. Call once from AddonLoad / AddonUnload respectively.
void InitCharacterState();
void ShutdownCharacterState();

// Per-frame update for the time-derived signals (currently the falling check:
// vertical speed from MumbleLink's avatar height). Call ONCE per gameplay frame
// from AddonRender, before anything reads CurrentEmoteBlock(). Cheap; samples
// only when MumbleLink reports a fresh game tick.
void TickCharacterState();

// True when the RealTime API addon is loaded and publishing live data (its
// DataLink exists AND GameBuild != 0 - RTAPI zeroes GameBuild on unload).
bool RTApiConnected();

// The current can't-emote reason, honoring settings: None unless
// QuickbarGreyUnusable is on; Mounted whenever mounted; the RTAPI states only
// when QuickbarPreciseStateDetection is on AND RTAPI is connected; Airborne (jumps
// + falls) when QuickbarPreciseStateDetection is on (MumbleLink-derived, RTAPI not
// required), reported only when no more-specific RTAPI state applies.
EmoteBlock CurrentEmoteBlock();

// True while the player is in combat (MumbleLink; no RTAPI needed). Drives the
// Quickbar's optional hide-in-combat - this is NOT a send block (emotes work
// in combat).
bool InCombatNow();

// True while the player is moving horizontally (any input, including mouse-walk),
// derived from MumbleLink position velocity by TickCharacterState. Feeds the
// "grey / refuse while moving" gate alongside the held-key check, so mouse-only
// movement (which presses no key) is caught too.
bool MovementActive();

// i18n key for the toast / greyed-cell explainer matching a block reason.
const char* EmoteBlockKey(EmoteBlock reason);

// Dev diagnostic (not for end users): one-line summary of the raw RTAPI DataLink
// probe - which key resolves and the GameBuild it reads. Surfaced in the General
// options tab in dev builds only, to chase down "RealTime API not found" reports.
const char* RTApiDebugInfo();

// Airborne / movement detection thresholds. Read by TickCharacterState
// (CharacterState.cpp) at every fresh game tick. In dev builds the getters
// return references to function-local statics so devtools/AirborneTuner's
// sliders can mutate them live; in shipped builds they're constexpr inline
// returning literals, so the compiler folds every call site to an immediate
// (codegen identical to the previous file-static constexpr block).
//
// kAirSpeed       - |vUp| (m/s) above which a tick counts as airborne.
//                   Below: slopes/stairs/walking jitter; above: jump launch or
//                   a real fall.
// kFallEngageTicks - fast-descending ticks before a FALL engages (jumps
//                    engage immediately - this only debounces the fall case).
// kReleaseSec      - descending/settled time before clearing airborne. Must
//                    exceed the time to fall from 0 to kAirSpeed at GW2
//                    gravity, so raise this alongside kAirSpeed.
// kMoveSpeed       - horizontal m/s above which a tick counts as moving.
//                    Catches mouse-walk (which presses no key).
// kMoveReleaseSec  - below-threshold time before "stopped" (anti-flicker).
namespace cs_constants {
#ifdef EMOT3_DEVTOOLS
inline float&  AirSpeed()        { static float  v = 3.5f; return v; }
inline int&    FallEngageTicks() { static int    v = 2;    return v; }
inline double& ReleaseSec()      { static double v = 0.25; return v; }
inline float&  MoveSpeed()       { static float  v = 1.0f; return v; }
inline double& MoveReleaseSec()  { static double v = 0.15; return v; }
#else
inline constexpr float  AirSpeed()        { return 3.5f; }
inline constexpr int    FallEngageTicks() { return 2;    }
inline constexpr double ReleaseSec()      { return 0.25; }
inline constexpr float  MoveSpeed()       { return 1.0f; }
inline constexpr double MoveReleaseSec()  { return 0.15; }
#endif
}  // namespace cs_constants
