#include "Feedback.h"

#include "Profiling.h"   // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)
#include "imgui/imgui.h"

#include <algorithm>
#include <cfloat>

namespace {

// Fade timing (seconds). Held fully opaque for kHold, then linearly faded to
// zero over kFade. Tuned to feel responsive - long enough to read a short line,
// short enough to never linger. Change here if it feels off.
constexpr double kHold  = 2.0;
constexpr double kFade  = 0.5;
constexpr double kTotal = kHold + kFade;

// Single-slot feedback state. start < 0 means "nothing showing".
struct State {
    std::string     msg;
    double          start  = -1.0;            // ImGui::GetTime() at last (re)trigger
    FeedbackSurface origin = FeedbackSurface::None;
    ImVec2          anchor = ImVec2(0, 0);    // click position, so it shows by the action
};
State           s_state;
FeedbackSurface s_activeSurface = FeedbackSurface::None;

}  // namespace

void SetActiveFeedbackSurface(FeedbackSurface surface) { s_activeSurface = surface; }

void ShowFeedback(const std::string& message) {
    if (message.empty()) return;
    // Single slot: newest wins; an identical repeat just refreshes the timer
    // (so mashing a blocked cell keeps one fresh line, never stacks). Anchor to
    // the cursor = where the click happened, so the line appears by the emote
    // you clicked rather than parked in a corner.
    s_state.msg    = message;
    s_state.start  = ImGui::GetTime();
    s_state.origin = s_activeSurface;
    s_state.anchor = ImGui::GetMousePos();
}

void DrawFeedbackOverlay(FeedbackSurface thisSurface, bool highContrast) {
    PROFILE_SCOPE("feedback");  // dev perf overlay - summed across both surfaces
    if (s_state.start < 0.0)            return;   // nothing showing
    if (s_state.origin != thisSurface) return;   // belongs to the other window

    const double elapsed = ImGui::GetTime() - s_state.start;
    if (elapsed >= kTotal) return;                 // expired (state overwritten next time)
    const float fade = (elapsed <= kHold)
        ? 1.0f
        : (float)(1.0 - (elapsed - kHold) / kFade);
    if (fade <= 0.0f) return;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiIO&    io    = ImGui::GetIO();
    // Foreground list = drawn last, so it sits ON TOP of the emote grid (which
    // lives in a child window that composites above the parent's draw list - a
    // window-draw-list overlay would land behind it).
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Roomier than a button and a touch larger than body text, so it reads as a
    // deliberate message rather than a faint hint. Metrics still theme-derived.
    const ImVec2 pad(std::max(style.FramePadding.x, 6.0f) + 6.0f,
                     std::max(style.FramePadding.y, 4.0f) + 4.0f);
    const float  fontSz = ImGui::GetFontSize() * 1.10f;
    const float  wrapW  = 320.0f;  // comfortable reading width; longer lines wrap
    const char*  text   = s_state.msg.c_str();
    const ImVec2 textSz = ImGui::GetFont()->CalcTextSizeA(fontSz, FLT_MAX, wrapW, text);

    const float minW  = 150.0f;    // short messages still read as a real panel
    const float pillW = std::max(textSz.x + 2.0f * pad.x, minW);
    const float pillH = textSz.y + 2.0f * pad.y;

    // Sit just ABOVE the click so the cursor doesn't cover it; flip below if it
    // would run off the top.
    const ImVec2 a = s_state.anchor;
    float top    = a.y - 14.0f - pillH;
    if (top < 4.0f) top = a.y + 18.0f;
    float left   = a.x - pillW * 0.5f;
    // Clamp fully on-screen (viewport), so it's never half-cut on a small bar.
    left = std::min(std::max(left, 4.0f), io.DisplaySize.x - pillW - 4.0f);
    top  = std::min(std::max(top,  4.0f), io.DisplaySize.y - pillH - 4.0f);
    const ImVec2 pMin(left, top), pMax(left + pillW, top + pillH);

    // Colours from the active theme (re-skins with the user's theme), with the
    // alpha floors legibility over the game world requires. Tooltip/popup idiom
    // (PopupBg + Border), not the scrollbar's grab colour.
    ImVec4 bg = style.Colors[ImGuiCol_PopupBg];
    bg.w = std::max(bg.w, highContrast ? 1.00f : 0.94f);
    ImVec4 bd = style.Colors[ImGuiCol_Border];
    bd.w = std::max(bd.w, highContrast ? 0.70f : 0.35f);  // faint edge; gentle in HC
    ImVec4 tx = style.Colors[ImGuiCol_Text];

    bg.w *= fade;
    bd.w *= fade;
    tx.w *= fade;

    const float rounding = style.FrameRounding;
    dl->AddRectFilled(pMin, pMax, ImGui::GetColorU32(bg), rounding);
    // ImDrawCornerFlags_All (1.80) so the border rounds to match the fill - the
    // default param is All, but we must name it to reach the thickness arg.
    dl->AddRect(pMin, pMax, ImGui::GetColorU32(bd), rounding,
                ImDrawCornerFlags_All, 1.0f);
    // Centre the (wrapped) text within the pill.
    const ImVec2 tpos(pMin.x + (pillW - textSz.x) * 0.5f,
                      pMin.y + (pillH - textSz.y) * 0.5f);
    dl->AddText(ImGui::GetFont(), fontSz, tpos, ImGui::GetColorU32(tx),
                text, nullptr, wrapW);
}
