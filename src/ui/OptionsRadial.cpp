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
// One selectable wizard row == one (ref, variant). Emotes contribute a single row;
// a /me-mote contributes one row per non-empty variant (Default/You/All), so the
// same /me-mote can be included in several states as distinct wheel items.
struct WizItem {
    RadialItemRef ref;            // Type/Id/Variant
    bool          include = false;
    bool          isMeMote = false;
    bool          autoTarget = false;   // emote @ auto-target hint
    std::string   name;                 // display name (variant suffix already applied)
};
bool                  s_wizActive = false;   // form is open (drives BeginPopupModal)
int                   s_wizPhase  = 0;        // 0 = form, 1 = done
std::string           s_wizCategory;
char                  s_wizName[128] = {};
std::vector<WizItem>  s_wizItems;
RadialWheelOptions    s_wizOpt;
bool                  s_wizAutoSplit = false;
RadialExportResult    s_wizResult;
// Edit mode: when s_wizEditSlug is non-empty the wizard re-opens an EXISTING wheel
// (seeded from its source category with its last selection + options) and writes
// back in place; empty = creating a new wheel.
std::string           s_wizEditSlug;
int                   s_wizEditId = 0;

// Inline deploy feedback (no Nexus toast for a routine in-tab action; see RenderRadialTab).
std::string s_deployMsg;
bool        s_deployOk = false;

// rename inline edit
std::string s_renameSlug;       // slug being renamed ("" = none)
char        s_renameBuf[128] = {};

int IncludedCount() {
    int n = 0;
    for (const auto& w : s_wizItems) if (w.include) ++n;
    return n;
}

// Tooltip for the item just submitted (keyed i18n).
void ItemTip(const char* tipKey) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L(tipKey));
}

// Float slider that resets to `def` on right-click (the established right-click-reset
// idiom) and carries a tooltip.
void SliderWithReset(const char* label, float* v, float lo, float hi, float def,
                     const char* tipKey) {
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat(label, v, lo, hi, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L(tipKey));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) *v = def;
}

// Right-align the cursor for a row of two buttons of width w each.
void RightAlignButtons(float w, int count) {
    float total = w * count + ImGui::GetStyle().ItemSpacing.x * (count - 1);
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > total) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total));
}

// Snapshot a category's refs into s_wizItems (in order). Emotes -> one row; /me-motes
// -> one row per non-empty variant (Default checked, You/All available but unchecked).
// Shared by the create and edit entry points.
void SeedWizardFromCategory(const std::string& category) {
    s_wizCategory = category;
    s_wizItems.clear();
    for (const auto& fc : g_Settings.FavoriteCategories) {
        if (fc.Name != category) continue;
        for (const auto& ref : fc.Refs) {
            if (ref.Type == EFavoriteRefType::Emote) {
                WizItem w;
                w.ref.Type = EFavoriteRefType::Emote;
                w.ref.Id   = ref.Id;
                w.ref.Variant = EMeMoteVariant::Default;
                w.include  = true;
                std::lock_guard<std::mutex> lk(g_EmotesMutex);
                const Emote* e = FindEmote(ref.Id);
                w.name = (e && !e->Name.empty()) ? e->Name : ref.Id;
                w.autoTarget = e && e->IsTargetable && g_Settings.SendTargetableOnTarget;
                s_wizItems.push_back(std::move(w));
            } else {
                std::string base; bool hasYou = false, hasAll = false;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    const MeMote* m = FindMeMote(ref.Id);
                    base   = (m && !m->Name.empty()) ? m->Name : ref.Id;
                    hasYou = m && !m->TextYou.empty();
                    hasAll = m && !m->TextAll.empty();
                }
                auto pushVar = [&](EMeMoteVariant v, const char* suffix, bool inc) {
                    WizItem w;
                    w.isMeMote = true;
                    w.ref.Type = EFavoriteRefType::MeMote;
                    w.ref.Id   = ref.Id;
                    w.ref.Variant = v;
                    w.include  = inc;
                    w.name     = base + suffix;
                    s_wizItems.push_back(std::move(w));
                };
                pushVar(EMeMoteVariant::Default, "", true);
                if (hasYou) pushVar(EMeMoteVariant::You, " (you)", false);
                if (hasAll) pushVar(EMeMoteVariant::All, " (all)", false);
            }
        }
        break;
    }
}

// Create a NEW wheel from a category (full default selection).
void OpenWizard(const std::string& category) {
    s_wizOpt = RadialWheelOptions{};   // defaults (Normal, ReleaseOrClick, gate on, icon 0.8)
    s_wizAutoSplit = false;
    s_wizPhase = 0;
    s_wizResult = RadialExportResult{};
    s_wizEditSlug.clear();
    s_wizEditId = 0;
    SeedWizardFromCategory(category);
    std::snprintf(s_wizName, sizeof(s_wizName), "%s", category.c_str());
    s_wizActive = true;
    ImGui::OpenPopup("###radialwizard");
}

// EDIT an existing wheel: re-open the same dialog seeded from its source category, with
// the wheel's last selection (items + /me-mote variants) and options pre-applied, so the
// user reconfigures and writes back in place. Seeding from the FULL category (not just
// the wheel's subset) is what lets a split/subset wheel pull items back in.
void OpenWizardForEdit(const RadialExport& w) {
    s_wizOpt = w.Options;
    s_wizAutoSplit = false;
    s_wizPhase = 0;
    s_wizResult = RadialExportResult{};
    s_wizEditSlug = w.Slug;
    s_wizEditId   = w.Id;
    SeedWizardFromCategory(w.SourceCategory);
    // Apply the wheel's selection: a row is checked iff (type,id,variant) is in w.Items.
    for (auto& row : s_wizItems) {
        bool inWheel = false;
        for (const auto& it : w.Items)
            if (it.Type == row.ref.Type && it.Id == row.ref.Id &&
                it.Variant == row.ref.Variant) { inWheel = true; break; }
        row.include = inWheel;
    }
    std::snprintf(s_wizName, sizeof(s_wizName), "%s", w.Name.c_str());
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

    const bool editing = !s_wizEditSlug.empty();
    std::string title = std::string(L(editing ? "opt.radial.edit_title"
                                               : "opt.radial.wizard_title")) +
                        " '" + (editing ? std::string(s_wizName) : s_wizCategory) +
                        "'###radialwizard";
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    if (s_wizPhase == 1) {
        // ---- done panel ----
        ImGui::TextColored(kGreen, "%s", L("opt.radial.done_title"));
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
#ifdef EMOT3_PLUS
        ImGui::TextWrapped("%s", L("opt.radial.done_next_plus"));
#else
        ImGui::TextWrapped("%s", L("opt.radial.done_next"));
#endif
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        for (size_t i = 0; i < s_wizResult.names.size(); ++i) {
            const int id = i < s_wizResult.ids.size() ? s_wizResult.ids[i] : 0;
            ImGui::BulletText("%s", s_wizResult.names[i].c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("KB_RADIAL%d", id);
            ImGui::SameLine();
            std::string btn = std::string(L("opt.radial.copy_name")) + "##cn" + std::to_string(i);
            if (ImGui::SmallButton(btn.c_str()))
                ImGui::SetClipboardText(s_wizResult.names[i].c_str());
        }
        ImGui::Spacing();
        ImGui::Separator();
        RightAlignButtons(120.0f, 1);
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
        ImGui::TextUnformatted(Ellipsize(w.name, 240.0f).c_str());
        if (w.autoTarget) {
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
        ItemTip("opt.radial.opt_type_tip");

        // SelectionMode: Click=1 / Release=2 / ReleaseOrClick=3
        const char* sels[3] = { L("opt.radial.sel_click"), L("opt.radial.sel_release"),
                                L("opt.radial.sel_release_or_click") };
        int selIdx = (s_wizOpt.SelectionMode >= 1 && s_wizOpt.SelectionMode <= 3)
                         ? s_wizOpt.SelectionMode - 1 : 2;
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo(L("opt.radial.opt_selmode"), &selIdx, sels, 3))
            s_wizOpt.SelectionMode = selIdx + 1;
        ItemTip("opt.radial.opt_selmode_tip");

        SliderWithReset(L("opt.radial.opt_scale"), &s_wizOpt.Scale, 0.5f, 2.0f,
                        1.0f, "opt.radial.opt_scale_tip");
        SliderWithReset(L("opt.radial.opt_iconscale"), &s_wizOpt.IconScale, 0.5f, 2.0f,
                        0.8f, "opt.radial.opt_iconscale_tip");
        ImGui::Checkbox(L("opt.radial.opt_tooltip"), &s_wizOpt.ShowItemNameTooltip);
        ItemTip("opt.radial.opt_tooltip_tip");
        ImGui::Checkbox(L("opt.radial.opt_gate"), &s_wizOpt.GateByState);
        ItemTip("opt.radial.gate_tip");
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

    // Buttons (bottom-right)
    ImGui::Spacing();
    ImGui::Separator();
    const bool canExport = !nameEmpty && included >= 1 &&
                           (included <= cap || s_wizAutoSplit);
    RightAlignButtons(120.0f, 2);
    if (ImGui::Button(L("common.cancel"), ImVec2(120, 0))) {
        s_wizActive = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (!canExport) BeginDisabledCompat();
    const char* exportLbl = editing ? L("common.save") : L("opt.radial.export_confirm");
    if (ImGui::Button(exportLbl, ImVec2(120, 0)) && canExport) {
        std::vector<RadialItemRef> items;
        for (const auto& w : s_wizItems) if (w.include) items.push_back(w.ref);
        const bool willSplit = s_wizAutoSplit && (int)items.size() > cap;
        if (editing && !willSplit) {
            // In-place single-wheel edit: keep this wheel's slug + id. Partial when it
            // isn't a 1:1 default mirror of the category (matches ExportCategoryAsWheels).
            int catCount = 0;
            for (const auto& fc : g_Settings.FavoriteCategories)
                if (fc.Name == s_wizCategory) { catCount = (int)fc.Refs.size(); break; }
            bool partial = ((int)items.size() != catCount);
            if (!partial)
                for (const auto& it : items)
                    if (it.Variant != EMeMoteVariant::Default) { partial = true; break; }
            bool ok = ReExportWheel(s_wizEditSlug, s_wizEditId, s_wizName, s_wizCategory,
                                    partial, items, s_wizOpt);
            s_wizResult = RadialExportResult{};
            s_wizResult.ok = ok;
            s_wizResult.wheelsWritten = ok ? 1 : 0;
            if (ok) {
                s_wizResult.ids.push_back(s_wizEditId);
                s_wizResult.names.push_back(std::string(kRadialPackNamePrefix) + s_wizName);
            }
        } else {
            // New export, or an edit that grew past capacity -> replace with a (split) set.
            if (editing) RemoveWheel(s_wizEditSlug);
            s_wizResult = ExportCategoryAsWheels(s_wizCategory, s_wizName, items, s_wizOpt,
                                                 s_wizAutoSplit);
        }
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

// Is (type,id) still present in this wheel's source category?
bool CategoryHasRef(const RadialExport& w, const RadialItemRef& r) {
    for (const auto& fc : g_Settings.FavoriteCategories) {
        if (fc.Name != w.SourceCategory) continue;
        for (const auto& ref : fc.Refs)
            if (ref.Type == r.Type && ref.Id == r.Id) return true;
        return false;
    }
    return false;
}

// Does re-exporting this wheel change it? Full wheels must match a fresh category
// snapshot exactly; partial (subset / auto-split) wheels only drift when one of
// their refs left the category (additions / order don't matter - the subset is
// deliberate). Caller guarantees the source category exists.
bool WheelDrift(const RadialExport& w) {
    if (!w.Partial) return w.Items != ResnapshotCategory(w);
    for (const auto& it : w.Items) if (!CategoryHasRef(w, it)) return true;
    return false;
}

}  // namespace

void RenderRadialTab() {
    EnsureStatus();

    // 1. Intro (base build omits the +plus deploy mention)
#ifdef EMOT3_PLUS
    ImGui::TextWrapped("%s", L("opt.radial.intro_plus"));
#else
    ImGui::TextWrapped("%s", L("opt.radial.intro"));
#endif

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

    // 3. Create — any favorites category, any number of times (a category can back
    // several wheels, e.g. an auto-split or curated subsets; Edit reconfigures each).
    OptionsSection(L("opt.radial.sec_create"));
    ImGui::TextUnformatted(L("opt.radial.create_label"));
    {
        std::vector<std::string> avail;
        for (const auto& fc : g_Settings.FavoriteCategories) avail.push_back(fc.Name);

        static int s_sel = 0;
        if (avail.empty()) {
            ImGui::TextDisabled("%s", L("opt.radial.no_categories"));
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
                // Drift = re-exporting would change this wheel (see WheelDrift:
                // exact match for full wheels, lenient "a ref left the category" for
                // partial/subset/split wheels). Only when the category still exists.
                bool drift = false;
                if (sourceExists && !w.ParseError)
                    drift = WheelDrift(w);

                ImGui::SameLine();
                if (w.ParseError) {
                    ImGui::TextColored(kRed, "%s", L("opt.radial.st_broken"));
                } else if (!sourceExists) {
                    ImGui::TextColored(kRed, "%s", L("opt.radial.st_source_missing"));
                } else if (drift) {
                    ImGui::TextColored(kAmber, "%s", L("opt.radial.st_changed"));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("opt.radial.st_changed_tip"));
                } else if (!s_rmDetected) {
                    ImGui::TextColored(kAmber, "%s", L("opt.radial.st_not_detected"));
                } else {
                    ImGui::TextColored(kGreen, "%s", L("opt.radial.st_in_sync"));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("opt.radial.st_in_sync_tip"));
                }

                // actions — Edit re-opens the wizard pre-filled (selection + options),
                // so updating a wheel always goes through the same dialog as creating one.
                if (sourceExists && !w.ParseError) {
                    if (ImGui::SmallButton(L("opt.radial.edit"))) OpenWizardForEdit(w);
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

                // rename popup (now with an explicit Cancel)
                if (s_renameSlug == w.Slug && ImGui::BeginPopup("##radialrename")) {
                    ImGui::TextUnformatted(L("opt.radial.rename"));
                    bool empty = (s_renameBuf[0] == '\0');
                    if (empty) PushInvalidInputStyle();
                    ImGui::SetNextItemWidth(200.0f);
                    bool enter = ImGui::InputText("##rn", s_renameBuf, sizeof(s_renameBuf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
                    if (empty) { PopInvalidInputStyle(); DrawInvalidInputBorder(); }
                    bool save = (enter || ImGui::Button(L("common.save"))) && !empty;
                    ImGui::SameLine();
                    if (ImGui::Button(L("common.cancel"))) {
                        s_renameSlug.clear();
                        ImGui::CloseCurrentPopup();
                    } else if (save) {
                        RenameWheel(w.Slug, s_renameBuf);
                        s_renameSlug.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                // remove confirm — a proper modal (matches the destructive-action idiom)
                {
                    ImVec2 ds = ImGui::GetIO().DisplaySize;
                    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f),
                                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                }
                if (ImGui::BeginPopupModal("##radialremove", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
                    ImGui::TextWrapped("%s", L("opt.radial.remove_confirm"));
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                    bool removed = false;
                    if (ImGui::Button(L("opt.radial.remove"), ImVec2(110, 0))) {
                        RemoveWheel(w.Slug);   // the row vanishing is the feedback
                        removed = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(L("common.cancel"), ImVec2(110, 0)))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    if (removed) { ImGui::PopID(); break; }  // list mutated; rebuild next frame
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
            // Inline feedback in the tab (the window is open) instead of a Nexus toast.
            s_deployOk = r.ok;
            char msg[128];
            if (r.ok) std::snprintf(msg, sizeof(msg), L("opt.radial.deployed_inline"),
                                    r.packs, r.icons);
            else      std::snprintf(msg, sizeof(msg), "%s", L("opt.radial.deploy_failed_inline"));
            s_deployMsg = msg;
        }
        if (none) EndDisabledCompat();
        PlusBadge();  // tag the deploy convenience as an emot3 (Plus) feature
        if (!s_deployMsg.empty())
            ImGui::TextColored(s_deployOk ? kGreen : kRed, "%s", s_deployMsg.c_str());
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
