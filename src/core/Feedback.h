#pragma once
// =====================================================================
//  In-window refusal feedback (replaces Nexus' SendAlert toasts).
//
//  WHY NOT NEXUS ALERTS:
//  Nexus' UI.SendAlert(const char*) takes only a message - no duration,
//  no id, no replace semantics (see vendor/nexus/Nexus.h). Nexus alone
//  decides how long an alert lingers and how alerts stack, so emot3's
//  refusal messages showed "for an eternity" and monopolised the shared
//  alert area, delaying other addons' toasts. There is no API knob to fix
//  that, so we render our own feedback instead.
//
//  THE MODEL:
//  A short, self-fading reason line that pops up AT THE CLICK - anchored to
//  the cursor position captured when the refusal fired - so your eye is
//  already there (a bottom-docked line was easy to miss when you clicked an
//  emote at the top of a big grid). It's painted on the FOREGROUND draw list
//  (GetForegroundDrawList = rendered last) so it sits on top of the emote
//  grid; the grid lives in a child window that composites above the parent,
//  so a plain window-draw-list overlay would land BEHIND the cells. It
//  reserves no layout (pure draw-list), so nothing perturbs the Quickbar
//  fit-to-grid snap math. Single slot: the newest message wins immediately;
//  an identical repeat (mashing a blocked cell) just refreshes the fade timer
//  - it never stacks. Position is clamped to the viewport so it's never
//  half-cut on a small bar (it may overhang the window edges - readability
//  beats staying strictly inside a 2-icon bar).
//
//  LOOK & FEEL (coherent on purpose):
//  It reads as ImGui's own tooltip/popup idiom - an ImGuiCol_PopupBg panel
//  with a 1px ImGuiCol_Border - NOT like the thin grey scrollbar grab. All
//  colours come from the active theme via GetColorU32 and metrics from
//  ImGui::GetStyle() (FrameRounding / FramePadding), so it re-skins with the
//  user's theme like the rest of the UI. It's sized with a minimum width +
//  roomy padding and text a touch larger than body copy so it reads as a
//  deliberate message, not a faint hint. The only non-theme touches are
//  flooring a couple of alphas for legibility over the game world when the
//  Quickbar background is hidden - the same justified exception
//  PushHighContrastButtonStyles makes. High-contrast mode is a gentle nudge
//  (opaque backing, slightly stronger border), deliberately not a loud effect.
//
//  ORIGIN TARGETING:
//  Each window calls SetActiveFeedbackSurface() at the top of its render;
//  ShowFeedback() tags the message with that surface; DrawFeedbackOverlay()
//  draws only on the surface the refusal came from - so a blocked Quickbar
//  click shows in the Quickbar, a main-panel send-gate refusal shows in the
//  main panel, never the wrong one or both.
//
//  HOW TO ADD A FEEDBACK SITE:
//  From any render-thread refusal point, call ShowFeedback(L("your.key")).
//  That's it - no registration, no new render callback.
//
//  THREADING: render-thread only (all call sites run inside a render
//  callback), so the single-slot state needs no mutex.
// =====================================================================

#include <string>

// Which of our windows is currently rendering. Set by SetActiveFeedbackSurface
// so ShowFeedback can tag a message's origin, and passed to DrawFeedbackOverlay
// so each window only draws messages that originated in it.
enum class FeedbackSurface { None, MainPanel, Quickbar };

// Call once at the top of each window's render (QuickbarRender / AddonRender).
void SetActiveFeedbackSurface(FeedbackSurface surface);

// Show (or replace/refresh) the current feedback line. message is the already-
// resolved, localized text (e.g. L("cells.blocked_mounted")).
void ShowFeedback(const std::string& message);

// Draw the fading line inside the CURRENT ImGui window (call after EndChild,
// before End). Draws only when a live message's origin == thisSurface. No-op
// otherwise. highContrast strengthens the backing slightly for HUD legibility.
void DrawFeedbackOverlay(FeedbackSurface thisSurface, bool highContrast);
