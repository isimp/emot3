#include "Quickbar.h"
#include "Globals.h"
#include "I18n.h"
#include "Settings.h"
#include "EmoteData.h"
#include "MeMotes.h"          // /me-motes Quickbar category + favorites mixing
#include "CharacterState.h" // CurrentEmoteBlock / InCombatNow / g_QbBlockReason / g_QbUnusableKey
#include "EmoteAction.h" // CurrentSendBusy / EmoteSendSwallowActive (grey-while-busy)
#include "Feedback.h"    // SetActiveFeedbackSurface / DrawFeedbackOverlay
#include "Cells.h"
#include "Layout.h"      // Ellipsize - shared with the emote-name label fitting
#include "MainPanel.h"   // LoadEmoteTextures - shared with AddonRender
#include "Logging.h"     // LOG_DEBUG (active-category switch)
#include "Profiling.h"   // dev perf overlay
#include "QuickbarDebug.h" // dev sizing readout (only in EMOT3_DEVTOOLS builds)
#include "QuickbarWheel.h" // click-through wheel routing (+plus flavor only)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // ImGuiWindow::ScrollbarY for hit-test

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
// Drag-snap metrics for the Quickbar window (experimental QuickbarSnapWindow).
// step + the grid's top offset are recomputed from CURRENT settings each frame
// before Begin (so scaling / view-mode changes snap correctly with no lag);
// only insetX/Y (the chrome: title/category bar, scrollbar, padding) is
// measured last frame, which is fine since chrome doesn't move when you scale.
struct QbSnapMetrics {
    bool  valid = false;
    float stepX = 0.f, stepY = 0.f;      // cellW+spacingX, cellH+spacingY (current)
    float spacingX = 0.f, spacingY = 0.f;
    float insetX = 0.f, insetY = 0.f;    // window size - child content avail
                                         // (insetY already folds in the grid top offset)
};
QbSnapMetrics g_qbSnap;

// Measured chrome from last frame: window size minus the child's content avail,
// captured before the grid's top-pad Dummy. Persisted so the per-frame metrics
// build can add the (current) grid top offset on top.
float s_qbInsetX = 0.f, s_qbInsetY = 0.f;
bool  s_qbInsetValid = false;

// Round each axis of the window's desired size so the child content region
// lands on a whole number of cells: window = inset + N*step - spacing.
void QbSnapSizeCallback(ImGuiSizeCallbackData* d) {
    const QbSnapMetrics* m = (const QbSnapMetrics*)d->UserData;
    if (!m) return;
    auto snap = [](float desired, float inset, float step, float spacing) {
        if (step <= 1.f) return desired;                  // not measured yet
        int n = (int)(((desired - inset) + spacing) / step + 0.5f);
        if (n < 1) n = 1;
        // Ceil so that after ImGui rounds the window to integer pixels the
        // viewport is >= the (fractional, at non-1.0 scales) grid content -
        // otherwise a sub-pixel overflow makes ImGui draw a 1px scrollbar
        // (the leftover vertical bar) or clip the last row. <1px of slack,
        // imperceptible.
        return std::ceil(inset + n * step - spacing);
    };
    d->DesiredSize.x = snap(d->DesiredSize.x, m->insetX, m->stepX, m->spacingX);
    d->DesiredSize.y = snap(d->DesiredSize.y, m->insetY, m->stepY, m->spacingY);
}

// --- Owned-scroll custom scrollbar (fit mode) ------------------------------
// We take scrolling away from ImGui (NoScrollbar) so it can never spawn the
// glitchy auto-bar that cascaded into the column-drop / wrap feedback. This
// draws a replacement bar in the reserved gutter beside (vertical) or below
// (horizontal) the content child, styled from ImGui's OWN scrollbar colors so
// it's indistinguishable from the main panel's stock bar.
//
// The interaction is a real ImGui InvisibleButton over the gutter, NOT raw io:
// that's what makes the button consume the click so ImGui doesn't read it as a
// window-move drag (the "thumb moves the window" bug when Allow move is on).
// Only called when the content actually overflows, so there's no bar/track
// when everything fits. The caller registers the gutter rect in g_QbIconRects
// so it stays grabbable under click-through.
float s_qbBarGrabPx = 0.f;   // cursor offset within the thumb at grab time

// One wheel event -> final scroll offset on an axis: applies the per-mode step,
// optional cell snap, clamping, and edge-triggered wrap (QuickbarScrollWrap).
// `wheel` is io.MouseWheel's sign (+ up/left, - down/right); `maxS` is the
// reachable max (the caller caps it to ImGui's reported GetScrollMax*).
//
// Wrap fires only when the wheel pushes PAST an end AND the view is already
// parked at that end - so a mid-list overshoot just clamps to the end and the
// NEXT notch wraps. Both ends are symmetric.
//
// "Parked at the end" is tested with a tolerance, NOT `cur >= maxS - 0.5`:
// ImGui's real scroll clamp can rest ~1px below the GetScrollMax* it reports, and
// the cell-aligned max can sit a fraction above what's actually reachable (varies
// with icon scale). So at rest, `cur` can be a px or so short of `maxS` and never
// climb the rest - a 0.5px compare then stops "wrap down" from ever firing while
// "wrap up" (the start is always exactly 0) keeps working. The tolerance is half
// a cell when snapping (resting positions are a full cell apart, so it can't
// misfire one cell early) and a couple px otherwise.
// Pages-mode helpers. WheelScroll's round-to-nearest works for cell-snap (the
// cull math guarantees maxS is a cell-multiple, so every snap landing is a real
// cell row), but breaks for pages: maxS = (totalRows - visRows) * cellStep is
// NOT generally a multiple of pageStep = visRows * cellStep, so a wheel-up from
// the end computes cur - pageStep that rounds to zero past the "midpoint" of
// the last partial page, skipping the page boundary just below the end. The
// "endTol = snapStep / 2" wrap zone also explodes from half-a-cell to half-a-
// viewport. These helpers replace both behaviours for pages mode only - cells/
// off still use WheelScroll unchanged.

// Drag-snap target: round to whichever of (page boundary below, page boundary
// above, maxS) sits closest to `target`. Lets the drag reach maxS even when
// maxS isn't a whole multiple of the page step.
float SnapPage(float target, float pageStep, float maxS) {
    target = std::max(0.f, std::min(target, maxS));
    if (pageStep <= 1.f) return target;
    const float below = std::floor(target / pageStep) * pageStep;
    const float above = std::min(below + pageStep, maxS);
    return (target - below <= above - target) ? below : above;
}

// Wheel one page from cur. wheel<0 (scroll down) goes to the next page
// boundary above cur, or maxS, whichever is smaller; wheel>0 (scroll up) goes
// to the previous page boundary, or 0. Quantizes cur to the nearest page (or
// maxS) FIRST so a cur that's drifted slightly off a page boundary (ImGui
// float-rounding, post-resize) still steps cleanly - without this, cur=542
// when page=542.4 would compute curPage=0 and "advance" by 0.4 px, tripping
// the wrap. wrap fires only when no movement is possible (parked at the
// extreme); a cur mid-content never wraps.
float PageWheelScroll(float cur, float wheel, float pageStep,
                      float maxS, bool wrap) {
    if (wheel == 0.f || pageStep <= 1.f) return cur;
    const float eps = 2.0f;
    const float curSnap = SnapPage(cur, pageStep, maxS);  // align to a page or maxS
    float newScroll;
    if (wheel < 0.f) {
        if (curSnap >= maxS - 0.5f) {
            newScroll = maxS;                              // already at end
        } else {
            const int curPage = (int)std::floor(curSnap / pageStep);
            newScroll = std::min(maxS, (float)(curPage + 1) * pageStep);
        }
    } else {
        if (curSnap <= 0.5f) {
            newScroll = 0.f;                               // already at start
        } else {
            // -0.5 so curSnap exactly at a page boundary still steps back.
            const int newPage = (int)std::floor((curSnap - 0.5f) / pageStep);
            newScroll = std::max(0.f, (float)newPage * pageStep);
        }
    }
    if (wrap && std::fabs(newScroll - cur) < eps) {
        if (wheel < 0.f) return 0.f;   // parked at end   -> wrap to start
        if (wheel > 0.f) return maxS;  // parked at start -> wrap to end
    }
    return newScroll;
}

float WheelScroll(float cur, float wheel, float step, float snapStep,
                  float maxS, bool snap, bool wrap) {
    float s = cur - wheel * step;
    if (snap && snapStep > 1.f) s = std::round(s / snapStep) * snapStep;
    float clamped = std::max(0.f, std::min(s, maxS));
    if (wrap && maxS > 1.f) {
        float endTol = (snap && snapStep > 1.f) ? snapStep * 0.5f : 2.f;
        if (wheel < 0.f && s > maxS + 0.5f && cur >= maxS - endTol) return 0.f;    // parked at end   -> wrap to start
        if (wheel > 0.f && s < -0.5f       && cur <= endTol)        return maxS;   // parked at start -> wrap to end
    }
    return clamped;
}

void QbCustomScrollbar(ImGuiWindow* child, bool axisY,
                       float scroll, float scrollMax, float viewport,
                       ImVec2 childPos, ImVec2 childSize,
                       float gutter, bool flatten,
                       ImVec2& outMin, ImVec2& outMax) {
    const ImGuiStyle& st = ImGui::GetStyle();
    // Gutter rect: right strip (vertical) or bottom strip (horizontal).
    // KEEP this at the architectural position (flush against the content
    // child). The InvisibleButton below uses mn/mx for its hit area and its
    // cursor advance; pushing this into WindowPadding territory inflates the
    // outer window's ContentSize and causes per-frame layout jitter in
    // horizontal mode (the horizontal gutter advances cursor.y, ImGui sees
    // content overflow the window's content region, layout twitches).
    ImVec2 mn = axisY ? ImVec2(childPos.x + childSize.x, childPos.y)
                      : ImVec2(childPos.x,               childPos.y + childSize.y);
    ImVec2 mx = ImVec2(mn.x + (axisY ? gutter : childSize.x),
                       mn.y + (axisY ? childSize.y : gutter));
    outMin = mn; outMax = mx;

    float trackStart  = axisY ? mn.y : mn.x;
    float trackLen    = axisY ? (mx.y - mn.y) : (mx.x - mn.x);
    float content     = scrollMax + viewport;
    float thumbLen    = (content > 0.f) ? trackLen * (viewport / content) : trackLen;
    thumbLen = std::min(std::max(thumbLen, st.GrabMinSize), trackLen);
    float maxThumbOff = std::max(0.f, trackLen - thumbLen);

    // Real button over the gutter — consumes the click (blocks window-move).
    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton(axisY ? "##qbvscroll" : "##qbhscroll",
                           ImVec2(mx.x - mn.x, mx.y - mn.y));
    bool hovered = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();

    float mAxis    = axisY ? ImGui::GetIO().MousePos.y : ImGui::GetIO().MousePos.x;
    float thumbOff = (scrollMax > 0.f) ? (scroll / scrollMax) * maxThumbOff : 0.f;
    float newScroll = scroll;
    if (ImGui::IsItemActivated()) {
        // Grab on the thumb keeps the offset; grab on the track centers it.
        float thumbStart = trackStart + thumbOff;
        s_qbBarGrabPx = (mAxis >= thumbStart && mAxis <= thumbStart + thumbLen)
                            ? mAxis - thumbStart : thumbLen * 0.5f;
    }
    if (held && maxThumbOff > 0.f) {
        float wantOff = (mAxis - s_qbBarGrabPx) - trackStart;
        newScroll = (wantOff / maxThumbOff) * scrollMax;
        // Snap the drag too, matching the wheel: rounds to whole cells in Cells
        // mode and to whole pages in Pages mode (one page = the VIEWPORT size
        // on the scroll axis = total grid extent - max scroll). Off lets the
        // drag land at any pixel.
        //
        // Cells: maxScroll is a whole multiple of cellStep (the cull math
        // guarantees it), so std::round + clamp lands cleanly.
        // Pages: maxScroll generally ISN'T a multiple of pageStep (e.g. 10
        // total rows, 4 visible, page = 4 rows but max scroll = 6 rows), so
        // we use SnapPage which treats maxS as a third snap target alongside
        // the page boundaries below/above target. Lets the drag actually
        // reach the end of the list.
        const float cellStep  = axisY ? g_QbStepY     : g_QbStepX;
        if (g_Settings.QuickbarSnapScroll == EQbScrollSnap::Cells && cellStep > 1.f) {
            newScroll = std::round(newScroll / cellStep) * cellStep;
        } else if (g_Settings.QuickbarSnapScroll == EQbScrollSnap::Pages) {
            const float totalSize = axisY ? (g_QbRows * g_QbStepY)
                                          : (g_QbCols * g_QbStepX);
            const float maxScrl   = axisY ? g_QbMaxScrollY : g_QbMaxScrollX;
            const float pageStep  = std::max(cellStep, totalSize - maxScrl);
            newScroll = SnapPage(newScroll, pageStep, scrollMax);
        }
        newScroll = std::max(0.f, std::min(newScroll, scrollMax));
        if (axisY) ImGui::SetScrollY(child, newScroll);
        else       ImGui::SetScrollX(child, newScroll);
    }

    // Draw: a slim centered pill that grows on hover/drag - distinct from
    // ImGui's static full-width bar, while reusing ImGui's own scrollbar COLORS
    // (unchanged). The InvisibleButton above already owns the full-gutter hit
    // area, so the visible bar can be much thinner than the gutter without
    // hurting grabbability. Thickness animates between a rest and a "hot"
    // fraction of the gutter; a faint groove at rest thickness anchors it.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    static float s_qbThumbAnim = 0.45f;
    float targetFrac = (held || hovered) ? 0.80f : 0.45f;
    float dt = ImGui::GetIO().DeltaTime;
    s_qbThumbAnim += (targetFrac - s_qbThumbAnim) * std::min(1.f, std::max(0.f, dt) * 12.f);
    float restThick  = std::max(4.f, std::floor(gutter * 0.45f));
    float thumbThick = std::max(4.f, std::floor(gutter * s_qbThumbAnim));

    float drawOff = (scrollMax > 0.f) ? (newScroll / scrollMax) * maxThumbOff : 0.f;
    ImGuiCol grabCol = held    ? ImGuiCol_ScrollbarGrabActive
                     : hovered ? ImGuiCol_ScrollbarGrabHovered
                               : ImGuiCol_ScrollbarGrab;
    ImU32 grab = ImGui::GetColorU32(grabCol);
    ImU32 bg   = ImGui::GetColorU32(ImGuiCol_ScrollbarBg);

    // Visible bar centered in the "lane" - the strip from content edge to
    // window border, which is gutter + WindowPadding on the bar's axis.
    // Centering in the lane (rather than just the gutter) avoids the
    // bar reading as left/top-shifted because the gutter itself sits inside
    // WindowPadding. The lane formula and the gutter-centered formula
    // collapse to the same value when WindowPadding = 0. When hot
    // (thumbThick > rest), the bar may visually extend slightly past the
    // gutter into WindowPadding territory - that's drawn-only, no layout
    // impact, and the central bulk of the bar stays inside the InvisibleButton's
    // hit area.
    const float lane = gutter + (axisY ? st.WindowPadding.x : st.WindowPadding.y);
    if (axisY) {
        float gx = mn.x + (lane - restThick) * 0.5f;             // groove (rest)
        if (!flatten)
            dl->AddRectFilled(ImVec2(gx, mn.y), ImVec2(gx + restThick, mx.y),
                              bg, restThick * 0.5f);
        float tx = mn.x + (lane - thumbThick) * 0.5f;            // thumb (animated)
        dl->AddRectFilled(ImVec2(tx, trackStart + drawOff),
                          ImVec2(tx + thumbThick, trackStart + drawOff + thumbLen),
                          grab, std::min(thumbThick, thumbLen) * 0.5f);
    } else {
        float gy = mn.y + (lane - restThick) * 0.5f;
        if (!flatten)
            dl->AddRectFilled(ImVec2(mn.x, gy), ImVec2(mx.x, gy + restThick),
                              bg, restThick * 0.5f);
        float ty = mn.y + (lane - thumbThick) * 0.5f;
        dl->AddRectFilled(ImVec2(trackStart + drawOff, ty),
                          ImVec2(trackStart + drawOff + thumbLen, ty + thumbThick),
                          grab, std::min(thumbThick, thumbLen) * 0.5f);
    }
}

// Purely visual scroll-edge hints: a hint at the start/end of the active scroll
// axis when more content lies that way. Drawn on the FOREGROUND draw list (the
// grid is a child window that composites ABOVE the parent, so a parent-window
// overlay lands behind the cells - the same z-order trap the feedback overlay
// hit; foreground renders last, on top). Clamped to the content rect, with NO
// layout reservation (the fit-to-grid snap math must stay untouched) and NO input
// (no g_QbIconRects - not draggable). Per-end gated: start edge (top/left) only
// when scrolled in, end edge (bottom/right) only when more remains. Hints are
// mutually exclusive with the scrollbar (the EQbScrollIndicator combo), so the
// gutter is normally 0 here. Two renderers by background mode:
//   bg shown  -> a SUBTLE dark inner shadow (depth cue; kept light so it doesn't
//                bury the icons - the deep shadow occluded them).
//   bg hidden -> a dampened accent edge LINE + soft inward GLOW (colour, not a
//                dark veil, so it survives over the game world).
// `bordered` (Text-only mode: cells are full bordered buttons): the hint sits
// right on the button frames, so the hard line is DROPPED (glow only) and the
// shadow eased, to avoid doubling up with / darkening the borders.
void QbScrollEdgeHints(bool axisY, float scroll, float scrollMax,
                       ImVec2 childPos, ImVec2 childSize,
                       float gutter, bool flatten, bool highContrast,
                       bool wrap, bool bordered) {
    // scrollMax is the CELL-ALIGNED max (g_QbMaxScroll*), where the last whole
    // row/column sits flush - NOT ImGui's GetScrollMax*, which carries trailing
    // item-spacing the user can never scroll to, so the end hint would never
    // clear. With wheel-wrap on, either end always leads somewhere (it loops), so
    // both hints stay lit whenever content overflows.
    constexpr float kEps = 1.0f;                  // sub-pixel/snapped scroll guard
    bool showStart = wrap || scroll > kEps;
    bool showEnd   = wrap || scroll < scrollMax - kEps;
    if (!showStart && !showEnd) return;

    // Content rect, trimmed on the CROSS axis by the reserved scrollbar gutter so
    // the hint never paints over the slim bar (vertical scroll -> bar on the
    // right; horizontal -> bar on the bottom). gutter is 0 when no bar is active.
    ImVec2 mn = childPos;
    ImVec2 mx = ImVec2(childPos.x + childSize.x, childPos.y + childSize.y);
    if (axisY) mx.x -= gutter;
    else       mx.y -= gutter;
    if (mx.x - mn.x < 1.f || mx.y - mn.y < 1.f) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    if (!flatten) {
        // Background shown: a dark inner shadow - present enough to read clearly,
        // still soft enough (over a ~20px falloff) not to bury the icons. Eased in
        // Text-only mode, where it would otherwise darken the bordered buttons.
        const float band = 20.f;
        float aEdge = (highContrast ? 0.68f : 0.55f) * (bordered ? 0.55f : 1.0f);
        ImU32 edge = ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, aEdge));
        ImU32 fade = edge & ~IM_COL32_A_MASK;
        if (axisY) {
            if (showStart) dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + band), edge, edge, fade, fade);
            if (showEnd)   dl->AddRectFilledMultiColor(ImVec2(mn.x, mx.y - band), mx, fade, fade, edge, edge);
        } else {
            if (showStart) dl->AddRectFilledMultiColor(mn, ImVec2(mn.x + band, mx.y), edge, fade, fade, edge);
            if (showEnd)   dl->AddRectFilledMultiColor(ImVec2(mx.x - band, mn.y), mx, fade, edge, edge, fade);
        }
        return;
    }

    // Background hidden: a dampened accent edge line + soft inward glow. Muted
    // slate-blue (not neon) so it isn't shiny; highContrast nudges it bolder. The
    // glow is a multi-colour rect (accent at the edge -> transparent inward); the
    // line is a thin filled rect right on the edge, drawn on top.
    bool  strong = highContrast;
    bool  drawLine = !bordered;                   // line clashes with button frames
    float band   = 20.f;                          // glow depth inward
    float lineW  = 2.0f;
    // Without the hard line (Text-only), nudge the glow up a touch so the cue
    // doesn't disappear.
    float aGlow  = (strong ? 0.32f : 0.22f) + (drawLine ? 0.f : 0.08f);
    ImU32 glowE  = ImGui::GetColorU32(ImVec4(0.48f, 0.62f, 0.80f, aGlow));
    ImU32 glowF  = glowE & ~IM_COL32_A_MASK;      // accent, alpha 0
    ImU32 line   = ImGui::GetColorU32(ImVec4(0.60f, 0.74f, 0.90f, strong ? 0.85f : 0.68f));
    if (axisY) {
        if (showStart) {
            dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + band), glowE, glowE, glowF, glowF);
            if (drawLine) dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + lineW), line);
        }
        if (showEnd) {
            dl->AddRectFilledMultiColor(ImVec2(mn.x, mx.y - band), mx, glowF, glowF, glowE, glowE);
            if (drawLine) dl->AddRectFilled(ImVec2(mn.x, mx.y - lineW), mx, line);
        }
    } else {
        if (showStart) {
            dl->AddRectFilledMultiColor(mn, ImVec2(mn.x + band, mx.y), glowE, glowF, glowF, glowE);
            if (drawLine) dl->AddRectFilled(mn, ImVec2(mn.x + lineW, mx.y), line);
        }
        if (showEnd) {
            dl->AddRectFilledMultiColor(ImVec2(mx.x - band, mn.y), mx, glowF, glowE, glowE, glowF);
            if (drawLine) dl->AddRectFilled(ImVec2(mx.x - lineW, mn.y), mx, line);
        }
    }
}
}  // namespace

void QuickbarRender() {
    // Tag this render pass so any refusal raised from a Quickbar cell shows its
    // feedback line in the Quickbar (not the main panel). See Feedback.h.
    SetActiveFeedbackSurface(FeedbackSurface::Quickbar);
    // Click-through wheel capture is republished at the end of a successful
    // render; clear it first so every early return (not gameplay, hidden,
    // collapsed, empty) leaves the WndProc with nothing to capture and no stale
    // rect. No-op in dist. The tiny window to the end-of-frame publish is
    // sub-millisecond and self-correcting.
    QbWheelPublish(false, 0.f, 0.f, 0.f, 0.f);

    if (!NexusLink || !NexusLink->IsGameplay) return;
    // Auto-hide while the fullscreen world map is open (it covers the screen).
    // Placed after the wheel-capture clear above so no stale capture rect is
    // left for the WndProc while we're suppressed. Per-frame, not persisted:
    // the bar reappears when the map closes. Only the fullscreen map sets this;
    // the minimap doesn't, so the HUD survives normal play.
    if (MumbleLink && MumbleLink->Context.IsMapOpen) return;

    // Catalog / icon-source change since the last reload? Reprime the
    // emote textures. The main panel's AddonRender does the same
    // check, but only when it's actually rendering - so if the user
    // has the main window closed and only the Quickbar open, that
    // path never fires and the Quickbar keeps showing whatever was
    // cached the last time the main window was open. Symmetric check
    // here covers toggles like the AI fallback regardless of which
    // surface is currently visible. Idempotent: when both surfaces
    // are open, AddonRender (registered first) handles it and
    // LoadEmoteTextures clears the flag, so we no-op here.
    if (g_EmotesDirty) LoadEmoteTextures();

    if (!g_Settings.ShowQuickbar) return;
    PROFILE_SCOPE("qb.frame");  // dev perf overlay

    // Combat behavior (its own Quickbar setting, off/grey/hide). Emotes still
    // WORK in combat everywhere else, so this is Quickbar-only - never a send
    // block on the main panel or the shared gate. Hide pulls the bar now (like
    // the world-map auto-hide above); Grey is folded into the reason below so it
    // dims + blocks the cells; Off ignores combat. MumbleLink IsInCombat; no
    // RealTime API needed.
    const bool inCombat = InCombatNow();
    if (inCombat && g_Settings.QuickbarCombatBehavior == EQbCombat::Hide) return;

    // Usability gate: the current can't-emote reason - mounted via MumbleLink,
    // plus downed/swimming/underwater/gliding/flying via the optional RealTime
    // API when precise detection is on (see core/CharacterState). Consumed by
    // RenderEmoteCell for the grey + the click/right-click explainer, and shared
    // with the send gate so every surface agrees.
    g_QbBlockReason = CurrentEmoteBlock();

    // Unified "why this Quickbar emote can't be used this frame" reason, with ONE
    // interaction (QuickbarUnusableBehavior) applied. Whenever greying/hiding is
    // active (QuickbarGreyUnusable), it covers BOTH the game-state block (mounted /
    // RTAPI states / airborne) AND the transient send refusals - a GW2 text box
    // focused, or moving / a printable key held. The transient cases used to be
    // separate opt-ins, but they're robust + cheap now, so they just ride the master
    // setting. Same detector as the send gate (CurrentSendBusy) so greying and
    // refusal can't drift; mapped to the present-tense cells.blocked_* wording. Two
    // settings carve out their own case: "send while moving" (+plus swallow) drops
    // the movement source (a held key no longer refuses), and "close chat on send"
    // drops the textbox source (a click closes + sends). The movement case is
    // debounced: keys are tapped constantly in play, and reacting to a quick tap
    // would flicker the bar.
    const char* reason = nullptr;
    {
        PROFILE_SCOPE("qb.busy");  // dev perf overlay
        static double   s_heldSince = -1.0;     // movement-hold start (debounce); <0 = none
        constexpr double kHeldGreyDelay = 0.25; // sustained-hold threshold (seconds)
        const double now = ImGui::GetTime();

        // Transient busy state, cheap to read every frame: textbox is a MumbleLink
        // bit, held keys are a WndProc-maintained atomic, movement is the position-
        // velocity flag - no per-frame key polling. Evaluated even while a game-state
        // block (mounted) masks the reason, so the movement debounce tracks the REAL
        // hold duration and survives the block (no unmount-while-moving flicker).
        const bool greying       = g_Settings.QuickbarGreyUnusable;
        const bool checkMovement = greying && !EmoteSendSwallowActive();  // swallow handles held keys
        const SendBusy busy = greying
            ? CurrentSendBusy(checkMovement, /*ignoreTextbox=*/g_Settings.CloseChatOnSend)
            : SendBusy::None;
        if (busy == SendBusy::KeysHeld) {
            if (s_heldSince < 0.0) s_heldSince = now;   // start (or keep) the hold clock
        } else {
            s_heldSince = -1.0;
        }

        // Pick the reason. A DEFINITE game state (mounted / downed / swimming /
        // gliding / ...) wins. Then the transient refusals - and these outrank
        // AIRBORNE on purpose: when you're running and briefly clip "airborne",
        // "moving" is the clearer message. Airborne is the fallback, shown only when
        // nothing else applies (e.g. a jump from standstill). Movement only greys
        // after the sustained-hold threshold (the clock survives a mounted spell, so
        // the grey carries over when you unmount still moving - no flicker).
        if (g_QbBlockReason != EmoteBlock::None && g_QbBlockReason != EmoteBlock::Airborne) {
            reason = EmoteBlockKey(g_QbBlockReason);
        } else if (greying) {
            if (busy == SendBusy::Typing) {
                reason = "cells.blocked_typing";
            } else if (busy == SendBusy::KeysHeld && s_heldSince >= 0.0 &&
                       now - s_heldSince >= kHeldGreyDelay) {
                reason = "cells.blocked_moving";
            } else if (g_QbBlockReason == EmoteBlock::Airborne) {
                reason = "cells.blocked_airborne";
            }
        }
    }

    // One interaction applies to whichever source fired: Hide pulls the whole bar
    // (was game-state only; now any enabled source, since the debounce tamed the
    // transient cases); Grey dims + blocks the cells in place further down.
    if (reason && g_Settings.QuickbarUnusableBehavior == EUnusableBehavior::Hide) {
        g_QbUnusableKey = nullptr;
        return;
    }
    // Combat-grey is the lowest-priority source: a real can't-emote reason
    // (mounted etc.) owns the cell's explainer when both apply. (Combat-hide
    // already returned above.)
    if (!reason && inCombat && g_Settings.QuickbarCombatBehavior == EQbCombat::Grey)
        reason = "cells.blocked_combat";
    g_QbUnusableKey = reason;

    // Min size: width fits one Icon-mode cell (56 + window padding/scrollbar ≈ 90).
    // Height fits title bar + toggle row + one cell.
    // Experimental drag-snap: round the window to whole cells on both axes via
    // a size-callback. step + grid top offset are computed from CURRENT
    // settings here (so scaling / view-mode changes snap without a frame lag);
    // chrome inset comes from last frame's measurement. Falls back to the plain
    // min constraint until the QB has drawn once (chrome not measured yet).
    if (g_Settings.QuickbarSnapWindow && s_qbInsetValid) {
        const ImGuiStyle& style = ImGui::GetStyle();
        EViewMode mode = g_Settings.QuickbarViewMode;
        float qbScale = std::max(g_Settings.QuickbarIconScale, MinIconScaleForMode(mode));
        float cw, ch, isz;
        EmoteCellSize(mode, qbScale, cw, ch, isz);
        float topPad = EmoteGridTopPad(mode);
        float gridTopOffset = (topPad > 0.f) ? topPad + style.ItemSpacing.y : 0.f;

        g_qbSnap.valid    = true;
        g_qbSnap.stepX    = cw + style.ItemSpacing.x;
        g_qbSnap.stepY    = ch + style.ItemSpacing.y;
        g_qbSnap.spacingX = style.ItemSpacing.x;
        g_qbSnap.spacingY = style.ItemSpacing.y;
        g_qbSnap.insetX   = s_qbInsetX;
        g_qbSnap.insetY   = s_qbInsetY + gridTopOffset;  // grid starts below the top-pad

        ImGui::SetNextWindowSizeConstraints(ImVec2(90.f, 90.f), ImVec2(FLT_MAX, FLT_MAX),
                                            QbSnapSizeCallback, &g_qbSnap);
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(90.f, 90.f), ImVec2(FLT_MAX, FLT_MAX));
    }
    ImGui::SetNextWindowSize(ImVec2(360, 200), ImGuiCond_FirstUseEver);

    // One-shot geometry apply from a preset (ApplyQuickbarPreset set the
    // pending values). Clamp to the display so an off-screen or oversized saved
    // geometry still lands fully on screen and usable. The Quickbar is an
    // in-game overlay, so the usable area is the whole display: top-left (0,0),
    // size io.DisplaySize. ImGuiCond_Always overrides NoMove/NoResize (those
    // only gate user interaction, not programmatic placement) and wins over the
    // FirstUseEver size above. Clamped at consume time, so a preset saved on a
    // bigger monitor is corrected against whatever resolution is current now.
    if (g_QbApplyGeometry) {
        ImVec2 disp = ImGui::GetIO().DisplaySize;
        float w = g_QbApplyW, h = g_QbApplyH;
        if (w < 90.f) w = 90.f;  else if (disp.x > 0.f && w > disp.x) w = disp.x;
        if (h < 90.f) h = 90.f;  else if (disp.y > 0.f && h > disp.y) h = disp.y;
        float x = g_QbApplyX, y = g_QbApplyY;
        float maxX = disp.x - w, maxY = disp.y - h;
        if (x < 0.f) x = 0.f; else if (maxX >= 0.f && x > maxX) x = maxX;
        if (y < 0.f) y = 0.f; else if (maxY >= 0.f && y > maxY) y = maxY;
        ImGui::SetNextWindowPos (ImVec2(x, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        g_QbApplyGeometry = false;
    }

    ImGuiWindowFlags qbFlags = ImGuiWindowFlags_None;
    if (!g_Settings.QuickbarAllowResize)   qbFlags |= ImGuiWindowFlags_NoResize;
    if (!g_Settings.QuickbarAllowMove)     qbFlags |= ImGuiWindowFlags_NoMove;
    if (!g_Settings.ShowQuickbarBg)        qbFlags |= ImGuiWindowFlags_NoBackground;
    if (!g_Settings.ShowQuickbarTitle)     qbFlags |= ImGuiWindowFlags_NoTitleBar;
    // The OUTER window must never scroll: its content is just the category bar
    // + the content child (which fills the rest). Whenever we own scrolling -
    // either SnapWindow (window-fit needs to break the auto-bar feedback loop)
    // or SnapScroll (cell-snap drag wants the custom slim-pill bar) - we
    // suppress the outer ImGui scrollbar regardless of the "Show scrollbar"
    // setting. Otherwise, when Show scrollbar is on and the outer content is a
    // hair too tall (snap rounding or a tab-bar wrap), the outer bar appears,
    // steals width and wraps a column ("scrollbar shows when everything fits"
    // / "few-px push"). With it suppressed that becomes a harmless sub-pixel
    // clip. Pure free mode (neither snap) keeps the old gate.
    const bool ownScroll = g_Settings.QuickbarSnapWindow ||
                           g_Settings.QuickbarSnapScroll != EQbScrollSnap::Off;
    if (ownScroll || g_Settings.QuickbarScrollIndicator != EQbScrollIndicator::Scrollbar)
        qbFlags |= ImGuiWindowFlags_NoScrollbar;

    // Click-through: if the cursor isn't over any rect we captured last
    // frame, render with NoInputs so the click falls through to the game.
    // Gated on background being hidden — see the Options comment for why.
    //
    // Carry-over state that smooths the experience:
    //
    //   s_dragInProgress — true while the user is holding LMB after
    //     pressing it over an input-active rect (resize corner, scrollbar,
    //     title bar, etc.). The cursor leaves the small grip almost
    //     immediately during a resize drag, so without this we'd flip
    //     NoInputs on mid-drag and ImGui would drop the operation.
    //
    //   s_wheelStickyFrames — short grace period after every wheel event,
    //     during which NoInputs stays off. Helps two cases:
    //       (1) scrolling over an icon when the cursor jitters briefly off
    //           the icon between wheel ticks (the user noticed this as
    //           "feels way smoother");
    //       (2) continuous wheeling — second tick onwards lands even when
    //           the first one didn't (ImGui's wheel hits the previous
    //           frame's hovered window, so the very first event of a fresh
    //           session over a fully click-through area is still lost —
    //           known limitation).
    //     Only refreshed when content is actually scrollable so the grace
    //     period doesn't briefly suppress click-through when wheeling has
    //     no effect anyway.
    static ImGuiWindow* s_qbChildLast       = nullptr;
    static bool         s_dragInProgress    = false;
    static int          s_wheelStickyFrames = 0;
    // Did the content have scroll range last frame? Set at end of render from
    // GetScrollMax (valid even with NoScrollbar, so it works for owned scroll),
    // replacing the old `childWin->ScrollbarY/X` test which is always false
    // once we drop ImGui's scrollbar in fit mode.
    static bool         s_qbScrollable      = false;

    if (g_Settings.QuickbarClickThrough && !g_Settings.ShowQuickbarBg) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mp = io.MousePos;

        bool overActive = false;
        for (auto& r : g_QbIconRects) {
            if (mp.x >= r.first.x && mp.x <= r.second.x &&
                mp.y >= r.first.y && mp.y <= r.second.y) {
                overActive = true;
                break;
            }
        }

        // Track held-drag started over an active rect so click-then-drag
        // operations (resize, title-bar move, scrollbar grab) survive when
        // the cursor leaves the rect they started on.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overActive)
            s_dragInProgress = true;
        if (!io.MouseDown[0])
            s_dragInProgress = false;

        if (s_qbScrollable && (io.MouseWheel != 0.f || io.MouseWheelH != 0.f))
            s_wheelStickyFrames = 30;
        bool wheelGrace = s_wheelStickyFrames > 0;
        if (s_wheelStickyFrames > 0) --s_wheelStickyFrames;

        if (!overActive && !wheelGrace && !s_dragInProgress)
            qbFlags |= ImGuiWindowFlags_NoInputs;
    } else {
        // Click-through disabled — clear the latches so they don't leak.
        s_dragInProgress    = false;
        s_wheelStickyFrames = 0;
    }
    // Always reset before drawing — cells/selectors will refill it this frame.
    g_QbIconRects.clear();

    // Hiding the background also flattens the title bar AND the scrollbar
    // track to fully transparent. NoBackground only suppresses
    // ImGuiCol_WindowBg; title bar and scrollbar have their own colours,
    // which we push to zero alpha here. The scrollbar grab itself stays
    // visible so it still signals scroll position.
    //
    // With no background the bottom-right resize grip (drawn by ImGui
    // regardless of NoBackground - only the WindowBg fill is suppressed) is
    // nearly invisible over the game world: its default rest alpha is ~0.20,
    // so the user can't find the corner to drag-resize. Push the grip colours
    // brighter (accent blue, matching the active category tab) so the corner
    // reads as a clear affordance, while still brightening on hover/active.
    // Harmless when "Allow resize" is off - NoResize means no grip is drawn.
    // Kept a fixed 7-push count (not gated on QuickbarAllowResize) so the three
    // flattenChrome pop sites below stay in sync.
    bool flattenChrome = !g_Settings.ShowQuickbarBg;
    if (flattenChrome) {
        ImGui::PushStyleColor(ImGuiCol_TitleBg,          ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,      ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ResizeGrip,        ImVec4(0.40f, 0.68f, 1.00f, 0.65f));
        ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.50f, 0.75f, 1.00f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ResizeGripActive,  ImVec4(0.60f, 0.82f, 1.00f, 1.00f));
    }

    if (!ImGui::Begin("emot3 Quickbar##qb", &g_Settings.ShowQuickbar, qbFlags)) {
        // Begin returning false also covers the collapsed state — only the
        // title bar is drawn. Register that rect so the collapse / close
        // buttons stay clickable while click-through is on; otherwise the
        // user can't un-collapse or close the window.
        if (g_Settings.QuickbarClickThrough && g_Settings.ShowQuickbarTitle) {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 sz  = ImGui::GetWindowSize();
            g_QbIconRects.emplace_back(pos,
                                       ImVec2(pos.x + sz.x, pos.y + sz.y));
        }
        ImGui::End();
        if (flattenChrome) ImGui::PopStyleColor(7);
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
        return;
    }

    // Window is visible (not collapsed): mirror its live geometry so presets
    // can capture the current size/position, and mark it valid. Capturing a
    // preset before the QB has ever drawn leaves g_QbGeometryValid false, so
    // such a preset simply stores no geometry.
    {
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        g_QbWinX = wp.x; g_QbWinY = wp.y;
        g_QbWinW = ws.x; g_QbWinH = ws.y;
        g_QbGeometryValid = true;
    }

    // The Quickbar hit-tests the cursor against its own window rect by hand for
    // the wheel (category cycle + owned scroll) so those keep working under
    // click-through, where NoInputs suppresses ImGui's hover. That manual test
    // ignores window stacking, though, so without this the bar would steal the
    // wheel from any ImGui window (another Nexus addon, or our own main panel)
    // drawn on top of it. ImGui's HoveredWindow already encodes z-order, so we
    // treat the bar as occluded when a DIFFERENT root window is hovered.
    // HoveredWindow is null over the bare game world (the bar is NoInputs there,
    // and nothing else is under the cursor), so click-through scrolling - the
    // whole reason the manual test exists - is preserved.
    ImGuiWindow* qbWindow = ImGui::GetCurrentWindow();
    bool qbOccludedByOther = false;
    {
        ImGuiWindow* hov = GImGui->HoveredWindow;
        qbOccludedByOther = hov && hov->RootWindow != qbWindow->RootWindow;
    }

    // Register the title bar as an input-active rect so dragging the window
    // still works while click-through is on. Skipped when the title bar is
    // hidden anyway.
    if (g_Settings.QuickbarClickThrough && g_Settings.ShowQuickbarTitle) {
        ImVec2 pos = ImGui::GetWindowPos();
        float  th  = ImGui::GetFrameHeight();
        g_QbIconRects.emplace_back(
            pos,
            ImVec2(pos.x + ImGui::GetWindowSize().x, pos.y + th));
    }
    // Resize grip — bottom-right ~20px square. Only when resize is allowed
    // (NoResize off) and click-through is on; otherwise the grip isn't drawn
    // or input handling already works.
    if (g_Settings.QuickbarClickThrough && g_Settings.QuickbarAllowResize) {
        ImVec2 pos  = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();
        const float gripSz = 20.f;
        ImVec2 corner(pos.x + size.x, pos.y + size.y);
        g_QbIconRects.emplace_back(
            ImVec2(corner.x - gripSz, corner.y - gripSz), corner);

        // When the QB is click-through and the cursor isn't over an active
        // rect, this frame's window is NoInputs - and ImGui auto-adds NoResize
        // to any NoInputs window ("Automatically disable manual moving/resizing
        // when NoInputs is set", imgui.cpp), so it draws NO resize grip at all.
        // That's why the highlighted grip (bg-off) vanished unless a cell was
        // hovered. Draw our own static grip marker in that NoInputs state so the
        // corner stays findable. It reuses the pushed ImGuiCol_ResizeGrip colour
        // and matches ImGui's grip triangle/size, so as the cursor enters the
        // corner (clearing NoInputs next frame) ImGui's real interactive grip
        // takes over with no visible jump. Only needed with the background off
        // (bg on => ImGui's own always-visible grip already shows).
        if (flattenChrome && (qbFlags & ImGuiWindowFlags_NoInputs)) {
            float fs = ImGui::GetFontSize();
            float rounding = ImGui::GetStyle().WindowRounding;
            float drawSz = std::floor(std::max(fs * 1.10f, rounding + 1.0f + fs * 0.2f));
            ImGui::GetWindowDrawList()->AddTriangleFilled(
                ImVec2(corner.x - drawSz, corner.y),
                ImVec2(corner.x,          corner.y - drawSz),
                corner,
                ImGui::GetColorU32(ImGuiCol_ResizeGrip));
        }
    }

    // Combined category list for this frame: the user's favorites
    // categories first, then any enabled built-in (synthetic) categories.
    // The bar, the wheel-cycle count, the active-index clamp and the cell-
    // list build below all walk this one list, so favorites and built-ins
    // are handled uniformly. Rebuilt every frame (cheap; nothing cached).
    enum class QbCatKind { Favorite, Core, MadKing, Unlocked, UnlockedAll, MeMotes };
    struct QbCat { QbCatKind kind; int favIdx; std::string name; };
    std::vector<QbCat> cats;
    cats.reserve(g_Settings.FavoriteCategories.size() + 5);
    if (g_Settings.QuickbarShowFavoriteCategories)
        for (int i = 0; i < (int)g_Settings.FavoriteCategories.size(); ++i)
            cats.push_back({ QbCatKind::Favorite, i, g_Settings.FavoriteCategories[i].Name });
    // Synthetic built-ins, alphabetical by display name (Core, Mad King,
    // Unlocked, Unlocked (all), /me-motes).
    if (g_Settings.QuickbarShowCoreCategory)
        cats.push_back({ QbCatKind::Core, -1, L("qb.cat_core") });
    if (g_Settings.QuickbarShowMadKingCategory)
        cats.push_back({ QbCatKind::MadKing, -1, L("qb.cat_mad_king") });
    if (g_Settings.QuickbarShowMeMotesCategory)
        cats.push_back({ QbCatKind::MeMotes, -1, L("qb.cat_me_motes") });
    if (g_Settings.QuickbarShowUnlockedCategory)
        cats.push_back({ QbCatKind::Unlocked, -1, L("qb.cat_unlocked") });
    if (g_Settings.QuickbarShowUnlockedAllCategory)
        cats.push_back({ QbCatKind::UnlockedAll, -1, L("qb.cat_unlocked_all") });

    // Empty state — nothing to show (no favorites categories and every
    // built-in category turned off).
    if (cats.empty()) {
        ImGui::TextWrapped("%s", L("qb.no_categories"));
        ImGui::End();
        if (flattenChrome) ImGui::PopStyleColor(7);
        return;
    }

    // Clamp active index defensively. Toggling a built-in category on/off
    // shifts the combined-list indices; the clamp absorbs a now-stale index
    // exactly as it already does when a favorites category is deleted.
    int& active = g_Settings.QuickbarCategoryIdx;
    if (active < 0) active = 0;
    if (active >= (int)cats.size())
        active = (int)cats.size() - 1;

    // Wheel cycles category — two independent triggers fed by the user's
    // settings:
    //   "wheel-over-bar" → fires only when the cursor is over the
    //                      category tabs / dropdown row. The bar's
    //                      screen rect is captured at render time below
    //                      and read here on the next frame.
    //   "wheel anywhere" → fires when the cursor is anywhere over the
    //                      QB window (overrides icon list scroll).
    //
    // Either trigger is enough — if both are on they overlap harmlessly
    // since the cycle/consume logic only runs once. Manual cursor hit-
    // testing matches the horizontal-scroll handler's approach so this
    // still works in click-through mode (ImGui::IsWindowHovered would
    // lie when NoInputs is set).
    //
    // Static at function scope so the render block further down can
    // refresh s_qbBar* each frame; otherwise a hidden / empty bar would
    // leave a stale hot-zone.
    static ImVec2 s_qbBarMin = ImVec2(0, 0);
    static ImVec2 s_qbBarMax = ImVec2(0, 0);  // min.x >= max.x → invalid

    // Merge the click-through-routed wheel with ImGui's. The WndProc consumes
    // WM_MOUSEWHEEL over the QB in click-through mode (so the game doesn't zoom)
    // and accumulates the raw delta; here we drain it once per frame. Outside
    // click-through, raw is 0 and qbWheel == io.MouseWheel - exactly today's
    // behavior. In click-through the message never reached ImGui, so the routed
    // delta is the only source. Used by the wheel-cycle block here and the
    // scroll handler in the content child below.
    float qbWheelRouted = QbWheelDrain();              // 0 in dist / when not captured
    bool  fromWndProc   = qbWheelRouted != 0.f;
    float qbWheel       = ImGui::GetIO().MouseWheel + qbWheelRouted;

    {
        if (qbWheel != 0.f) {
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 mp   = io.MousePos;
            ImVec2 wPos = ImGui::GetWindowPos();
            ImVec2 wSz  = ImGui::GetWindowSize();
            bool overQb  = mp.x >= wPos.x && mp.x <= wPos.x + wSz.x &&
                           mp.y >= wPos.y && mp.y <= wPos.y + wSz.y;
            bool overBar = (s_qbBarMin.x < s_qbBarMax.x) &&
                           mp.x >= s_qbBarMin.x && mp.x <= s_qbBarMax.x &&
                           mp.y >= s_qbBarMin.y && mp.y <= s_qbBarMax.y;

            bool cycle = !qbOccludedByOther &&
                         ((g_Settings.QuickbarWheelCycle == EWheelCycle::Anywhere && overQb) ||
                          (g_Settings.QuickbarWheelCycle == EWheelCycle::OverBar  && overBar));

            int catCount = (int)cats.size();
            if (cycle && catCount > 1) {
                // Wheel up (positive) → previous, wheel down → next.
                // Matches browser tab cycling convention. Rounded so
                // multi-notch frames step correctly.
                int notches = -(int)(qbWheel > 0
                                     ? qbWheel + 0.5f
                                     : qbWheel - 0.5f);
                // Wrap (default) cycles around the ends via modulo; off clamps.
                int target = active + notches;
                int newIdx = g_Settings.QuickbarWheelCycleWrap
                           ? ((target % catCount) + catCount) % catCount
                           : std::max(0, std::min(target, catCount - 1));
                if (newIdx != active) {
                    active = newIdx;
                    LOG_DEBUG("quickbar: category -> \"%s\" (wheel)", cats[active].name.c_str());
                    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
                }
                // Consume the wheel so the content child doesn't also scroll.
                // (Horizontal too — io.MouseWheelH isn't routed via WndProc.)
                qbWheel        = 0.f;
                io.MouseWheelH = 0.f;
            }
        }
    }

    // Reset the bar's tracked rect each frame so a hidden / empty bar
    // doesn't leave a stale hot-zone behind. The render block below
    // re-arms it when it actually draws the bar.
    s_qbBarMin = ImVec2(0, 0);
    s_qbBarMax = ImVec2(0, 0);

    // Category selector. Shown whenever there's at least one category
    // and the user hasn't hidden the bar. Previously gated on size > 1
    // — restoring it for single-category setups gives the user a label
    // and a place for the wheel-cycle setting to feel anchored.
    if (g_Settings.ShowQuickbarCategoryBar && !cats.empty()) {
        // High-contrast chrome: make the tabs / combo solid enough to
        // stay visible when the QB background is hidden, while still
        // giving hover a real visual jump.
        //
        // Earlier passes tried to just bump alpha. With the default
        // theme that left Button at alpha 0.85 and ButtonHovered at
        // alpha 1.0 - same RGB, ~15% brightness delta. Technically a
        // hover effect, in practice invisible over the game world.
        //
        // This pass differentiates by RGB: the resting state gets the
        // theme colour darkened (so it reads as "dim but solid"), the
        // hover / active states keep the theme colour unchanged at
        // full alpha (so they pop). The relative direction (resting
        // dim, hover bright) is what the eye picks up, not the
        // absolute alpha number. Active-tab override (the blue push
        // further down) is unchanged either way.
        int hiContrastPushes = 0;
        if (g_Settings.QuickbarHighContrast) {
            // FrameBg* too: the category bar carries the dropdown combo.
            hiContrastPushes = PushHighContrastButtonStyles(/*includeFrameBg=*/true);
        }
        // Capture the bar's bounding rect so next frame's wheel handler
        // can hit-test against it. Top-left is the cursor's screen pos
        // before any of the controls render; bottom-right is captured
        // after the bar finishes (just before Separator).
        ImVec2 barTop = ImGui::GetCursorScreenPos();
        float  barWidth = ImGui::GetContentRegionAvail().x;
        if (g_Settings.QuickbarUseDropdown) {
            // Ellipsize the collapsed preview (like a too-long emote name)
            // instead of letting ImGui hard-clip it; full name in a tooltip.
            // The open popup auto-widens to the longest item, so its entries
            // never need truncation - only the fixed-width preview does.
            const ImGuiStyle& style = ImGui::GetStyle();
            ImGui::SetNextItemWidth(-1.f);
            float comboW      = ImGui::CalcItemWidth();   // resolves the -1
            float previewMaxW = std::max(1.f, comboW - ImGui::GetFrameHeight()
                                                     - style.FramePadding.x * 2.f);
            std::string preview     = Ellipsize(cats[active].name, previewMaxW);
            bool        previewClip = (preview != cats[active].name);

            if (ImGui::BeginCombo("##qbcatdrop", preview.c_str())) {
                for (int i = 0; i < (int)cats.size(); ++i) {
                    bool sel = (i == active);
                    if (ImGui::Selectable(cats[i].name.c_str(), sel)) {
                        if (i != active)
                            LOG_DEBUG("quickbar: category -> \"%s\"", cats[i].name.c_str());
                        active = i;
                        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            // Category selector counts as an input-active rect.
            g_QbIconRects.emplace_back(ImGui::GetItemRectMin(),
                                       ImGui::GetItemRectMax());
            // Full active name on hover when the preview was truncated.
            if (previewClip && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", cats[active].name.c_str());
        } else {
            // Tab-style buttons — active one highlighted blue. Wrap onto a
            // new line when the next button wouldn't fit horizontally.
            const ImGuiStyle& style = ImGui::GetStyle();
            float availW = ImGui::GetContentRegionAvail().x;
            float lineW  = 0.f;

            for (int i = 0; i < (int)cats.size(); ++i) {
                // Ellipsize a too-long name (like an emote name) instead of
                // hard-clipping it, so it never spills past the window edge;
                // the full name goes in a hover tooltip. textMaxW is the text
                // region if the button were forced to the full width.
                float textMaxW = std::max(1.f, availW - style.FramePadding.x * 2.f);
                std::string label = Ellipsize(cats[i].name, textMaxW);
                bool  clipped = (label != cats[i].name);
                // Natural width of the (possibly ellipsized) label - it now
                // fits, so the button can auto-size as normal.
                float btnW = ImGui::CalcTextSize(label.c_str()).x
                           + style.FramePadding.x * 2.f;

                if (i > 0) {
                    float needed = style.ItemSpacing.x + btnW;
                    if (lineW + needed <= availW) {
                        ImGui::SameLine();
                        lineW += needed;
                    } else {
                        lineW = btnW;  // new line starts fresh
                    }
                } else {
                    lineW = btnW;
                }

                bool isActive = (i == active);
                if (isActive) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.28f, 0.55f, 0.90f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.68f, 1.00f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f, 0.45f, 0.80f, 1.0f));
                }
                ImGui::PushID(i);
                if (ImGui::Button(label.c_str())) {
                    if (!isActive)
                        LOG_DEBUG("quickbar: category -> \"%s\"", cats[i].name.c_str());
                    active = i;
                    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
                }
                // Each tab button gets its own input-active rect so users
                // can click any of them while click-through is on, even
                // though the gaps between them stay click-through.
                g_QbIconRects.emplace_back(ImGui::GetItemRectMin(),
                                           ImGui::GetItemRectMax());
                // Full name on hover when the label was clipped — read the
                // button's hover state now, before SetTooltip opens its own
                // window (which would change the "last item").
                bool showTip = clipped && ImGui::IsItemHovered();
                ImGui::PopID();
                if (isActive) ImGui::PopStyleColor(3);
                if (showTip) ImGui::SetTooltip("%s", cats[i].name.c_str());
            }
        }
        // Capture the bottom of the bar — use the cursor's Y position
        // before the separator so the rect tracks the actual button row
        // height, including wrapped tab lines.
        float barBottomY = ImGui::GetCursorScreenPos().y;
        s_qbBarMin = barTop;
        s_qbBarMax = ImVec2(barTop.x + barWidth, barBottomY);
        if (hiContrastPushes > 0) ImGui::PopStyleColor(hiContrastPushes);
        ImGui::Separator();
    }

    // Build cell list for the active category (no filter/search in the QB).
    const QbCat& activeCat = cats[active];
    std::vector<CellInfo> items;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        PROFILE_SCOPE("qb.build");  // dev perf overlay
        // Index the catalog once (O(N)) so resolving a favorites category's
        // ids is O(1) each instead of FindEmote's linear scan - the cost the
        // user hit with a category holding many favorites. unlocked() is
        // precomputed per cell so RenderEmoteCell doesn't re-derive it. Same
        // per-frame, no-caching approach the main panel uses (shared helper).
        CatalogIndex idx;
        BuildCatalogIndex(g_Settings.ManuallyUnlocked, idx);

        // /me-motes snapshot for favorites mixing + the dedicated category.
        // Built under g_MeMotesMutex once so the cell loop can dereference
        // freely without nesting locks.
        std::unordered_map<std::string, const MeMote*> meMotesById;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            meMotesById.reserve(g_MeMotes.size());
            for (const auto& mm : g_MeMotes) meMotesById[mm.Id] = &mm;
        }

        if (activeCat.kind == QbCatKind::Favorite) {
            // Favorites: resolve the stored refs in the user's order. Each
            // ref is routed to its catalog by Type (Emote via idx.byId,
            // /me-mote via meMotesById); CellInfo carries the right pointer
            // and RenderEmoteCell dispatches accordingly.
            const auto& cat = g_Settings.FavoriteCategories[activeCat.favIdx];
            for (size_t i = 0; i < cat.Refs.size(); ++i) {
                const auto& ref = cat.Refs[i];
                if (ref.Type == EFavoriteRefType::Emote) {
                    auto it = idx.byId.find(ref.Id);
                    if (it == idx.byId.end()) continue;
                    const Emote* e = it->second;
                    bool unlk = idx.unlocked(*e);
                    items.push_back({ e, nullptr, (int)i, unlk, std::string() });
                } else {  // MeMote
                    auto it = meMotesById.find(ref.Id);
                    if (it == meMotesById.end()) continue;
                    items.push_back({ nullptr, it->second, (int)i, /*unlocked=*/true, std::string() });
                }
            }
        } else if (activeCat.kind == QbCatKind::MeMotes) {
            // Dedicated /me-motes category: surface every /me-mote, sorted
            // by Name (or Id when Name empty).
            for (const auto& kv : meMotesById)
                items.push_back({ nullptr, kv.second, -1, /*unlocked=*/true, std::string() });
            std::sort(items.begin(), items.end(),
                [](const CellInfo& a, const CellInfo& b) {
                    const std::string& na = !a.m->Name.empty() ? a.m->Name : a.m->Id;
                    const std::string& nb = !b.m->Name.empty() ? b.m->Name : b.m->Id;
                    return na < nb;
                });
        } else {
            // Built-in Emote category: pull straight from the catalog by class,
            // read-only (favIdx -1), alphabetical by display name.
            for (const auto& e : g_Emotes) {
                bool unlk = idx.unlocked(e);
                bool take = false;
                switch (activeCat.kind) {
                    case QbCatKind::Core:        take = e.IsCore;           break;
                    case QbCatKind::MadKing:     take = e.IsMadKing;        break;
                    case QbCatKind::Unlocked:    take = !e.IsCore && unlk;  break;
                    case QbCatKind::UnlockedAll: take = unlk;               break;
                    default:                                                break;
                }
                if (take) items.push_back({ &e, nullptr, -1, unlk, std::string() });
            }
            std::sort(items.begin(), items.end(),
                [](const CellInfo& a, const CellInfo& b) { return a.e->Name < b.e->Name; });
        }
    }

    // Scroll model for this frame. Either snap setting (SnapWindow or
    // SnapScroll) flips us into "we OWN scrolling" mode: ImGui's auto-bar is
    // suppressed and we draw the custom slim-pill bar instead. Pure free mode
    // (neither set) keeps ImGui's bar exactly as before. ownScroll was
    // computed up at the outer-window flags above; fitScroll is only for the
    // window-snap callback's gating (the size constraints).
    const bool  fitScroll   = g_Settings.QuickbarSnapWindow;
    const bool  horizScroll = g_Settings.QuickbarHorizontalScroll;
    const bool  showBar     = g_Settings.QuickbarScrollIndicator == EQbScrollIndicator::Scrollbar;
    const float qbScrSz     = ImGui::GetStyle().ScrollbarSize;
    // Same-frame overflow prediction. The cell renderer (Cells.cpp's
    // RenderEmoteSection) will compute the exact same GridFit downstream
    // because it calls the same helper with the same avail; both branches
    // are guaranteed to agree, so the gutter we reserve here matches the
    // layout that lands inside the child. No lag, no flicker at the boundary.
    //
    // The prediction uses the avail BEFORE we reserve a gutter (the "what
    // would fit with no bar?" question) - if the answer is "doesn't fit",
    // we reserve, and Cells.cpp re-computes against the shrunken avail. Per
    // the monotonic-overflow note: reserving the gutter never reduces
    // overflow (only shrinks the content), so this can't oscillate.
    bool predictedOverflow = false;
    if (!items.empty()) {
        const ImGuiStyle& st = ImGui::GetStyle();
        EViewMode mode = g_Settings.QuickbarViewMode;
        float qbScale = std::max(g_Settings.QuickbarIconScale, MinIconScaleForMode(mode));
        float cw, ch, isz;
        EmoteCellSize(mode, qbScale, cw, ch, isz);
        float topPad = EmoteGridTopPad(mode);
        float gridTopOffset = (topPad > 0.f) ? topPad + st.ItemSpacing.y : 0.f;
        ImVec2 predAvail = ImGui::GetContentRegionAvail();
        predAvail.y = std::max(1.f, predAvail.y - gridTopOffset);
        GridFit gf = ComputeQuickbarGridFit((int)items.size(), predAvail,
                                             ImVec2(cw, ch),
                                             ImVec2(st.ItemSpacing.x, st.ItemSpacing.y),
                                             horizScroll);
        predictedOverflow = horizScroll ? gf.overflowsX : gf.overflowsY;
    }
    const bool barActive = ownScroll && showBar && predictedOverflow;
    float  qbGutter = barActive ? qbScrSz : 0.f;
    ImVec2 qbChildSizeArg(0, 0);
    if (qbGutter > 0.f) {
        ImVec2 av = ImGui::GetContentRegionAvail();
        qbChildSizeArg = horizScroll
            ? ImVec2(av.x, std::max(1.f, av.y - qbGutter))
            : ImVec2(std::max(1.f, av.x - qbGutter), av.y);
    }
    {
        ImGuiWindowFlags childFlags = ImGuiWindowFlags_None;
        if (!g_Settings.ShowQuickbarBg)
            childFlags |= ImGuiWindowFlags_NoBackground;
        if (ownScroll) {
            // Own the wheel and draw our own bar; never let ImGui add one.
            childFlags |= ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse;
        } else {
            // Pure free mode: ImGui's scrollbar, gated on the setting. The
            // SnapScroll case is handled by the ownScroll branch above, so
            // here horizontal routing is the only remaining reason to take
            // the wheel ourselves.
            if (!showBar) childFlags |= ImGuiWindowFlags_NoScrollbar;
            if (horizScroll && showBar)
                childFlags |= ImGuiWindowFlags_HorizontalScrollbar;
            if (horizScroll)
                childFlags |= ImGuiWindowFlags_NoScrollWithMouse;
            // Scroll-wrap drives the (vertical) wheel itself, so take it from
            // ImGui in smooth mode too; the scrollbar still drags normally
            // (NoScrollWithMouse only disables the wheel, not the bar).
            else if (g_Settings.QuickbarScrollWrap)
                childFlags |= ImGuiWindowFlags_NoScrollWithMouse;
        }
        // The child is a separate ImGui window with its own hit-test, so
        // parent's NoInputs alone doesn't pass clicks through the body.
        // Mirror the flag onto the child too.
        if (qbFlags & ImGuiWindowFlags_NoInputs)
            childFlags |= ImGuiWindowFlags_NoInputs;
        ImGui::BeginChild("##qbcontent", qbChildSizeArg, false, childFlags);
    }
    // Child content region this frame - used for the drag-snap inset
    // (window size - this) at the end of the render.
    ImVec2 qbChildAvail = ImGui::GetContentRegionAvail();
    // Wheel handling we own (NoScrollWithMouse set above). Two reasons to own
    // it: horizontal mode routes the Y-wheel to X-scroll (ImGui only does Y);
    // and owned-scroll mode (either snap setting) steps by whole cells when
    // SnapScroll is on - "scroll to the end of a long list, then back up"
    // re-aligns the top row instead of inheriting ImGui's ragged max-scroll
    // offset. Hit-test the cursor against the window rect directly so this
    // works under click-through too (NoInputs suppresses ImGui hover, not the
    // wheel). g_QbStep* are last frame's cell steps (set by RenderEmoteSection).
    {
        bool horiz = horizScroll;
        // Snap mode drives both the per-notch step AND the round-to-grid: Off
        // is smooth (snap=false, step = font*5px), Cells = one cell per notch,
        // Pages = the full visible viewport per notch (g_QbRows/g_QbCols last
        // frame's count). pageStep falls back to cellStep when the QB hasn't
        // drawn yet (vis count == 0), so the very first wheel works.
        bool snap  = g_Settings.QuickbarSnapScroll != EQbScrollSnap::Off;
        bool pages = g_Settings.QuickbarSnapScroll == EQbScrollSnap::Pages;
        // Own the wheel whenever the child can't scroll itself: horizontal
        // routing OR owned-scroll mode (child is NoScrollWithMouse there).
        // Note: snap is NOT forced on by SnapWindow - the cell-aligned
        // max-scroll clamp (g_QbMaxScroll*, below) is what lands owned-scroll
        // ends flush, so the user can still pick smooth scrolling while
        // fitting the window.
        // Also own it when the WndProc routed a delta (click-through: the
        // message was consumed before ImGui, so we apply the routed value).
        // And in pure-free SMOOTH vertical mode when scroll-wrap is on - we
        // drive the wheel ourselves there to implement the wrap (the child is
        // NoScrollWithMouse above for the same reason).
        const bool wrap = g_Settings.QuickbarScrollWrap;
        bool own   = horiz || ownScroll || fromWndProc || (wrap && !horiz && !ownScroll);
        ImGuiIO& io = ImGui::GetIO();
        if (own && qbWheel != 0.f) {
            ImVec2 mp = io.MousePos, wPos = ImGui::GetWindowPos(), wSz = ImGui::GetWindowSize();
            // Include the gutter strip on the scroll axis so wheel still
            // works when the cursor is over our custom scrollbar. The gutter
            // sits just outside the child's content rect on that axis -
            // without this inflation, hovering the bar drops the wheel.
            float overX = horiz ? 0.f : qbGutter;
            float overY = horiz ? qbGutter : 0.f;
            // !qbOccludedByOther: yield the wheel to any ImGui window stacked on
            // top of the bar (z-order), matching the main panel. Over the bare
            // game world HoveredWindow is null, so click-through scroll is kept.
            bool over = !qbOccludedByOther &&
                        mp.x >= wPos.x && mp.x <= wPos.x + wSz.x + overX &&
                        mp.y >= wPos.y && mp.y <= wPos.y + wSz.y + overY;
            if (over) {
                // Owned scroll bounds by whole cells (g_QbMaxScroll*, the
                // cell-aligned max); pure free mode uses ImGui's pixel
                // ScrollMax.
                // Page step = viewport size on the scroll axis (NOT total
                // content). g_QbRows / g_QbCols hold TOTAL row/col counts on
                // the scroll axis; the visible portion is what's left after
                // subtracting g_QbMaxScroll*. When content fits, maxScroll is
                // 0 and pageStep collapses to a single viewport.
                //
                // Pages mode goes through PageWheelScroll, which steps in the
                // wheel direction (floor/ceil, not round-to-nearest) - so
                // wheel-up from the end always lands on the page boundary
                // strictly below, even when maxS is not a whole multiple of
                // pageStep. Wrap fires only when no movement is possible
                // (parked at the extreme); a cur mid-content never wraps.
                // Cells / Off keep the existing WheelScroll path: cells'
                // maxS is a multiple of cellStep (the cull guarantees it), so
                // round-to-nearest works cleanly.
                if (horiz) {
                    const float cellStep = g_QbStepX;
                    const float maxS = ownScroll
                        ? std::min(g_QbMaxScrollX, ImGui::GetScrollMaxX())
                        : ImGui::GetScrollMaxX();
                    if (pages) {
                        const float pageStep = std::max(cellStep,
                                                        g_QbCols * cellStep - g_QbMaxScrollX);
                        ImGui::SetScrollX(PageWheelScroll(ImGui::GetScrollX(),
                                                          qbWheel, pageStep, maxS, wrap));
                    } else {
                        const float step = (snap && cellStep > 1.f) ? cellStep
                                                                    : ImGui::GetFontSize() * 5.f;
                        ImGui::SetScrollX(WheelScroll(ImGui::GetScrollX(), qbWheel, step,
                                                      cellStep, maxS, snap, wrap));
                    }
                } else {  // vertical (owned: snap / fit mode, or scroll-wrap)
                    const float cellStep = g_QbStepY;
                    const float maxS = ownScroll
                        ? std::min(g_QbMaxScrollY, ImGui::GetScrollMaxY())
                        : ImGui::GetScrollMaxY();
                    if (pages) {
                        const float pageStep = std::max(cellStep,
                                                        g_QbRows * cellStep - g_QbMaxScrollY);
                        ImGui::SetScrollY(PageWheelScroll(ImGui::GetScrollY(),
                                                          qbWheel, pageStep, maxS, wrap));
                    } else {
                        const float step = (snap && cellStep > 1.f) ? cellStep
                                                                    : ImGui::GetFontSize() * 5.f;
                        ImGui::SetScrollY(WheelScroll(ImGui::GetScrollY(), qbWheel, step,
                                                      cellStep, maxS, snap, wrap));
                    }
                }
            }
        }
    }
    // Pin scroll to cell-aligned bounds (last frame's g_QbMaxScroll*, =0 when
    // the content fits): the ~1px ImGui ContentSize overhead is never
    // scrollable, so a fitting category doesn't drift by a pixel and an
    // overflowing one ends flush on the last whole row/column (no notch).
    // Applies whenever we own scrolling - either snap setting.
    if (ownScroll) {
        if (ImGui::GetScrollX() > g_QbMaxScrollX) ImGui::SetScrollX(g_QbMaxScrollX);
        if (ImGui::GetScrollY() > g_QbMaxScrollY) ImGui::SetScrollY(g_QbMaxScrollY);
    }
    if (items.empty()) {
        // TextWrapped + a manual disabled color so narrow QB windows don't
        // overflow horizontally with the hint text.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", L("qb.empty_category"));
        ImGui::PopStyleColor();
    } else {
        // Defensive clamps for stored values older than the current
        // bounds (or edited by hand in settings.json). The Options slider
        // already enforces these, but the renderer shouldn't trust it.
        float qbScale = std::max(g_Settings.QuickbarIconScale,
                                 MinIconScaleForMode(g_Settings.QuickbarViewMode));
        { PROFILE_SCOPE("qb.draw");  // dev perf overlay
        RenderEmoteSection(items, /*allowReorder=*/false, /*categoryIdx=*/-1,
                           g_Settings.QuickbarViewMode, qbScale,
                           /*isQuickbar=*/true,
                           /*horizontal=*/g_Settings.QuickbarHorizontalScroll);
        }
    }
    // Capture scroll state + child rect INSIDE the child (scroll queries read
    // the current window) for the custom bar, the grace gate, and the inset.
    ImGuiWindow* childWin = ImGui::GetCurrentWindow();
    float  qbScrollY    = ImGui::GetScrollY(),    qbScrollMaxY = ImGui::GetScrollMaxY();
    float  qbScrollX    = ImGui::GetScrollX(),    qbScrollMaxX = ImGui::GetScrollMaxX();
    ImVec2 qbChildPos   = childWin->Pos;
    ImVec2 qbChildSize  = childWin->Size;
    s_qbChildLast  = childWin;
    // Overflow as an integer cell-count test (RenderEmoteSection set
    // g_QbOverflow this frame): more cells than the viewport shows. This is
    // immune to ImGui's sub-pixel ContentSize rounding (which a pixel scrollMax
    // threshold had to paper over with a magic constant) and to view-mode /
    // scale changes. Empty category never overflows (the grid early-returns
    // without refreshing the flag).
    s_qbScrollable = !items.empty() && g_QbOverflow;

    // Publish the click-through wheel-capture rect for the WndProc (+plus only;
    // QbWheelPublish is a no-op in base builds, and gated on the Plus toggle inside).
    // Decoupled from scrollability: capture wherever the wheel has a job, with
    // the EXACT rect it acts on, so category cycling works over the bar / empty
    // content even when the list fully fits:
    //   - whole QB window when the content scrolls OR "cycle anywhere" is on;
    //   - just the category-bar rect when only "cycle over bar" applies (so a
    //     wheel over static content there still passes to the game);
    //   - otherwise nothing (passes through).
    // g_QbWin* / s_qbBar* are this frame's rects in ImGui display (== game
    // client) coords, matching the WndProc's ScreenToClient.
    {
        bool ctActive = g_Settings.QuickbarClickThrough && !g_Settings.ShowQuickbarBg;
        bool multiCat = cats.size() > 1;
        bool anywhere = multiCat && g_Settings.QuickbarWheelCycle == EWheelCycle::Anywhere;
        bool overBar  = multiCat && g_Settings.QuickbarWheelCycle == EWheelCycle::OverBar;
        if (ctActive && (s_qbScrollable || anywhere)) {
            QbWheelPublish(true, g_QbWinX, g_QbWinY, g_QbWinW, g_QbWinH);
        } else if (ctActive && overBar && s_qbBarMin.x < s_qbBarMax.x) {
            QbWheelPublish(true, s_qbBarMin.x, s_qbBarMin.y,
                           s_qbBarMax.x - s_qbBarMin.x, s_qbBarMax.y - s_qbBarMin.y);
        } else {
            QbWheelPublish(false, 0.f, 0.f, 0.f, 0.f);
        }
    }
    ImGui::EndChild();

    if (barActive) {
        // Custom scrollbar in the reserved gutter, drawn after EndChild (in
        // the parent draw list, unclipped) and styled from ImGui's own
        // scrollbar colors so it matches the main panel's stock bar.
        ImVec2 barMin, barMax;
        QbCustomScrollbar(childWin, /*axisY=*/!horizScroll,
                          horizScroll ? qbScrollX       : qbScrollY,
                          horizScroll ? g_QbMaxScrollX  : g_QbMaxScrollY,  // cell-aligned max
                          horizScroll ? qbChildSize.x   : qbChildSize.y,
                          qbChildPos, qbChildSize, qbGutter, flattenChrome,
                          barMin, barMax);
        // Keep it grabbable AND consume clicks (don't pass to game) under
        // click-through.
        if (g_Settings.QuickbarClickThrough)
            g_QbIconRects.emplace_back(barMin, barMax);
    } else if (g_Settings.QuickbarClickThrough && childWin) {
        // Free mode (unchanged): keep ImGui's scrollbar strips grabbable
        // while click-through is on.
        float scrSz = ImGui::GetStyle().ScrollbarSize;
        if (childWin->ScrollbarY)
            g_QbIconRects.emplace_back(
                ImVec2(qbChildPos.x + qbChildSize.x - scrSz, qbChildPos.y),
                ImVec2(qbChildPos.x + qbChildSize.x,         qbChildPos.y + qbChildSize.y));
        if (childWin->ScrollbarX)
            g_QbIconRects.emplace_back(
                ImVec2(qbChildPos.x,                 qbChildPos.y + qbChildSize.y - scrSz),
                ImVec2(qbChildPos.x + qbChildSize.x, qbChildPos.y + qbChildSize.y));
    }

    // Purely visual scroll-edge hints (no g_QbIconRects - not interactive). Drawn
    // on the foreground list (see the helper), no layout reservation. Adapts to
    // the active scroll axis (horizScroll -> X, else Y). The end-gate uses the
    // cell-aligned max in BOTH modes (g_QbMaxScroll*), so the bottom/right hint
    // clears flush on the last whole row/column. Gutter trim keeps it off the bar:
    // the reserved custom-bar gutter in owned mode, ImGui's own bar width in free.
    if (g_Settings.QuickbarScrollIndicator == EQbScrollIndicator::Hints
            && g_QbOverflow && !items.empty() && childWin) {
        float hScroll = horizScroll ? qbScrollX      : qbScrollY;
        float hMax    = horizScroll ? g_QbMaxScrollX : g_QbMaxScrollY;
        float hGutter;
        if (barActive) {
            hGutter = qbGutter;
        } else {
            bool imBar = horizScroll ? childWin->ScrollbarX : childWin->ScrollbarY;
            hGutter = imBar ? ImGui::GetStyle().ScrollbarSize : 0.f;
        }
        QbScrollEdgeHints(/*axisY=*/!horizScroll, hScroll, hMax,
                          qbChildPos, qbChildSize, hGutter,
                          flattenChrome, g_Settings.QuickbarHighContrast,
                          /*wrap=*/g_Settings.QuickbarScrollWrap,
                          /*bordered=*/g_Settings.QuickbarViewMode == EViewMode::TextOnly);
    }

    // Measure the chrome inset (window size minus the child's content avail,
    // captured before the grid's top-pad) for next frame's snap. Only the
    // chrome is lagged; step + top offset are recomputed live at the top.
    {
        float insetX = g_QbWinW - qbChildAvail.x;
        float insetY = g_QbWinH - qbChildAvail.y;
        if (insetX >= 0.f && insetY >= 0.f) {
            s_qbInsetX = insetX;
            s_qbInsetY = insetY;
            s_qbInsetValid = true;
        }
    }

#ifdef EMOT3_DEVTOOLS
    // Dev sizing readout: snapshot the real numbers so the fit/snap math can
    // be diagnosed from actual values (see QuickbarDebug.h). Gated by
    // EMOT3_DEVTOOLS (the dev tools axis), not EMOT3_PLUS (the swallow axis).
    if (qbdbg::Enabled()) {
        const ImGuiStyle& st = ImGui::GetStyle();
        float topPad = EmoteGridTopPad(g_Settings.QuickbarViewMode);
        qbdbg::QbMetrics& dm = qbdbg::M();
        dm.winW = g_QbWinW;            dm.winH = g_QbWinH;
        dm.availX = qbChildAvail.x;    dm.availY = qbChildAvail.y;
        dm.contentX = childWin->ContentSize.x; dm.contentY = childWin->ContentSize.y;
        dm.scrollMaxX = qbScrollMaxX;  dm.scrollMaxY = qbScrollMaxY;
        dm.scrollX = qbScrollX;        dm.scrollY = qbScrollY;
        dm.cellMaxX = g_QbMaxScrollX;  dm.cellMaxY = g_QbMaxScrollY;
        dm.stepX = g_QbStepX;          dm.stepY = g_QbStepY;
        dm.insetX = s_qbInsetX;        dm.insetY = s_qbInsetY;
        dm.gridTopOffset = (topPad > 0.f) ? topPad + st.ItemSpacing.y : 0.f;
        dm.gutter = qbGutter;
        dm.cols = g_QbCols;            dm.rows = g_QbRows;
        dm.items = (int)items.size();
        dm.fit = fitScroll;            dm.horiz = horizScroll;
        dm.barActive = barActive;      dm.scrollable = s_qbScrollable;
    }
#endif

    // Refusal line (replaces Nexus toasts): pops up at the click on the
    // foreground draw list (on top of the grid), reserves no layout. Gated to
    // Quickbar-origin messages; high-contrast follows the Quickbar setting.
    DrawFeedbackOverlay(FeedbackSurface::Quickbar, g_Settings.QuickbarHighContrast);

    ImGui::End();
    if (flattenChrome) ImGui::PopStyleColor(7);
}
