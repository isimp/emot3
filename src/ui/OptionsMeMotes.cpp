#include "OptionsMeMotes.h"

#include "Favorites.h"   // RemoveRefFromCategories on delete
#include "Globals.h"
#include "I18n.h"        // L(), TooltipText
#include "IconBrowse.h"  // StartIconBrowse for the Browse... button
#include "Layout.h"      // PushDestructiveButtonStyles, PushInvalidInputStyle, DrawInvalidInputBorder, Ellipsize
#include "Logging.h"
#include "MeMotes.h"
#include "OptionsCommon.h"   // OptionsSection
#include "Settings.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // PushItemFlag

#include <algorithm>
#include <cfloat>        // FLT_MIN
#include <cstdio>
#include <cstring>       // std::memcpy
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
//  Options > /me-motes tab
//
//  Editor for the /me-mote catalog (see data/MeMotes.h). Structurally and
//  visually mirrors OptionsEmotes — section + add input + toolbar + bordered
//  320 px scrollable child + collapsible rows with the same two-column form,
//  same auto-commit-on-focus-loss pattern, same Browse/Clear icon controls,
//  same destructive-styled Delete. The two editors should read as siblings.
//
//  Key differences vs OptionsEmotes:
//    - Add input takes an Id directly (not a slash command) — /me-motes have
//      no command. Validated by NormalizeMeMoteId + a g_MeMotes collision
//      check.
//    - Three text bodies (default / you / all) instead of one Command field.
//      TextDefault is required and validated red-bordered on empty; You/All
//      are optional (their right-click variants gray out when unset).
//    - No IsCore/IsTargetable/IsMadKing flags — those concepts don't apply.
//    - Aliases are free-form search words, not slash commands; they parse on
//      whitespace + comma (matches OptionsEmotes' alias parser, minus the
//      NormalizeEmoteCommand pass that would add a leading slash).
//
//  Mutation discipline:
//    - All g_MeMotes mutations under g_MeMotesMutex (consistent with the
//      per-frame renderer + the background load).
//    - Persist() (MarkMeMotesDirty + SaveMeMotesJson) MUST be called outside
//      the mutex — std::mutex is non-recursive and SaveMeMotesJson re-acquires
//      it internally. Every callsite scopes its lock and releases first.
//    - LOG_DEBUG on every kind of mutation (add / rename / icon / aliases /
//      text default/you/all / delete).
// ============================================================================

namespace {

// Per-row edit buffer. Lives outside the InputText calls so click-away
// reliably commits without depending on ImGui's IsItemDeactivatedAfterEdit
// + a stable user buffer. No `icon` field — the icon path is set via the
// Browse/Clear buttons (StartIconBrowse / direct clear), never typed.
struct RowBuffer {
    std::string name;
    std::string aliases;     // edit buffer; whitespace + comma separated
    std::string textDefault;
    std::string textYou;
    std::string textAll;
    bool        initialized = false;
};

// Per-row state, keyed on the stable /me-mote Id. Entries persist across
// frames (and across collapses) so InputText buffers for a re-expanded row
// keep their state. Matches OptionsEmotes' s_rowOpen / s_rowBufs maps.
std::unordered_map<std::string, bool>       s_rowOpen;
std::unordered_map<std::string, RowBuffer>  s_rowBufs;

// Seed the per-row buffer from the live MeMote. Aliases re-joined with single
// spaces (matches the catalog's display form).
void Seed(RowBuffer& rb, const MeMote& m) {
    rb.name = m.Name;
    rb.aliases.clear();
    for (size_t i = 0; i < m.Aliases.size(); ++i) {
        if (i) rb.aliases += ' ';
        rb.aliases += m.Aliases[i];
    }
    rb.textDefault = m.TextDefault;
    rb.textYou     = m.TextYou;
    rb.textAll     = m.TextAll;
    rb.initialized = true;
}

// Trim ASCII whitespace.
std::string Trim(std::string s) {
    auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && isws(s.front())) s.erase(s.begin());
    while (!s.empty() && isws(s.back()))  s.pop_back();
    return s;
}

// Split an alias buffer on whitespace + comma, trim each token, drop empties,
// dedupe (first-wins). Mirrors the OptionsEmotes alias parser exactly minus
// the per-token NormalizeEmoteCommand pass (which adds a leading slash —
// /me-mote aliases are free-form search words, not slash commands, so we
// just take the trimmed token as-is).
std::vector<std::string> ParseAliases(const std::string& buf) {
    std::vector<std::string> out;
    std::string tok;
    auto flush = [&]() {
        std::string t = Trim(tok);
        tok.clear();
        if (t.empty()) return;
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(std::move(t));
    };
    for (char ch : buf) {
        if (ch == ' ' || ch == '\t' || ch == ',') flush();
        else                                       tok += ch;
    }
    flush();
    return out;
}

// Join an alias list back to a single-space-separated display form.
std::string JoinAliases(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ' ';
        out += v[i];
    }
    return out;
}

// Persist + invalidate after a mutation. MUST be called with g_MeMotesMutex
// NOT held — SaveMeMotesJson re-acquires the mutex internally and std::mutex
// is non-recursive (taking it twice = UB / crash). Every callsite scopes its
// lock and releases before Persist().
void Persist() {
    MarkMeMotesDirty();
    if (!g_MeMotesJsonPath.empty()) SaveMeMotesJson(g_MeMotesJsonPath);
}

// Render one labeled single-line InputText row inside an open 2-column table.
// Auto-commits when its buffer differs from the stored value and the input
// isn't currently being edited. validate=true paints the invalid-input style
// + draws the red border when the trimmed buffer is empty (used for required
// fields like TextDefault and Name); invalidTooltipKey shows a tooltip on
// hover when invalid (e.g. "Name cannot be empty.").
//
// Returns true iff a commit fired (caller persists outside the mutex).
bool FieldRow(const char* labelKey, const char* hintKey, const char* idSuffix,
              std::string& buf, std::string& stored, bool required,
              const char* invalidTooltipKey = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L(labelKey));
    ImGui::TableSetColumnIndex(1);

    std::string trimmed = Trim(buf);
    bool dirty   = (trimmed != stored);
    bool invalid = required && trimmed.empty();

    // 1 KB scratch covers the longest expected /me-mote body. Pastes longer
    // than that get truncated (matches OptionsEmotes' fixed-buffer policy).
    char scratch[1024];
    size_t copyN = buf.size();
    if (copyN >= sizeof(scratch)) copyN = sizeof(scratch) - 1;
    std::memcpy(scratch, buf.data(), copyN);
    scratch[copyN] = 0;

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (invalid) PushInvalidInputStyle();
    const std::string inputId  = std::string("##") + idSuffix;
    const char*       hint     = hintKey ? L(hintKey) : "";
    if (ImGui::InputTextWithHint(inputId.c_str(), hint, scratch, sizeof(scratch))) {
        buf = scratch;
    }
    bool active = ImGui::IsItemActive();
    if (invalid) {
        PopInvalidInputStyle();
        DrawInvalidInputBorder();
    }
    if (invalid && invalidTooltipKey && ImGui::IsItemHovered())
        TooltipText(invalidTooltipKey);

    bool committed = false;
    if (dirty && !active && !invalid) {
        stored = trimmed;
        buf    = trimmed;
        committed = true;
    }
    return committed;
}

} // namespace

void RenderMeMotesTab() {
    // ---- Intro --------------------------------------------------------
    ImGui::TextWrapped("%s", L("opt.mm.hlp_top"));
    ImGui::Spacing();

    // ---- "Add to /me-motes" section: inline Id input + Add button -----
    // Mirrors OptionsEmotes' "Add to catalog" Add input exactly: an
    // OptionsSection heading, an InputTextWithHint width 180 px, then the
    // Add button on SameLine. Validation states (empty / collision) paint
    // the invalid input style + red border + tooltip; Add is disabled
    // until valid.
    OptionsSection(L("opt.sec.add_me_mote"));

    static char s_newIdBuf[64] = {};
    std::string rawNew   = s_newIdBuf;
    std::string normNew  = NormalizeMeMoteId(rawNew);
    bool newHasInput = !Trim(rawNew).empty();

    bool newIdDup = false;
    if (newHasInput && !normNew.empty()) {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        newIdDup = (FindMeMote(normNew) != nullptr);
    }
    bool newInvalid = newHasInput && (normNew.empty() || newIdDup);

    ImGui::SetNextItemWidth(180.f);
    if (newInvalid) PushInvalidInputStyle();
    ImGui::InputTextWithHint("##new_me_mote_id", L("opt.mm.new_id_hint"),
                             s_newIdBuf, sizeof(s_newIdBuf));
    if (newInvalid) {
        PopInvalidInputStyle();
        DrawInvalidInputBorder();
    }
    if (newInvalid && ImGui::IsItemHovered()) {
        if (normNew.empty())
            TooltipText("opt.mm.id_min");
        else
            ImGui::SetTooltip(L("opt.mm.id_exists"), normNew.c_str());
    }

    ImGui::SameLine();
    bool addEnabled = newHasInput && !newInvalid;
    if (!addEnabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }
    bool addPressed = ImGui::Button(L("opt.mm.add_button"));
    if (!addEnabled) {
        ImGui::PopStyleVar();
        ImGui::PopItemFlag();
    }
    if (addPressed && addEnabled) {
        std::string newId = normNew;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            MeMote m;
            m.Id = newId;
            g_MeMotes.push_back(std::move(m));
        }
        s_rowOpen[newId] = true;     // open the new row for immediate editing
        LOG_DEBUG("/me-mote added (id=%s)", newId.c_str());
        Persist();                    // outside the lock
        s_newIdBuf[0] = '\0';         // clear input on commit
    }

    // Snapshot Ids under the mutex so per-row work doesn't hold it across
    // ImGui draws. Sort by Id for stable display order (matches catalog).
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        ids.reserve(g_MeMotes.size());
        for (const auto& m : g_MeMotes) ids.push_back(m.Id);
    }
    std::sort(ids.begin(), ids.end());

    // ---- List toolbar: count + Expand all / Collapse all -------------
    static int s_setAllOpen = 0;
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(L("opt.mm.count"), (int)ids.size());

        const ImGuiStyle& st = ImGui::GetStyle();
        float wExpand   = ImGui::CalcTextSize(L("opt.em.expand_all")).x   + st.FramePadding.x * 2.f;
        float wCollapse = ImGui::CalcTextSize(L("opt.em.collapse_all")).x + st.FramePadding.x * 2.f;
        float total     = wExpand + wCollapse + st.ItemSpacing.x;
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > total)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total));
        if (ImGui::SmallButton(L("opt.em.expand_all")))   s_setAllOpen =  1;
        ImGui::SameLine();
        if (ImGui::SmallButton(L("opt.em.collapse_all"))) s_setAllOpen = -1;
    }

    // Width of the shared label column — max of every field-label width so
    // inputs across rows align (matches OptionsEmotes' labelColW pre-calc).
    float labelColW = 0.f;
    for (const char* k : { "opt.mm.lbl_name", "opt.mm.lbl_icon",
                           "opt.mm.lbl_aliases", "opt.mm.lbl_text_default",
                           "opt.mm.lbl_text_you", "opt.mm.lbl_text_all" })
        labelColW = std::max(labelColW, ImGui::CalcTextSize(L(k)).x);
    labelColW += ImGui::GetStyle().FramePadding.x * 2.f;

    const ImVec4 kIdColor(0.92f, 0.78f, 0.32f, 1.f);

    std::string deleteId;

    // ---- Scrollable bordered fixed-height list -----------------------
    ImGui::BeginChild("##memotelist", ImVec2(0, 320.f), true);

    if (ids.empty()) {
        ImGui::TextDisabled("%s", L("opt.mm.empty"));
        ImGui::EndChild();
        s_setAllOpen = 0;
        return;
    }

    for (const std::string& id : ids) {
        bool exists = false;
        std::string displayName;
        std::string iconPathSnapshot;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            if (const MeMote* m = FindMeMote(id)) {
                exists = true;
                displayName       = m->Name.empty() ? id : m->Name;
                iconPathSnapshot  = m->IconPath;
            }
        }
        if (!exists) continue;

        ImGui::PushID(id.c_str());

        // Apply queued Expand-all / Collapse-all before reading state.
        bool& rowOpen = s_rowOpen[id];
        if (s_setAllOpen != 0) rowOpen = (s_setAllOpen > 0);

        // ---- Row header: ArrowButton + colored Id + Delete -----------
        if (ImGui::ArrowButton("##expand", rowOpen ? ImGuiDir_Down : ImGuiDir_Right))
            rowOpen = !rowOpen;
        ImGui::SameLine();
        ImGui::TextColored(kIdColor, "%s", id.c_str());
        if (ImGui::IsItemHovered()) TooltipText("opt.mm.id_tooltip");

        if (displayName != id) {
            ImGui::SameLine();
            ImGui::TextDisabled("— %s", displayName.c_str());
        }
        {
            const char* lbl = L("common.delete");
            float btnW = ImGui::CalcTextSize(lbl).x +
                         ImGui::GetStyle().FramePadding.x * 2.f;
            ImGui::SameLine();
            float remainingW = ImGui::GetContentRegionAvail().x;
            if (remainingW > btnW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (remainingW - btnW));
            int destrPushes = PushDestructiveButtonStyles(/*includeText=*/true);
            if (ImGui::SmallButton(lbl)) deleteId = id;
            ImGui::PopStyleColor(destrPushes);
        }

        if (!rowOpen) {
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PopID();
            continue;
        }

        // ---- Body table ---------------------------------------------
        ImGui::Indent(10.f);
        if (ImGui::BeginTable("##mmfields", 2,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                                    labelColW);
            ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);

            RowBuffer& rb = s_rowBufs[id];
            if (!rb.initialized) {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (const MeMote* m = FindMeMote(id)) Seed(rb, *m);
            }

            bool anyChanged = false;

            // ---- Name (REQUIRED) ----
            {
                std::string stored;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) stored = m->Name;
                }
                std::string newVal = stored;
                if (FieldRow("opt.mm.lbl_name", "opt.mm.name_hint", "mm_name",
                             rb.name, newVal, /*required=*/true,
                             /*invalidTooltipKey=*/"common.name_empty")) {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (m->Name != newVal) {
                            LOG_DEBUG("/me-mote rename: id=%s, '%s' -> '%s'",
                                      id.c_str(), m->Name.c_str(), newVal.c_str());
                            m->Name = newVal;
                            anyChanged = true;
                        }
                    }
                }
            }

            // ---- Icon (Browse + Clear; no editable text field) ----
            // Matches OptionsEmotes' icon row: a disabled-text status
            // describing the resolved path, then Browse + Clear on a
            // SameLine row. No InputText — the path is set exclusively by
            // the file picker or cleared to fall back to the icon chain.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(L("opt.mm.lbl_icon"));
            ImGui::TableSetColumnIndex(1);
            {
                // Status line — Ellipsized so a long path can't push the
                // Browse/Clear buttons off the row.
                std::string status = iconPathSnapshot.empty()
                                       ? std::string(L("opt.mm.icon_none"))
                                       : (std::string(L("opt.mm.icon_custom_prefix")) + iconPathSnapshot);
                float availW = ImGui::GetContentRegionAvail().x;
                if (availW < 40.f) availW = 40.f;
                std::string fit = Ellipsize(status, availW);
                ImGui::TextDisabled("%s", fit.c_str());
                if (ImGui::IsItemHovered() && !iconPathSnapshot.empty())
                    ImGui::SetTooltip("%s", iconPathSnapshot.c_str());

                bool busy = g_IconBrowse.active.load() || g_IconBrowse.ready.load();
                if (busy) {
                    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                        ImGui::GetStyle().Alpha * 0.5f);
                }
                if (ImGui::SmallButton((std::string(L("opt.em.browse")) + "##mmiconbrowse").c_str())
                    && !busy) {
                    StartIconBrowse(EIconTargetKind::MeMote, id, iconPathSnapshot);
                }
                if (busy) {
                    ImGui::PopStyleVar();
                    ImGui::PopItemFlag();
                }
                if (ImGui::IsItemHovered() && !busy) TooltipText("opt.em.browse_tooltip");

                ImGui::SameLine();
                bool hasOverride = !iconPathSnapshot.empty();
                if (!hasOverride) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                                      ImGui::GetStyle().Alpha * 0.30f);
                if (ImGui::SmallButton((std::string(L("common.clear")) + "##mmiconclr").c_str())
                    && hasOverride) {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (!m->IconPath.empty()) {
                            LOG_DEBUG("/me-mote icon clear: id=%s", id.c_str());
                            m->IconPath.clear();
                            anyChanged = true;
                        }
                    }
                }
                if (!hasOverride) ImGui::PopStyleVar();
                if (ImGui::IsItemHovered() && hasOverride)
                    TooltipText("opt.em.clear_icon_tooltip");
            }

            // ---- Aliases ----
            // Whitespace + comma separated; parse + dedupe on commit,
            // re-join with single spaces for display. Mirrors
            // OptionsEmotes' alias row exactly (minus the slash normalization).
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(L("opt.mm.lbl_aliases"));
            ImGui::TableSetColumnIndex(1);
            {
                char aliasBuf[256];
                size_t copyN = rb.aliases.size();
                if (copyN >= sizeof(aliasBuf)) copyN = sizeof(aliasBuf) - 1;
                std::memcpy(aliasBuf, rb.aliases.data(), copyN);
                aliasBuf[copyN] = 0;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputTextWithHint("##mm_aliases",
                                             L("opt.mm.aliases_hint"),
                                             aliasBuf, sizeof(aliasBuf))) {
                    rb.aliases = aliasBuf;
                }
                bool aliasActive = ImGui::IsItemActive();
                if (!aliasActive) {
                    std::vector<std::string> parsed = ParseAliases(rb.aliases);
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (parsed != m->Aliases) {
                            LOG_DEBUG("/me-mote aliases edit: id=%s (%d alias(es))",
                                      id.c_str(), (int)parsed.size());
                            m->Aliases = parsed;
                            anyChanged = true;
                        }
                    }
                    rb.aliases = JoinAliases(parsed);
                }
            }

            // ---- TextDefault (REQUIRED) ----
            {
                std::string stored;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) stored = m->TextDefault;
                }
                std::string newVal = stored;
                if (FieldRow("opt.mm.lbl_text_default", "opt.mm.text_default_hint",
                             "mm_text_default", rb.textDefault, newVal,
                             /*required=*/true,
                             /*invalidTooltipKey=*/"opt.mm.text_default_empty")) {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (m->TextDefault != newVal) {
                            LOG_DEBUG("/me-mote text default edit: id=%s", id.c_str());
                            m->TextDefault = newVal;
                            anyChanged = true;
                        }
                    }
                }
            }

            // ---- TextYou (optional) ----
            {
                std::string stored;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) stored = m->TextYou;
                }
                std::string newVal = stored;
                if (FieldRow("opt.mm.lbl_text_you", "opt.mm.text_you_hint",
                             "mm_text_you", rb.textYou, newVal,
                             /*required=*/false)) {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (m->TextYou != newVal) {
                            LOG_DEBUG("/me-mote text_you edit: id=%s", id.c_str());
                            m->TextYou = newVal;
                            anyChanged = true;
                        }
                    }
                }
            }

            // ---- TextAll (optional) ----
            {
                std::string stored;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) stored = m->TextAll;
                }
                std::string newVal = stored;
                if (FieldRow("opt.mm.lbl_text_all", "opt.mm.text_all_hint",
                             "mm_text_all", rb.textAll, newVal,
                             /*required=*/false)) {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (m->TextAll != newVal) {
                            LOG_DEBUG("/me-mote text_all edit: id=%s", id.c_str());
                            m->TextAll = newVal;
                            anyChanged = true;
                        }
                    }
                }
            }

            ImGui::EndTable();

            if (anyChanged) Persist();
        }
        ImGui::Unindent(10.f);
        ImGui::Spacing();

        // Divider so the list reads as discrete entries (matches OptionsEmotes).
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
    s_setAllOpen = 0;

    // Deferred delete — applied after the iteration so the snapshot doesn't
    // shift mid-loop.
    if (!deleteId.empty()) {
        std::string nameForLog;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            auto it = std::find_if(g_MeMotes.begin(), g_MeMotes.end(),
                                   [&](const MeMote& m) { return m.Id == deleteId; });
            if (it != g_MeMotes.end()) {
                nameForLog = it->Name;
                g_MeMotes.erase(it);
            }
        }
        LOG_DEBUG("/me-mote deleted: id=%s (name='%s')",
                  deleteId.c_str(), nameForLog.c_str());
        RemoveRefFromCategories(EFavoriteRefType::MeMote, deleteId);
        s_rowOpen.erase(deleteId);
        s_rowBufs.erase(deleteId);
        Persist();
    }
}
