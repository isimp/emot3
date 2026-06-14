#include "Options.h"
#include "OptionsCommon.h"
#include "Globals.h"
#include "I18n.h"
#include "Settings.h"
#include "SaveScheduler.h"  // RequestSave (debounced, off-thread settings writes)
#include "EmoteData.h"     // CatalogIndex / BuildCatalogIndex (live count)
#include "CatalogView.h"   // GetCatalogView (shared cached index)
#include "UnlockScan.h"    // StartUnlockSync / status
#include "Profiling.h"     // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

#include <cfloat>   // FLT_MIN
#include <mutex>
#include <string>

namespace {

void SaveNow() { RequestSave(SaveKind::Settings); }

// Live unlocked/locked tally over the catalog (excludes core, which is always
// unlocked). Same shared index helper the render builds use.
void DrawLiveCount() {
    int unlockedCount = 0, lockedCount = 0;
    const CatalogView& cv = GetCatalogView();   // shared cached index (no per-frame rebuild)
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (const auto& e : g_Emotes) {
            if (e.IsCore) continue;
            if (cv.idx.unlocked(e)) ++unlockedCount;
            else                    ++lockedCount;
        }
    }
    ImGui::TextDisabled(L("opt.gen.unlocks_count"), unlockedCount, lockedCount);
}

// Status line under the Sync button.
void DrawSyncStatus() {
    const UnlockSyncStatus& st = UnlockSyncStatusInfo();
    switch (st.phase) {
        case UnlockSyncPhase::Idle:
            break;
        case UnlockSyncPhase::Fetching:
            ImGui::TextDisabled("%s", L("opt.unl.status_fetching"));
            break;
        case UnlockSyncPhase::PendingPermission:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", L("opt.unl.status_pending"));
            ImGui::PopStyleColor();
            break;
        case UnlockSyncPhase::Done:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 0.55f, 1.0f));
            ImGui::TextWrapped(L("opt.unl.status_done"),
                               st.unlocked, st.custom, st.apiCount, st.unknownApi);
            ImGui::PopStyleColor();
            break;
        case UnlockSyncPhase::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("%s",
                st.errorKey.empty() ? L("opt.unl.err_http") : L(st.errorKey.c_str()));
            ImGui::PopStyleColor();
            break;
    }
}

// The optional "sync from the GW2 API" block. Manual right-click management is
// always available (it doesn't need a section) - this only adds the API on top.
void DrawApiSync() {
    OptionsSection(L("opt.unl.sec_sync"));
    // The "only adds, never locks" caveat lives once in the tab intro
    // (opt.unl.intro) and again in the collapsible explainer below.

    // --- Key source ---
    if (ImGui::RadioButton(L("opt.unl.src_hs"), g_Settings.UnlockApiKeySource == 0)) {
        g_Settings.UnlockApiKeySource = 0; SaveNow();
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.unl.src_hs_tooltip");
    ImGui::SameLine();
    if (ImGui::RadioButton(L("opt.unl.src_own"), g_Settings.UnlockApiKeySource == 1)) {
        g_Settings.UnlockApiKeySource = 1; SaveNow();
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.unl.src_own_tooltip");

    if (g_Settings.UnlockApiKeySource == 1) {
        char buf[256];
        strncpy_s(buf, sizeof(buf), g_Settings.Gw2ApiKey.c_str(), _TRUNCATE);
        {
            InputFieldOpts o; o.flags = ImGuiInputTextFlags_Password;  // width 0 = fill
            if (InputFieldWithHint("##gw2apikey", "opt.unl.key_hint", buf, sizeof(buf), o))
                g_Settings.Gw2ApiKey = buf;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveNow();
        ImGui::TextWrapped("%s", L("opt.unl.key_help"));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", L("opt.unl.key_storage_note"));
        ImGui::PopStyleColor();
    }

    // --- Auto-sync ---
    ImGui::Spacing();
    if (ImGui::Checkbox(L("opt.unl.autosync"), &g_Settings.UnlockAutoSync)) SaveNow();
    if (ImGui::IsItemHovered()) TooltipOnOff("opt.unl.autosync.on", "opt.unl.autosync.off", /*defaultIsOn=*/false);

    // --- Sync now + status ---
    ImGui::Spacing();
    const bool busy = UnlockSyncBusy();
    if (busy) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
    if (ImGui::Button(L("opt.unl.sync_now")) && !busy) StartUnlockSync();
    if (busy) { ImGui::PopStyleVar(); ImGui::PopItemFlag(); }
    ImGui::Spacing();
    DrawSyncStatus();

    // --- Explainer (collapsible details) ---
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(L("opt.unl.explain_header"))) {
        ImGui::Indent();
        ImGui::TextWrapped("%s", L("opt.unl.explain_body"));
        ImGui::Unindent();
    }
}

}  // namespace

// ---- Tab: Unlocks -----------------------------------------------------

void RenderUnlocksTab() {
    PROFILE_SCOPE("opt.unlocks");  // dev perf overlay

    OptionsSection(L("opt.sec.unlocks"));
    // Plain intro: how unlock state is managed (manual + optional API sync).
    ImGui::TextWrapped("%s", L("opt.unl.intro"));
    ImGui::Spacing();
    DrawLiveCount();
    ImGui::Spacing();

    DrawApiSync();
}
