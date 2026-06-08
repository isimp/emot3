#include "OptionsRadial.h"
#include "OptionsCommon.h"  // OptionsSection
#include "Globals.h"        // g_RadialsDir
#include "I18n.h"           // L
#include "Settings.h"       // g_Settings (FavoriteCategories, SendTargetableOnTarget)
#include "EmoteData.h"      // g_Emotes, g_EmotesMutex, FindEmote
#include "MeMotes.h"        // g_MeMotes, g_MeMotesMutex, FindMeMote, EMeMoteVariant
#include "RadialExports.h"  // record + read helpers
#include "RadialExport.h"   // ExportCategoryAsWheels / ReExportWheel / RenameWheel / RemoveWheel
#include "Icons.h"          // EnsureEmoteTexture / EnsureMeMoteTexture (wizard thumbnails)
#include "Layout.h"         // Ellipsize, PushInvalidInputStyle / DrawInvalidInputBorder
#include "RadialDeploy.h"   // IsRadialMenusInstalled / RadialMenusDir / DeployToRadialMenus
#include "Logging.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

// This vendored ImGui predates BeginDisabled(); use the PushItemFlag + alpha dim
// pattern the rest of the addon uses.
void BeginDisabledCompat() {
    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
}
void EndDisabledCompat() {
    ImGui::PopStyleVar();
    ImGui::PopItemFlag();
}

// ---- status cache (avoid a per-frame dir stat; refresh on entry + mutations) ----
bool s_statusKnown = false;
bool s_rmDetected  = false;
void RefreshStatus() { s_rmDetected = IsRadialMenusInstalled(); s_statusKnown = true; }
void EnsureStatus()  { if (!s_statusKnown) RefreshStatus(); }

// status colors (reuse the established palette)
const ImVec4 kGreen(0.45f, 0.85f, 0.50f, 1.0f);
const ImVec4 kAmber(0.92f, 0.78f, 0.32f, 1.0f);
const ImVec4 kRed  (1.00f, 0.45f, 0.40f, 1.0f);

// ---- wizard state ---------------------------------------------------------
struct WizItem {
    RadialItemRef ref;            // Type/Id/Variant (variant editable for /me-motes)
    bool          include = true;
    bool          isMeMote = false;
    bool          hasYou = false, hasAll = false;  // gate the variant combo
    bool          autoTarget = false;              // emote @ auto-target hint
    std::string   name;
};
bool                  s_wizActive = false;   // form is open (drives BeginPopupModal)
int                   s_wizPhase  = 0;        // 0 = form, 1 = done
std::string           s_wizCategory;
char                  s_wizName[128] = {};
std::vector<WizItem>  s_wizItems;
RadialWheelOptions    s_wizOpt;
bool                  s_wizAutoSplit = false;
RadialExportResult    s_wizResult;

// rename inline edit
std::string s_renameSlug;       // slug being renamed ("" = none)
char        s_renameBuf[128] = {};

int IncludedCount() {
    int n = 0;
    for (const auto& w : s_wizItems) if (w.include) ++n;
    return n;
}

void OpenWizard(const std::string& category) {
    s_wizCategory = category;
    s_wizItems.clear();
    s_wizOpt = RadialWheelOptions{};   // defaults (Normal, ReleaseOrClick, gate on)
    s_wizAutoSplit = false;
    s_wizPhase = 0;
    s_wizResult = RadialExportResult{};

    // default wheel name = category name
    std::snprintf(s_wizName, sizeof(s_wizName), "%s", category.c_str());

    // Snapshot the category's refs (in order). Resolve display name + per-type flags.
    for (const auto& fc : g_Settings.FavoriteCategories) {
        if (fc.Name != category) continue;
        for (const auto& ref : fc.Refs) {
            WizItem w;
            w.ref.Type = ref.Type;
            w.ref.Id   = ref.Id;
            w.ref.Variant = EMeMoteVariant::Default;
            if (ref.Type == EFavoriteRefType::Emote) {
                std::lock_guard<std::mutex> lk(g_EmotesMutex);
                const Emote* e = FindEmote(ref.Id);
                w.name = (e && !e->Name.empty()) ? e->Name : ref.Id;
                w.autoTarget = e && e->IsTargetable && g_Settings.SendTargetableOnTarget;
            } else {
                w.isMeMote = true;
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                const MeMote* m = FindMeMote(ref.Id);
                w.name   = (m && !m->Name.empty()) ? m->Name : ref.Id;
                w.hasYou = m && !m->TextYou.empty();
                w.hasAll = m && !m->TextAll.empty();
            }
            s_wizItems.push_back(std::move(w));
        }
        break;
    }
    s_wizActive = true;
    ImGui::OpenPopup("###radialwizard");
}

// Small icon thumbnail for a wizard row; falls back to a "(no icon)" tag.
void DrawThumb(const WizItem& w, float sz) {
    Texture* tex = nullptr;
    if (w.isMeMote) {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        if (const MeMote* m = FindMeMote(w.ref.Id)) tex = EnsureMeMoteTexture(*m);
    } else {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        if (const Emote* e = FindEmote(w.ref.Id)) tex = EnsureEmoteTexture(*e);
    }
    if (tex && tex->Resource)
        ImGui::Image((ImTextureID)tex->Resource, ImVec2(sz, sz));
    else {
        ImGui::Dummy(ImVec2(sz, sz));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("opt.radial.no_icon"));
    }
}

// ---- the export wizard modal ----------------------------------------------
void RenderWizard() {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460, 320), ImVec2(720, 760));

    std::string title = std::string(L("opt.radial.wizard_title")) + " '" +
                        s_wizCategory + "'###radialwizard";
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    if (s_wizPhase == 1) {
        // ---- done panel ----
        ImGui::TextColored(kGreen, "%s", L("opt.radial.done_title"));
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextWrapped("%s", L("opt.radial.done_next"));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        for (size_t i = 0; i < s_wizResult.names.size(); ++i) {
            ImGui::BulletText("%s  (KB_RADIAL%d)", s_wizResult.names[i].c_str(),
                              i < s_wizResult.ids.size() ? s_wizResult.ids[i] : 0);
            ImGui::SameLine();
            std::string btn = std::string(L("opt.radial.copy_name")) + "##cn" + std::to_string(i);
            if (ImGui::SmallButton(btn.c_str()))
                ImGui::SetClipboardText(s_wizResult.names[i].c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button(L("common.close"), ImVec2(120, 0))) {
            s_wizActive = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    // ---- form ----
    const int cap = s_wizOpt.Small ? kRadialCapSmall : kRadialCapNormal;
    const int included = IncludedCount();

    // Wheel name (validated non-empty)
    ImGui::TextUnformatted(L("opt.radial.name_label"));
    bool nameEmpty = (s_wizName[0] == '\0');
    if (nameEmpty) PushInvalidInputStyle();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##radialname", s_wizName, sizeof(s_wizName));
    if (nameEmpty) { PopInvalidInputStyle(); DrawInvalidInputBorder(); }

    // Items (bordered scroll child)
    ImGui::Spacing();
    ImGui::TextUnformatted(L("opt.radial.items_label"));
    ImGui::BeginChild("##radialitems", ImVec2(0, 220), true);
    const float thumb = ImGui::GetFontSize() * 1.4f;
    for (size_t i = 0; i < s_wizItems.size(); ++i) {
        WizItem& w = s_wizItems[i];
        ImGui::PushID((int)i);
        ImGui::Checkbox("##inc", &w.include);
        ImGui::SameLine();
        DrawThumb(w, thumb);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Ellipsize(w.name, 200.0f).c_str());
        if (w.isMeMote) {
            // variant combo (non-empty bodies only)
            ImGui::SameLine();
            const char* opts[3] = { L("opt.mm.variant_default"), L("opt.mm.variant_you"),
                                    L("opt.mm.variant_all") };
            int cur = (int)w.ref.Variant;
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::BeginCombo("##var", opts[cur])) {
                for (int v = 0; v < 3; ++v) {
                    bool enabled = (v == 0) || (v == 1 && w.hasYou) || (v == 2 && w.hasAll);
                    if (!enabled) ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                    if (ImGui::Selectable(opts[v], cur == v) && enabled)
                        w.ref.Variant = (EMeMoteVariant)v;
                    if (!enabled) ImGui::PopItemFlag();
                }
                ImGui::EndCombo();
            }
        } else if (w.autoTarget) {
            ImGui::SameLine();
            ImGui::TextColored(kAmber, "%s", L("opt.radial.auto_target"));
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Capacity readout + split / count warnings
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d / %d", included, cap);
        bool over = included > cap;
        ImGui::TextColored(over ? kAmber : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                           "%s %s", L("opt.radial.capacity"), buf);
        if (over) {
            ImGui::SameLine();
            ImGui::Checkbox(L("opt.radial.auto_split"), &s_wizAutoSplit);
            if (!s_wizAutoSplit)
                ImGui::TextColored(kAmber, "%s", L("opt.radial.over_cap"));
        }
        if (included == 1) ImGui::TextColored(kAmber, "%s", L("opt.radial.warn_one"));
        if (included == 0) ImGui::TextColored(kRed,   "%s", L("opt.radial.warn_zero"));
    }

    // Wheel options
    OptionsSection(L("opt.radial.sec_options"));
    {
        int typeIdx = s_wizOpt.Small ? 1 : 0;
        const char* types[2] = { L("opt.radial.type_normal"), L("opt.radial.type_small") };
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo(L("opt.radial.opt_type"), &typeIdx, types, 2))
            s_wizOpt.Small = (typeIdx == 1);

        // SelectionMode: Click=1 / Release=2 / ReleaseOrClick=3
        const char* sels[3] = { L("opt.radial.sel_click"), L("opt.radial.sel_release"),
                                L("opt.radial.sel_release_or_click") };
        int selIdx = (s_wizOpt.SelectionMode >= 1 && s_wizOpt.SelectionMode <= 3)
                         ? s_wizOpt.SelectionMode - 1 : 2;
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo(L("opt.radial.opt_selmode"), &selIdx, sels, 3))
            s_wizOpt.SelectionMode = selIdx + 1;

        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat(L("opt.radial.opt_scale"), &s_wizOpt.Scale, 0.5f, 2.0f, "%.2f");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat(L("opt.radial.opt_iconscale"), &s_wizOpt.IconScale, 0.5f, 2.0f, "%.2f");
        ImGui::Checkbox(L("opt.radial.opt_tooltip"), &s_wizOpt.ShowItemNameTooltip);
        ImGui::Checkbox(L("opt.radial.opt_gate"), &s_wizOpt.GateByState);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("opt.radial.gate_tip"));
    }

    // Disclosure
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), L("opt.radial.disclosure"), included);
        ImGui::TextWrapped("%s", buf);
    }
    if (!s_rmDetected)
        ImGui::TextColored(kAmber, "%s", L("opt.radial.rm_undetected_warn"));
    ImGui::PopTextWrapPos();

    // Buttons
    ImGui::Spacing();
    if (ImGui::Button(L("common.cancel"), ImVec2(120, 0))) {
        s_wizActive = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    const bool canExport = !nameEmpty && included >= 1 &&
                           (included <= cap || s_wizAutoSplit);
    if (!canExport) BeginDisabledCompat();
    if (ImGui::Button(L("opt.radial.export_confirm"), ImVec2(140, 0)) && canExport) {
        std::vector<RadialItemRef> items;
        for (const auto& w : s_wizItems) if (w.include) items.push_back(w.ref);
        s_wizResult = ExportCategoryAsWheels(s_wizCategory, s_wizName, items, s_wizOpt,
                                             s_wizAutoSplit);
        RefreshStatus();
        s_wizPhase = 1;  // -> done panel (modal stays open)
    }
    if (!canExport) EndDisabledCompat();

    ImGui::EndPopup();
}

// Rebuild a wheel's ref list from its current source category (for Re-export),
// preserving the variant of refs already in the wheel; new /me-mote refs default.
std::vector<RadialItemRef> ResnapshotCategory(const RadialExport& w) {
    std::vector<RadialItemRef> out;
    const FavoriteCategory* cat = nullptr;
    for (const auto& fc : g_Settings.FavoriteCategories)
        if (fc.Name == w.SourceCategory) { cat = &fc; break; }
    if (!cat) return out;
    for (const auto& ref : cat->Refs) {
        RadialItemRef r;
        r.Type = ref.Type;
        r.Id   = ref.Id;
        r.Variant = EMeMoteVariant::Default;
        for (const auto& old : w.Items)
            if (old.Type == ref.Type && old.Id == ref.Id) { r.Variant = old.Variant; break; }
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace

void RenderRadialTab() {
    EnsureStatus();

    // 1. Intro
    ImGui::TextWrapped("%s", L("opt.radial.intro"));

    // 2. Status
    OptionsSection(L("opt.radial.sec_status"));
    if (s_rmDetected) ImGui::TextColored(kGreen, "%s", L("opt.radial.status_detected"));
    else              ImGui::TextColored(kAmber, "%s", L("opt.radial.status_not_detected"));
    ImGui::SameLine();
    if (ImGui::SmallButton(L("opt.radial.refresh"))) RefreshStatus();
    ImGui::TextDisabled("%s %s", L("opt.radial.staging_root"), g_RadialsDir.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(L("opt.radial.copy_path")))
        ImGui::SetClipboardText(g_RadialsDir.c_str());

    // 3. Create
    OptionsSection(L("opt.radial.sec_create"));
    ImGui::TextUnformatted(L("opt.radial.create_label"));
    {
        std::vector<std::string> exported = ExportedSourceCategories();
        std::vector<std::string> avail;
        for (const auto& fc : g_Settings.FavoriteCategories)
            if (std::find(exported.begin(), exported.end(), fc.Name) == exported.end())
                avail.push_back(fc.Name);

        static int s_sel = 0;
        if (g_Settings.FavoriteCategories.empty()) {
            ImGui::TextDisabled("%s", L("opt.radial.no_categories"));
        } else if (avail.empty()) {
            ImGui::TextDisabled("%s", L("opt.radial.all_exported"));
        } else {
            if (s_sel >= (int)avail.size()) s_sel = 0;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("##radialcat", avail[s_sel].c_str())) {
                for (int i = 0; i < (int)avail.size(); ++i)
                    if (ImGui::Selectable(avail[i].c_str(), s_sel == i)) s_sel = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("opt.radial.export_btn"))) OpenWizard(avail[s_sel]);
        }
    }

    // 4. Exported wheels
    OptionsSection(L("opt.radial.sec_wheels"));
    {
        std::vector<RadialExport> wheels;
        { std::lock_guard<std::mutex> lk(g_RadialExportsMutex); wheels = g_RadialExports; }
        if (wheels.empty()) {
            ImGui::TextDisabled("%s", L("opt.radial.none_yet"));
        } else {
            for (const auto& w : wheels) {
                ImGui::PushID(w.Slug.c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(Ellipsize(w.Name, 180.0f).c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%s, %d)", w.SourceCategory.c_str(), (int)w.Items.size());

                // status
                bool sourceExists = false;
                for (const auto& fc : g_Settings.FavoriteCategories)
                    if (fc.Name == w.SourceCategory) { sourceExists = true; break; }
                ImGui::SameLine();
                if (w.ParseError)        ImGui::TextColored(kRed,   "%s", L("opt.radial.st_broken"));
                else if (!sourceExists)  ImGui::TextColored(kRed,   "%s", L("opt.radial.st_source_missing"));
                else if (!s_rmDetected)  ImGui::TextColored(kAmber, "%s", L("opt.radial.st_not_detected"));
                else                     ImGui::TextColored(kGreen, "%s", L("opt.radial.st_in_sync"));

                // actions
                if (sourceExists && !w.ParseError) {
                    if (ImGui::SmallButton(L("opt.radial.reexport"))) {
                        std::vector<RadialItemRef> items = ResnapshotCategory(w);
                        ReExportWheel(w.Slug, w.Id, w.Name, w.SourceCategory, items, w.Options);
                        RefreshStatus();
                    }
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton(L("opt.radial.rename"))) {
                    s_renameSlug = w.Slug;
                    std::snprintf(s_renameBuf, sizeof(s_renameBuf), "%s", w.Name.c_str());
                    ImGui::OpenPopup("##radialrename");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(L("opt.radial.remove")))
                    ImGui::OpenPopup("##radialremove");

                // rename popup
                if (s_renameSlug == w.Slug && ImGui::BeginPopup("##radialrename")) {
                    ImGui::TextUnformatted(L("opt.radial.rename"));
                    bool empty = (s_renameBuf[0] == '\0');
                    if (empty) PushInvalidInputStyle();
                    ImGui::SetNextItemWidth(200.0f);
                    bool enter = ImGui::InputText("##rn", s_renameBuf, sizeof(s_renameBuf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
                    if (empty) { PopInvalidInputStyle(); DrawInvalidInputBorder(); }
                    if ((enter || ImGui::Button(L("common.save"))) && !empty) {
                        RenameWheel(w.Slug, s_renameBuf);
                        s_renameSlug.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                // remove confirm
                if (ImGui::BeginPopup("##radialremove")) {
                    ImGui::TextWrapped("%s", L("opt.radial.remove_confirm"));
                    if (ImGui::Button(L("opt.radial.remove"), ImVec2(110, 0))) {
                        RemoveWheel(w.Slug);
                        ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                        ImGui::PopID();
                        break;  // list mutated; rebuild next frame
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(L("common.cancel"), ImVec2(110, 0)))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
    }

    // 5. Apply — +plus deploys with one click; base build hints the manual copy.
    OptionsSection(L("opt.radial.sec_apply"));
    {
#ifdef EMOT3_PLUS
        std::vector<RadialExport> wheels;
        { std::lock_guard<std::mutex> lk(g_RadialExportsMutex); wheels = g_RadialExports; }
        char btn[64];
        std::snprintf(btn, sizeof(btn), L("opt.radial.deploy_btn"), (int)wheels.size());
        bool none = wheels.empty() || !s_rmDetected;
        if (none) BeginDisabledCompat();
        if (ImGui::Button(btn) && !none) {
            RadialDeployResult r = DeployToRadialMenus();
            if (r.ok) LOG_INFO("radials: deploy ok (%d packs, %d icons)", r.packs, r.icons);
        }
        if (none) EndDisabledCompat();
        ImGui::TextWrapped("%s", L("opt.radial.reload_reminder"));
#else
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextWrapped("%s", L("opt.radial.apply_hint"));
        ImGui::PopTextWrapPos();
#endif
    }

    // wizard (rendered unconditionally so OpenPopup from Create works)
    if (s_wizActive) RenderWizard();
}
