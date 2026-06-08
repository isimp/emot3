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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L(tooltipKey));
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
    if (!enabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
    bool changed = ImGui::Checkbox(L(labelKey), state);
    if (!enabled) {
        ImGui::PopStyleVar();
        ImGui::PopItemFlag();
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

#ifdef EMOT3_PLUS
namespace {
// Gold "Plus" tag rendered on the same line, after a +plus setting's label, so it
// visibly reads as an emot3 (Plus) feature in the Options UI. Same gold as the
// update banner / chat-unbound warning. Hovering it explains the marker.
void PlusBadge() {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f), "Plus");
    if (ImGui::IsItemHovered()) TooltipText("opt.plus_badge");
}
}  // namespace

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
    if (!enabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
    bool changed = ImGui::Checkbox(L(labelKey), state);
    bool cbHovered = ImGui::IsItemHovered();  // capture before the badge becomes the last item
    PlusBadge();                              // dims with the row while inside the alpha push
    if (!enabled) {
        ImGui::PopStyleVar();
        ImGui::PopItemFlag();
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

void RadialMembershipNote(EFavoriteRefType type, const std::string& id) {
    std::vector<std::string> wheels = RadialWheelsContaining(type, id);
    if (wheels.empty()) return;
    ImGui::SameLine();
    std::string note;
    if (wheels.size() == 1)
        note = std::string(L("opt.radial.via_one")) + " '" +
               Ellipsize(wheels[0], 160.0f) + "'";
    else
        note = std::string(L("opt.radial.via_many")) + " (" +
               std::to_string(wheels.size()) + ")";
    ImGui::TextDisabled("- %s", note.c_str());
    if (ImGui::IsItemHovered()) TooltipText("opt.radial.via_tip");
}
