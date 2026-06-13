#include "Options.h"
#include "OptionsCommon.h"
#include "CharacterState.h"  // RTApiConnected for the precise-detection status line
#include "ChatWatch.h"        // ChatWatchAvailable (auto-motes dependency gate)
#include "EmoteData.h"        // g_Emotes / g_EmotesMutex (auto-mote trigger count)
#include "Globals.h"
#include "I18n.h"
#include "Settings.h"
#include "SaveScheduler.h"   // RequestSave (debounced, off-thread settings writes)
#include "PlusSettings.h"    // g_PlusSettings ("send while moving"; empty in base builds)
#include "Layout.h"          // ToggleButton
#include "Usage.h"           // usage::Reset (the Reset-usage button)
#include "NexusShortcut.h"   // ApplyNexusShortcut on settings changes
#include "Logging.h"         // LOG_TRACE (setting-change audit trail)
#include "UpdateCheck.h"     // Plus update banner (PlusUpdateAvailable / OpenReleasesPage; stubs otherwise)
#include "Profiling.h"       // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

#include <algorithm>   // std::max (reset-usage modal button sizing)
#include <mutex>       // g_EmotesMutex (auto-mote trigger count)
#include <string>
#include <vector>
// ---- Tab: General -----------------------------------------------------

void RenderGeneralOptionsTab() {
    PROFILE_SCOPE("opt.general");  // dev perf overlay

    // Plus-only: a newer release is available. The public build auto-updates via
    // Nexus; the Plus build is manual, so surface a banner with the releases link +
    // a "copy link" button (we copy rather than open a browser - keeps the player
    // in-game). PlusUpdateAvailable() is a false stub in non-Plus builds, so this
    // whole block compiles away there.
    if (PlusUpdateAvailable()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, 1.0f));
        ImGui::TextWrapped(L("opt.gen.update_available"), PlusLatestVersion().c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%s", ReleasesUrl());
        if (ImGui::Button(L("opt.gen.update_copy")))
            ImGui::SetClipboardText(ReleasesUrl());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // Primary visibility toggle at the top of the tab - same
    // affordance as the Quickbar tab's main toggle, same primary-
    // action styling (bumped frame padding + Spacing + inline
    // status), and the same label-flip so the verb describes the
    // click action rather than promising "Show" when the window is
    // already open. Replaces the old "Show panel during gameplay"
    // checkbox, which read like an edge-case toggle rather than the
    // primary visibility control it actually is.
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, 6.f));
    const char* mainToggleLabel = g_Settings.ShowWindow
                                  ? L("opt.gen.hide_main")
                                  : L("opt.gen.show_main");
    // Window visibility is navigation state — ToggleButton flips ShowWindow in
    // place; no eager save (rides along the next real write / unload flush).
    ToggleButton(mainToggleLabel, &g_Settings.ShowWindow);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        TooltipText("opt.gen.toggle_tooltip");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_Settings.ShowWindow ? L("opt.currently_visible")
                                                    : L("opt.currently_hidden"));
    ImGui::Spacing();

    // ===== Language =====
    OptionsSection(L("opt.sec.language"));
    {
        // "auto" + every UI language we ship a table for. We never list a
        // language we don't have; an unsupported Nexus language under
        // "auto" simply renders English (handled by L()'s fallback).
        const std::vector<std::string>& uiLangs = AvailableUiLanguages();
        const std::string& cur = g_Settings.UiLanguage;
        std::string curLabel = (cur == "auto" || cur.empty())
            ? std::string(L("options.ui_language.auto"))
            : UiLanguageDisplayName(cur);

        ImGui::Text("%s:", L("options.ui_language.label"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::BeginCombo("##uilang", curLabel.c_str())) {
            bool autoSel = (cur == "auto" || cur.empty());
            if (ImGui::Selectable(L("options.ui_language.auto"), autoSel) && !autoSel) {
                g_Settings.UiLanguage = "auto";
                SetUiLanguage("auto");
                RequestSave(SaveKind::Settings);
            }
            for (const auto& code : uiLangs) {
                bool sel = (cur == code);
                if (ImGui::Selectable(UiLanguageDisplayName(code).c_str(), sel) && !sel) {
                    g_Settings.UiLanguage = code;
                    SetUiLanguage(code);
                    RequestSave(SaveKind::Settings);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            TooltipText("options.ui_language.tooltip");

        // Spell out the interface-vs-emote-language split here: this control
        // only affects emot3's own UI text. The wording of the emote
        // commands/names is a separate setting in the Emotes tab. Muted so it
        // reads as a footnote to the control, not another control.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", L("options.ui_language.note"));
        ImGui::PopStyleColor();
    }

    // ===== Notifications =====
    // Whether emot3 prompts (a first-run-style dialog in the Library) when a
    // version update added bundled emotes the user doesn't have. The dialog's
    // "Don't ask again" flips this too. See the notifier in MainPanel.cpp.
    OptionsSection(L("opt.sec.notifications"));
    CheckboxWithSaveAndTooltip("opt.gen.notify_new_bundled",
                               &g_Settings.NotifyNewBundledEmotes, /*defaultIsOn=*/true);

    // ===== Nexus access bar icon =====
    //
    // The two controls under this header are tightly related — one
    // toggles the icon, the other reassigns its left-click. Without the
    // section title the swap control read as orphaned ("which left
    // click?"); grouping them solves that.
    OptionsSection(L("opt.sec.nexus_icon"));

    if (CheckboxWithSaveAndTooltip("opt.gen.show_shortcut", &g_Settings.ShowNexusShortcut, /*defaultIsOn=*/true)) {
        ApplyNexusShortcut();   // register or remove based on the new state
    }

    // What a left-click on the icon opens (Library / Quickbar / palette),
    // indented under the parent toggle and gated on the icon being shown —
    // meaningless without an icon to click on. Right-click always opens the
    // context menu. Replaced the old Library/Quickbar swap checkbox when the
    // palette became a third target.
    ImGui::Indent();
    {
        const bool iconShown = g_Settings.ShowNexusShortcut;
        if (!iconShown) BeginDisabledCompat();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(L("opt.gen.shortcut_click"));
        ImGui::SameLine();
        int sc = (int)g_Settings.ShortcutClickAction;
        const char* scItems[] = { L("shortcut_target.library"),
                                  L("shortcut_target.quickbar"),
                                  L("shortcut_target.palette") };
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::Combo("##shortcutclick", &sc, scItems, IM_ARRAYSIZE(scItems)) &&
            iconShown) {
            g_Settings.ShortcutClickAction = (EShortcutClick)sc;
            LOG_TRACE("setting general.shortcut_click = %d", sc);
            RequestSave(SaveKind::Settings);
            ApplyNexusShortcut();   // re-register against the chosen keybind
        }
        if (ImGui::IsItemHovered()) {
            if (!iconShown) {
                TooltipText("opt.gen.swap_disabled");
            } else {
                static const TooltipOption kClickOpts[] = {
                    { "shortcut_target.library",  "shortcut_target.library.desc",  true  },
                    { "shortcut_target.quickbar", "shortcut_target.quickbar.desc", false },
                    { "shortcut_target.palette",  "shortcut_target.palette.desc",  false },
                };
                TooltipOptions("opt.gen.shortcut_click_tip", kClickOpts,
                               IM_ARRAYSIZE(kClickOpts));
            }
        }
        if (!iconShown) EndDisabledCompat();
    }
    ImGui::Unindent();

    // ===== Sending =====
    OptionsSection(L("opt.sec.sending"));

    // Global minimum interval between any two sends (anti-spam) - top of the
    // section. Shown in SECONDS (stored as ms); applies to EVERY surface (clicks,
    // keybinds, radial, auto-motes) via the shared send gate. Label behind the
    // slider, saves on release, right-click resets - matching the other sliders.
    {
        const float lo = kSendMinIntervalFloorMs / 1000.0f;
        const float hi = kSendMinIntervalCeilMs  / 1000.0f;
        float sec = g_Settings.SendMinIntervalMs / 1000.0f;
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::SliderFloat(L("opt.gen.send_interval"), &sec, lo, hi, "%.2f s"))
            g_Settings.SendMinIntervalMs = (uint32_t)(sec * 1000.0f + 0.5f);  // round to ms
        if (ImGui::IsItemDeactivatedAfterEdit()) RequestSave(SaveKind::Settings);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            g_Settings.SendMinIntervalMs = kSendMinIntervalDefaultMs;
            RequestSave(SaveKind::Settings);
        }
        if (ImGui::IsItemHovered()) TooltipText("opt.gen.send_interval_tip");
    }

    CheckboxWithSaveAndTooltip("opt.gen.send_on_click", &g_Settings.SendOnClick, /*defaultIsOn=*/true);

    // Independent of SendOnClick — /me-motes commit free-form text to chat,
    // which some users prefer to confirm-then-send rather than fire on click.
    // See SendOrFillMeMote in EmoteAction.cpp.
    CheckboxWithSaveAndTooltip("opt.gen.send_me_mote_on_click", &g_Settings.MeMoteSendOnClick, /*defaultIsOn=*/true);

    // Close an open chat / text box (Escape) and send, instead of refusing.
    CheckboxWithSaveAndTooltip("opt.gen.close_chat_on_send", &g_Settings.CloseChatOnSend, /*defaultIsOn=*/false);

    // Passive warning when the chat keybind isn't bound. See "Emote
    // send refusal gate" in emot3.md for the design.
    if (APIDefs && !APIDefs->GameBinds.IsBound(EGameBinds_UiChatCommand)) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", L("opt.gen.chat_unbound_warn"));
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    CheckboxWithSaveAndTooltip("opt.gen.send_on_target", &g_Settings.SendTargetableOnTarget, /*defaultIsOn=*/false);

#ifdef EMOT3_PLUS
    // +plus: "send while moving". Briefly holds your keyboard during the ~150ms
    // injection so held movement keys can't garble the command (you can click
    // mid-move). Default off = the safe refuse-when-a-printable-key-is-held gate.
    // Backed by plus.json; when on it disables the movement case of the "unusable
    // emotes" presentation below (the swallow handles held keys instead).
    PlusCheckbox("opt.gen.swallow_on_send", &g_PlusSettings.SwallowInputOnSend,
                 /*defaultIsOn=*/false);
#endif

#ifdef EMOT3_PLUS
    // ===== Updates (Plus only) =====
    // The base build's auto-updates (incl. its "AllowPrereleases" channel) come
    // from Nexus; Plus is Provider=None and runs its own notifier (UpdateCheck),
    // which Nexus' prerelease toggle doesn't reach. This is Plus' own opt-in to be
    // notified about preview/beta builds. Toggling re-arms the check so a pending
    // beta surfaces without a reload.
    OptionsSection(L("opt.sec.updates"));
    if (PlusCheckbox("opt.gen.notify_prereleases", &g_PlusSettings.NotifyPrereleases,
                     /*defaultIsOn=*/false)) {
        InitUpdateCheck();  // re-check against the newly selected channel
    }
#endif

    // ===== Icons =====
    OptionsSection(L("opt.sec.icons"));

    if (CheckboxWithSaveAndTooltip("opt.gen.ai_fallback", &g_Settings.UseAIIconFallback, /*defaultIsOn=*/false)) {
        // Bump both catalog epochs so newly-eligible entries re-resolve to their
        // AI artwork on next show (the fallback gates BOTH emote and /me-mote
        // tiers). With content-addressed keys this applies immediately - the
        // changed tier yields a new content key that loads fresh; no restart.
        MarkEmotesDirty();
        MarkMeMotesDirty();
    }

    CheckboxWithSaveAndTooltip("opt.gen.show_target_dot", &g_Settings.ShowTargetDot, /*defaultIsOn=*/true);

    // /me-mote indicator — small upper-right accent on /me-mote cells, marks
    // them as distinct from regular Emote cells. Same paragraph as the
    // target-dot toggle since they're conceptual siblings (and share the
    // top-right corner — never co-occur, /me-motes are never targetable).
    CheckboxWithSaveAndTooltip("opt.gen.show_me_mote_indicator", &g_Settings.ShowMeMoteIndicator, /*defaultIsOn=*/true);

    // How unusable emotes present on the Quickbar - one dropdown over the two
    // settings: Grey out / Hide / Do nothing. "Do nothing" is the off state
    // (QuickbarGreyUnusable=false), which also stops the send gate refusing
    // game-state blocks, matching the former master toggle. Display order
    // grey/hide/off; synth index maps to (QuickbarGreyUnusable, behaviour).
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L("opt.gen.unusable_behavior"));
    ImGui::SameLine();
    int uidx = !g_Settings.QuickbarGreyUnusable ? 2
             : (g_Settings.QuickbarUnusableBehavior == EUnusableBehavior::Hide ? 1 : 0);
    const char* uItems[] = { L("opt.gen.unusable_grey"), L("opt.gen.unusable_hide"),
                             L("opt.gen.unusable_off") };
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("##unusable", &uidx, uItems, IM_ARRAYSIZE(uItems))) {
        g_Settings.QuickbarGreyUnusable = (uidx != 2);
        if (uidx == 0)      g_Settings.QuickbarUnusableBehavior = EUnusableBehavior::Grey;
        else if (uidx == 1) g_Settings.QuickbarUnusableBehavior = EUnusableBehavior::Hide;
        LOG_TRACE("setting general.unusable: grey=%d behavior=%d",
                  g_Settings.QuickbarGreyUnusable, (int)g_Settings.QuickbarUnusableBehavior);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) {
        static const TooltipOption kUnusableOpts[] = {
            { "opt.gen.unusable_grey", "opt.gen.unusable_grey.desc", true  },
            { "opt.gen.unusable_hide", "opt.gen.unusable_hide.desc", false },
            { "opt.gen.unusable_off",  "opt.gen.unusable_off.desc",  false },
        };
        TooltipOptions("opt.gen.unusable_intro", kUnusableOpts, IM_ARRAYSIZE(kUnusableOpts));
    }

    // Detection + the extra sources only matter while the interaction isn't "off";
    // indent them under it so the grouping reads at a glance.
    if (g_Settings.QuickbarGreyUnusable) {
        ImGui::Indent();

        // Airborne (jumps + falls) - its own toggle, MumbleLink-derived (no addon).
        CheckboxWithSaveAndTooltip("opt.gen.airborne", &g_Settings.QuickbarAirborneDetection, /*defaultIsOn=*/true);

        CheckboxWithSaveAndTooltip("opt.gen.precise_state", &g_Settings.QuickbarPreciseStateDetection, /*defaultIsOn=*/true);
        // Live status: precise detection is a no-op without the RealTime API addon.
        if (RTApiConnected())
            ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.45f, 1.f), "%s", L("opt.gen.rtapi_connected"));
        else
            ImGui::TextDisabled("%s", L("opt.gen.rtapi_missing"));
#ifdef EMOT3_DEVTOOLS
        // Dev-tool raw probe so a "not found" report can be diagnosed in place.
        // A diagnostic, so it's on the dev-tools axis (EMOT3_DEVTOOLS): present
        // in Dev/Debug, absent from the public emot3.dll AND emot3_plus.dll.
        ImGui::TextDisabled("%s", RTApiDebugInfo());
#endif
        // The transient refusals (text box focused, moving / key held) now ride
        // this same grey/hide automatically - no separate opt-in (see Quickbar.cpp
        // qb.busy). "Send while moving" (+plus) and "close chat on send" carve out
        // the movement / textbox cases respectively.

        ImGui::Unindent();
    }

    // (The Unlocks section moved to its own "Unlocks" tab — see OptionsUnlocks.cpp.)

    // ===== Auto-motes (chat triggers) =====
    // Master enable + watched channels + the auto-fire cooldown. The per-emote
    // trigger WORDS live in the Catalog tab's "Chat triggers" field; this is the
    // on/off + scope. Needs the optional "Events: Chat" addon (core/ChatWatch) -
    // the notice below is advisory and the feature no-ops without it.
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

        // Watched channels - sub-settings, disabled until the feature is on. Manual
        // checkboxes wrapped in the disabled idiom (DisabledCheckbox would need
        // per-channel on/off tooltip keys; a single help label is plenty here).
        const bool active = g_Settings.AutoMotesEnabled;
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

    // Reset the usage history that feeds the recently/frequently-used Quickbar
    // categories (the toggles now live on the Quickbar tab). Its own section (like
    // the Catalog tab's "Clear catalog") so the destructive action reads as
    // separate. Modeled on Clear catalog (destructive button + centered confirm
    // modal) but a MILDER red - clearing usage is recoverable (it re-accrues).
    OptionsSection(L("opt.sec.usage"));
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.42f, 0.22f, 0.22f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.28f, 0.28f, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.68f, 0.32f, 0.32f, 0.90f));
        if (ImGui::Button(L("opt.gen.reset_usage")))
            ImGui::OpenPopup("###resetusage");
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("opt.gen.reset_usage.tip"));

        std::string resetTitle = std::string(L("opt.gen.reset_usage.title")) + "###resetusage";
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(resetTitle.c_str(), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextWrapped("%s", L("opt.gen.reset_usage.confirm"));
            ImGui::PopTextWrapPos();
            ImGui::TextDisabled("%s", L("opt.gen.reset_usage.note"));
            ImGui::Spacing();
            float btnW = std::max(ImGui::CalcTextSize(L("opt.gen.reset_usage.yes")).x,
                                  ImGui::CalcTextSize(L("common.cancel")).x)
                       + ImGui::GetStyle().FramePadding.x * 2.f + 8.f;
            if (ImGui::Button(L("opt.gen.reset_usage.yes"), ImVec2(btnW, 0))) {
                usage::Reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("common.cancel"), ImVec2(btnW, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // Favorite categories are created and managed entirely in the Library now
    // (add via its "+ Category" bar; collapse / drag-reorder / rename / delete
    // from each category header). That removed this tab's duplicate editor - see
    // RenderCategoryHeader in Cells.cpp.
}