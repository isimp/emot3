#include "OptionsMeMotes.h"

#include "Favorites.h"   // RemoveRefFromCategories on delete
#include "Usage.h"        // usage::RemoveRef on delete (Recently/Frequently used)
#include "Globals.h"
#include "SaveScheduler.h"  // RequestSave (debounced, off-thread /me-mote writes)
#include "I18n.h"        // L(), TooltipText
#include "IconBrowse.h"  // ApplyIconPathToTarget (cross-catalog routing) + EIconTargetKind
#include "IconPicker.h"  // OpenIconPicker ("Library..." button)
#include "EmoteBinds.h"  // SyncEmoteBinds on a keybind toggle
#include "Layout.h"      // PushDestructiveButtonStyles, PushInvalidInputStyle, DrawInvalidInputBorder, Ellipsize
#include "Logging.h"
#include "StringUtil.h"  // TrimWhitespace (shared helper)
#include "MeMotes.h"
#include "OptionsCommon.h"   // OptionsSection
#include "Profiling.h"   // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)
#include "Icons.h"       // ResolveMeMoteIconSource + MeMoteIconSource (icon status tier)
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
#include <unordered_set>
#include <vector>

// ============================================================================
//  Options > /me-motes tab
//
//  Editor for the /me-mote catalog (see data/MeMotes.h). Structurally and
//  visually mirrors OptionsEmotes — section + add input + toolbar + bordered
//  320 px scrollable child + collapsible rows with the same two-column form,
//  same auto-commit-on-focus-loss pattern, same Library/Clear icon controls,
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
// in-app picker (Library...) or cleared, never typed; an arbitrary absolute
// path is a power-user me_motes.json hand-edit.
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
// keep their state. Matches OptionsEmotes' s_rowOpen / s_rowBufs maps,
// including the per-frame prune of dead Ids in RenderMeMotesTab.
std::unordered_map<std::string, bool>       s_rowOpen;
std::unordered_map<std::string, RowBuffer>  s_rowBufs;

// Per-row icon-source status cache, keyed on the stable Id. Mirrors
// OptionsEmotes' IconStatusCache exactly — `ResolveMeMoteIconSource` does a
// `GetFileAttributesA` disk stat plus a bundled-table scan, so doing that
// for every OPEN row every frame busts the "no per-frame disk I/O in render
// path" rule. The recompute trigger keys on the two inputs that actually
// change the resolved tier: the user's `m.IconPath` and the AI-fallback
// toggle. (A PNG dropped into icons/<id>.png while the tab is open still
// needs a tab reopen to surface — same expectation as the texture cache's
// restart-to-apply note.)
struct MmIconStatus {
    std::string iconPath;
    bool        aiFallback = false;
    bool        valid      = false;
    bool        isDiskFile = false;  // Custom / FolderOverride -> offer Refresh
    std::string text;
};
std::unordered_map<std::string, MmIconStatus> s_mmIconStatus;

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

// Split an alias buffer on whitespace + comma, trim each token, drop empties,
// dedupe (first-wins). Mirrors the OptionsEmotes alias parser exactly minus
// the per-token NormalizeEmoteCommand pass (which adds a leading slash —
// /me-mote aliases are free-form search words, not slash commands, so we
// just take the trimmed token as-is).
std::vector<std::string> ParseAliases(const std::string& buf) {
    std::vector<std::string> out;
    std::string tok;
    auto flush = [&]() {
        std::string t = TrimWhitespace(tok);
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
    RequestSave(SaveKind::MeMotes);
}

// Render one labeled single-line InputText row inside an open 2-column table.
// Auto-commits when its buffer differs from the stored value and the input
// isn't currently being edited. validate=true paints the invalid-input style
// + draws the red border when the trimmed buffer is empty (used for required
// fields like TextDefault and Name); invalidTooltipKey shows a tooltip on
// hover when invalid (e.g. "Name cannot be empty.").
//
// charBudget > 0 reserves a fixed zone on the RIGHT of the field and draws a
// "N / budget" count INSIDE the frame there — no separate line below, and it
// never overlaps typed text (the input is narrowed by the zone width). Color
// thresholds: 0 .. 80% of budget = muted gray; > 80% .. budget = amber; >
// budget = red, with the "will be cut off" note moved to a hover tooltip. Drives
// the GW2 chat-line-length (kMeMoteBodyCharBudget = 195) guide for the three body
// fields; pass 0 for non-body fields (full-width input, no counter). Counting is
// `buf.size()` (UTF-8 byte count) — close enough for an author-side guide; GW2's
// exact rule at multibyte edge cases is fuzzy and the warning is non-blocking
// either way (long bodies still save + send).
//
// Returns true iff a commit fired (caller persists outside the mutex).
bool FieldRow(const char* labelKey, const char* hintKey, const char* idSuffix,
              std::string& buf, std::string& stored, bool required,
              const char* invalidTooltipKey = nullptr,
              int charBudget = 0)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L(labelKey));
    ImGui::TableSetColumnIndex(1);

    std::string trimmed = TrimWhitespace(buf);
    bool dirty   = (trimmed != stored);
    bool invalid = required && trimmed.empty();

    // 1 KB scratch covers the longest expected /me-mote body. Pastes longer
    // than that get truncated (matches OptionsEmotes' fixed-buffer policy).
    char scratch[1024];
    size_t copyN = buf.size();
    if (copyN >= sizeof(scratch)) copyN = sizeof(scratch) - 1;
    std::memcpy(scratch, buf.data(), copyN);
    scratch[copyN] = 0;

    const ImGuiStyle& st = ImGui::GetStyle();

    // In-field char counter (charBudget > 0): reserve a fixed-width zone on the
    // right, sized to the worst case so the input edge doesn't jitter, and draw
    // the count INSIDE the frame there. To keep the field ONE visual element
    // (uniform hover/active, no seam between input and zone) we draw the frame
    // background ourselves at full width and render the InputText with a
    // transparent frame on top. charBudget == 0 fields just get an empty zone.
    char   countBuf[24] = {0};
    ImVec4 countColor;
    bool   over  = false;
    float  zoneW = 0.f;
    if (charBudget > 0) {
        const int n      = (int)buf.size();
        const int warnAt = (charBudget * 80) / 100;   // 156 for 195
        over = (n > charBudget);
        std::snprintf(countBuf, sizeof countBuf, "%d / %d", n, charBudget);
        if      (n <= warnAt) countColor = st.Colors[ImGuiCol_TextDisabled]; // informational
        else if (!over)       countColor = ImVec4(0.92f, 0.78f, 0.32f, 1.0f); // amber — getting close
        else                  countColor = ImVec4(1.0f, 0.45f, 0.40f, 1.0f);  // red   — over the cap
        char worst[24];
        std::snprintf(worst, sizeof worst, "%d / %d", charBudget, charBudget);
        zoneW = ImGui::CalcTextSize(worst).x + st.FramePadding.x * 2.f;
    }

    const std::string inputId  = std::string("##") + idSuffix;
    const float       fullW    = ImGui::GetContentRegionAvail().x;
    const ImVec2      framePos = ImGui::GetCursorScreenPos();
    const float       frameH   = ImGui::GetFrameHeight();
    const ImVec2      frameMax(framePos.x + fullW, framePos.y + frameH);

    // One unified frame bg under the whole field. fieldActive tracks live focus,
    // fieldHovered the WHOLE rect (so the reserved zone lights up with the input,
    // not just the input half). IsMouseHoveringRect clips to the visible region by
    // default, so a row scrolled partly out of view won't false-hover.
    const bool fieldActive  = (ImGui::GetActiveID() == ImGui::GetID(inputId.c_str()));
    const bool fieldHovered = ImGui::IsMouseHoveringRect(framePos, frameMax);
    ImU32 frameBg;
    if (invalid) {   // matches PushInvalidInputStyle (Layout.cpp) by state
        frameBg = ImGui::ColorConvertFloat4ToU32(
            fieldActive  ? ImVec4(0.70f, 0.16f, 0.16f, 1.f)
          : fieldHovered ? ImVec4(0.65f, 0.14f, 0.14f, 1.f)
                         : ImVec4(0.55f, 0.10f, 0.10f, 1.f));
    } else {
        frameBg = ImGui::GetColorU32(fieldActive  ? ImGuiCol_FrameBgActive
                                   : fieldHovered ? ImGuiCol_FrameBgHovered
                                                  : ImGuiCol_FrameBg);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(framePos, frameMax, frameBg, st.FrameRounding, ImDrawCornerFlags_All);

    // Input on top, transparent-framed so only our unified bg shows; narrowed by
    // the zone so typed text never reaches the counter.
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(0, 0, 0, 0));
    ImGui::SetNextItemWidth(charBudget > 0 ? (fullW - zoneW) : -FLT_MIN);
    const char* hint = hintKey ? L(hintKey) : "";
    if (ImGui::InputTextWithHint(inputId.c_str(), hint, scratch, sizeof(scratch)))
        buf = scratch;
    ImGui::PopStyleColor(3);
    const bool active = ImGui::IsItemActive();

    // Count, right-aligned inside the reserved zone, vertically centered.
    if (charBudget > 0) {
        const ImVec2 tsz = ImGui::CalcTextSize(countBuf);
        dl->AddText(ImVec2(frameMax.x - st.FramePadding.x - tsz.x,
                           framePos.y + (frameH - tsz.y) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(countColor), countBuf);
    }

    // Invalid: red border around the full field + explanatory tooltip on hover.
    if (invalid)
        dl->AddRect(framePos, frameMax, IM_COL32(255, 80, 80, 255),
                    st.FrameRounding, ImDrawCornerFlags_All, 2.0f);
    if (invalid && invalidTooltipKey && fieldHovered)
        TooltipText(invalidTooltipKey);

    // Over budget: count is already red; the verbose "will be cut off" note is a
    // hover hint rather than a reclaimed line.
    if (charBudget > 0 && over && fieldHovered && !invalid)
        TooltipText("opt.mm.body_truncate_warn");

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
    PROFILE_SCOPE("opt.me_motes");  // dev perf overlay — sibling to opt.emotes / opt.general / etc.
    // ---- Intro --------------------------------------------------------
    ImGui::TextWrapped("%s", L("opt.mm.hlp_top"));

    // Collapsible explainer for the icon resolution chain (where each row's
    // icon comes from). Closed by default; mirrors the Catalog tab's
    // explainer in structure but spells out the /me-mote 4-tier chain (no
    // BundledOfficial — the catalog is user-content, ArenaNet doesn't ship
    // art for it). Its order must match ResolveMeMoteIconSource (Icons.cpp)
    // and the per-row icon-status switch above; if they drift, the user sees
    // the lie before we do.
    if (ImGui::CollapsingHeader(L("opt.mm.icon_header"))) {
        ImGui::Indent();
        ImGui::TextWrapped("%s", L("opt.mm.icon_explainer"));
        ImGui::Unindent();
    }
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
    bool newHasInput = !TrimWhitespace(rawNew).empty();

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

    // Prune per-row UI state for /me-motes that no longer exist, mirroring
    // OptionsEmotes' prune pass. Today the Delete button is the only removal
    // path and it erases both maps, but this keeps them robust against any
    // future bulk-removal path AND forces a fresh re-Seed after a reload that
    // repopulated g_MeMotes from disk (otherwise a surviving, initialized
    // buffer could commit stale text over the freshly-loaded value).
    {
        std::unordered_set<std::string> live(ids.begin(), ids.end());
        for (auto it = s_rowOpen.begin(); it != s_rowOpen.end(); ) {
            if (live.count(it->first)) ++it;
            else                       it = s_rowOpen.erase(it);
        }
        for (auto it = s_rowBufs.begin(); it != s_rowBufs.end(); ) {
            if (live.count(it->first)) ++it;
            else                       it = s_rowBufs.erase(it);
        }
        for (auto it = s_mmIconStatus.begin(); it != s_mmIconStatus.end(); ) {
            if (live.count(it->first)) ++it;
            else                       it = s_mmIconStatus.erase(it);
        }
    }

    // ---- List toolbar: count + Expand all / Collapse all -------------
    static int s_setAllOpen = 0;
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(L("opt.mm.count"), (int)ids.size());

        // Rescan / Expand all / Collapse all, right-aligned together (same as the
        // Emote tab). Rescan re-stats every row so a PNG dropped at icons/<id>.png
        // for a letter entry (which has no per-row Refresh) is picked up without a
        // tab reopen.
        const ImGuiStyle& st = ImGui::GetStyle();
        float wRescan   = ImGui::CalcTextSize(L("opt.icon.rescan")).x     + st.FramePadding.x * 2.f;
        float wExpand   = ImGui::CalcTextSize(L("opt.em.expand_all")).x   + st.FramePadding.x * 2.f;
        float wCollapse = ImGui::CalcTextSize(L("opt.em.collapse_all")).x + st.FramePadding.x * 2.f;
        float total     = wRescan + wExpand + wCollapse + st.ItemSpacing.x * 2.f;
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > total)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total));
        if (ImGui::SmallButton(L("opt.icon.rescan"))) {
            s_mmIconStatus.clear();   // every row re-stats its icon status next render
            MarkMeMotesDirty();       // every visible cell re-resolves (memo drops on the epoch)
            LOG_INFO("Rescan icons (/me-motes): cleared the icon-status cache + bumped the epoch");
        }
        if (ImGui::IsItemHovered())
            TooltipText("opt.icon.rescan_tooltip");
        ImGui::SameLine();
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
            bool keybindChanged = false;  // UserKeybind / variant toggle -> re-sync binds

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

            // ---- Icon (Library + Clear; no editable text field) ----
            // Matches OptionsEmotes' icon row: a disabled-text status
            // describing the resolved path, then Library + Clear on a
            // SameLine row. No InputText — the path is set exclusively by
            // the file picker or cleared to fall back to the icon chain.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(L("opt.mm.lbl_icon"));
            ImGui::TableSetColumnIndex(1);
            {
                // Status line — describes where the icon currently resolves
                // from. Order must match ResolveMeMoteIconSource (Icons.cpp);
                // if they drift, the label lies about what actually loads.
                // Ellipsized so a long path can't push the buttons off the
                // row.
                //
                // Cached per row (s_mmIconStatus) — ResolveMeMoteIconSource
                // does a GetFileAttributesA disk stat plus a bundled-table
                // scan, and the editor's OPEN-row count can grow once the
                // user authors a sizeable catalog. Recompute only when an
                // input that actually affects the resolved tier changed:
                // m.IconPath or the AI-fallback toggle. Mirrors the Emote
                // tab's IconStatusCache exactly. (A PNG dropped into
                // icons/<id>.png while the tab is open still needs a tab
                // reopen to surface — same expectation as the texture
                // cache's restart-to-apply note.)
                MmIconStatus& ics = s_mmIconStatus[id];
                if (!ics.valid || ics.iconPath != iconPathSnapshot ||
                    ics.aiFallback != g_Settings.UseAIIconFallback) {
                    ics.iconPath   = iconPathSnapshot;
                    ics.aiFallback = g_Settings.UseAIIconFallback;
                    MeMoteIconSource src = MeMoteIconSource::TextFallback;
                    {
                        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                        if (const MeMote* mm = FindMeMote(id))
                            src = ResolveMeMoteIconSource(*mm);
                    }
                    switch (src) {
                        case MeMoteIconSource::BundledChosen: {
                            // Icon picker: a "bundled:<bucket>:<name>" ref names
                            // a specific bundled icon (resolves regardless of
                            // UseAIIconFallback). Show the chosen name.
                            const BundledIcon* tbl = nullptr; int cnt = 0; std::string nm;
                            ParseBundledIconRef(iconPathSnapshot, tbl, cnt, nm);
                            ics.text = std::string(L("opt.mm.icon_bundled_chosen_prefix")) + nm;
                            break;
                        }
                        case MeMoteIconSource::Custom:
                            // IconPath set AND file exists at the resolved path.
                            ics.text = std::string(L("opt.mm.icon_custom_prefix")) +
                                       iconPathSnapshot;
                            break;
                        case MeMoteIconSource::FolderOverride:
                            // No IconPath, but icons/<id>.png exists on disk —
                            // the user dropped a PNG matching the stable Id
                            // and it auto-picks up without opening the picker.
                            ics.text = std::string(L("opt.mm.icon_folder_override_prefix")) +
                                       id + ".png";
                            break;
                        case MeMoteIconSource::BundledAI:
                            // No on-disk match but the bundled AI tier covers
                            // this Id — surface explicitly so the user knows
                            // why a cell has an icon they didn't set.
                            ics.text = std::string(L("opt.mm.icon_bundled_ai"));
                            break;
                        case MeMoteIconSource::TextFallback:
                            // Resolver returned TextFallback. That covers two
                            // shapes: (a) no IconPath, no folder file, no AI;
                            // (b) IconPath set but the file is MISSING (and
                            // no AI rescues it). Split for clearer user
                            // feedback — a missing custom path is almost
                            // always a typo.
                            ics.text = !iconPathSnapshot.empty()
                                         ? std::string(L("opt.mm.icon_custom_missing_prefix")) +
                                               iconPathSnapshot
                                         : std::string(L("opt.mm.icon_none"));
                            break;
                    }
                    ics.isDiskFile = (src == MeMoteIconSource::Custom ||
                                      src == MeMoteIconSource::FolderOverride);
                    ics.valid = true;
                }
                float availW = ImGui::GetContentRegionAvail().x;
                if (availW < 40.f) availW = 40.f;
                std::string fit = Ellipsize(ics.text, availW);
                ImGui::TextDisabled("%s", fit.c_str());
                if (ImGui::IsItemHovered() && !iconPathSnapshot.empty())
                    ImGui::SetTooltip("%s", iconPathSnapshot.c_str());

                // In-app icon picker (visual grid). The OS "From file..." dialog
                // was removed once the picker landed: it pulls from every bundled
                // bucket AND the user's icons/ folder, so a drop-in PNG is the
                // disk-side self-service path and the Library button covers the
                // in-app side. Hand-edit `me_motes.json` "icon" for an arbitrary
                // absolute path.
                if (ImGui::SmallButton((std::string(L("opt.pick.button")) + "##mmiconlib").c_str()))
                    OpenIconPicker(EIconTargetKind::MeMote, id, iconPathSnapshot);
                if (ImGui::IsItemHovered())
                    TooltipText("opt.pick.button_tooltip");
                // Refresh: only for a disk-file icon (bundled/letter can't change
                // on disk). Forces a full re-stat of this row - clears the icon
                // memo (cell reloads) AND this row's status cache (so the status
                // line + this button's own isDiskFile visibility re-resolve; both
                // are otherwise cached until IconPath/AI changes, which a bare file
                // edit/delete does NOT do). Replaced in place -> reloads (mtime/size
                // keyed, no churn when unchanged); DELETED -> cell resets to the
                // fallback, status flips to the resolved fallback, button hides.
                if (ics.isDiskFile) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton((std::string(L("opt.icon.refresh")) + "##mmiconrefresh").c_str())) {
                        InvalidateMeMoteIcon(id);    // cell re-resolves next render
                        ics.valid = false;           // status line + isDiskFile recompute next frame
                    }
                    if (ImGui::IsItemHovered())
                        TooltipText("opt.icon.refresh_tooltip");
                }
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
                             /*invalidTooltipKey=*/"opt.mm.text_default_empty",
                             /*charBudget=*/kMeMoteBodyCharBudget)) {
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
                             /*required=*/false,
                             /*invalidTooltipKey=*/nullptr,
                             /*charBudget=*/kMeMoteBodyCharBudget)) {
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
                             /*required=*/false,
                             /*invalidTooltipKey=*/nullptr,
                             /*charBudget=*/kMeMoteBodyCharBudget)) {
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

            // ---- Keybind (opt-in Nexus InputBinds — one per bound variant) ----
            // Each ticked variant registers its own Nexus InputBind, so a hotkey
            // or a RadialMenus wheel item can target that specific body. You/All
            // are only bindable when that body is set.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(L("opt.mm.lbl_keybind"));
            ImGui::TableSetColumnIndex(1);
            {
                bool kd = false, ky = false, ka = false, hasYou = false, hasAll = false;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) {
                        kd = m->KeybindDefault; ky = m->KeybindYou; ka = m->KeybindAll;
                        hasYou = !m->TextYou.empty(); hasAll = !m->TextAll.empty();
                    }
                }
                // One checkbox per variant. Disabled (greyed, still hoverable for
                // the tooltip) when that body is empty. The setter writes the
                // matching field under the mutex.
                auto variantCheckbox = [&](EMeMoteVariant which, const char* lblKey,
                                           bool cur, bool enabled) {
                    if (!enabled) {
                        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                            ImGui::GetStyle().Alpha * 0.5f);
                    }
                    bool v = enabled && cur;
                    if (ImGui::Checkbox(L(lblKey), &v) && enabled) {
                        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                        if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                            switch (which) {
                                case EMeMoteVariant::Default: m->KeybindDefault = v; break;
                                case EMeMoteVariant::You:     m->KeybindYou     = v; break;
                                case EMeMoteVariant::All:     m->KeybindAll     = v; break;
                            }
                            anyChanged = keybindChanged = true;
                            LOG_DEBUG("/me-mote keybind toggle: id=%s", id.c_str());
                        }
                    }
                    bool hov = ImGui::IsItemHovered();
                    if (!enabled) { ImGui::PopStyleVar(); ImGui::PopItemFlag(); }
                    if (hov) TooltipText("opt.mm.keybind_help");
                };
                variantCheckbox(EMeMoteVariant::Default, "opt.mm.variant_default", kd, true);
                ImGui::SameLine();
                variantCheckbox(EMeMoteVariant::You,     "opt.mm.variant_you",     ky, hasYou);
                ImGui::SameLine();
                variantCheckbox(EMeMoteVariant::All,     "opt.mm.variant_all",     ka, hasAll);
            }

            ImGui::EndTable();

            if (anyChanged) Persist();
            if (keybindChanged) SyncEmoteBinds();  // register/deregister the bind
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
        DeleteMeMote(deleteId);      // erase + favorites cascade + persist + dirty
        usage::RemoveRef(EFavoriteRefType::MeMote, deleteId);  // drop it from Recently/Frequently used
        s_rowOpen.erase(deleteId);   // UI-only: drop this row's expand + edit-buffer state
        s_rowBufs.erase(deleteId);
    }

    // ===== Bundled samples (uncommon: reseed) =====
    // Mirrors OptionsEmotes' "Bundled emotes" section structurally: language
    // combo above, Restore below. Idempotent by Id — never duplicates, never
    // overwrites a user-edited sample, never resurrects a deleted one if the
    // user hasn't touched g_MeMoteLanguage (existing samples stay; only the
    // language of NEWLY added entries is governed by the combo).
    OptionsSection(L("opt.mm.sec_restore"));
    ImGui::TextWrapped("%s", L("opt.mm.restore_explainer"));
    ImGui::Spacing();
    {
        std::vector<std::string> mmLangs = AvailableMeMoteLanguages();
        // g_MeMoteLanguage is clamped at every write to AvailableMeMoteLanguages(),
        // so this lookup almost always hits — but a manually-cleared me_motes.json
        // can leave g_MeMoteLanguage empty for a frame, and an upcoming bundle drop
        // could theoretically narrow the language set. Falling back to "en" keeps
        // the preview readable in either case.
        std::string current = g_MeMoteLanguage.empty() ? std::string("en")
                                                       : g_MeMoteLanguage;
        int curIdx = 0;
        for (size_t i = 0; i < mmLangs.size(); ++i)
            if (mmLangs[i] == current) { curIdx = (int)i; break; }

        ImGui::Text("%s", L("opt.mm.me_mote_language"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.f);
        std::string mmPreview = (curIdx < (int)mmLangs.size())
            ? std::string(L(("lang." + mmLangs[curIdx]).c_str()))
            : std::string("en");
        if (ImGui::BeginCombo("##memotelang", mmPreview.c_str())) {
            for (size_t i = 0; i < mmLangs.size(); ++i) {
                bool sel = ((int)i == curIdx);
                if (ImGui::Selectable(L(("lang." + mmLangs[i]).c_str()), sel) && !sel) {
                    // Preference only for NEW bundled /me-motes added by Restore
                    // below — does NOT relocalize existing /me-motes (same
                    // discipline as Options > Catalog's emote-language combo).
                    LOG_INFO("/me-mote language (for new bundled samples) -> %s",
                             mmLangs[i].c_str());
                    g_MeMoteLanguage = mmLangs[i];
                    RequestSave(SaveKind::MeMotes);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            TooltipText("opt.mm.me_mote_language_tooltip");

        ImGui::Spacing();
        if (ImGui::Button(L("opt.mm.restore_bundled"))) {
            int added = SeedBundledMeMotes(
                g_MeMoteLanguage.empty() ? std::string("en") : g_MeMoteLanguage);
            if (added > 0) Persist();
        }
        if (ImGui::IsItemHovered()) TooltipText("opt.mm.restore_bundled_tooltip");
    }
}
