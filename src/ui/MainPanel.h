#pragma once

// The main emot3 window — toolbar (filter, view, scale, Quickbar toggle),
// search bar, "+ Category" inline editor, and the scrollable list of
// favorites + Core + Unlockable sections.

void AddonRender();

// New-bundled-emote notifier detection. Diffs the embedded table against the
// user's snapshot (g_Settings.KnownBundledEmotes) and stages the first-run-style
// prompt (g_PromptNewBundledEmotes / g_NewBundledEmoteIds) when notifications are
// on and there's something to offer. Respects g_Settings.NotifyNewBundledEmotes
// (off = no prompt, snapshot left untouched so re-enabling later still surfaces
// what was added). Called once at AddonLoad; also re-used by the dev notifier
// tool so its "simulate" path exercises the exact gating, not a bypass. See
// emot3.md "Bundled-emote notifier".
void DetectNewBundledEmotes();

// Texture priming. Both are idempotent: re-running just hits Nexus'
// texture cache (cheap). LoadEmoteTextures re-runs whenever the
// catalog changes (g_EmotesDirty), so AddonRender still calls it for
// reload cases; we also call it from AddonLoad so the Quickbar has
// textures the moment it renders, instead of waiting for the main
// window to be opened at least once.
void LoadEmoteTextures();
void LoadUiIconOverrides();
