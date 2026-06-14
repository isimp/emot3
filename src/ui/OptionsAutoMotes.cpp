#include "OptionsCommon.h"   // RenderAutoMotesTab decl + OptionsSection / CheckboxWithSaveAndTooltip
#include "ChatWatch.h"       // ChatWatchAvailable (auto-motes dependency gate)
#include "EmoteData.h"       // g_Emotes / g_EmotesMutex (auto-mote trigger count)
#include "Globals.h"
#include "I18n.h"            // L() / TooltipText
#include "Settings.h"        // g_Settings + kAutoMoteMinInterval* constants
#include "SaveScheduler.h"   // RequestSave (debounced, off-thread settings writes)
#include "Profiling.h"       // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

#include <mutex>             // g_EmotesMutex (auto-mote trigger count)

// ---- Tab: Auto-motes --------------------------------------------------
// Chat-keyword -> auto-fire a catalog emote. Was a General section; promoted to its
// own tab in v1.5. Master enable + watched channels + map scope + the auto-fire
// cooldown. The per-emote trigger WORDS live in the Catalog tab's "Chat triggers"
// field. Needs the optional "Events: Chat" addon (core/ChatWatch) - the notice is
// advisory and the feature no-ops without it. Declared in OptionsCommon.h.
void RenderAutoMotesTab() {
    PROFILE_SCOPE("opt.automotes");  // dev perf overlay
    OptionsSection(L("opt.am.section"));
    {
        const bool available = ChatWatchAvailable();
        ImGui::TextWrapped("%s", L("opt.am.hlp_top"));
        if (!available) {
            // Amber dependency notice (same styling as the competitive banner).
            // Reads false until the first chat line if it loaded before us, so the
            // wording stays soft ("not detected").
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", L("opt.am.needs_events_chat"));
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();

        // Master enable. ALWAYS toggleable - the "not detected" notice above is
        // advisory, not a gate. A push-only dependency like Events: Chat can't be
        // probed reliably the way RTAPI's DataLink can (its EV_ADDON_LOADED doesn't
        // replay for our re-subscribe after a reload), so hard-disabling on
        // detection made an emot3 reload look broken until the first chat line.
        // Firing degrades gracefully to a no-op when the addon is absent. Mirrors
        // RTAPI's precise-state toggle, which is likewise usable + status-lined,
        // not disabled. Auto-saves like the other g_Settings checkboxes.
        CheckboxWithSaveAndTooltip("opt.am.enabled", &g_Settings.AutoMotesEnabled,
                                   /*defaultIsOn=*/false);
        const bool active = g_Settings.AutoMotesEnabled;

        // Won't-fire warning, right under the enable toggle: the feature is ON but a
        // sub-setting makes it inert - no watched channel, or no map type. Amber +
        // un-greyed, like the unbound-chat / Events:Chat notices, so an "enabled but
        // nothing happens" config is visible rather than silently dead.
        if (active) {
            const bool anyChannel =
                g_Settings.AutoMoteWatchLocal || g_Settings.AutoMoteWatchParty ||
                g_Settings.AutoMoteWatchMap   || g_Settings.AutoMoteWatchSquad ||
                g_Settings.AutoMoteWatchGuild || g_Settings.AutoMoteWatchWhisper;
            const bool anyMap =
                g_Settings.AutoMoteInOpenWorld || g_Settings.AutoMoteInInstances;
            const char* warn = !anyMap     ? "opt.am.warn_no_maps"
                             : !anyChannel ? "opt.am.warn_no_channels"
                             : nullptr;
            if (warn) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s", L(warn));
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }
        }

        // Watched channels - sub-settings, disabled until the feature is on. Manual
        // checkboxes wrapped in the disabled idiom (DisabledCheckbox would need
        // per-channel on/off tooltip keys; a single help label is plenty here).
        if (!active) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        }
        ImGui::TextDisabled("%s", L("opt.am.channels_label"));
        auto channelToggle = [&](const char* key, bool* v) {
            if (ImGui::Checkbox(L(key), v)) RequestSave(SaveKind::Settings);
        };
        channelToggle("opt.am.ch_local",   &g_Settings.AutoMoteWatchLocal);
        ImGui::SameLine(180.f);
        channelToggle("opt.am.ch_party",   &g_Settings.AutoMoteWatchParty);
        ImGui::SameLine(340.f);
        channelToggle("opt.am.ch_map",     &g_Settings.AutoMoteWatchMap);
        channelToggle("opt.am.ch_squad",   &g_Settings.AutoMoteWatchSquad);
        ImGui::SameLine(180.f);
        channelToggle("opt.am.ch_guild",   &g_Settings.AutoMoteWatchGuild);
        ImGui::SameLine(340.f);
        channelToggle("opt.am.ch_whisper", &g_Settings.AutoMoteWatchWhisper);

        // Where they may fire, by GW2 map type (reused from MumbleLink - no custom
        // map lists). Two coarse buckets; PvP/WvW are always off (the send gate
        // refuses there) so they aren't shown. Same greyed scope + helper.
        ImGui::Spacing();
        ImGui::TextDisabled("%s", L("opt.am.maps_label"));
        channelToggle("opt.am.map_open_world", &g_Settings.AutoMoteInOpenWorld);
        if (ImGui::IsItemHovered()) TooltipText("opt.am.map_open_world_tip");
        ImGui::SameLine(180.f);
        channelToggle("opt.am.map_instances",  &g_Settings.AutoMoteInInstances);
        if (ImGui::IsItemHovered()) TooltipText("opt.am.map_instances_tip");
        ImGui::TextDisabled("%s", L("opt.am.map_competitive_note"));

        // Auto-mote-specific minimum interval - ADDITIONAL to the global "Minimum
        // time between sends" (auto-fires obey both). Seconds (stored ms), label
        // behind the slider, right-click resets. Greyed with the channels.
        ImGui::Spacing();
        {
            const float lo = kAutoMoteMinIntervalFloorMs / 1000.0f;
            const float hi = kAutoMoteMinIntervalCeilMs  / 1000.0f;
            float sec = g_Settings.AutoMoteMinIntervalMs / 1000.0f;
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::SliderFloat(L("opt.am.min_interval"), &sec, lo, hi, "%.0f s"))
                g_Settings.AutoMoteMinIntervalMs = (uint32_t)(sec * 1000.0f + 0.5f);
            if (ImGui::IsItemDeactivatedAfterEdit()) RequestSave(SaveKind::Settings);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                g_Settings.AutoMoteMinIntervalMs = kAutoMoteMinIntervalDefaultMs;
                RequestSave(SaveKind::Settings);
            }
            if (ImGui::IsItemHovered()) TooltipText("opt.am.min_interval_tip");
        }

        if (!active) { ImGui::PopStyleVar(); ImGui::PopItemFlag(); }

        // Tie to the Catalog, where the per-emote trigger WORDS are set: show how
        // many emotes carry triggers, and nudge when the feature's on but nothing
        // is wired yet. Counting the catalog here is cheap (only while this tab is
        // open). Shown un-greyed - it's an informational link, not a control.
        int triggerCount = 0;
        {
            std::lock_guard<std::mutex> lk(g_EmotesMutex);
            for (const auto& e : g_Emotes) if (!e.AutoKeywords.empty()) ++triggerCount;
        }
        ImGui::Spacing();
        if (triggerCount > 0)
            ImGui::TextDisabled(L("opt.am.trigger_count"), triggerCount);
        else if (active)
            ImGui::TextDisabled("%s", L("opt.am.no_triggers"));
    }
}
