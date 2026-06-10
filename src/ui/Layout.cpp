#include "Layout.h"

#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

void RightAlignCursor(float width) {
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width));
}

void RightAlignButtons(float w, int count) {
    RightAlignCursor(w * count + ImGui::GetStyle().ItemSpacing.x * (count - 1));
}

void BeginDisabledCompat() {
    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
}
void EndDisabledCompat() {
    ImGui::PopStyleVar();
    ImGui::PopItemFlag();
}

std::string Ellipsize(const std::string& name, float maxW) {
    if (ImGui::CalcTextSize(name.c_str()).x <= maxW) return name;
    std::string s = name;
    while (!s.empty()) {
        std::string trial = s + "..";
        if (ImGui::CalcTextSize(trial.c_str()).x <= maxW) return trial;
        s.pop_back();
    }
    return std::string("..");
}

// FitName (two-line label fit) used to live here. The only caller was the
// per-cell Full-mode label in RenderEmoteCell, which now goes through
// ui/TextCache (TextCache::FitNameCached) - same algorithm, but memoized on
// (emote id, max width) so it runs once per name change instead of once per
// cell per frame.

bool RenderStyledFallback(const char* uniqueId, const char* displayName,
                          float size, float alphaMul)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos    = ImGui::GetCursorScreenPos();

    bool clicked = ImGui::InvisibleButton(uniqueId, ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    auto C = [alphaMul](int r, int g, int b) {
        int a = (int)(255.f * alphaMul);
        return IM_COL32(r, g, b, a);
    };

    ImU32 bg, border;
    if (active)       { bg = C(180, 110, 50); border = C(220, 160, 80); }
    else if (hovered) { bg = C(145,  85, 40); border = C(200, 140, 75); }
    else              { bg = C( 95,  55, 30); border = C(170, 115, 60); }
    ImU32 txt = C(245, 230, 195);

    ImVec2 br(pos.x + size, pos.y + size);
    dl->AddRectFilled(pos, br, bg, 4.f);
    dl->AddRect(pos, br, border, 4.f, 0, 1.f);

    std::string label(displayName);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    while (ts.x > size - 6 && label.size() > 2) {
        label.pop_back();
        ts = ImGui::CalcTextSize(label.c_str());
    }
    dl->AddText(ImVec2(pos.x + (size - ts.x) * 0.5f,
                       pos.y + (size - ts.y) * 0.5f),
                txt, label.c_str());

    return clicked;
}

bool ToggleButton(const char* label, bool* state) {
    bool active = *state;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.28f, 0.55f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.68f, 1.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f, 0.45f, 0.80f, 1.0f));
    }
    bool clicked = ImGui::Button(label);
    if (active) ImGui::PopStyleColor(3);
    if (clicked) *state = !*state;
    return clicked;
}

int PushHighContrastButtonStyles(bool includeFrameBg) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // Resting state darkened (dim but solid); hover/active kept bright (full
    // alpha). The eye reads the RGB jump dim->bright, which survives the
    // translucent game world where an alpha-only delta wouldn't.
    auto darkenRest = [](ImVec4 c) {
        c.x *= 0.55f; c.y *= 0.55f; c.z *= 0.55f;
        if (c.w < 0.85f) c.w = 0.85f;
        return c;
    };
    auto brightenHover = [](ImVec4 c) {
        if (c.w < 1.0f) c.w = 1.0f;
        return c;
    };
    ImGui::PushStyleColor(ImGuiCol_Button,        darkenRest   (style.Colors[ImGuiCol_Button]));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brightenHover(style.Colors[ImGuiCol_ButtonHovered]));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  brightenHover(style.Colors[ImGuiCol_ButtonActive]));
    if (!includeFrameBg) return 3;
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        darkenRest   (style.Colors[ImGuiCol_FrameBg]));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, brightenHover(style.Colors[ImGuiCol_FrameBgHovered]));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  brightenHover(style.Colors[ImGuiCol_FrameBgActive]));
    return 6;
}

int PushDestructiveButtonStyles(bool includeText) {
    // Warm-red button used by destructive actions (Clear catalog, per-row
    // Delete). Readable as "this removes something" without shouting in the
    // resting state. includeText also tints the label light red (the per-row
    // SmallButton uses it; the larger Clear button doesn't).
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.12f, 0.12f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.85f, 0.22f, 0.22f, 1.00f));
    if (!includeText) return 3;
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 0.85f, 0.85f, 1.00f));
    return 4;
}

void PushInvalidInputStyle() {
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                           ImVec4(0.55f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                           ImVec4(0.65f, 0.14f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                           ImVec4(0.70f, 0.16f, 0.16f, 1.0f));
}
void PopInvalidInputStyle() {
    ImGui::PopStyleColor(3);
}
void DrawInvalidInputBorder() {
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(
        mn, mx, IM_COL32(255, 80, 80, 255),
        /*rounding*/ 3.f, /*flags*/ 0, /*thickness*/ 2.0f);
}
