#pragma once
// =====================================================================
//  Character-state gating: the single source of truth for "can the
//  player emote right now?" and "is the player in combat?".
//
//  Bridges two signals:
//   - MumbleLink (always present): mounted + in-combat.
//   - The optional GW2 RealTime API addon (RTAPI, a DataLink shared-memory
//     block): the states MumbleLink can't see - downed, swimming,
//     underwater, gliding, flying.
//
//  Centralising it here keeps the send refusal (EmoteAction) and the
//  Quickbar's grey/hide visual in agreement, and degrades gracefully when
//  RTAPI isn't installed (mounted-only, exactly as before).
// =====================================================================

// Why the player currently can't emote. None = usable. Mounted comes from
// MumbleLink; the rest require RTAPI + the precise-detection opt-in.
enum class EmoteBlock {
    None = 0,
    Mounted,
    Downed,
    Swimming,
    Underwater,
    Gliding,
    Flying,
};

// The Quickbar's game-state block reason for the current frame, written by
// QuickbarRender (= CurrentEmoteBlock()). Kept ONLY to drive the hide decision
// (game states can hide the bar). None until the QB has evaluated once.
extern EmoteBlock g_QbBlockReason;

// The unified "why this Quickbar emote can't be used this frame" i18n key, or
// nullptr when usable. Written by QuickbarRender, read by RenderEmoteCell for the
// grey + dim + right-click hint + click feedback. Covers all ENABLED sources: the
// game-state blocks (mounted/...) plus - opt-in - the addon's transient send
// refusals (QuickbarUnusableTextbox / QuickbarUnusableMovement, via CurrentSendBusy).
// The chosen interaction (QuickbarUnusableBehavior grey/hide) applies to whichever
// fired. One key for every consumer = no scattered reason logic.
extern const char* g_QbUnusableKey;

// Resolve the RTAPI DataLink + subscribe to addon load/unload so the pointer
// stays correct. Call once from AddonLoad / AddonUnload respectively.
void InitCharacterState();
void ShutdownCharacterState();

// True when the RealTime API addon is loaded and publishing live data (its
// DataLink exists AND GameBuild != 0 - RTAPI zeroes GameBuild on unload).
bool RTApiConnected();

// The current can't-emote reason, honoring settings: None unless
// QuickbarGreyUnusable is on; Mounted whenever mounted; the RTAPI states only
// when QuickbarPreciseStateDetection is on AND RTAPI is connected.
EmoteBlock CurrentEmoteBlock();

// True while the player is in combat (MumbleLink; no RTAPI needed). Drives the
// Quickbar's optional hide-in-combat - this is NOT a send block (emotes work
// in combat).
bool InCombatNow();

// i18n key for the toast / greyed-cell explainer matching a block reason.
const char* EmoteBlockKey(EmoteBlock reason);

// Dev diagnostic (not for end users): one-line summary of the raw RTAPI DataLink
// probe - which key resolves and the GameBuild it reads. Surfaced in the General
// options tab in dev builds only, to chase down "RealTime API not found" reports.
const char* RTApiDebugInfo();
