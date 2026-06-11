#include "Palette.h"
#include "Globals.h"
#include "Logging.h"
#include "I18n.h"
#include "Settings.h"
#include "EmoteData.h"
#include "MeMotes.h"
#include "SearchMatch.h"   // MatchEmoteSearch / MatchMeMoteSearch (shared with the Library)
#include "StringUtil.h"    // ToLower / TrimWhitespace
#include "EmoteAction.h"   // SendOrFill* (returns sent-vs-refused)
#include "Usage.h"         // usage::Frequent (the zero-query list)
#include "Icons.h"         // EnsureEmoteTexture / EnsureMeMoteTexture (lazy cache)
#include "Layout.h"        // Ellipsize (side label cap)
#include "Feedback.h"      // surface tagging + the refusal overlay
#include "Profiling.h"     // dev perf overlay

#include "imgui/imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

const char* const PALETTE_WND_NAME = "emot3 Quick Send##pal";

namespace {

// ---- transient state (all reset by ResetPalette) ---------------------------
bool s_open      = false;
bool s_takeFocus = false;   // one-shot: focus the query field (open / after Enter-refusal)
char s_query[64] = {};      // same cap as the Library search box
int  s_sel       = 0;       // selected row index

// Display cap (g_Settings.PaletteMaxResults, clamped 5..15 at load). No
// scrolling on purpose: past the cap the palette answer is "type more", not
// "scroll" - a dim "+N more" line says the cap was hit.
int MaxRows() { return g_Settings.PaletteMaxResults; }

// One result row. COPIES of the catalog entries, not pointers: the list is
// built under the catalog mutexes but rendered (and sent) after they drop, so
// pointers into g_Emotes / g_MeMotes would dangle across an editor mutation.
// At most ~2x MaxRows() small structs per frame while open - trivial.
struct PalRow {
    bool   isMeMote = false;
    Emote  e;
    MeMote m;
    std::string aliasHit;  // set when matched ONLY via this alias (shown as the side label)
    bool   prefix = false; // ranking bucket: name/command starts with the query
};

bool StartsWith(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0;
}

bool IsManuallyUnlocked(const std::string& id) {
    return std::find(g_Settings.ManuallyUnlocked.begin(),
                     g_Settings.ManuallyUnlocked.end(), id)
        != g_Settings.ManuallyUnlocked.end();
}

// Zero-query rows: the frequently- or recently-used list (the user's
// PaletteEmptyQuery pick), resolved against the catalogs. The two mutexes are
// deliberately taken in sequence, never nested (the MainPanel build does the
// same) - resolution goes into order-preserving slots so the usage ranking
// survives the two passes.
void BuildUsageRows(std::vector<PalRow>& rows) {
    const bool recent = g_Settings.PaletteEmptyQuery == EPaletteEmptyQuery::Recent;
    const std::vector<FavoriteRef>& freq =
        recent ? usage::RecentlyUsed((size_t)MaxRows())
               : usage::Frequent((size_t)MaxRows());
    std::vector<PalRow> slots(freq.size());
    std::vector<char>   ok(freq.size(), 0);
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (size_t i = 0; i < freq.size(); ++i) {
            if (freq[i].Type != EFavoriteRefType::Emote) continue;
            const Emote* e = FindEmote(freq[i].Id);
            // Re-locked since it was recorded (user unmarked the unlock) ->
            // the game won't play it any more, keep it out of the send list.
            if (!e || !(e->IsCore || IsManuallyUnlocked(e->Id))) continue;
            slots[i].e = *e;
            ok[i] = 1;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        for (size_t i = 0; i < freq.size(); ++i) {
            if (freq[i].Type != EFavoriteRefType::MeMote) continue;
            for (const auto& m : g_MeMotes) {
                if (m.Id != freq[i].Id) continue;
                slots[i].isMeMote = true;
                slots[i].m = m;
                ok[i] = 1;
                break;
            }
        }
    }
    for (size_t i = 0; i < slots.size(); ++i)
        if (ok[i] && (int)rows.size() < MaxRows()) rows.push_back(std::move(slots[i]));
}

// Query rows: everything sendable that matches (locked emotes are excluded -
// the game won't play them - and the Library's class/section filters
// deliberately don't apply: this is a send surface, not a browse surface).
// Two bounded buckets (prefix hits first) instead of collect-all-then-sort,
// so a short query doesn't copy half the catalog per frame.
void BuildQueryRows(const std::string& needle, std::vector<PalRow>& rows,
                    int& totalMatches) {
    std::vector<PalRow> pre, rest;
    auto add = [&](PalRow&& r) {
        std::vector<PalRow>& dst = r.prefix ? pre : rest;
        if ((int)dst.size() < MaxRows()) dst.push_back(std::move(r));
    };
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        CatalogIndex idx;
        BuildCatalogIndex(g_Settings.ManuallyUnlocked, idx);
        for (const auto& e : g_Emotes) {
            if (!idx.unlocked(e)) continue;
            SearchHit h = MatchEmoteSearch(e, needle);
            if (!h.hit) continue;
            ++totalMatches;
            PalRow r;
            r.e = e;
            if (h.aliasOnly) r.aliasHit = *h.aliasOnly;
            const std::string ln = ToLower(e.Name), lc = ToLower(e.Command);
            r.prefix = StartsWith(ln, needle) || StartsWith(lc, needle) ||
                       (!lc.empty() && lc[0] == '/' && StartsWith(lc.substr(1), needle));
            add(std::move(r));
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        for (const auto& m : g_MeMotes) {
            SearchHit h = MatchMeMoteSearch(m, needle);
            if (!h.hit) continue;
            ++totalMatches;
            PalRow r;
            r.isMeMote = true;
            r.m = m;
            if (h.aliasOnly) r.aliasHit = *h.aliasOnly;
            r.prefix = StartsWith(ToLower(m.Name), needle);
            add(std::move(r));
        }
    }
    rows = std::move(pre);
    for (auto& r : rest) {
        if ((int)rows.size() >= MaxRows()) break;
        rows.push_back(std::move(r));
    }
}

// Up/Down move the selection while the query field keeps focus (the console
// idiom - the user never leaves the text field). Clamped against the row
// count in the render (the list isn't built yet when this runs).
int QueryEditCb(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if      (data->EventKey == ImGuiKey_UpArrow)   --s_sel;
        else if (data->EventKey == ImGuiKey_DownArrow) ++s_sel;
    }
    return 0;
}

}  // namespace

void TogglePalette() {
    s_open = !s_open;
    if (s_open) {
        s_takeFocus = true;
        s_sel = 0;
        if (g_Settings.PaletteClearOnOpen) s_query[0] = '\0';
    }
    LOG_DEBUG("Keybind: quick-send palette %s", s_open ? "opened" : "closed");
}

void ResetPalette() {
    s_open      = false;
    s_takeFocus = false;
    s_sel       = 0;
    s_query[0]  = '\0';
}

void PaletteRender() {
    if (!s_open) return;
    // Same per-frame suppressions as the Library, but a transient popup just
    // CLOSES on them (loading screen / character select / fullscreen map)
    // instead of waiting them out.
    if (!NexusLink || !NexusLink->IsGameplay ||
        (MumbleLink && MumbleLink->Context.IsMapOpen)) {
        s_open = false;
        return;
    }
    PROFILE_SCOPE("pal.frame");  // dev perf overlay
    SetActiveFeedbackSurface(FeedbackSurface::Palette);

    // Centered, upper third, fixed width, auto height. Appearing-cond
    // re-centers on every open; NoMove keeps the spot predictable (there's
    // no title bar, so a background-drag would move it by accident).
    ImGuiIO& io = ImGui::GetIO();
    const float kW = 460.f * g_Settings.PaletteScale;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.28f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(kW, 0.f), ImVec2(kW, FLT_MAX));
    if (s_takeFocus) ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin(PALETTE_WND_NAME, nullptr, flags)) { ImGui::End(); return; }

    // Esc closes in ONE press. Handled here, not via Nexus' CloseOnEscape:
    // an active InputText eats the first Esc to deactivate-and-REVERT the
    // buffer (the query visibly jumped back to its pre-edit text) and only a
    // second Esc reached the hook. Reading the key directly and closing this
    // frame means the revert never renders.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape), false)) {
        s_open = false;
        ImGui::End();
        return;
    }

    // Spotlight semantics: losing window focus in any way (clicking the game
    // world, another window, alt-tab) closes the palette - it's a transient
    // prompt, not a panel to park. s_takeFocus graces the frames where focus
    // was just requested but may not have applied yet.
    if (!s_takeFocus &&
        !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        s_open = false;
        ImGui::End();
        return;
    }

    // Query field. EnterReturnsTrue -> send the selection. AutoSelectAll: a
    // reopen restores the last query selected, so typing replaces it.
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (s_takeFocus) { ImGui::SetKeyboardFocusHere(); s_takeFocus = false; }
    const bool enter = ImGui::InputTextWithHint(
        "##palquery", L("pal.hint"), s_query, sizeof(s_query),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_AutoSelectAll, QueryEditCb);

    // Build this frame's rows. Filtering starts from the FIRST character
    // (unlike the Library's 2-char rule): the palette list is capped + ranked,
    // so a 1-char query is already useful and the early feedback matters more
    // than scan cost (the catalog is small).
    const std::string needle = ToLower(TrimWhitespace(s_query));
    std::vector<PalRow> rows;
    int total = 0;
    if (needle.empty()) {
        if (g_Settings.PaletteEmptyQuery != EPaletteEmptyQuery::Off)
            BuildUsageRows(rows);
        total = (int)rows.size();
    } else {
        BuildQueryRows(needle, rows, total);
    }

    if (rows.empty()) s_sel = 0;
    else if (s_sel < 0) s_sel = 0;
    else if (s_sel >= (int)rows.size()) s_sel = (int)rows.size() - 1;

    ImGui::Spacing();

    int activate = -1;
    float iconSz = (ImGui::GetFontSize() + 6.f) * g_Settings.PaletteScale;
    if (iconSz < ImGui::GetTextLineHeight()) iconSz = ImGui::GetTextLineHeight();
    // Only let a hover steal the selection when the mouse actually moved -
    // otherwise the row under a parked cursor wins every Up/Down keypress.
    const bool mouseMoved = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    for (int i = 0; i < (int)rows.size(); ++i) {
        const PalRow& r = rows[i];
        ImGui::PushID(i);

        Texture* t = r.isMeMote ? EnsureMeMoteTexture(r.m) : EnsureEmoteTexture(r.e);
        if (t && t->Resource)
            ImGui::Image((ImTextureID)t->Resource, ImVec2(iconSz, iconSz));
        else
            ImGui::Dummy(ImVec2(iconSz, iconSz));  // async load: keep the column stable
        ImGui::SameLine();

        // Side label (dim, right-aligned): the matched alias when the hit came
        // only via an alias (it shows nowhere else - the confusing case), else
        // the command. The Selectable spans the FULL row (hover/selection
        // highlight covers the side label too); the side text is then painted
        // on top via the draw list, and the name is clipped so it can't run
        // underneath it.
        std::string side = !r.aliasHit.empty() ? r.aliasHit
                         : (r.isMeMote ? std::string("/me") : r.e.Command);
        side = Ellipsize(side, kW * 0.35f);
        const float sideW = ImGui::CalcTextSize(side.c_str()).x;
        const float fullW = ImGui::GetContentRegionAvail().x;
        const float pad   = ImGui::GetStyle().ItemSpacing.x;
        float nameW = fullW - sideW - 2.f * pad;
        if (nameW < 40.f) nameW = 40.f;
        std::string name = Ellipsize(r.isMeMote ? r.m.Name : r.e.Name, nameW);
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        // "###" id: the visible text is user-authored (could contain "##"),
        // so keep it out of the ImGui ID. Width 0 = span the row.
        if (ImGui::Selectable((name + "###row").c_str(), i == s_sel, 0,
                              ImVec2(0.f, iconSz)))
            activate = i;
        if (mouseMoved && ImGui::IsItemHovered()) s_sel = i;
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rowPos.x + fullW - sideW - pad,
                   rowPos.y + (iconSz - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), side.c_str());
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    if (rows.empty()) {
        if (!needle.empty())
            ImGui::TextDisabled("%s", L("pal.no_match"));
        else if (g_Settings.PaletteEmptyQuery != EPaletteEmptyQuery::Off)
            ImGui::TextDisabled("%s", L("pal.empty_usage"));  // suggestions on, none yet
    } else if (total > (int)rows.size()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), L("pal.more"), total - (int)rows.size());
        ImGui::TextDisabled("%s", buf);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", L("pal.footer"));

    if (enter && !rows.empty()) activate = s_sel;
    if (activate >= 0 && activate < (int)rows.size()) {
        const PalRow& r = rows[activate];
        // Left-click Library semantics: targetable honors the user's
        // send-on-target setting, never sync'd. The full gate applies; on a
        // refusal the palette stays open (the overlay names the reason) and
        // the query field re-takes focus so the typing flow survives Enter.
        const bool sent = r.isMeMote
            ? SendOrFillMeMote(r.m, EMeMoteVariant::Default)
            : SendOrFillEmote(r.e, /*useTarget=*/g_Settings.SendTargetableOnTarget,
                              /*useSync=*/false);
        if (sent) s_open = false;
        else if (enter) s_takeFocus = true;
    }

    // Refusal overlay on THIS surface (the palette is open + focused, so the
    // in-window sink is right - SendAlert is for surfaces that may be closed).
    DrawFeedbackOverlay(FeedbackSurface::Palette, /*highContrast=*/false,
                        ImGui::GetWindowDrawList());
    ImGui::End();
}
