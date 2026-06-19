#pragma once
// =====================================================================
//  Character-state gating: the single source of truth for "can the
//  player emote right now?" and "is the player in combat?".
//
//  Bridges two signals:
//   - MumbleLink (always present): mounted + in-combat, and - via the airborne
//     detector (core/AirborneDetect, derived from the avatar's height over time) -
//     airborne (jumps + falls; GW2 exposes no "in the air" flag).
//   - The optional GW2 RealTime API addon (RTAPI, a DataLink shared-memory block):
//     the states MumbleLink can't see - downed, swimming, underwater, gliding, flying.
//
//  This file owns the RTAPI plumbing + the GATING (which settings turn each signal into
//  a can't-emote reason); the airborne/moving DETECTION algorithm lives in its own file
//  (core/AirborneDetect) so it's easy to inspect and reuse. Centralising the gating here
//  keeps the send refusal (EmoteAction) and the Quickbar's grey/hide visual in agreement,
//  and degrades gracefully when RTAPI isn't installed (mounted + airborne still work).
// =====================================================================

#include <cstdint>

// Why the player currently can't emote. None = usable. Mounted comes from MumbleLink;
// Airborne (jumps + falls) is MumbleLink-derived (height velocity) under its own
// BlockWhileAirborne opt-in; the rest require RTAPI + the precise-detection opt-in.
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
// grey + dim + right-click hint + click feedback. Covers all sources when blocking
// is active (BlockUnusableEmotes): the game-state blocks (mounted / airborne /
// RTAPI states) plus the transient send refusals (text box focused, or moving / a
// key held, via CurrentSendBusy). The chosen presentation (QuickbarUnusableDisplay
// grey/hide/normal) applies to whichever fired. One key for every consumer = no
// scattered reason logic.
extern const char* g_QbUnusableKey;

// Resolve the RTAPI DataLink + subscribe to addon load/unload so the pointer
// stays correct. Call once from AddonLoad / AddonUnload respectively.
void InitCharacterState();
void ShutdownCharacterState();

// Per-frame update for the time-derived signals (airborne + moving). Call ONCE per
// gameplay frame from AddonRender, before anything reads CurrentEmoteBlock(). Delegates
// the detection to air::Tick (core/AirborneDetect); cheap (samples only on a fresh tick).
void TickCharacterState();

// True when the RealTime API addon is loaded and publishing live data (its
// DataLink exists AND GameBuild != 0 - RTAPI zeroes GameBuild on unload).
bool RTApiConnected();

// The RTAPI CharacterState bitfield (ECharacterState flags), or 0 when RTAPI isn't live.
// Lets the airborne detector read the glide/fly state for its fall-prime without owning
// the RTAPI plumbing (which stays here).
uint32_t RTApiCharacterStateBits();

// The current can't-emote reason, honoring settings: None unless BlockUnusableEmotes is
// on; Mounted whenever mounted; the RTAPI states only when PreciseStateDetection
// is on AND RTAPI is connected; Airborne (jumps + falls) when BlockWhileAirborne is
// on (its own toggle, MumbleLink-derived, RTAPI not required), reported only when no
// more-specific RTAPI state applies.
EmoteBlock CurrentEmoteBlock();

// True while the player is in combat (MumbleLink; no RTAPI needed). Drives the
// Quickbar's optional hide-in-combat - this is NOT a send block (emotes work
// in combat).
bool InCombatNow();

// True while the player is in a competitive game mode (PvP / WvW), from GW2's own
// MumbleLink IsCompetitive bit. UNLIKE the EmoteBlock reasons above, this is an
// UNCONDITIONAL compliance lockout with NO user setting: emot3 injects input to
// send emotes, which stays fully disabled in competitive. Consumed directly by the
// send gate (EmoteAction - refuses every surface) and the Quickbar (hides the bar).
bool InCompetitiveMode();

// True while the player is moving horizontally (any input, including mouse-walk),
// derived from MumbleLink position velocity by the airborne detector. Feeds the
// "grey / refuse while moving" gate alongside the held-key check, so mouse-only
// movement (which presses no key) is caught too. (Thin alias for air::IsMoving.)
bool MovementActive();

// i18n key for the toast / greyed-cell explainer matching a block reason.
const char* EmoteBlockKey(EmoteBlock reason);

// Dev diagnostic (not for end users): one-line summary of the raw RTAPI DataLink
// probe - which key resolves and the GameBuild it reads. Surfaced in the General
// options tab in dev builds only, to chase down "RealTime API not found" reports.
const char* RTApiDebugInfo();
