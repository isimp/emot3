#include "OwnedScroll.h"

#include "QuickbarGeometry.h"  // g_QbStepX/Y, g_QbRows/Cols, g_QbMaxScroll*
#include "Settings.h"   // g_Settings.QuickbarSnapScroll / EQbScrollSnap

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // ImGuiWindow + SetScrollX/Y(window, ...)

#include <algorithm>
#include <cmath>

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

namespace {
float s_qbBarGrabPx = 0.f;   // cursor offset within the thumb at grab time

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
}  // namespace

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
// axis when more content lies that way. Drawn into the GRID CHILD's own draw list
// (`childWin->DrawList`), appended AFTER EndChild so it lands on top of the cells
// (later in the same list = on top) while staying in the Quickbar's window
// z-order - so other windows and the category-dropdown popup correctly render
// ABOVE it. (The earlier foreground-list approach rendered last GLOBALLY, so the
// hints shone through every window above the bar and over the open dropdown.) A
// parent-window overlay can't be used: the child composites above the parent, so
// it would land behind the cells. Clamped to the content rect, with NO layout
// reservation (the fit-to-grid snap math must stay untouched) and NO input
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
void QbScrollEdgeHints(ImDrawList* dl, bool axisY, float scroll, float scrollMax,
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

    // The child's draw list clip stack is in an unspecified state after EndChild;
    // pin it to the content rect so the hints can't be clipped away or bleed past
    // the grid. Balanced by a PopClipRect on every exit below.
    dl->PushClipRect(mn, mx, false);

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
        dl->PopClipRect();
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
    dl->PopClipRect();
}
