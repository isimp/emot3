#pragma once
// =====================================================================
//  Airborne + movement detection (MumbleLink-derived).
//
//  GW2 exposes no "in the air" flag, so this infers airborne (jumps + falls) and
//  "moving" purely from the avatar's world position over time. It's deliberately
//  self-contained so the algorithm is easy to inspect and copy: the per-frame
//  update reads the avatar position (MumbleLink) and produces IsAirborne() /
//  IsMoving(); the only outside signals are the optional RTAPI glide/fly state and
//  the MumbleLink mount index, used solely for the hand-off "fall prime" - the core
//  detection needs neither.
//
//  The GATING (which settings turn airborne into a can't-emote reason, and the
//  RTAPI-only states downed/swimming/gliding/flying) lives in CharacterState. This
//  file is just the signal. Watch it live via the dev "[debug] Airborne tuner".
//
//  THE MODEL - vertical-only, horizontal-independent (so it behaves the same whether
//  the "moving" send-gate is active or not; Plus "send while moving" drops that gate):
//   LaunchSpeed     - a raw upward vUp spike = a jump starting (instant engage).
//   HardRise/Fall   - vUpEMA past what a walkable surface produces = airborne. ASYMMETRIC
//   Speed             (+HardRiseSpeed for a jump rise, -HardFallSpeed for a fall) because
//                     the ground envelope is too (stairs-up only ~+5 m/s; steep stairs-down
//                     spike to ~-9). The fall side is high (deep falls only); shallower
//                     falls go through the debounced ballistic path below.
//   FallAccelDrop   - in the air the velocity accelerates downward at ~g the whole arc
//                     (rise/apex/fall); a surface holds it steady. So a sustained downward
//                     acceleration of the SMOOTHED velocity over AccelWindowSec engages a
//                     fall and holds the arc; FallEngageSec debounces it so a brief steep-
//                     stair spike (<~0.1s) is rejected while a real fall (>0.4s) passes.
//                     Measured as a change over a fixed window - never an instantaneous
//                     d/dt on raw position (that noise sank an earlier ballistic attempt).
//   Release          GroundSettleSpeed + SettleAccel ("vUp settled near 0 AND acceleration
//                     stopped") give a prompt, horizontal-independent landing; ReleaseSec is
//                     only the backstop timeout.
//   Prime            RTAPI glide/fly -> off, or a dismount (MountIndex -> None), open a
//                     PrimeFallSec window that relaxes the fall threshold (PrimeFallScale)
//                     and engages the instant you descend, so the mount/glide -> fall
//                     hand-off doesn't flicker the buttons usable.
//  De-noise: MumbleLink position is quantized and dt jitters, so a lone sample can read
//  hundreds of m/s (and the differentiated acceleration ~10x that). A median-of-3 on the
//  instantaneous velocity + a sanity clamp + the EMA tame it before the decision.
// =====================================================================

#include <cstdint>

namespace air {

// Per-frame update. Call ONCE per gameplay frame (from CharacterState::TickCharacterState),
// before anything reads the state. Samples only on a fresh MumbleLink game tick; cheap.
void Tick();

bool  IsAirborne();   // jump or fall this frame
bool  IsMoving();     // horizontal movement this frame (any input, incl. mouse-walk)
float VertSpeed();    // last vertical speed, m/s (+ up / - down)
float HorizSpeed();   // last horizontal speed, m/s

}  // namespace air

// ---- tuning thresholds -----------------------------------------------
// Read by air::Tick at every fresh game tick. In dev builds the getters return
// references to function-local statics so devtools/AirborneTuner's sliders can mutate
// them live; in shipped builds they're constexpr inline returning literals, so the
// compiler folds every call site to an immediate (no storage, no load). Defaults are
// CALIBRATED from in-game traces (jumps/falls/stairs/glide/mount). NOTE: a short fall and
// a steep short staircase are kinematically the same brief downward spike - the only
// separation is depth (HardFallSpeed sits in the ~2 m/s gap) + brevity (FallEngageSec).
namespace cs_constants {
#ifdef EMOT3_DEVTOOLS
inline float&  LaunchSpeed()       { static float  v = 7.0f; return v; }  // raw vUp spike -> airborne
inline float&  HardRiseSpeed()     { static float  v = 6.0f; return v; }  // +vUpEMA above this -> airborne (jump rise; > stairs-up ~+5)
inline float&  HardFallSpeed()     { static float  v = 10.0f; return v; } // -vUpEMA below this -> airborne INSTANTLY (deep fall; above steep stairs ~-9). Shallower falls go via the debounced ballistic path
inline float&  FallAccelDrop()     { static float  v = 3.5f; return v; }  // downward accel over window -> fall (> ground peaks)
inline double& AccelWindowSec()    { static double v = 0.16; return v; }  // window for the accel measure
inline float&  FallArmSpeed()      { static float  v = 2.0f; return v; }  // accel-fall noise floor (|vUpEMA| past this)
inline double& FallEngageSec()     { static double v = 0.15; return v; }  // ballistic debounce - rejects the brief stair spike
inline float&  SettleAccel()       { static float  v = 1.5f; return v; }  // |accel| below = acceleration stopped
inline float&  GroundSettleSpeed() { static float  v = 3.5f; return v; }  // |vUpEMA| below + settled = grounded (> walk ~2.7)
inline double& LandConfirmSec()    { static double v = 0.08; return v; }  // surface-consistent hold before release
inline double& ReleaseSec()        { static double v = 0.30; return v; }  // release backstop timeout
inline double& PrimeFallSec()      { static double v = 0.40; return v; }  // post glide/fly-off / dismount expect-fall window
inline float&  PrimeFallScale()    { static float  v = 0.6f; return v; }  // HardFallSpeed multiplier while primed
inline double& VelWindowSec()      { static double v = 0.04; return v; }  // EMA time-constant for vUp / horiz
inline float&  MoveSpeed()         { static float  v = 1.0f; return v; }  // horizontal "moving" threshold (separate signal)
inline double& MoveReleaseSec()    { static double v = 0.15; return v; }  // moving release debounce
inline float&  ClimbSlopeMax()     { static float  v = 0.0f; return v; }  // OPTIONAL horiz slope suppressor; 0 = off
#else
inline constexpr float  LaunchSpeed()       { return 7.0f; }
inline constexpr float  HardRiseSpeed()     { return 6.0f; }
inline constexpr float  HardFallSpeed()     { return 10.0f; }
inline constexpr float  FallAccelDrop()     { return 3.5f; }
inline constexpr double AccelWindowSec()    { return 0.16; }
inline constexpr float  FallArmSpeed()      { return 2.0f; }
inline constexpr double FallEngageSec()     { return 0.15; }
inline constexpr float  SettleAccel()       { return 1.5f; }
inline constexpr float  GroundSettleSpeed() { return 3.5f; }
inline constexpr double LandConfirmSec()    { return 0.08; }
inline constexpr double ReleaseSec()        { return 0.30; }
inline constexpr double PrimeFallSec()      { return 0.40; }
inline constexpr float  PrimeFallScale()    { return 0.6f; }
inline constexpr double VelWindowSec()      { return 0.04; }
inline constexpr float  MoveSpeed()         { return 1.0f; }
inline constexpr double MoveReleaseSec()    { return 0.15; }
inline constexpr float  ClimbSlopeMax()     { return 0.0f; }
#endif
}  // namespace cs_constants
