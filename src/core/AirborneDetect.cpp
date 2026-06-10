#include "AirborneDetect.h"

#include "Globals.h"        // MumbleLink (avatar position + mount index)
#include "CharacterState.h" // RTApiConnected / RTApiCharacterStateBits (RTAPI plumbing lives there)

#include <cmath>            // std::fabs, std::sqrt
#include <chrono>           // steady_clock (monotonic time; keeps this imgui-free)
#include <cstdint>

#include "rtapi/RTAPI.h"    // CS_IsGliding / CS_IsFlying (bit masks for the fall-prime edge)

#ifdef EMOT3_DEVTOOLS
#include "AirborneTuner.h"  // airtuner::OnSample (per-tick dev overlay callback)
#endif

namespace {
// The detection output (also the readable state). Written by air::Tick, read by the
// accessors below + CharacterState's gating (CurrentEmoteBlock / MovementActive).
bool  s_airborne   = false;  // in the air this frame (jump or fall)
float s_vertVel    = 0.f;    // last vertical speed, m/s (+ up / - down)
bool  s_moving     = false;  // horizontal movement this frame (any input)
float s_horizSpeed = 0.f;    // last horizontal speed, m/s
}  // namespace

namespace air {

void Tick() {
    // Sample the avatar's height only when MumbleLink reports a fresh game tick
    // (UITick), so the speed reflects the game's update cadence rather than our
    // render rate. The dt / teleport guards drop loading-screen stalls and map-jump
    // spikes (either would fake an enormous velocity).
    static bool     have        = false;
    static unsigned lastUITick  = 0;
    static float    lastX = 0.f, lastY = 0.f, lastZ = 0.f;
    static double   lastT       = 0.0;
    static double   groundSince = -1.0;  // release backstop timer
    static double   stillSince  = -1.0;  // move release timer
    static double   fallSince   = -1.0;  // ballistic-fall engage debounce
    static double   landSince   = -1.0;  // surface-consistent landing confirm
    static float    vUpEMA      = 0.f;   // smoothed vertical velocity (VelWindowSec)
    static float    horizEMA    = 0.f;   // smoothed horizontal speed
    static float    vMed1 = 0.f, vMed2 = 0.f;  // last 2 instantaneous vUp (median-of-3 impulse filter)
    static float    hMed1 = 0.f, hMed2 = 0.f;  // last 2 instantaneous horiz
    // Smoothed-vUp history (timestamps + values) for the windowed acceleration that
    // drives the ballistic engage/hold/release - a change over a fixed time span, not
    // an instantaneous d/dt on raw position (that was the noise the earlier ballistic
    // detector tripped on).
    static const int kVRing = 16;
    static float    vRingV[kVRing] = {};
    static double   vRingT[kVRing] = {};
    static int      vRingHead = 0, vRingCount = 0;
    static uint32_t prevCS = 0;             // last tick's RTAPI bits (glide/fly edge detect)
    static unsigned prevMount = 0;          // last tick's MountIndex (dismount edge detect)
    static double   primeFallUntil = -1.0;  // expect-fall window deadline (-1 = inactive)

    if (!MumbleLink) {
        have = false;
        groundSince = stillSince = fallSince = landSince = -1.0;
        vUpEMA = horizEMA = 0.f;
        vRingHead = vRingCount = 0; prevCS = prevMount = 0; primeFallUntil = -1.0;
        vMed1 = vMed2 = hMed1 = hMed2 = 0.f;
        s_airborne = false; s_vertVel = 0.f; s_moving = false; s_horizSpeed = 0.f;
        return;
    }

    const unsigned tick = MumbleLink->UITick;
    if (tick == lastUITick) return;   // no fresh game data this frame

    // Monotonic seconds (steady_clock; not ImGui::GetTime - keeps core imgui-free).
    // Used only for self-consistent deltas, so the arbitrary epoch is fine.
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const float  x = MumbleLink->AvatarPosition.X;
    const float  y = MumbleLink->AvatarPosition.Y;
    const float  z = MumbleLink->AvatarPosition.Z;
    if (have) {
        const double dt        = now - lastT;
        // Too-fine sample: a sub-8ms gap means we sampled twice within one game tick
        // (dt jitter). Dividing by it fabricates a huge velocity, so wait and accumulate
        // - don't update last*, so the next tick sees the full dt/dy. This is a SKIP, not
        // a stall, so we don't re-baseline.
        if (dt > 0.0 && dt < 0.008) return;
        const float  dy        = y - lastY;
        const float  horizDist = std::sqrt((x - lastX) * (x - lastX) +
                                           (z - lastZ) * (z - lastZ));
        // Guards: drop stalls (dt huge) and teleport / map-load spikes (a big
        // vertical or horizontal jump in one tick) - either would fake a velocity.
        if (dt >= 0.008 && dt < 0.5 && std::fabs(dy) < 50.f && horizDist < 100.f) {
            // ---- vertical: airborne (jumps + falls), horizontal-INDEPENDENT ----
            // Instantaneous velocity, de-spiked. MumbleLink position is quantized and dt
            // jitters, so a lone sample can read hundreds of m/s; the (differentiated)
            // acceleration then reads ~10x too high (g measured at ~245 vs GW2's ~20).
            // A median-of-3 rejects single-tick spikes the EMA only smears, and a sanity
            // clamp caps anything past a real GW2 fall before it enters the EMA.
            float vInst = dy / (float)dt;
            float hInst = horizDist / (float)dt;
            if (vInst >  60.f) vInst =  60.f; else if (vInst < -60.f) vInst = -60.f;
            if (hInst >  50.f) hInst =  50.f;
            auto med3 = [](float a, float b, float c) {
                return a < b ? (b < c ? b : (a < c ? c : a)) : (a < c ? a : (b < c ? c : b));
            };
            const float vUp   = med3(vInst, vMed1, vMed2);
            const float horiz = med3(hInst, hMed1, hMed2);
            vMed2 = vMed1; vMed1 = vInst; hMed2 = hMed1; hMed1 = hInst;
            // EMA-smoothed velocity; alpha from the time-constant so it's framerate-correct.
            // VelWindowSec == 0 -> raw (median only).
            const float win   = (float)cs_constants::VelWindowSec();
            const float alpha = win > 0.f ? (float)(dt / (win + dt)) : 1.f;
            vUpEMA   += alpha * (vUp   - vUpEMA);
            horizEMA += alpha * (horiz - horizEMA);
            s_vertVel = vUp;                             // de-spiked, for readout

            // Windowed vertical acceleration = change in the SMOOTHED velocity over
            // AccelWindowSec. The ballistic signature: in the air vUp accelerates
            // downward the whole arc (rise/apex/fall), so accel stays < 0; a surface
            // holds vUp steady (flat ~0, stairs/ramp ~constant), so accel sits ~0.
            // A change over a fixed span - never an instantaneous d/dt on raw position.
            vRingV[vRingHead] = vUpEMA; vRingT[vRingHead] = now;
            vRingHead = (vRingHead + 1) % kVRing;
            if (vRingCount < kVRing) ++vRingCount;
            const double awin = cs_constants::AccelWindowSec();
            float  vWindowAgo = vUpEMA; double bestAge = -1.0;
            for (int i = 0; i < vRingCount; ++i) {
                const double age = now - vRingT[i];     // sample closest to exactly a window ago
                if (age >= awin && (bestAge < 0.0 || age < bestAge)) { bestAge = age; vWindowAgo = vRingV[i]; }
            }
            if (bestAge < 0.0) {  // ring doesn't span the window yet: use the oldest sample
                const int oldest = (vRingHead - vRingCount + kVRing) % kVRing;
                vWindowAgo = vRingV[oldest];
            }
            const float accel = vUpEMA - vWindowAgo;    // <= 0 => decelerating / falling

            // Prime an "expect-fall" window on the edges that hand off INTO a fall: a
            // glide/fly state ending (RTAPI) OR a dismount (MumbleLink MountIndex -> None,
            // works without RTAPI). Without priming the hand-off flickers - the mount/glide
            // block ends a beat before the fall builds enough speed to engage. The RTAPI
            // bits come via CharacterState's accessor (0 when no RTAPI), so the core stays
            // addon-agnostic.
            const bool     rtapiLive = RTApiConnected();
            const uint32_t cs        = RTApiCharacterStateBits();   // 0 if RTAPI not live
            const bool     gliding   = (cs & CS_IsGliding) != 0;
            const bool     flying    = (cs & CS_IsFlying)  != 0;
            const uint32_t airMask   = CS_IsGliding | CS_IsFlying;
            const unsigned curMount  = (unsigned)MumbleLink->Context.MountIndex;
            const bool glideFlyOff   = (prevCS & airMask) && !(cs & airMask);   // false without RTAPI (cs=0)
            const bool dismount      = (prevMount != 0u) && (curMount == 0u);
            if (glideFlyOff || dismount) primeFallUntil = now + cs_constants::PrimeFallSec();
            prevCS = cs; prevMount = curMount;
            const bool   primed      = (primeFallUntil >= 0.0 && now <= primeFallUntil);
            const float  effHardFall = primed ? cs_constants::HardFallSpeed() * cs_constants::PrimeFallScale()
                                              : cs_constants::HardFallSpeed();
            const double effFallEng  = primed ? 0.0 : cs_constants::FallEngageSec();

            // Optional, default-OFF horizontal slope suppressor (ClimbSlopeMax == 0 =>
            // off). The airborne decision is horizontal-independent by default, so it
            // behaves the same whether the "moving" send-gate is active or not. Only
            // enable this if calibration shows vertical-only can't split steep stairs.
            const float slope = cs_constants::ClimbSlopeMax();
            const bool slopeExplained = slope > 0.f && std::fabs(vUpEMA) <= horizEMA * slope;

            // ENGAGE - all vertical, horizontal-independent.
            // Up-spike on the smoothed velocity (jump onset). During the prime window also
            // accept the INSTANTANEOUS (pre-median) spike: a dismount pops you up for a
            // single tick that the median-of-3 smooths away, leaving a 1-tick usable flash
            // between the mount block lifting and airborne engaging. Trusting the raw value
            // only while primed keeps ground quantization noise out.
            const bool launch    = vUp >= cs_constants::LaunchSpeed() ||
                                   (primed && vInst >= cs_constants::LaunchSpeed());
            // Asymmetric hard band: rising past HardRiseSpeed (jump) OR falling past
            // HardFallSpeed. The two sit in different ground gaps (stairs-up ~+5 vs
            // stairs-down ~-9), so neither false-fires on a staircase; the fall side is
            // what RTAPI priming relaxes.
            const bool hardBand  = vUpEMA >= cs_constants::HardRiseSpeed() || vUpEMA <= -effHardFall;
            // Ballistic = sustained downward acceleration at ~g, in EITHER velocity
            // direction. A jump's RISE is ballistic too (moving up while decelerating at g);
            // gating on downward velocity missed the whole up-half (jumps barely greyed).
            // Stairs/ramps move at ~constant velocity (accel ~ 0), so ACCELERATION - not
            // velocity sign - separates a jump from a staircase. The |vUpEMA| floor only
            // rejects near-zero apex/standing noise.
            const bool ballistic = (-accel) >= cs_constants::FallAccelDrop() &&
                                   std::fabs(vUpEMA) >= cs_constants::FallArmSpeed();
            // During the prime window (post glide/fly-off or dismount) a fall is expected:
            // engage the instant we're clearly descending, instead of waiting for the fall
            // to build up to the hard band. A landing decelerates (vUpEMA climbs back toward
            // 0) so it won't trip this - closes the glide-cancel / dismount hand-off flicker.
            const bool primedFall = primed && vUpEMA <= -cs_constants::FallArmSpeed();
            const bool airborneNow = launch || hardBand || ballistic || primedFall;  // tuner rise/fall dots
            const bool rise = airborneNow && vUpEMA > 0.f;
            const bool fall = airborneNow && vUpEMA <= 0.f;

            if ((launch || hardBand || primedFall) && !slopeExplained) {
                s_airborne = true; fallSince = -1.0; groundSince = landSince = -1.0;
            } else if (ballistic && !slopeExplained) {
                if (fallSince < 0.0) fallSince = now;
                if (now - fallSince >= effFallEng) { s_airborne = true; groundSince = landSince = -1.0; }
            } else if (s_airborne) {
                fallSince = -1.0;
                // HOLD the arc while still ballistic; RELEASE when the motion is
                // surface-consistent (vUp settled near 0 AND acceleration stopped), else
                // fall back to the ReleaseSec timeout (e.g. landing onto a down-slope).
                const bool surfaceConsistent =
                    std::fabs(vUpEMA) < cs_constants::GroundSettleSpeed() &&
                    std::fabs(accel)  < cs_constants::SettleAccel();
                if (surfaceConsistent && !hardBand) {
                    if (landSince < 0.0) landSince = now;
                    if (now - landSince >= cs_constants::LandConfirmSec()) {
                        s_airborne = false; groundSince = landSince = -1.0;
                    }
                } else if (primed) {
                    // Inside the expect-fall window a brief not-yet-settled stretch is the
                    // dismount/glide-cancel APEX (arcing from up to down), not a landing -
                    // hold through it instead of timing out (which flickered the buttons
                    // usable mid-apex). A real landing still releases via the branch above.
                    groundSince = landSince = -1.0;
                } else {
                    landSince = -1.0;
                    if (groundSince < 0.0) groundSince = now;
                    if (now - groundSince >= cs_constants::ReleaseSec()) s_airborne = false;
                }
            } else {
                fallSince = groundSince = landSince = -1.0;
            }

            // ---- horizontal: moving (smoothed; catches mouse-walk). SEPARATE signal -
            // the airborne decision above never reads it, so airborne is identical with
            // or without the move send-gate. ----
            s_horizSpeed = horizEMA;
            if (s_horizSpeed > cs_constants::MoveSpeed()) { s_moving = true; stillSince = -1.0; }
            else if (s_moving) {
                if (stillSince < 0.0) stillSince = now;
                if (now - stillSince >= cs_constants::MoveReleaseSec()) s_moving = false;
            }
#ifdef EMOT3_DEVTOOLS
            const bool b_surface = std::fabs(vUpEMA) < cs_constants::GroundSettleSpeed() &&
                                   std::fabs(accel)  < cs_constants::SettleAccel();
            // Feed the Airborne tuner overlay (dev-only). Free in non-dev builds (the
            // whole block + the include compile out).
            airtuner::OnSample({
                s_vertVel, vUpEMA, s_horizSpeed, y, (float)dt,
                tick - lastUITick,
                s_airborne, s_moving, rise, fall,
                landSince, fallSince, groundSince, stillSince, now,
                accel, launch, hardBand, ballistic, b_surface,
                primed, gliding, flying, rtapiLive, vInst
            });
#else
            (void)rise; (void)fall; (void)gliding; (void)flying; (void)rtapiLive;
#endif
        } else {
            // Stall (loading / alt-tab) or teleport spike: re-baseline.
            s_airborne = false; s_vertVel = 0.f; s_moving = false; s_horizSpeed = 0.f;
            groundSince = stillSince = fallSince = landSince = -1.0;
            vUpEMA = horizEMA = 0.f;
            vRingHead = vRingCount = 0; prevCS = prevMount = 0; primeFallUntil = -1.0;
            vMed1 = vMed2 = hMed1 = hMed2 = 0.f;
        }
    }
    lastUITick = tick; lastX = x; lastY = y; lastZ = z; lastT = now; have = true;
}

bool  IsAirborne() { return s_airborne; }
bool  IsMoving()   { return s_moving; }
float VertSpeed()  { return s_vertVel; }
float HorizSpeed() { return s_horizSpeed; }

}  // namespace air
