#include "OptionsMeMotes.h"

#include "Favorites.h"   // RemoveRefFromCategories on delete
#include "Globals.h"
#include "I18n.h"        // L(), TooltipText
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
#include <cstring>       // strncpy_s
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
//  Options > /me-motes tab
//
//  Editor for the /me-mote catalog (see data/MeMotes.h). Mirrors the
//  OptionsEmotes pattern row-by-row — same row header (ArrowButton + colored
//  Id + right-aligned destructive Delete), same two-column field layout
//  (fixed-width label column + stretch field column), same auto-commit on
//  focus loss, same InputTextWithHint placeholders, same PushInvalidInputStyle
//  + DrawInvalidInputBorder helpers for required fields — so the two editors
//  read identically to users who've used both.
//
//  All fields are single-line InputText (not InputTextMultiline). That
//  matches the "short text input like the chat frame" mental model and
//  keeps long bodies horizontally scrolling within their column instead of
//  wrapping into a tall textarea.
//
//  Mutation discipline mirrors OptionsEmotes.cpp:
//    - All mutations under g_MeMotesMutex.
//    - Persist() (MarkMeMotesDirty + SaveMeMotesJson) MUST be called outside
//      the mutex — std::mutex is non-recursive and SaveMeMotesJson re-acquires
//      it internally. Every callsite scopes its lock and releases before
//      Persist().
//    - LOG_DEBUG on every kind of mutation (add / rename / icon / aliases /
//      text default/you/all / delete).
//
//  ID lifecycle: a new /me-mote is created with "me_mote_<N>" (smallest unused
//  N). After save, the Id never changes — even on rename — so favorites refs
//  stay pointing at the same entry across rename. Matches the official
//  Emote.Id invariant.
// ============================================================================

namespace {

// Per-row edit buffer. Lives outside the InputText calls so click-away
// reliably commits (no fragile IsItemDeactivatedAfterEdit dependency on a
// stable user buffer). Mirrors OptionsEmotes::RowBuffer.
struct RowBuffer {
    std::string name;
    std::string icon;
    std::string aliases;
    std::string textDefault;
    std::string textYou;
    std::string textAll;
    bool        initialized = false;
};

// Per-row state, keyed on the stable /me-mote Id. Entries persist across
// frames (and across collapses) so the InputText state for a row that the
// user collapsed and re-expanded is restored. Matches OptionsEmotes's
// s_rowOpen / s_rowBufs maps.
std::unordered_map<std::string, bool>       s_rowOpen;
std::unordered_map<std::string, RowBuffer>  s_rowBufs;

// Seed the per-row buffer from the live MeMote (first time the row renders).
void Seed(RowBuffer& rb, const MeMote& m) {
    rb.name        = m.Name;
    rb.icon        = m.IconPath;
    rb.aliases.clear();
    for (size_t i = 0; i < m.Aliases.size(); ++i) {
        if (i) rb.aliases += ", ";
        rb.aliases += m.Aliases[i];
    }
    rb.textDefault = m.TextDefault;
    rb.textYou     = m.TextYou;
    rb.textAll     = m.TextAll;
    rb.initialized = true;
}

// Trim ASCII whitespace — single-line inputs may carry surrounding spaces
// from copy-paste.
std::string Trim(std::string s) {
    auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && isws(s.front())) s.erase(s.begin());
    while (!s.empty() && isws(s.back()))  s.pop_back();
    return s;
}

// Parse comma-separated aliases into a vector, trimmed + deduped (first-wins).
std::vector<std::string> ParseAliases(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        cur = Trim(cur);
        if (!cur.empty() &&
            std::find(out.begin(), out.end(), cur) == out.end()) {
            out.push_back(cur);
        }
        cur.clear();
    };
    for (char c : s) {
        if (c == ',') flush();
        else          cur += c;
    }
    flush();
    return out;
}

// Smallest unused "me_mote_<N>" id. Stable across frames so callers can rely
// on it being the canonical placeholder for new entries.
std::string MakeFreshId() {
    int n = 1;
    while (true) {
        std::string candidate = "me_mote_" + std::to_string(n);
        if (!FindMeMote(candidate)) return candidate;
        ++n;
    }
}

// Persist + invalidate after a mutation. MUST be called with g_MeMotesMutex
// NOT held — SaveMeMotesJson re-acquires the mutex internally and std::mutex
// is non-recursive. Every call site scopes its lock and releases before
// Persist().
void Persist() {
    MarkMeMotesDirty();
    if (!g_MeMotesJsonPath.empty()) SaveMeMotesJson(g_MeMotesJsonPath);
}

// Inline field helper: render a labeled single-line InputText that
// auto-commits on focus loss when its buffer differs from the stored value.
// The label column is `labelKey` (i18n); the field column stretches via
// SetNextItemWidth(-FLT_MIN). validate=true paints the invalid-input style
// when the trimmed buffer is empty; `committed` returns true when this
// frame's defocus committed a change.
//
// Returns true iff a commit fired (caller persists outside the lock).
bool FieldRow(const char* labelKey, const char* hintKey, const char* idSuffix,
              std::string& buf, std::string& stored, bool required,
              size_t bufSize = 256, const char* tooltipKey = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L(labelKey));
    ImGui::TableSetColumnIndex(1);

    std::string trimmed = Trim(buf);
    bool dirty   = (trimmed != stored);
    bool invalid = required && trimmed.empty();

    // Stack-allocated scratch buffer for InputText. 1024 covers the longest
    // expected /me-mote body line; longer pastes are silently truncated (same
    // as OptionsEmotes for its aliases field). The buf string mirrors what
    // ImGui has, so a focus-loss commit always sees the latest text.
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
    if (tooltipKey && ImGui::IsItemHovered()) TooltipText(tooltipKey);

    // Auto-commit pattern: dirty + not actively being edited + not currently
    // invalid (for required fields). The caller persists outside the lock.
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
    // ---- Header --------------------------------------------------------
    ImGui::TextWrapped("%s", L("opt.mm.hlp_top"));
    ImGui::Spacing();

    // ---- "Add /me-mote" button -----------------------------------------
    if (ImGui::Button(L("opt.mm.btn_add"))) {
        std::string newId;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            MeMote m;
            m.Id = MakeFreshId();
            newId = m.Id;
            g_MeMotes.push_back(std::move(m));
        }
        s_rowOpen[newId] = true;  // open the new row for immediate editing
        LOG_DEBUG("/me-mote added (id=%s)", newId.c_str());
        Persist();  // outside the lock
    }
    ImGui::Spacing();

    // Snapshot Ids under the mutex so the per-row work doesn't hold it across
    // ImGui draws.
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        ids.reserve(g_MeMotes.size());
        for (const auto& m : g_MeMotes) ids.push_back(m.Id);
    }
    if (ids.empty()) {
        ImGui::TextDisabled("%s", L("opt.mm.empty"));
        return;
    }

    // Width of the shared label column — max of every field-label width so
    // inputs across rows align. Mirrors OptionsEmotes's labelColW pre-calc.
    float labelColW = 0.f;
    for (const char* k : { "opt.mm.lbl_name", "opt.mm.lbl_icon",
                           "opt.mm.lbl_aliases", "opt.mm.lbl_text_default",
                           "opt.mm.lbl_text_you", "opt.mm.lbl_text_all" })
        labelColW = std::max(labelColW, ImGui::CalcTextSize(L(k)).x);
    labelColW += ImGui::GetStyle().FramePadding.x * 2.f;

    // Gold colour for the read-only Id text in the row header — visually
    // matches OptionsEmotes::kCmdColor exactly so the two editors read as
    // siblings.
    const ImVec4 kIdColor(0.92f, 0.78f, 0.32f, 1.f);

    std::string deleteId;   // deferred — set inside the loop, applied after

    for (const std::string& id : ids) {
        // Snapshot the live row's display label (Name; falls back to Id).
        // The body block re-takes the mutex when it mutates.
        bool exists = false;
        std::string displayName;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            if (const MeMote* m = FindMeMote(id)) {
                exists = true;
                displayName = m->Name.empty() ? id : m->Name;
            }
        }
        if (!exists) continue;  // racily removed

        ImGui::PushID(id.c_str());

        // ---- Row header: ArrowButton + colored Id + Delete -------------
        bool& rowOpen = s_rowOpen[id];
        if (ImGui::ArrowButton("##expand", rowOpen ? ImGuiDir_Down : ImGuiDir_Right))
            rowOpen = !rowOpen;
        ImGui::SameLine();
        ImGui::TextColored(kIdColor, "%s", id.c_str());
        if (ImGui::IsItemHovered())
            TooltipText("opt.mm.id_tooltip");

        // Name preview to the right of the Id (helps scan the list when
        // collapsed). Skipped when displayName == id (would just duplicate).
        if (displayName != id) {
            ImGui::SameLine();
            ImGui::TextDisabled("— %s", displayName.c_str());
        }

        // Right-align destructive Delete. Mirrors OptionsEmotes layout dance.
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
            ImGui::PopID();
            continue;
        }

        // ---- Body — two-column table, single-line InputTexts -----------
        ImGui::Indent(10.f);
        if (ImGui::BeginTable("##mmfields", 2,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                                    labelColW);
            ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);

            // First-time buffer seed.
            RowBuffer& rb = s_rowBufs[id];
            if (!rb.initialized) {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (const MeMote* m = FindMeMote(id)) Seed(rb, *m);
            }

            // Collect everything that committed this frame; persist once at
            // the end so multiple-field defocus in one frame is a single save.
            bool anyChanged = false;

            // ---- Name (free-form display label) ----
            {
                std::string stored;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) stored = m->Name;
                }
                std::string newVal = stored;
                if (FieldRow("opt.mm.lbl_name", "opt.mm.name_hint", "mm_name",
                             rb.name, newVal, /*required=*/false)) {
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

            // ---- Icon path (optional, free-form) ----
            {
                std::string stored;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) stored = m->IconPath;
                }
                std::string newVal = stored;
                if (FieldRow("opt.mm.lbl_icon", "opt.mm.icon_hint", "mm_icon",
                             rb.icon, newVal, /*required=*/false)) {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (m->IconPath != newVal) {
                            LOG_DEBUG("/me-mote icon edit: id=%s", id.c_str());
                            m->IconPath = newVal;
                            anyChanged = true;
                        }
                    }
                }
            }

            // ---- Aliases (comma-separated; the rb buffer carries the
            //               joined form, the stored value is rejoined for
            //               diffing). ----
            {
                std::string storedJoined;
                std::vector<std::string> storedAliases;
                {
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (const MeMote* m = FindMeMote(id)) {
                        storedAliases = m->Aliases;
                        for (size_t i = 0; i < storedAliases.size(); ++i) {
                            if (i) storedJoined += ", ";
                            storedJoined += storedAliases[i];
                        }
                    }
                }
                std::string newVal = storedJoined;
                if (FieldRow("opt.mm.lbl_aliases", "opt.mm.aliases_hint",
                             "mm_aliases", rb.aliases, newVal, /*required=*/false)) {
                    std::vector<std::string> newAliases = ParseAliases(rb.aliases);
                    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                    if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                        if (m->Aliases != newAliases) {
                            LOG_DEBUG("/me-mote aliases edit: id=%s (%d alias(es))",
                                      id.c_str(), (int)newAliases.size());
                            m->Aliases = std::move(newAliases);
                            anyChanged = true;
                        }
                    }
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
                             /*required=*/true, /*bufSize=*/1024)) {
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
                             /*required=*/false, /*bufSize=*/1024)) {
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
                             /*required=*/false, /*bufSize=*/1024)) {
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

            if (anyChanged) Persist();    // outside any lock above
        }
        ImGui::Unindent(10.f);

        // Help blurb for the variants (small disabled text under the table).
        ImGui::TextDisabled("%s", L("opt.mm.hlp_variants"));
        ImGui::Spacing();

        ImGui::PopID();
    }

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
        if (!nameForLog.empty() || true) {
            LOG_DEBUG("/me-mote deleted: id=%s (name='%s')",
                      deleteId.c_str(), nameForLog.c_str());
        }
        RemoveRefFromCategories(EFavoriteRefType::MeMote, deleteId);
        s_rowOpen.erase(deleteId);
        s_rowBufs.erase(deleteId);
        Persist();
    }
}
