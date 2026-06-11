#include "Options.h"
#include "OptionsCommon.h"
#include "CharacterState.h"  // RTApiConnected for the precise-detection status line
#include "Globals.h"
#include "I18n.h"
#include "Settings.h"
#include "SaveScheduler.h"   // RequestSave (debounced, off-thread settings writes)
#include "PlusSettings.h"    // g_PlusSettings ("send while moving"; empty in base builds)
#include "Layout.h"          // ToggleButton
#include "Usage.h"           // usage::Reset (the Reset-usage button)
#include "NexusShortcut.h"   // ApplyNexusShortcut on settings changes
#include "Palette.h"         // PaletteGhostPulse / KeepAlivePulse (live preview while tuning)
#include "Logging.h"         // LOG_TRACE (setting-change audit trail)
#include "UpdateCheck.h"     // Plus update banner (PlusUpdateAvailable / OpenReleasesPage; stubs otherwise)
#include "Profiling.h"       // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

#include <algorithm>   // std::max (reset-usage modal button sizing)
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

    // ===== Emote palette =====
    OptionsSection(L("opt.sec.palette"));

    // The whole section is wrapped in a group so engagement can drive the
    // palette's preview pulses below: geometry controls show a live ghost,
    // and an open palette survives being tuned from over here (interacting
    // with this window is otherwise exactly the focus loss that closes it).
    ImGui::BeginGroup();
    bool palGeo = false;  // a geometry control (rows / size / position) is engaged

    // Discoverability: the palette only exists behind its Nexus keybind
    // (deliberately settings-free to open), so the section leads with where
    // to assign one.
    ImGui::TextDisabled("%s", L("opt.pal.bind_hint"));
    ImGui::Spacing();

    // Result-row cap. The palette never scrolls - more rows = taller window.
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderInt(L("opt.pal.max_results"), &g_Settings.PaletteMaxResults, 5, 15);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.max_results = %d", g_Settings.PaletteMaxResults);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.max_results_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Size factor (window width + row height; same slider idiom as the icon
    // scales: save on release, right-click resets).
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.pal.scale"), &g_Settings.PaletteScale, 0.8f, 1.5f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.scale = %.2f", g_Settings.PaletteScale);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.PaletteScale = 1.0f;
        LOG_TRACE("setting palette.scale = %.2f (reset)", g_Settings.PaletteScale);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.scale_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Vertical anchor (fraction of screen height; horizontal stays centered).
    // The palette positions itself every frame, so dragging this with the
    // palette open moves it live.
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.pal.y_pos"), &g_Settings.PaletteYPos, 0.f, 0.8f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.y_pos = %.2f", g_Settings.PaletteYPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.PaletteYPos = 0.2f;
        LOG_TRACE("setting palette.y_pos = %.2f (reset)", g_Settings.PaletteYPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.y_pos_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Empty-query suggestions: Frequently / Recently used (the usage log's two
    // views, same labels as the Quickbar categories) or nothing.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L("opt.pal.empty_query"));
    ImGui::SameLine();
    int pq = (int)g_Settings.PaletteEmptyQuery;
    const char* pqItems[] = { L("qb.cat_frequent"), L("qb.cat_recently_used"),
                              L("tt.off") };
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("##palempty", &pq, pqItems, IM_ARRAYSIZE(pqItems))) {
        g_Settings.PaletteEmptyQuery = (EPaletteEmptyQuery)pq;
        LOG_TRACE("setting palette.empty_query = %d", pq);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) {
        static const TooltipOption kPalEmptyOpts[] = {
            { "qb.cat_frequent",      "opt.pal.empty_frequent.desc", true  },
            { "qb.cat_recently_used", "opt.pal.empty_recent.desc",   false },
            { "tt.off",               "opt.pal.empty_off.desc",      false },
        };
        TooltipOptions("opt.pal.empty_query_tip", kPalEmptyOpts,
                       IM_ARRAYSIZE(kPalEmptyOpts));
    }

    CheckboxWithSaveAndTooltip("opt.pal.clear_on_open", &g_Settings.PaletteClearOnOpen,
                               /*defaultIsOn=*/true);

    ImGui::EndGroup();
    // Engagement -> preview pulses (see Palette.h). IsItemActive keeps a
    // slider drag counted even when the mouse leaves the section rect.
    const bool palSectionHovered = ImGui::IsItemHovered();
    if (palGeo) PaletteGhostPulse();
    if (palGeo || palSectionHovered) PaletteKeepAlivePulse();

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

    // ===== Quickbar categories =====
    // Built-in (synthetic) categories the Quickbar's category bar offers
    // alongside the user's favorites categories. See Quickbar.cpp.
    OptionsSection(L("opt.sec.qb_categories"));

    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_favorites", &g_Settings.QuickbarShowFavoriteCategories, /*defaultIsOn=*/true);

    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_core", &g_Settings.QuickbarShowCoreCategory, /*defaultIsOn=*/false);

    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_mad_king", &g_Settings.QuickbarShowMadKingCategory, /*defaultIsOn=*/false);

    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_unlocked", &g_Settings.QuickbarShowUnlockedCategory, /*defaultIsOn=*/false);

    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_unlocked_all", &g_Settings.QuickbarShowUnlockedAllCategory, /*defaultIsOn=*/true);

    // /me-motes — opt-in like the other built-in categories. Inert until the
    // Quickbar's category-build code surfaces the /me-mote category (next
    // checkpoint), but the toggle persists either way.
    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_me_motes", &g_Settings.QuickbarShowMeMotesCategory, /*defaultIsOn=*/false);

    // Synthetic usage categories (data/Usage.h) — derived from the usage log,
    // not editable, so they live only in the Quickbar (no Library section).
    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_recently_used", &g_Settings.QuickbarShowRecentlyUsedCategory, /*defaultIsOn=*/false);

    CheckboxWithSaveAndTooltip("opt.gen.qb_cat_frequent", &g_Settings.QuickbarShowFrequentCategory, /*defaultIsOn=*/false);

    // Reset the usage history that feeds the two categories above. Its own section
    // (like the Catalog tab's "Clear catalog") so the destructive action reads as
    // separate from the category toggles. Modeled on Clear catalog (destructive
    // button + centered confirm modal) but a MILDER red - clearing usage is
    // recoverable (it re-accrues as you use emotes), not the catalog-wipe it rhymes
    // with.
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