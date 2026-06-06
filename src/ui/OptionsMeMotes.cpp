#include "OptionsMeMotes.h"

#include "Favorites.h"   // RemoveRefFromCategories on delete
#include "Globals.h"
#include "I18n.h"
#include "Logging.h"
#include "MeMotes.h"
#include "Settings.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>     // std::memcpy
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
//  Options > Text Emotes tab
//
//  Editor for the /me-mote catalog. One row per /me-mote, expandable to show
//  the editable fields: Name, Aliases, IconPath, TextDefault (required),
//  TextYou (optional), TextAll (optional). TextDefault is the body that
//  fires on left-click; You / All are the right-click variants (the cell-
//  level menu disables the entries when their bodies are empty).
//
//  Mutation discipline mirrors OptionsEmotes.cpp:
//    - All mutations happen under g_MeMotesMutex (consistent with the per-
//      frame renderer + the background load).
//    - Every commit calls MarkMeMotesDirty() so TextCache + Library + Quickbar
//      invalidate, and SaveMeMotesJson(g_MeMotesJsonPath) so the file heals.
//    - LOG_DEBUG on every mutation (add / rename / text edit / alias edit /
//      icon path edit / delete) — same diagnostic level as OptionsEmotes.
//
//  ID lifecycle: a new /me-mote is created with the stable Id
//  "me_mote_<N>" (smallest unused N). After save, the Id never changes —
//  even if the user renames the display Name — so favorites refs stay
//  pointing at the same entry across rename. Matches the official Emote.Id
//  invariant.
// ============================================================================

namespace {

// Edit buffers for an in-flight expanded row. ImGui::InputText writes into
// these char[]s; on commit we copy back into the MeMote struct. Mirrors
// OptionsEmotes' RowBuffer concept but with the /me-mote field set.
struct RowBuffer {
    char Name       [128] = {};
    char Aliases    [256] = {};
    char Icon       [260] = {};   // MAX_PATH-ish; user-typed path to a custom icon
    char TextDefault[512] = {};
    char TextYou    [512] = {};
    char TextAll    [512] = {};
    bool Initialized = false;
};

// Per-row buffer keyed on the /me-mote Id. Re-uses the buffer across frames
// so the InputText state survives ImGui re-renders without refilling from the
// MeMote struct every frame (which would wipe in-progress typing). Cleared
// when the row collapses or the /me-mote is deleted.
struct EditorState {
    std::string     OpenId;   // Id of the currently-expanded row, "" = none
    RowBuffer       Buf;
};
EditorState g_State;

// Copy a string into a fixed-size buffer, NUL-terminating + truncating.
void CopyTo(char* dst, size_t dstSize, const std::string& src) {
    if (!dstSize) return;
    size_t n = src.size();
    if (n >= dstSize) n = dstSize - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

// Reset the editor's in-flight buffer from the given /me-mote. Called when a
// row is opened, and when an open row's underlying entry was edited from
// elsewhere (rare; covers the safe path).
void SeedBuf(const MeMote& m) {
    CopyTo(g_State.Buf.Name,        sizeof(g_State.Buf.Name),        m.Name);
    {
        // Join aliases as comma-separated for the editor (matches the Emote
        // catalog convention).
        std::string joined;
        for (size_t i = 0; i < m.Aliases.size(); ++i) {
            if (i) joined += ", ";
            joined += m.Aliases[i];
        }
        CopyTo(g_State.Buf.Aliases, sizeof(g_State.Buf.Aliases), joined);
    }
    CopyTo(g_State.Buf.Icon,        sizeof(g_State.Buf.Icon),        m.IconPath);
    CopyTo(g_State.Buf.TextDefault, sizeof(g_State.Buf.TextDefault), m.TextDefault);
    CopyTo(g_State.Buf.TextYou,     sizeof(g_State.Buf.TextYou),     m.TextYou);
    CopyTo(g_State.Buf.TextAll,     sizeof(g_State.Buf.TextAll),     m.TextAll);
    g_State.Buf.Initialized = true;
}

// Pick the smallest unused "me_mote_<N>" id. Stable: callers can rely on this
// being the canonical placeholder.
std::string MakeFreshId() {
    int n = 1;
    while (true) {
        std::string candidate = "me_mote_" + std::to_string(n);
        if (!FindMeMote(candidate)) return candidate;
        ++n;
    }
}

// Parse a comma-separated alias list into individual trimmed words. Skips
// empties; dedupe preserves first-occurrence order.
std::vector<std::string> ParseAliases(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        // Trim
        auto isws = [](char c) { return c == ' ' || c == '\t'; };
        while (!cur.empty() && isws(cur.front())) cur.erase(cur.begin());
        while (!cur.empty() && isws(cur.back()))  cur.pop_back();
        if (!cur.empty() &&
            std::find(out.begin(), out.end(), cur) == out.end()) {
            out.push_back(cur);
        }
        cur.clear();
    };
    for (const char* p = s; *p; ++p) {
        if (*p == ',') flush();
        else           cur += *p;
    }
    flush();
    return out;
}

// Persist + invalidate after a mutation. MUST be called with g_MeMotesMutex
// NOT held — SaveMeMotesJson re-acquires the mutex internally and std::mutex
// is non-recursive (taking it twice = undefined behavior / crash). Every call
// site explicitly releases the mutex first.
void Persist() {
    MarkMeMotesDirty();
    if (!g_MeMotesJsonPath.empty()) SaveMeMotesJson(g_MeMotesJsonPath);
}

} // namespace

void RenderMeMotesTab() {
    // ---- Header help blurb -----------------------------------------------
    ImGui::TextWrapped("%s", L("opt.mm.hlp_top"));
    ImGui::Spacing();

    // ---- "Add text emote" button -----------------------------------------
    if (ImGui::Button(L("opt.mm.btn_add"))) {
        std::string newId;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            MeMote m;
            m.Id = MakeFreshId();
            newId = m.Id;
            g_MeMotes.push_back(std::move(m));
            // Seed the editor buffer while we still own the entry's storage.
            SeedBuf(g_MeMotes.back());
        }
        // Open the new row + persist outside the lock (Persist takes the mutex
        // via SaveMeMotesJson; std::mutex is non-recursive).
        g_State.OpenId = newId;
        LOG_DEBUG("/me-mote added (id=%s)", newId.c_str());
        Persist();
    }
    ImGui::Spacing();

    // ---- Per-row editor list ---------------------------------------------
    // Snapshot Ids under the mutex; the per-row block re-acquires the mutex
    // when it needs to mutate. Iterating over a snapshot avoids holding the
    // mutex across ImGui draws.
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

    std::string idToDelete;  // deferred; deleting mid-iteration would invalidate

    for (const std::string& id : ids) {
        // Snapshot the row's display label (Name, falls back to Id).
        std::string label;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            const MeMote* m = FindMeMote(id);
            if (!m) continue;                            // racily deleted
            label = m->Name.empty() ? id : m->Name;
        }
        // Unique header ID via the stable /me-mote Id.
        std::string headerLabel = label + "###mm_" + id;
        bool isOpen = (g_State.OpenId == id);
        ImGui::SetNextItemOpen(isOpen, ImGuiCond_Always);
        bool nowOpen = ImGui::CollapsingHeader(headerLabel.c_str());
        // Track the open/close transition: collapse drops the buffer so a
        // reopen seeds fresh from the struct.
        if (nowOpen && !isOpen) {
            g_State.OpenId = id;
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            if (const MeMote* m = FindMeMote(id)) SeedBuf(*m);
        } else if (!nowOpen && isOpen) {
            g_State.OpenId.clear();
            g_State.Buf.Initialized = false;
        }
        if (!nowOpen) continue;

        // ---- Expanded body: fields + delete -----------------------------
        ImGui::PushID(id.c_str());
        ImGui::Indent();

        // Name
        if (ImGui::InputText(L("opt.mm.lbl_name"),
                             g_State.Buf.Name, sizeof(g_State.Buf.Name),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                    std::string newName = g_State.Buf.Name;
                    if (newName != m->Name) {
                        LOG_DEBUG("/me-mote rename: id=%s, '%s' -> '%s'",
                                  id.c_str(), m->Name.c_str(), newName.c_str());
                        m->Name = std::move(newName);
                        changed = true;
                    }
                }
            }
            if (changed) Persist();
        }

        // Icon path
        if (ImGui::InputText(L("opt.mm.lbl_icon"),
                             g_State.Buf.Icon, sizeof(g_State.Buf.Icon),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                    std::string newIcon = g_State.Buf.Icon;
                    if (newIcon != m->IconPath) {
                        LOG_DEBUG("/me-mote icon edit: id=%s", id.c_str());
                        m->IconPath = std::move(newIcon);
                        changed = true;
                    }
                }
            }
            if (changed) Persist();
        }

        // Aliases (comma-separated)
        if (ImGui::InputText(L("opt.mm.lbl_aliases"),
                             g_State.Buf.Aliases, sizeof(g_State.Buf.Aliases),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                    std::vector<std::string> newAliases = ParseAliases(g_State.Buf.Aliases);
                    if (newAliases != m->Aliases) {
                        LOG_DEBUG("/me-mote aliases edit: id=%s (%d alias(es))",
                                  id.c_str(), (int)newAliases.size());
                        m->Aliases = std::move(newAliases);
                        changed = true;
                    }
                }
            }
            if (changed) Persist();
        }

        ImGui::Spacing();

        // TextDefault — REQUIRED. Red placeholder text when empty
        // signals validation; commit blocks unchanged content.
        bool defaultEmpty = (g_State.Buf.TextDefault[0] == 0);
        if (defaultEmpty) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.35f, 0.10f, 0.10f, 0.50f));
        }
        if (ImGui::InputTextMultiline(L("opt.mm.lbl_text_default"),
                                      g_State.Buf.TextDefault,
                                      sizeof(g_State.Buf.TextDefault),
                                      ImVec2(0, ImGui::GetTextLineHeight() * 3),
                                      ImGuiInputTextFlags_CtrlEnterForNewLine |
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                    std::string newText = g_State.Buf.TextDefault;
                    if (newText.empty()) {
                        LOG_DEBUG("/me-mote text default edit refused (empty): id=%s",
                                  id.c_str());
                        // Re-seed the buffer to restore the previous value.
                        CopyTo(g_State.Buf.TextDefault, sizeof(g_State.Buf.TextDefault),
                               m->TextDefault);
                    } else if (newText != m->TextDefault) {
                        LOG_DEBUG("/me-mote text default edit: id=%s", id.c_str());
                        m->TextDefault = std::move(newText);
                        changed = true;
                    }
                }
            }
            if (changed) Persist();
        }
        if (defaultEmpty) ImGui::PopStyleColor();
        if (defaultEmpty) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "%s", L("opt.mm.err_empty_default"));
        }

        // TextYou — optional
        if (ImGui::InputTextMultiline(L("opt.mm.lbl_text_you"),
                                      g_State.Buf.TextYou,
                                      sizeof(g_State.Buf.TextYou),
                                      ImVec2(0, ImGui::GetTextLineHeight() * 3),
                                      ImGuiInputTextFlags_CtrlEnterForNewLine |
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                    std::string newText = g_State.Buf.TextYou;
                    if (newText != m->TextYou) {
                        LOG_DEBUG("/me-mote text_you edit: id=%s", id.c_str());
                        m->TextYou = std::move(newText);
                        changed = true;
                    }
                }
            }
            if (changed) Persist();
        }

        // TextAll — optional
        if (ImGui::InputTextMultiline(L("opt.mm.lbl_text_all"),
                                      g_State.Buf.TextAll,
                                      sizeof(g_State.Buf.TextAll),
                                      ImVec2(0, ImGui::GetTextLineHeight() * 3),
                                      ImGuiInputTextFlags_CtrlEnterForNewLine |
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                if (MeMote* m = const_cast<MeMote*>(FindMeMote(id))) {
                    std::string newText = g_State.Buf.TextAll;
                    if (newText != m->TextAll) {
                        LOG_DEBUG("/me-mote text_all edit: id=%s", id.c_str());
                        m->TextAll = std::move(newText);
                        changed = true;
                    }
                }
            }
            if (changed) Persist();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("%s", L("opt.mm.hlp_variants"));

        ImGui::Spacing();

        // Delete button — confirmed via a popup so a misclick on a long
        // /me-mote can't wipe it.
        if (ImGui::Button(L("opt.mm.btn_delete"))) {
            ImGui::OpenPopup("##mm_delete_confirm");
        }
        if (ImGui::BeginPopup("##mm_delete_confirm")) {
            ImGui::Text("%s", L("opt.mm.confirm_delete"));
            ImGui::Separator();
            if (ImGui::Button(L("opt.mm.confirm_yes"))) {
                idToDelete = id;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("opt.mm.confirm_no"))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Unindent();
        ImGui::PopID();
    }

    // Deferred delete (outside the iteration to keep the snapshot consistent).
    if (!idToDelete.empty()) {
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            auto it = std::find_if(g_MeMotes.begin(), g_MeMotes.end(),
                                   [&](const MeMote& m) { return m.Id == idToDelete; });
            if (it != g_MeMotes.end()) {
                LOG_DEBUG("/me-mote deleted: id=%s (name='%s')",
                          idToDelete.c_str(), it->Name.c_str());
                g_MeMotes.erase(it);
            }
        }
        // Also evict any favorites refs that point at this /me-mote.
        // RemoveRefFromCategories handles its own save; chain with our own
        // save for the catalog file.
        RemoveRefFromCategories(EFavoriteRefType::MeMote, idToDelete);
        if (g_State.OpenId == idToDelete) {
            g_State.OpenId.clear();
            g_State.Buf.Initialized = false;
        }
        Persist();
    }
}
