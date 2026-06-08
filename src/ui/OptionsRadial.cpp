#include "OptionsRadial.h"
#include "OptionsCommon.h"  // OptionsSection
#include "Globals.h"        // g_RadialsDir
#include "I18n.h"           // L
#include "Settings.h"       // g_Settings (FavoriteCategories, SendTargetableOnTarget)
#include "EmoteData.h"      // g_Emotes, g_EmotesMutex, FindEmote
#include "MeMotes.h"        // g_MeMotes, g_MeMotesMutex, FindMeMote, EMeMoteVariant
#include "RadialExports.h"  // record + read helpers
#include "RadialExport.h"   // ExportGroup / EditGroup / RenameGroup / RemoveGroup
#include "Icons.h"          // EnsureEmoteTexture / EnsureMeMoteTexture (wizard thumbnails)
#include "Layout.h"         // Ellipsize, PushInvalidInputStyle / DrawInvalidInputBorder
#include "RadialDeploy.h"   // IsRadialMenusInstalled / RadialMenusDir / DeployToRadialMenus
#include "Logging.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <map>
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

// ---- status cache (avoid per-frame disk I/O; refresh on entry + mutations) -------
bool s_statusKnown = false;
bool s_rmDetected  = false;
// +plus: per-group RadialMenus sync state, recomputed in RefreshStatus (reads files).
std::map<std::string, RadialSyncState> s_syncState;
void RefreshStatus() {
    s_rmDetected = IsRadialMenusInstalled();
    s_statusKnown = true;
#ifdef EMOT3_PLUS
    s_syncState.clear();
    if (s_rmDetected) {
        std::map<std::string, std::vector<std::string>> bySlug;
        {
            std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
            for (const auto& w : g_RadialExports) bySlug[w.Group].push_back(w.Slug);
        }
        for (const auto& kv : bySlug) s_syncState[kv.first] = RadialMenusSyncState(kv.second);
    }
#endif
}
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
    int           page = 1;       // which wheel page this item lands on (1-based)
    bool          isMeMote = false;
    bool          autoTarget = false;   // emote @ auto-target hint
    bool          isNew = false;        // (edit) added to the category since export
    std::string   name;                 // display name (variant suffix already applied)
};
bool                  s_wizActive = false;   // form is open (drives BeginPopupModal)
bool                  s_wizOpenRequested = false;  // OpenPopup deferred to top-level scope
int                   s_wizPhase  = 0;        // 0 = form, 1 = done
std::string           s_wizCategory;
char                  s_wizName[128] = {};
std::vector<WizItem>  s_wizItems;
std::vector<std::string> s_wizRemoved;  // (edit) refs that left the category, dropped
RadialWheelOptions    s_wizOpt;
int                   s_wizPageCount = 1;     // number of wheel pages (split)
RadialExportResult    s_wizResult;
bool                  s_wizDoneSynced = false;  // done panel: already pushed to RadialMenus
// Edit mode: when s_wizEditGroup is non-empty the wizard re-opens an EXISTING logical
// export (seeded from its category with its last selection + page layout + options)
// and writes back under the same group; empty = creating a new one.
std::string           s_wizEditGroup;

// Inline deploy feedback (no Nexus toast for a routine in-tab action; see RenderRadialTab).
std::string s_deployMsg;
bool        s_deployOk = false;

// rename inline edit (keyed on a group id; "" = none) - matches the Library's inline
// category rename, no popup.
std::string s_renameGroup;
char        s_renameBuf[128] = {};
bool        s_renameFocus = false;  // grab keyboard focus on the frame rename starts
// remove modal: in +plus, whether to also delete the deployed copy from RadialMenus.
bool        s_removeAlsoRM = true;

int IncludedCount() {
    int n = 0;
    for (const auto& w : s_wizItems) if (w.include) ++n;
    return n;
}
int RadialCap() { return s_wizOpt.Small ? kRadialCapSmall : kRadialCapNormal; }

// Count included items assigned to a given 1-based page.
int PageCount(int page) {
    int n = 0;
    for (const auto& w : s_wizItems) if (w.include && w.page == page) ++n;
    return n;
}

// Distribute included items across pages in order, filling each to capacity.
void AutoFillPages() {
    const int cap = RadialCap();
    int idx = 0;
    for (auto& w : s_wizItems) {
        if (!w.include) continue;
        w.page = (idx / cap) + 1;
        ++idx;
    }
    int need = (idx + cap - 1) / cap;
    if (need < 1) need = 1;
    s_wizPageCount = std::max(s_wizPageCount, need);
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

// OpenPopup must run at the top-level ID scope (not inside a row's PushID), so the
// entry points just set state + request; RenderRadialTab fires the actual OpenPopup.
void RequestWizardOpen() { s_wizActive = true; s_wizOpenRequested = true; }

// Create a NEW logical export from a category (full default selection, one page).
void OpenWizard(const std::string& category) {
    s_wizOpt = RadialWheelOptions{};   // defaults (Normal, ReleaseOrClick, gate on, icon 0.8)
    s_wizPageCount = 1;
    s_wizPhase = 0;
    s_wizResult = RadialExportResult{};
    s_wizEditGroup.clear();
    s_wizRemoved.clear();
    SeedWizardFromCategory(category);     // all rows default page = 1
    std::snprintf(s_wizName, sizeof(s_wizName), "%s", category.c_str());
    RequestWizardOpen();
}

// EDIT an existing logical export (group): re-open the dialog seeded from its source
// category, with the export's selection, per-item PAGE assignment, and options
// pre-applied, so the user reconfigures and writes back under the same group. Seeding
// from the FULL category (not just the export's subset) is what lets it pull items back.
void OpenWizardForEdit(const std::string& group) {
    std::vector<RadialExport> pages = WheelsInGroup(group);  // sorted by Page
    if (pages.empty()) return;
    s_wizOpt   = pages.front().Options;
    s_wizPhase = 0;
    s_wizResult = RadialExportResult{};
    s_wizEditGroup = group;
    s_wizPageCount = (int)pages.size();
    SeedWizardFromCategory(pages.front().SourceCategory);
    // For each row, find which page holds its (type,id,variant); checked iff present.
    for (auto& row : s_wizItems) {
        row.include = false;
        for (const auto& pg : pages) {
            for (const auto& it : pg.Items)
                if (it.Type == row.ref.Type && it.Id == row.ref.Id &&
                    it.Variant == row.ref.Variant) {
                    row.include = true;
                    row.page    = pg.Page;
                    break;
                }
            if (row.include) break;
        }
    }

    // Surface what changed since export, so editing a drifted wheel is reviewable.
    // Removed = a wheel item whose (type,id) is no longer a category row (left the
    // category) - it can't be shown as a row, so list it. Computed for any export.
    s_wizRemoved.clear();
    auto inCategoryNow = [&](EFavoriteRefType t, const std::string& id) {
        for (const auto& row : s_wizItems)
            if (row.ref.Type == t && row.ref.Id == id) return true;
        return false;
    };
    {
        std::vector<std::string> seen;
        auto keyOf = [](EFavoriteRefType t, const std::string& id) {
            return std::to_string((int)t) + ":" + id;
        };
        for (const auto& pg : pages)
            for (const auto& it : pg.Items) {
                if (inCategoryNow(it.Type, it.Id)) continue;
                std::string k = keyOf(it.Type, it.Id);
                bool dup = false;
                for (const auto& s : seen) if (s == k) { dup = true; break; }
                if (dup) continue;
                seen.push_back(k);
                std::string nm;
                if (it.Type == EFavoriteRefType::Emote) {
                    std::lock_guard<std::mutex> lk(g_EmotesMutex);
                    const Emote* e = FindEmote(it.Id);
                    nm = (e && !e->Name.empty()) ? e->Name : it.Id;
                } else {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    const MeMote* m = FindMeMote(it.Id);
                    nm = (m && !m->Name.empty()) ? m->Name : it.Id;
                }
                s_wizRemoved.push_back(nm);
            }
    }
    // New = a category entry not present in the wheel. Only flag for a FULL export
    // (where the wheel was the whole category, so anything extra is genuinely added);
    // a subset export deliberately omits entries, so "new" would be misleading.
    if (!pages.front().Partial) {
        for (auto& row : s_wizItems) {
            bool wasInWheel = false;
            for (const auto& pg : pages) {
                for (const auto& it : pg.Items)
                    if (it.Type == row.ref.Type && it.Id == row.ref.Id) { wasInWheel = true; break; }
                if (wasInWheel) break;
            }
            row.isNew = !wasInWheel;
        }
    }

    std::snprintf(s_wizName, sizeof(s_wizName), "%s", pages.front().Name.c_str());
    RequestWizardOpen();
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

    const bool editing = !s_wizEditGroup.empty();
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
#ifdef EMOT3_PLUS
        // +plus: offer to push this wheel straight into RadialMenus from here (the
        // click is the approval). Overwrites same-named files; reload radials after.
        if (s_rmDetected && !s_wizResult.group.empty()) {
            if (!s_wizDoneSynced) {
                if (ImGui::Button(L("opt.radial.sync_now"))) {
                    std::vector<std::string> slugs;
                    for (const auto& w : WheelsInGroup(s_wizResult.group)) slugs.push_back(w.Slug);
                    DeployGroupToRadialMenus(slugs);
                    RefreshStatus();
                    s_wizDoneSynced = true;
                }
                PlusBadge();
            } else {
                ImGui::TextColored(kGreen, "%s", L("opt.radial.synced_inline"));
            }
            ImGui::Spacing();
        }
#endif
        RightAlignButtons(120.0f, 1);
        if (ImGui::Button(L("common.close"), ImVec2(120, 0))) {
            s_wizActive = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    // ---- form ----
    const int cap = RadialCap();
    const int included = IncludedCount();

    // Keep page count feasible: at least ceil(included/cap), never more pages than
    // items. Clamp each item into the valid page range (e.g. after a Pages "-").
    const int minPages = std::max(1, (included + cap - 1) / cap);
    const int maxPages = std::max(minPages, included);
    if (s_wizPageCount < minPages) s_wizPageCount = minPages;
    if (s_wizPageCount > maxPages) s_wizPageCount = maxPages;
    for (auto& w : s_wizItems) {
        if (w.page < 1) w.page = 1;
        if (w.page > s_wizPageCount) w.page = s_wizPageCount;
    }
    const bool split = s_wizPageCount > 1;

    // Wheel name (validated non-empty)
    ImGui::TextUnformatted(L("opt.radial.name_label"));
    bool nameEmpty = (s_wizName[0] == '\0');
    if (nameEmpty) PushInvalidInputStyle();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##radialname", s_wizName, sizeof(s_wizName));
    if (nameEmpty) { PopInvalidInputStyle(); DrawInvalidInputBorder(); }

    // Items (bordered scroll child). When split, each included row gets a page combo.
    ImGui::Spacing();
    ImGui::TextUnformatted(L("opt.radial.items_label"));
    // A borderless table keeps the columns aligned: item (stretch) + a fixed-width
    // wheel-assignment combo, so the selectors line up instead of floating after each
    // variable-width name. The wheel column only exists when the export is split.
    const float thumb   = ImGui::GetFontSize() * 1.4f;
    const int   itemCols = split ? 2 : 1;
    const ImGuiTableFlags tflags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##radialitems", itemCols, tflags, ImVec2(0, 220))) {
        ImGui::TableSetupColumn("##item", ImGuiTableColumnFlags_WidthStretch);
        if (split)
            ImGui::TableSetupColumn("##wheel", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 6.5f);
        for (size_t i = 0; i < s_wizItems.size(); ++i) {
            WizItem& w = s_wizItems[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##inc", &w.include);
            ImGui::SameLine();
            DrawThumb(w, thumb);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(w.name.c_str());  // table clips to the column
            if (w.isNew) {
                ImGui::SameLine();
                ImGui::TextColored(kGreen, "(%s)", L("opt.radial.item_new"));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("opt.radial.item_new_tip"));
            }
            if (w.autoTarget) {
                ImGui::SameLine();
                ImGui::TextColored(kAmber, "%s", L("opt.radial.auto_target"));
            }

            if (split) {
                ImGui::TableSetColumnIndex(1);
                if (w.include) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string cur = std::string(L("opt.radial.page_col")) + " " +
                                      std::to_string(w.page);
                    if (ImGui::BeginCombo("##pg", cur.c_str())) {
                        for (int p = 1; p <= s_wizPageCount; ++p) {
                            std::string lbl = std::string(L("opt.radial.page_col")) + " " +
                                              std::to_string(p);
                            if (ImGui::Selectable(lbl.c_str(), w.page == p)) w.page = p;
                        }
                        ImGui::EndCombo();
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Entries that left the category since export (can't be shown as rows): listed so
    // the user knows they'll be dropped on save.
    if (!s_wizRemoved.empty()) {
        std::string joined;
        for (size_t i = 0; i < s_wizRemoved.size(); ++i)
            joined += (i ? ", " : "") + s_wizRemoved[i];
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextColored(kAmber, "%s %s", L("opt.radial.removed_note"), joined.c_str());
        ImGui::PopTextWrapPos();
    }

    // Pages controls: stepper + auto-fill + per-page fill readout.
    bool anyOver = false, anyEmpty = false;
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(L("opt.radial.pages"));
        ImGui::SameLine();
        if (s_wizPageCount <= minPages) BeginDisabledCompat();
        if (ImGui::Button("-##pages") && s_wizPageCount > minPages) {
            --s_wizPageCount;
            for (auto& w : s_wizItems) if (w.page > s_wizPageCount) w.page = s_wizPageCount;
        }
        if (s_wizPageCount <= minPages) EndDisabledCompat();
        ImGui::SameLine();
        ImGui::Text("%d", s_wizPageCount);
        ImGui::SameLine();
        if (s_wizPageCount >= maxPages) BeginDisabledCompat();
        if (ImGui::Button("+##pages") && s_wizPageCount < maxPages) ++s_wizPageCount;
        if (s_wizPageCount >= maxPages) EndDisabledCompat();
        ImGui::SameLine();
        if (ImGui::Button(L("opt.radial.autofill"))) AutoFillPages();

        for (int p = 1; p <= s_wizPageCount; ++p) {
            const int c = PageCount(p);
            const bool bad = (c > cap) || (c == 0);
            if (bad) anyOver  = anyOver  || c > cap;
            if (c == 0) anyEmpty = true;
            char buf[64];
            std::snprintf(buf, sizeof(buf), L("opt.radial.page_fill"), p, c, cap);
            ImGui::TextColored(bad ? kAmber : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                               "%s", buf);
        }
        if (anyOver)  ImGui::TextColored(kAmber, "%s", L("opt.radial.page_over"));
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
    const bool canExport = !nameEmpty && included >= 1 && !anyOver && !anyEmpty;
    RightAlignButtons(120.0f, 2);
    if (ImGui::Button(L("common.cancel"), ImVec2(120, 0))) {
        s_wizActive = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (!canExport) BeginDisabledCompat();
    const char* exportLbl = editing ? L("common.save") : L("opt.radial.export_confirm");
    if (ImGui::Button(exportLbl, ImVec2(120, 0)) && canExport) {
        // Partition included items into pages by their assigned page number.
        std::vector<std::vector<RadialItemRef>> pages(s_wizPageCount);
        for (const auto& w : s_wizItems)
            if (w.include) pages[w.page - 1].push_back(w.ref);
        if (editing) s_wizResult = EditGroup(s_wizEditGroup, s_wizCategory, s_wizName,
                                             pages, s_wizOpt);
        else         s_wizResult = ExportGroup(s_wizCategory, s_wizName, pages, s_wizOpt);
        RefreshStatus();
        s_wizDoneSynced = false;
        s_wizPhase = 1;  // -> done panel (modal stays open)
    }
    if (!canExport) EndDisabledCompat();

    ImGui::EndPopup();
}

// Is (type,id) still present in `category`?
bool CategoryHasRefId(const std::string& category, EFavoriteRefType type,
                      const std::string& id) {
    for (const auto& fc : g_Settings.FavoriteCategories) {
        if (fc.Name != category) continue;
        for (const auto& ref : fc.Refs)
            if (ref.Type == type && ref.Id == id) return true;
        return false;
    }
    return false;
}

// Distinct count of a category's refs (each maps to one Default item in a full export).
int CategoryRefCount(const std::string& category) {
    for (const auto& fc : g_Settings.FavoriteCategories)
        if (fc.Name == category) return (int)fc.Refs.size();
    return 0;
}

// Would re-exporting this logical export change it? Aggregates the union of all the
// group's page items vs the source category: a full export must still cover exactly
// the category's default refs; a partial export only drifts when one of its refs left
// the category (added / reordered items don't matter - the subset is deliberate).
// Caller guarantees the source category exists.
bool GroupDrift(const std::vector<RadialExport>& pages, const std::string& category,
                bool partial) {
    std::vector<RadialItemRef> items;  // union across pages (dedupe by type/id/variant)
    for (const auto& pg : pages)
        for (const auto& it : pg.Items) {
            bool dup = false;
            for (const auto& e : items) if (e == it) { dup = true; break; }
            if (!dup) items.push_back(it);
        }
    if (partial) {
        for (const auto& it : items)
            if (!CategoryHasRefId(category, it.Type, it.Id)) return true;
        return false;
    }
    // Full: item set must equal the category's default refs (order-independent).
    if ((int)items.size() != CategoryRefCount(category)) return true;
    for (const auto& it : items) {
        if (it.Variant != EMeMoteVariant::Default) return true;
        if (!CategoryHasRefId(category, it.Type, it.Id)) return true;
    }
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

    // 3. Create — one export per category. Splitting is handled inside the export
    // (pages), so an already-exported category is reconfigured via Edit, not re-added.
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
        std::vector<RadialExport> all;
        { std::lock_guard<std::mutex> lk(g_RadialExportsMutex); all = g_RadialExports; }
        if (all.empty()) {
            ImGui::TextDisabled("%s", L("opt.radial.none_yet"));
        } else {
            // One row per logical export: collect group ids in first-seen order
            // (g_RadialExports is sorted by Name), then render each group's pages as one.
            std::vector<std::string> groups;
            for (const auto& w : all) {
                bool seen = false;
                for (const auto& g : groups) if (g == w.Group) { seen = true; break; }
                if (!seen) groups.push_back(w.Group);
            }
            for (const auto& group : groups) {
                std::vector<RadialExport> pages;
                for (const auto& w : all) if (w.Group == group) pages.push_back(w);
                std::sort(pages.begin(), pages.end(),
                          [](const RadialExport& a, const RadialExport& b) { return a.Page < b.Page; });
                const RadialExport& head = pages.front();
                int  totalItems = 0;
                bool parseErr   = false;
                for (const auto& pg : pages) { totalItems += (int)pg.Items.size(); parseErr = parseErr || pg.ParseError; }

                // This export's page slugs + whether any are deployed in RadialMenus
                // (drives the +plus delete / sync cross-addon paths).
                std::vector<std::string> groupSlugs;
                bool deployedInRM = false;
                for (const auto& pg : pages) {
                    groupSlugs.push_back(pg.Slug);
                    if (RadialMenusHasPack(pg.Slug)) deployedInRM = true;
                }

                ImGui::PushID(group.c_str());
                ImGui::AlignTextToFramePadding();
                if (s_renameGroup == group) {
                    // Inline rename (no popup) - mirrors the Library's category rename.
                    if (s_renameFocus) { ImGui::SetKeyboardFocusHere(); s_renameFocus = false; }
                    bool empty = (s_renameBuf[0] == '\0');
                    ImGui::SetNextItemWidth(180.0f);
                    if (empty) PushInvalidInputStyle();
                    bool enter = ImGui::InputText("##wrn", s_renameBuf, sizeof(s_renameBuf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
                    bool active = ImGui::IsItemActive();
                    if (empty) { PopInvalidInputStyle(); DrawInvalidInputBorder(); }
                    // Rename only rewrites the staged pack; the deployed copy (if any)
                    // becomes "out of date" and the user pushes it with the Sync button
                    // (so a RadialMenus write stays an explicit, confirmed action).
                    if (enter && !empty) {
                        RenameGroup(group, s_renameBuf); RefreshStatus();
                        s_renameGroup.clear();
                    } else if (!active && ImGui::IsItemDeactivated()) {
                        if (!empty && head.Name != s_renameBuf) {
                            RenameGroup(group, s_renameBuf); RefreshStatus();
                        }
                        s_renameGroup.clear();
                    }
                } else {
                    ImGui::TextUnformatted(Ellipsize(head.Name, 180.0f).c_str());
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", head.SourceCategory.c_str());
                ImGui::SameLine();
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), L("opt.radial.page_count"),
                                  totalItems, (int)pages.size());
                    ImGui::TextDisabled("%s", buf);
                }

                // status (computed on the group's aggregate)
                bool sourceExists = false;
                for (const auto& fc : g_Settings.FavoriteCategories)
                    if (fc.Name == head.SourceCategory) { sourceExists = true; break; }
                bool drift = false;
                if (sourceExists && !parseErr)
                    drift = GroupDrift(pages, head.SourceCategory, head.Partial);

                ImGui::SameLine();
                if (parseErr) {
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

#ifdef EMOT3_PLUS
                // +plus: how the staged wheel compares to the copy in RadialMenus' folder.
                if (s_rmDetected && !parseErr) {
                    auto it = s_syncState.find(group);
                    RadialSyncState ss = (it != s_syncState.end()) ? it->second
                                                                   : RadialSyncState::NotDeployed;
                    ImGui::SameLine();
                    if (ss == RadialSyncState::InSync)
                        ImGui::TextColored(kGreen, "%s", L("opt.radial.rm_synced"));
                    else if (ss == RadialSyncState::OutOfDate)
                        ImGui::TextColored(kAmber, "%s", L("opt.radial.rm_outofdate"));
                    else
                        ImGui::TextDisabled("%s", L("opt.radial.rm_notdeployed"));
                }
#endif

                // actions — Edit re-opens the wizard pre-filled for the whole export.
                if (sourceExists && !parseErr) {
                    if (ImGui::SmallButton(L("opt.radial.edit"))) OpenWizardForEdit(group);
                    ImGui::SameLine();
                }
#ifdef EMOT3_PLUS
                // +plus: push this wheel's files into RadialMenus (overwrites). The click
                // is the approval; deploy-all and remove keep their own confirms.
                if (!parseErr && s_rmDetected) {
                    if (ImGui::SmallButton(L("opt.radial.sync"))) {
                        DeployGroupToRadialMenus(groupSlugs);
                        RefreshStatus();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("opt.radial.sync_tip"));
                    ImGui::SameLine();
                }
#endif
                if (ImGui::SmallButton(L("opt.radial.rename"))) {
                    // Begin inline rename (the InputText renders in place of the name
                    // at the top of the row next frame).
                    s_renameGroup = group;
                    s_renameFocus = true;
                    std::snprintf(s_renameBuf, sizeof(s_renameBuf), "%s", head.Name.c_str());
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(L("opt.radial.remove"))) {
                    s_removeAlsoRM = true;
                    ImGui::OpenPopup("##radialremove");
                }
                // remove confirm — a proper modal (matches the destructive-action idiom)
                {
                    ImVec2 ds = ImGui::GetIO().DisplaySize;
                    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f),
                                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                }
                if (ImGui::BeginPopupModal("##radialremove", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                    ImGui::TextWrapped("%s", L("opt.radial.remove_confirm"));
                    ImGui::Spacing();
                    // Always: the binds go away, so a wheel still in RadialMenus goes dead.
                    ImGui::TextWrapped("%s", L("opt.radial.remove_keybind_warn"));
#ifdef EMOT3_PLUS
                    // +plus: offer to also delete the deployed copy (only if it's there).
                    if (deployedInRM) {
                        ImGui::Spacing();
                        ImGui::Checkbox(L("opt.radial.remove_rm_also"), &s_removeAlsoRM);
                    }
#else
                    // Base: we never touch RadialMenus' folder - tell the user to.
                    ImGui::Spacing();
                    ImGui::TextColored(kAmber, "%s", L("opt.radial.remove_rm_manual"));
#endif
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                    bool removed = false;
                    if (ImGui::Button(L("opt.radial.remove"), ImVec2(110, 0))) {
#ifdef EMOT3_PLUS
                        if (deployedInRM && s_removeAlsoRM) RemoveFromRadialMenus(groupSlugs);
#endif
                        RemoveGroup(group);   // the row vanishing is the feedback
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

    // 5. Apply (Deploy) — PLUS ONLY. The base build can't write into another addon's
    // folder, so it has no Apply section; its intro explains the manual copy + reload.
#ifdef EMOT3_PLUS
    // Section header carries the Plus badge (this whole deploy convenience is +plus).
    ImGui::Spacing();
    ImGui::TextDisabled("%s", L("opt.radial.sec_apply"));
    PlusBadge();
    ImGui::Separator();
    {
        std::vector<RadialExport> wheels;
        { std::lock_guard<std::mutex> lk(g_RadialExportsMutex); wheels = g_RadialExports; }

        char btn[64];
        std::snprintf(btn, sizeof(btn), L("opt.radial.deploy_btn"), (int)wheels.size());
        bool none = wheels.empty() || !s_rmDetected;
        if (none) BeginDisabledCompat();
        if (ImGui::Button(btn) && !none) ImGui::OpenPopup("##radialdeploy");
        if (none) EndDisabledCompat();

        // Replacing files in RadialMenus needs explicit approval (overwrites, no backup).
        {
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f),
                                    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        }
        if (ImGui::BeginPopupModal("##radialdeploy", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextColored(kAmber, "%s", L("opt.radial.deploy_warn"));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button(L("opt.radial.deploy_go"), ImVec2(120, 0))) {
                RadialDeployResult r = DeployToRadialMenus();
                if (r.ok) LOG_INFO("radials: deploy ok (%d packs, %d icons)", r.packs, r.icons);
                s_deployOk = r.ok;  // inline feedback in the tab (window is open), no toast
                char msg[128];
                if (r.ok) std::snprintf(msg, sizeof(msg), L("opt.radial.deployed_inline"),
                                        r.packs, r.icons);
                else      std::snprintf(msg, sizeof(msg), "%s", L("opt.radial.deploy_failed_inline"));
                s_deployMsg = msg;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("common.cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!s_deployMsg.empty())
            ImGui::TextColored(s_deployOk ? kGreen : kRed, "%s", s_deployMsg.c_str());
        ImGui::TextWrapped("%s", L("opt.radial.reload_reminder"));
    }
#endif

    // Fire the deferred OpenPopup at the top-level ID scope (the Create/Edit buttons
    // can't, since Edit lives inside a per-row PushID — that mismatch was the
    // "Edit does nothing" bug). Then render the wizard.
    if (s_wizOpenRequested) { ImGui::OpenPopup("###radialwizard"); s_wizOpenRequested = false; }
    if (s_wizActive) RenderWizard();
}
