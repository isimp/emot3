#include "OptionsCommon.h"

#include "Globals.h"    // g_SettingsPath
#include "I18n.h"       // L
#include "Settings.h"   // SaveSettings, EFavoriteRefType
#include "RadialExports.h"  // RadialWheelsContaining (the "via radial" note)
#include "Layout.h"     // Ellipsize (truncate long wheel names in the note)
#include "SaveScheduler.h"  // RequestSave (debounced, off-thread settings writes)
#include "PlusSettings.h" // SavePlusSettings (+plus toggles; empty in base builds)
#include "Logging.h"    // LOG_TRACE (setting-change audit trail)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
// Compose "<labelKey>.on" / "<labelKey>.off" for the On/Off tooltip overloads.
std::string OnKey (const char* labelKey) { return std::string(labelKey) + ".on";  }
std::string OffKey(const char* labelKey) { return std::string(labelKey) + ".off"; }
}  // namespace

bool CheckboxWithSaveAndTooltip(const char* labelKey, bool* state,
                                const char* tooltipKey) {
    bool changed = ImGui::Checkbox(L(labelKey), state);
    if (changed) {
        LOG_TRACE("setting %s = %s", labelKey, *state ? "on" : "off");
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText(tooltipKey);
    return changed;
}

bool CheckboxWithSaveAndTooltip(const char* labelKey, bool* state, bool defaultIsOn) {
    bool changed = ImGui::Checkbox(L(labelKey), state);
    if (changed) {
        LOG_TRACE("setting %s = %s", labelKey, *state ? "on" : "off");
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered())
        TooltipOnOff(OnKey(labelKey).c_str(), OffKey(labelKey).c_str(), defaultIsOn);
    return changed;
}

bool DisabledCheckbox(const char* labelKey, bool* state, bool enabled,
                      bool defaultIsOn, const char* disabledTipKey) {
    // Greyed-out-when-disabled checkbox. ItemFlags_Disabled still lets
    // IsItemHovered fire, so we can explain *why* it's greyed; on a disabled
    // click ImGui would still flip the bool, so revert it. When enabled the
    // tooltip is the On/Off two-liner (<labelKey>.on/.off); when disabled it's
    // the prose disabledTipKey. Auto-saves; returns true only on an enabled
    // change so callers can chain extra work (e.g. ApplyNexusShortcut).
    if (!enabled) BeginDisabledCompat();
    bool changed = ImGui::Checkbox(L(labelKey), state);
    if (!enabled) {
        EndDisabledCompat();
        if (changed) { *state = !*state; changed = false; }
    }
    if (ImGui::IsItemHovered()) {
        if (enabled)
            TooltipOnOff(OnKey(labelKey).c_str(), OffKey(labelKey).c_str(), defaultIsOn);
        else
            TooltipText(disabledTipKey);
    }
    if (changed) {  // only true on an enabled change (disabled clicks were reverted)
        LOG_TRACE("setting %s = %s", labelKey, *state ? "on" : "off");
        RequestSave(SaveKind::Settings);
    }
    return changed;
}

bool InputFieldWithHint(const char* id, const char* hintKey,
                        char* buf, size_t bufSize, const InputFieldOpts& opts,
                        bool* outActive, bool* outHovered) {
    const ImGuiStyle& st = ImGui::GetStyle();

    // Optional in-frame char counter: reserve a fixed-width zone on the right
    // (sized to the worst case so the input edge doesn't jitter as digits change).
    char   countBuf[24] = {0};
    ImVec4 countColor(0, 0, 0, 0);
    bool   over  = false;
    float  zoneW = 0.f;
    if (opts.charBudget > 0) {
        const int n      = (int)std::strlen(buf);
        const int warnAt = (opts.charBudget * 80) / 100;
        over = (n > opts.charBudget);
        std::snprintf(countBuf, sizeof countBuf, "%d / %d", n, opts.charBudget);
        if      (n <= warnAt) countColor = st.Colors[ImGuiCol_TextDisabled];
        else if (!over)       countColor = ImVec4(0.92f, 0.78f, 0.32f, 1.0f);  // amber - close
        else                  countColor = ImVec4(1.00f, 0.45f, 0.40f, 1.0f);  // red   - over
        char worst[24];
        std::snprintf(worst, sizeof worst, "%d / %d", opts.charBudget, opts.charBudget);
        zoneW = ImGui::CalcTextSize(worst).x + st.FramePadding.x * 2.f;
    }

    const float  fullW    = opts.width > 0.f ? opts.width : ImGui::GetContentRegionAvail().x;
    const ImVec2 framePos = ImGui::GetCursorScreenPos();
    const float  frameH   = ImGui::GetFrameHeight();
    const ImVec2 frameMax(framePos.x + fullW, framePos.y + frameH);

    // One unified frame bg under the whole field (input + counter zone), so the
    // reserved zone lights up WITH the input. active = live focus; hovered = the
    // whole rect (IsMouseHoveringRect clips to the visible region, so a row
    // scrolled partly out of view won't false-hover). Disabled fields skip the
    // hover/active highlight (the caller dims the lot via the Alpha style var).
    const bool active  = opts.enabled && (ImGui::GetActiveID() == ImGui::GetID(id));
    const bool hovered = opts.enabled && ImGui::IsMouseHoveringRect(framePos, frameMax);

    // All draw-list colors get the global Alpha applied by hand (the draw list
    // bypasses ImGui's per-vertex alpha), so a greyed (alpha-pushed) field dims
    // its frame/border/counter along with the input. GetColorU32 would apply it
    // for theme colors, but the invalid/border/counter literals need it too.
    auto scaled = [&](ImVec4 c) {
        c.w *= st.Alpha;
        return ImGui::ColorConvertFloat4ToU32(c);
    };
    ImVec4 base = opts.invalid
        ? (active  ? ImVec4(0.70f, 0.16f, 0.16f, 1.f)
         : hovered ? ImVec4(0.65f, 0.14f, 0.14f, 1.f)
                   : ImVec4(0.55f, 0.10f, 0.10f, 1.f))
        : ImGui::GetStyleColorVec4(active  ? ImGuiCol_FrameBgActive
                                 : hovered ? ImGuiCol_FrameBgHovered
                                           : ImGuiCol_FrameBg);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(framePos, frameMax, scaled(base), st.FrameRounding, ImDrawCornerFlags_All);

    // Input on top, transparent-framed so only our unified bg shows; narrowed by
    // the counter zone so typed text never reaches it.
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(0, 0, 0, 0));
    ImGui::SetNextItemWidth(opts.charBudget > 0 ? (fullW - zoneW) : fullW);
    const char* hint = hintKey ? L(hintKey) : "";
    const bool edited = ImGui::InputTextWithHint(id, hint, buf, bufSize);
    ImGui::PopStyleColor(3);

    if (opts.charBudget > 0) {
        const ImVec2 tsz = ImGui::CalcTextSize(countBuf);
        dl->AddText(ImVec2(frameMax.x - st.FramePadding.x - tsz.x,
                           framePos.y + (frameH - tsz.y) * 0.5f),
                    scaled(countColor), countBuf);
    }
    if (opts.invalid)
        dl->AddRect(framePos, frameMax, scaled(ImVec4(1.f, 80.f / 255.f, 80.f / 255.f, 1.f)),
                    st.FrameRounding, ImDrawCornerFlags_All, 2.0f);

    if (outActive)  *outActive  = active;
    if (outHovered) *outHovered = hovered;
    return edited;
}

// ---- RowCuller: off-screen culling for variable-height row lists -------------
void RowCuller::Begin() {
    // Visible band in CONTENT space. GetCursorPosY (read per row) already includes
    // the scroll offset, so it's directly comparable to [scrollY, scrollY+height].
    // A couple of lines of overscan avoids pop-in at the scroll edges.
    const float scroll = ImGui::GetScrollY();
    const float over   = ImGui::GetTextLineHeightWithSpacing() * 2.f;
    top_ = scroll - over;
    bot_ = scroll + ImGui::GetWindowHeight() + over;
    active_ = activeNext_;
    activeNext_.clear();
}

bool RowCuller::BeginRow(const std::string& id) {
    y0_ = ImGui::GetCursorPosY();
    auto it = h_.find(id);
    const float rh = (it == h_.end()) ? 0.f : it->second;   // 0 = height not learned yet
    // Render for real when: it's the row being edited (never interrupt typing),
    // its height is unknown (learn it this frame), or its band is on-screen.
    const bool visible = (id == active_) || rh <= 0.f ||
                         (y0_ + rh >= top_ && y0_ <= bot_);
    if (visible) return true;
    // Off-screen: reserve the same start-to-start advance with a single spacer.
    // The measured height already includes one trailing ItemSpacing.y, and the
    // Dummy gets its own trailing spacing, so subtract one to match exactly.
    float dummyH = rh - ImGui::GetStyle().ItemSpacing.y;
    if (dummyH < 1.f) dummyH = 1.f;
    ImGui::Dummy(ImVec2(1.f, dummyH));
    return false;
}

void RowCuller::EndRow(const std::string& id, bool active) {
    h_[id] = ImGui::GetCursorPosY() - y0_;   // start-to-start advance for this row
    if (active) activeNext_ = id;
}

void RowCuller::Forget(const std::string& id) { h_.erase(id); }

#ifdef EMOT3_PLUS
// Gold "Plus" tag rendered on the same line, after a +plus control, so it visibly
// reads as an emot3 (Plus) feature in the Options UI. Same gold as the update banner
// / chat-unbound warning. Hovering it explains the marker. Exposed (declared in
// OptionsCommon.h) so the RadialMenus Deploy button can tag itself too.
void PlusBadge() {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f), "Plus");
    if (ImGui::IsItemHovered()) TooltipText("opt.plus_badge");
}

bool PlusCheckbox(const char* labelKey, bool* state, bool defaultIsOn) {
    bool changed = ImGui::Checkbox(L(labelKey), state);
    if (changed) {
        LOG_TRACE("plus setting %s = %s", labelKey, *state ? "on" : "off");
        SavePlusSettings();
    }
    if (ImGui::IsItemHovered())
        TooltipOnOff(OnKey(labelKey).c_str(), OffKey(labelKey).c_str(), defaultIsOn);
    PlusBadge();
    return changed;
}

bool PlusDisabledCheckbox(const char* labelKey, bool* state, bool enabled,
                          bool defaultIsOn, const char* disabledTipKey) {
    // Same greyed-out-when-disabled dance as DisabledCheckbox, but persists to
    // plus.json (g_PlusSettings) rather than settings.json.
    if (!enabled) BeginDisabledCompat();
    bool changed = ImGui::Checkbox(L(labelKey), state);
    bool cbHovered = ImGui::IsItemHovered();  // capture before the badge becomes the last item
    PlusBadge();                              // dims with the row while inside the alpha push
    if (!enabled) {
        EndDisabledCompat();
        if (changed) { *state = !*state; changed = false; }
    }
    if (cbHovered) {
        if (enabled)
            TooltipOnOff(OnKey(labelKey).c_str(), OffKey(labelKey).c_str(), defaultIsOn);
        else
            TooltipText(disabledTipKey);
    }
    if (changed) {  // only true on an enabled change (disabled clicks were reverted)
        LOG_TRACE("plus setting %s = %s", labelKey, *state ? "on" : "off");
        SavePlusSettings();
    }
    return changed;
}
#endif  // EMOT3_PLUS

void OptionsSection(const char* title) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", title);
    ImGui::Separator();
}

void RadialMembershipNote(EFavoriteRefType type, const std::string& id,
                          bool hasPersonalKeybind) {
    std::vector<std::string> wheels = RadialWheelsContaining(type, id);
    if (wheels.empty()) return;
    ImGui::SameLine();
    // "also in" when the user already has a personal key; "active via" when the wheel
    // is the ONLY reason it's bound (so they know it works without ticking the box).
    const char* oneKey  = hasPersonalKeybind ? "opt.radial.also_one"  : "opt.radial.via_one";
    const char* manyKey = hasPersonalKeybind ? "opt.radial.also_many" : "opt.radial.via_many";
    std::string note;
    if (wheels.size() == 1)
        note = std::string(L(oneKey)) + " '" + Ellipsize(wheels[0], 160.0f) + "'";
    else
        note = std::string(L(manyKey)) + " (" + std::to_string(wheels.size()) + ")";
    ImGui::TextDisabled("- %s", note.c_str());
    if (ImGui::IsItemHovered())
        TooltipText(hasPersonalKeybind ? "opt.radial.also_tip" : "opt.radial.via_tip");
}
