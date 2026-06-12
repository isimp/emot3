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
#include "Cells.h"         // RenderSendVariants (shared right-click menu body)
#include "Layout.h"        // Ellipsize (side label cap)
#include "Feedback.h"      // surface tagging + the refusal overlay
#include "Profiling.h"     // dev perf overlay

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // BringWindowToDisplayFront (always-on-top)

#include <Windows.h>  // GetTickCount64 (same-click toggle suppression)

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

const char* const PALETTE_WND_NAME = "emot3 Palette##pal";

namespace {

// ---- transient state (all reset by ResetPalette) ---------------------------
std::atomic<bool> s_open{false};  // atomic: read by the WndProc thread below
bool s_takeFocus = false;   // one-shot: focus the query field (open / after Enter-refusal)
char s_query[64] = {};      // same cap as the Library search box
int  s_sel       = 0;       // selected row index
// Send-variant cycling (Tab / Shift+Tab): index into the SELECTED row's
// variant list - 0 is the default Enter/click send, the rest mirror the
// row's context-menu items exactly (see BuildVariantList). Transient on
// purpose: reset on open, on any selection change or query edit, and
// clamped against the current row's actual list every frame.
int  s_varIdx    = 0;
int  s_varSel    = -1;      // the selection s_varIdx belongs to (change detector)
// True while a row's right-click menu was open last frame. The close / focus /
// Esc / click-away checks and the focus pin all stand down while it's set: the
// popup IS the focused window, its clicks land outside the palette rect, and a
// re-pin of the query field would kill it. Render-thread only.
bool s_ctxOpen   = false;

// Swallow the click that OPENED the palette: when the open came from a mouse
// press (the Nexus icon's left-click, a mouse-button keybind), that press is
// still in flight when the first frame renders - and the top suggestion row
// (by definition the emote just sent) can spawn under the cursor, so the
// release would "click" it and instantly re-send it. Mouse activations are
// ignored until every button has been up once since opening. The WndProc
// click-away has the same grace via s_rectValid; this is the ImGui-side twin.
bool s_mouseGuard = false;

// Enter twin of the mouse guard: EnterReturnsTrue only counts once Enter has
// been seen UP at least once since opening. Shields against a WEDGED Enter in
// ImGui's io - anything in the WndProc dispatch chain that eats a key-UP
// (our +plus injection swallow did, now fixed; another addon still could)
// leaves io.KeysDown stuck, and ImGui's key-repeat then auto-fires the send
// the moment the query field activates (the machine-gun resend bug).
bool s_enterGuard = false;

// Options-preview pulses (see Palette.h). Frame stamps with a 1-frame
// tolerance: the Options panel renders in a different callback phase than
// the palette, so same-frame vs next-frame ordering must both count.
int s_ghostFrame     = -1000;
int s_keepAliveFrame = -1000;

// ---- click-away detection (WndProc-fed) -------------------------------------
// Clicking the game world must close the palette, but those clicks are
// invisible to ImGui's focus bookkeeping under Nexus (and with the query field
// active, io.WantCaptureMouse stays true regardless of where the mouse is, so
// an io-side check can't see them either). entry.cpp's WndProc forwards every
// mouse-down to PaletteNoteMouseDown (observe-only, never consumed); the point
// is hit-tested against the window rect the render thread published last
// frame. Outside = close request, honored at the top of the next render.
// s_rectValid stays false until the first frame after opening, so a
// mouse-button-bound open click can't land on a stale rect and re-close it.
std::atomic<bool> s_closeRequest{false};
std::atomic<bool> s_rectValid{false};
std::atomic<int>  s_rectX{0}, s_rectY{0}, s_rectW{0}, s_rectH{0};

// Same-click toggle suppression. Clicking the Nexus quick-access icon while
// the palette is open races two correct behaviors: the mouse-DOWN lands
// outside the palette (click-away / focus-loss closes it), then Nexus invokes
// the toggle bind on the mouse-RELEASE - which now sees "closed" and REOPENS.
// So a close caused by an in-flight click ARMS suppression; the matching
// button release converts it to a short grace window; a toggle-OPEN arriving
// armed or inside the window is the same physical click and is swallowed
// once. Keyboard-caused closes (Esc, toggle, send) never arm, so keybind
// reopens are never delayed.
std::atomic<bool>     s_closedByClick{false};
std::atomic<uint64_t> s_suppressOpenUntil{0};

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
    std::string catHit;    // set when surfaced via a matching favorites CATEGORY name
                           // (shown gold in the side label - see the render loop)
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

    // Category pass: a query that matches a favorites CATEGORY name surfaces
    // that category's members - the user's own grouping doubles as a search
    // alias on everything in it (typing "soc" lists all of "Social"). Members
    // append AFTER the direct buckets in the category's user-arranged order,
    // never displacing a direct hit; an entry already shown stays shown once
    // (its direct match wins the side label), and an entry in two matching
    // categories shows the FIRST one. Same exclusions as above: locked
    // emotes don't surface.
    {
        // Collect (category, ref) pairs first; resolve below under the
        // catalog mutexes (sequential, never nested - BuildUsageRows idiom).
        struct CatRef { const std::string* cat; FavoriteRef ref; };
        std::vector<CatRef> catRefs;
        auto shown = [&](EFavoriteRefType t, const std::string& id) {
            for (const auto& r : rows) {
                if (r.isMeMote != (t == EFavoriteRefType::MeMote)) continue;
                if ((r.isMeMote ? r.m.Id : r.e.Id) == id) return true;
            }
            for (const auto& cr : catRefs)
                if (cr.ref.Type == t && cr.ref.Id == id) return true;
            return false;
        };
        for (const auto& cat : g_Settings.FavoriteCategories) {
            if (ToLower(cat.Name).find(needle) == std::string::npos) continue;
            for (const auto& ref : cat.Refs)
                if (!shown(ref.Type, ref.Id))
                    catRefs.push_back({ &cat.Name, ref });
        }
        if (catRefs.empty()) return;

        std::vector<PalRow> slots(catRefs.size());
        std::vector<char>   ok(catRefs.size(), 0);
        {
            std::lock_guard<std::mutex> lk(g_EmotesMutex);
            CatalogIndex idx;
            BuildCatalogIndex(g_Settings.ManuallyUnlocked, idx);
            for (size_t i = 0; i < catRefs.size(); ++i) {
                if (catRefs[i].ref.Type != EFavoriteRefType::Emote) continue;
                const Emote* e = FindEmote(catRefs[i].ref.Id);
                if (!e || !idx.unlocked(*e)) continue;
                slots[i].e = *e;
                ok[i] = 1;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            for (size_t i = 0; i < catRefs.size(); ++i) {
                if (catRefs[i].ref.Type != EFavoriteRefType::MeMote) continue;
                const MeMote* m = FindMeMote(catRefs[i].ref.Id);
                if (!m) continue;
                slots[i].isMeMote = true;
                slots[i].m = *m;
                ok[i] = 1;
            }
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!ok[i]) continue;
            ++totalMatches;  // counts toward "+N more" like any other match
            if ((int)rows.size() >= MaxRows()) continue;
            slots[i].catHit = *catRefs[i].cat;
            rows.push_back(std::move(slots[i]));
        }
    }
}

// /me-mote send-variant menu items (You / All; Default is the left-click /
// Enter default, omitted exactly like the Library cell menu). Items render
// disabled-but-visible when their body is empty - discoverable, matching
// Cells.cpp's sendVariantItem. Returns true when a variant actually sent.
bool MeMoteVariantItems(const MeMote& m) {
    bool sent = false;
    auto item = [&](EMeMoteVariant v, const char* key, bool enabled) {
        if (!enabled) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        }
        if (ImGui::MenuItem(L(key)) && enabled)
            sent = SendOrFillMeMote(m, v) || sent;
        if (!enabled) {
            ImGui::PopStyleVar();
            ImGui::PopItemFlag();
        }
    };
    item(EMeMoteVariant::You, "cells.send_you", !m.TextYou.empty());
    item(EMeMoteVariant::All, "cells.send_all", !m.TextAll.empty());
    return sent;
}

// The selected row's send-variant list for Tab cycling. Entry 0 is the
// default Enter/click send; the rest mirror the row's context menu exactly -
// same availability conditions, same cells.* labels (RenderSendVariants /
// MeMoteVariantItems are menu bodies, so the cycle keeps its own parallel
// table; the conditions are three lines each and covered by the same
// review). At most 4 entries (default + normal-or-target + sync +
// target-sync) / 3 for a /me-mote (default + You + All).
struct PalVariant {
    const char*    labelKey;   // cells.send_* (nullptr for the default entry)
    bool           useTarget;  // SendOrFillEmote args (emote rows)
    bool           useSync;
    EMeMoteVariant mv;         // SendOrFillMeMote arg (/me-mote rows)
};
int BuildVariantList(const PalRow& r, PalVariant out[4]) {
    int n = 0;
    out[n++] = { nullptr, false, false, EMeMoteVariant::Default };
    if (r.isMeMote) {
        if (!r.m.TextYou.empty())
            out[n++] = { "cells.send_you", false, false, EMeMoteVariant::You };
        if (!r.m.TextAll.empty())
            out[n++] = { "cells.send_all", false, false, EMeMoteVariant::All };
    } else {
        // Same fork as RenderSendVariants: when the default send already
        // targets (SendTargetableOnTarget), the alternative is the UNtargeted
        // send; otherwise it's the targeted one (if this emote can).
        const bool autoTarget = r.e.IsTargetable && g_Settings.SendTargetableOnTarget;
        if (autoTarget)
            out[n++] = { "cells.send_normal", false, false, EMeMoteVariant::Default };
        else if (r.e.IsTargetable)
            out[n++] = { "cells.send_target", true, false, EMeMoteVariant::Default };
        out[n++] = { "cells.send_sync", false, true, EMeMoteVariant::Default };
        if (r.e.IsTargetable)
            out[n++] = { "cells.send_target_sync", true, true, EMeMoteVariant::Default };
    }
    return n;
}

// Up/Down move the selection while the query field keeps focus (the console
// idiom - the user never leaves the text field). Clamped against the row
// count in the render (the list isn't built yet when this runs). Grow-up
// draws the list reversed (higher index = visually higher), so the arrows
// invert with it: Up always moves AWAY from the query field.
int QueryEditCb(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const int dir = g_Settings.PaletteGrowUp ? 1 : -1;
        if      (data->EventKey == ImGuiKey_UpArrow)   s_sel += dir;
        else if (data->EventKey == ImGuiKey_DownArrow) s_sel -= dir;
    }
    return 0;
}

}  // namespace

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
// Runtime state inspector section: the palette's transient state machine -
// the open flag, the guard set, and the preview pulses. The guards are the
// subtlest part of the feature (each shipped because of a real in-game bug),
// so a "why didn't my click/Enter register?" report is answerable by reading
// this live instead of re-deriving the state from behavior.
static DevStateRegistrar s_palState(DevStateCat::Content, "Palette", [] {
    DevStateRow("open",         "%s", s_open.load(std::memory_order_relaxed) ? "yes" : "no");
    DevStateRow("query",        "\"%s\"", s_query);
    DevStateRow("selected row", "%d", s_sel);
    DevStateRow("tab variant",  "idx %d (of row %d; 0 = default send)", s_varIdx, s_varSel);
    DevStateRow("context menu", "%s", s_ctxOpen ? "open" : "closed");
    DevStateRow("mouse guard",  "%s", s_mouseGuard ? "ARMED (open-click swallow)" : "clear");
    DevStateRow("enter guard",  "%s", s_enterGuard ? "ARMED (Enter not yet seen up)" : "clear");
    DevStateRow("click-away rect", "%s%s",
                s_rectValid.load(std::memory_order_relaxed) ? "armed" : "not armed",
                s_closeRequest.load(std::memory_order_relaxed) ? " | close PENDING" : "");
    DevStateRow("same-click suppress", "%s",
                s_closedByClick.load(std::memory_order_relaxed) ? "ARMED (click in flight)"
                : GetTickCount64() < s_suppressOpenUntil.load(std::memory_order_relaxed)
                    ? "grace window" : "clear");
    const int fc = ImGui::GetFrameCount();
    DevStateRow("options preview", "ghost %s / keep-alive %s",
                (fc - s_ghostFrame)     <= 1 ? "PULSING" : "idle",
                (fc - s_keepAliveFrame) <= 1 ? "PULSING" : "idle");
});
#endif  // EMOT3_DEVTOOLS

void TogglePalette() {
    const bool open = !s_open.load(std::memory_order_relaxed);
    if (open) {
        // Same-click suppression (see s_closedByClick): if the palette was
        // JUST closed by the very click whose release is now invoking this
        // toggle (the Nexus icon case), opening again would make the icon
        // feel like "reopen" instead of a toggle - swallow this one.
        const bool sameClick =
            s_closedByClick.exchange(false, std::memory_order_acq_rel) ||
            GetTickCount64() < s_suppressOpenUntil.load(std::memory_order_relaxed);
        if (sameClick) {
            s_suppressOpenUntil.store(0, std::memory_order_relaxed);
            LOG_DEBUG("Keybind: palette toggle-open suppressed (same click that closed it)");
            return;
        }
    }
    s_open = open;
    if (open) {
        s_takeFocus = true;
        s_sel = 0;
        s_varIdx = 0;
        s_varSel = -1;
        if (g_Settings.PaletteClearOnOpen) s_query[0] = '\0';
        s_closeRequest = false;  // a stale request must not close the new open
        s_rectValid    = false;  // no rect until the first frame renders
        s_mouseGuard   = true;   // the opening click must not click a row
        s_enterGuard   = true;   // a wedged Enter must not auto-send (see above)
    }
    LOG_DEBUG("Keybind: palette %s", open ? "opened" : "closed");
}

bool IsPaletteOpen() { return s_open.load(std::memory_order_relaxed); }

void PaletteGhostPulse()     { s_ghostFrame     = ImGui::GetFrameCount(); }
void PaletteKeepAlivePulse() { s_keepAliveFrame = ImGui::GetFrameCount(); }

void PaletteNoteMouseUp() {
    // The click that closed the palette has completed; the icon's toggle
    // invoke (if this was the icon) arrives within the next frame or two -
    // keep suppressing it for a short grace, then forget.
    if (s_closedByClick.exchange(false, std::memory_order_acq_rel))
        s_suppressOpenUntil.store(GetTickCount64() + 150,
                                  std::memory_order_relaxed);
}

void PaletteNoteMouseDown(int xClient, int yClient) {
    if (!s_open.load(std::memory_order_relaxed)) return;
    if (!s_rectValid.load(std::memory_order_acquire)) return;  // opening click
    const int x = s_rectX.load(std::memory_order_relaxed);
    const int y = s_rectY.load(std::memory_order_relaxed);
    const int w = s_rectW.load(std::memory_order_relaxed);
    const int h = s_rectH.load(std::memory_order_relaxed);
    if (xClient < x || yClient < y || xClient >= x + w || yClient >= y + h)
        s_closeRequest.store(true, std::memory_order_release);
}

void ResetPalette() {
    s_open           = false;
    s_takeFocus      = false;
    s_sel            = 0;
    s_varIdx         = 0;
    s_varSel         = -1;
    s_query[0]       = '\0';
    s_ctxOpen           = false;
    s_mouseGuard        = false;
    s_enterGuard        = false;
    s_closeRequest      = false;
    s_rectValid         = false;
    s_closedByClick     = false;
    s_suppressOpenUntil = 0;
    s_ghostFrame        = -1000;
    s_keepAliveFrame    = -1000;
}

void PaletteRender() {
    // Options-preview state (see PaletteGhostPulse/KeepAlivePulse): while the
    // user is engaged with the palette's option section, a CLOSED palette
    // renders as a non-interactive ghost (geometry sliders preview live) and
    // an OPEN one's auto-close stands down (clicking the slider is by
    // definition a click outside the palette).
    const int  fc        = ImGui::GetFrameCount();
    const bool keepAlive = (fc - s_keepAliveFrame) <= 1;
    const bool ghost     = !s_open.load() && (fc - s_ghostFrame) <= 1;

    // A WndProc click-away request (see PaletteNoteMouseDown) closes before
    // anything renders this frame. Always drained; discarded while a context
    // menu is open (its clicks land outside the palette rect by design) or
    // the options section is engaged. A real close here is click-caused by
    // definition - arm the same-click toggle suppression.
    if (s_closeRequest.exchange(false) && !s_ctxOpen && !keepAlive &&
        s_open.load(std::memory_order_relaxed)) {
        s_open          = false;
        s_closedByClick = true;
    }
    if (!s_open && !ghost) return;
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

    // Centered horizontally, vertical anchor from the user's setting, fixed
    // width, auto height. Positioned EVERY frame (the window is NoMove, so
    // nothing fights it): the position sliders move an open palette live,
    // and a resolution change re-centers automatically. The grow-up option
    // pins the BOTTOM edge at the anchor instead of the top, so the window
    // extends toward the screen top as the result list grows (pivot uses the
    // previous frame's size for an auto-resize window - a one-frame settle
    // on growth, same class as the top-anchored bottom edge).
    ImGuiIO& io = ImGui::GetIO();
    const float kW = 380.f * g_Settings.PaletteScale;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * g_Settings.PaletteXPos,
                                   io.DisplaySize.y * g_Settings.PaletteYPos),
                            ImGuiCond_Always,
                            ImVec2(0.5f, g_Settings.PaletteGrowUp ? 1.0f : 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(kW, 0.f), ImVec2(kW, FLT_MAX));
    // Background opacity (1.0 = leave the theme's WindowBg untouched).
    if (g_Settings.PaletteBgAlpha < 0.995f)
        ImGui::SetNextWindowBgAlpha(g_Settings.PaletteBgAlpha);
    if (s_takeFocus && !ghost) ImGui::SetNextWindowFocus();
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ghost) flags |= ImGuiWindowFlags_NoInputs;  // pure preview - never steals anything
    if (!ImGui::Begin(PALETTE_WND_NAME, nullptr, flags)) { ImGui::End(); return; }

    // Always on top: a focused window is normally frontmost anyway, but this
    // covers the frames where another window was submitted later or a refocus
    // is still in flight - the palette never renders underneath anything.
    // Stands down while a row context menu is open: the popup is its own
    // window, and re-raising the palette every frame would put it on top of
    // its OWN menu.
    if (!s_ctxOpen)
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // Esc closes in ONE press. Handled here, not via Nexus' CloseOnEscape:
    // an active InputText eats the first Esc to deactivate-and-REVERT the
    // buffer (the query visibly jumped back to its pre-edit text) and only a
    // second Esc reached the hook. Reading the key directly and closing this
    // frame means the revert never renders.
    // With "Esc clears first" on (the default), an Esc on a NON-empty query
    // wipes it instead of closing - the next Esc then closes. The wipe must
    // also pre-empt the InputText's internal Esc handler (it would write the
    // pre-edit text right back): deactivate the field this frame, re-pin via
    // s_takeFocus the next.
    if (!s_ctxOpen &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape), false)) {
        if (g_Settings.PaletteEscClearsFirst && s_query[0] != '\0') {
            s_query[0] = '\0';
            s_sel    = 0;
            s_varIdx = 0;
            ImGui::ClearActiveID();
            s_takeFocus = true;
        } else {
            s_open = false;
            ImGui::End();
            return;
        }
    }

    // Spotlight semantics: the palette is a transient prompt, not a panel to
    // park - clicking away or losing focus closes it. Two complementary
    // signals, because one alone misses a case under Nexus:
    //  - focus moved to ANOTHER ImGui window (or alt-tab): this focus check.
    //  - a click on the GAME WORLD: invisible to ImGui's focus bookkeeping
    //    (and io.WantCaptureMouse stays true while the query field is active,
    //    masking io-side click checks too) - covered by the WndProc hit-test
    //    (PaletteNoteMouseDown), honored at the top of this function.
    // s_takeFocus graces the opening frame (focus not yet applied); an open
    // context menu counts as "ours" even though it's a separate ImGui window;
    // keepAlive covers tuning from the options section (focus is over there).
    // A ghost has no focus to lose.
    if (!ghost && !s_takeFocus && !s_ctxOpen && !keepAlive &&
        !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        s_open = false;
        // Click on another ImGui window (the Nexus icon included) - arm the
        // same-click toggle suppression; a non-click focus loss doesn't.
        if (ImGui::IsAnyMouseDown()) s_closedByClick = true;
        ImGui::End();
        return;
    }

    // Publish this frame's window rect for the WndProc click-away hit-test
    // (client-space == ImGui screen-space under Nexus). Open palette only -
    // a ghost must not arm the hit-test.
    if (!ghost) {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        s_rectX.store((int)wp.x, std::memory_order_relaxed);
        s_rectY.store((int)wp.y, std::memory_order_relaxed);
        s_rectW.store((int)ws.x, std::memory_order_relaxed);
        s_rectH.store((int)ws.y, std::memory_order_relaxed);
        s_rectValid.store(true, std::memory_order_release);
    }

    // Grow-up INVERTS the whole layout, not just the window anchor: footer
    // on top, then the cap line, then the results in REVERSE order (best
    // match at the bottom), and the query field last - sitting at the pinned
    // bottom edge, so it never moves while the list above it grows. The
    // building blocks below are lambdas so both orders share one
    // implementation; submission order = visual order (immediate mode), so
    // in grow-up the rows reflect the previous frame's query edit - a
    // one-frame lag, invisible at game framerates (Enter still consumes the
    // rows exactly as displayed).
    const bool growUp = g_Settings.PaletteGrowUp;

    // Query field. EnterReturnsTrue -> send the selection. AutoSelectAll: a
    // reopen restores the last query selected, so typing replaces it.
    bool enter = false;
    auto queryField = [&]() {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (s_takeFocus) { ImGui::SetKeyboardFocusHere(); s_takeFocus = false; }
        enter = ImGui::InputTextWithHint(
            "##palquery", L("pal.hint"), s_query, sizeof(s_query),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_AutoSelectAll, QueryEditCb);
        // Claim Tab for the active query field: ImGui 1.80's
        // FocusableItemRegister tabs OUT of the active item (deactivating it -
        // caret loss + an AutoSelectAll re-select on the re-pin) unless the
        // item declared Tab as one of its keys. Tab is the variant-cycling
        // key, never a focus hop. The mask persists while the field stays
        // active and clears itself on any ActiveId change.
        if (ImGui::IsItemActive())
            GImGui->ActiveIdUsingKeyInputMask |= ((ImU64)1 << ImGuiKey_Tab);
        // Editing the query rebuilds the list - the same index would point at
        // a different emote, so an armed variant goes back to the default.
        if (ImGui::IsItemEdited()) s_varIdx = 0;

        // Enter guard (see s_enterGuard): a fire only counts once io has seen
        // Enter UP since opening - a wedged Enter otherwise key-repeats
        // straight into a send the moment the field activates.
        {
            const int ke  = io.KeyMap[ImGuiKey_Enter];
            const int kke = io.KeyMap[ImGuiKey_KeyPadEnter];
            const bool enterDown = (ke  >= 0 && io.KeysDown[ke]) ||
                                   (kke >= 0 && io.KeysDown[kke]);
            if (s_enterGuard && !enterDown) s_enterGuard = false;
            if (s_enterGuard) enter = false;
        }

        // Keep the query field ACTIVE for as long as the palette lives
        // (re-arm whenever nothing is active - e.g. after Enter deactivated
        // it, or a click on the window background). Type-ready at all times,
        // and it's what makes the palette FIRST in the Esc chain: Nexus'
        // escape-closing and the game's own Esc handling are both held off
        // while a text input is active, so an Esc here can only ever mean
        // "close the palette" - never "also close the Library" or "open the
        // game menu".
        // NEVER re-arm while a mouse button is down: on the click frame the
        // field releases ActiveId, and a queued refocus would steal it back
        // from the mid-press row button - whose press-on-RELEASE then never
        // fires (that exact sequence ate left-click sends). Also stands down
        // for a ghost and while the options section is engaged (a re-pin
        // would yank keyboard focus away from the controls being tuned).
        if (!ghost && !keepAlive && !s_takeFocus && !s_ctxOpen &&
            !ImGui::IsAnyItemActive() && !ImGui::IsAnyMouseDown())
            ImGui::SetKeyboardFocusHere(-1);
    };

    // Build this frame's rows. Filtering starts from the FIRST character,
    // same as the Library search (its old 2-char activation rule was
    // removed): the list is capped + ranked, so a 1-char query is already
    // useful and the scan cost is trivial (the catalog is small).
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

    // Variant cycling state for the SELECTED row (see BuildVariantList). A
    // selection change drops an armed variant - it belonged to another row.
    if (s_sel != s_varSel) { s_varIdx = 0; s_varSel = s_sel; }
    PalVariant vars[4];
    const int varCount = rows.empty() ? 0 : BuildVariantList(rows[s_sel], vars);
    if (s_varIdx >= varCount) s_varIdx = 0;
    // Tab / Shift+Tab step through the variants (wrapping). Read directly
    // like Esc above - the query field stays active (it claims Tab, see the
    // InputText) and the cycle is invisible to ImGui's own key handling.
    if (varCount > 1 && !ghost && !s_ctxOpen &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Tab), true))
        s_varIdx = (s_varIdx + (io.KeyShift ? varCount - 1 : 1)) % varCount;

    // Per-send override of the auto-submit settings - the user's palette
    // Enter mode, applied to row activations AND the context menu below
    // (set/reset brackets; see SetSendModeOverride).
    const ESendModeOverride sendMode =
        g_Settings.PaletteEnterMode == EPaletteEnterMode::Send ? ESendModeOverride::Send
      : g_Settings.PaletteEnterMode == EPaletteEnterMode::Fill ? ESendModeOverride::Fill
      :                                                          ESendModeOverride::None;

    // Open-click swallow (see s_mouseGuard): armed by TogglePalette, released
    // once no mouse button is held - from then on row clicks count.
    if (s_mouseGuard && !ImGui::IsAnyMouseDown()) s_mouseGuard = false;

    int activate = -1;
    ImVec2 selAnchor(0.f, 0.f);  // selected row's top-center (keyboard-send feedback anchor)
    float iconSz = (ImGui::GetFontSize() + 6.f) * g_Settings.PaletteScale;
    if (iconSz < ImGui::GetTextLineHeight()) iconSz = ImGui::GetTextLineHeight();
    // Compact mode (icons off): rows shrink to text height + breathing room
    // and the icon column disappears entirely (no texture fetches either).
    const bool  showIcons = g_Settings.PaletteShowIcons;
    const float rowH      = showIcons
        ? iconSz
        : ImGui::GetTextLineHeight() + 4.f * g_Settings.PaletteScale;
    const float textIndent = showIcons ? iconSz : 0.f;
    // Only let a hover steal the selection when the mouse actually moved -
    // otherwise the row under a parked cursor wins every Up/Down keypress.
    const bool mouseMoved = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;

    // Cap / empty-state line ("+N more", no-match, usage hint). Sits past the
    // far end of the list - below it growing down, ABOVE it growing up.
    auto capLine = [&]() {
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
    };

    // Footer key help. The Tab hint only shows when the selected row actually
    // has variants to cycle.
    auto footerLine = [&]() {
        ImGui::TextDisabled("%s", varCount > 1 ? L("pal.footer_tab")
                                               : L("pal.footer"));
    };

    // Everything ABOVE the list when growing up (footer farthest away, then
    // the cap line); growing down it's the query field that leads.
    if (growUp) {
        if (g_Settings.PaletteShowFooter) { footerLine(); ImGui::Spacing(); }
        capLine();
    } else {
        queryField();
        ImGui::Spacing();
    }

    bool anyCtx = false;  // a row context menu is open THIS frame
    for (int vi = 0; vi < (int)rows.size(); ++vi) {
        // Visual slot vi -> data index i: grow-up draws the list REVERSED so
        // the best match (index 0) sits at the BOTTOM, adjacent to the query
        // field (the arrows invert with it - see QueryEditCb).
        const int i = growUp ? (int)rows.size() - 1 - vi : vi;
        const PalRow& r = rows[i];
        ImGui::PushID(i);

        // ONE InvisibleButton spans the whole row (icon included); highlight,
        // icon, name, and side label are all drawn manually on top. One item =
        // one consistent hit-area for click, hover, and highlight. (The
        // earlier Selectable-next-to-Image split left the icon column outside
        // both the highlight and ImGui's hover state, so the hover and
        // selected cues could visibly disagree about the same row.)
        const float  rowW   = ImGui::GetContentRegionAvail().x;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const ImVec2 rowMax = ImVec2(rowMin.x + rowW, rowMin.y + rowH);
        if (i == s_sel) selAnchor = ImVec2(rowMin.x + rowW * 0.5f, rowMin.y);
        if (ImGui::InvisibleButton("row", ImVec2(rowW, rowH)) && !s_mouseGuard)
            activate = i;   // fires on click-release, like a Quickbar icon
        const bool rowHovered = !s_ctxOpen && ImGui::IsItemHovered();
        if (mouseMoved && rowHovered) s_sel = i;
        if (rowHovered && !s_mouseGuard &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            s_sel = i;
            ImGui::OpenPopup("palctx");
        }

        // Highlight: ONE rect per row with explicit precedence - pressed
        // (HeaderActive) > selected (Header; what Enter sends - follows the
        // mouse while it moves, the arrows otherwise) > parked-mouse hover on
        // a non-selected row (HeaderHovered, dimmer). A row never shows two
        // stacked/disagreeing states.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 bg = 0;
        if (ImGui::IsItemActive() && rowHovered)
            bg = ImGui::GetColorU32(ImGuiCol_HeaderActive);
        else if (i == s_sel)
            bg = ImGui::GetColorU32(ImGuiCol_Header);
        else if (rowHovered)
            bg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        if (bg)
            dl->AddRectFilled(rowMin, rowMax, bg,
                              ImGui::GetStyle().FrameRounding);

        // Icon (lazy cache; blank slot while the async load is in flight).
        // Skipped entirely in compact mode - no fetch, no column.
        if (showIcons) {
            Texture* t = r.isMeMote ? EnsureMeMoteTexture(r.m) : EnsureEmoteTexture(r.e);
            if (t && t->Resource)
                dl->AddImage((ImTextureID)t->Resource, rowMin,
                             ImVec2(rowMin.x + iconSz, rowMin.y + iconSz));
        }

        // Name + dim side label (the matched alias when the hit came only via
        // an alias - it shows nowhere else, the confusing case - otherwise the
        // command). The name is clipped so it can't run under the
        // right-aligned side text. An ARMED Tab variant takes over the
        // selected row's side slot in full text color - it announces what
        // Enter will do now, which beats the command echo in importance.
        const float pad = ImGui::GetStyle().ItemSpacing.x;
        std::string side = !r.aliasHit.empty() ? r.aliasHit
                         : (r.isMeMote ? std::string("/me") : r.e.Command);
        ImU32 sideCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (!r.catHit.empty()) {
            // Surfaced via a matching favorites category: the category name in
            // the Library's user-category gold (Cells.cpp header color),
            // dimmed to side-label weight - reads as "here because of your
            // category", distinct from the grey command/alias echo.
            side    = r.catHit;
            sideCol = ImGui::GetColorU32(ImVec4(0.92f, 0.78f, 0.32f, 0.80f));
        }
        if (i == s_sel && s_varIdx > 0 && s_varIdx < varCount) {
            side    = L(vars[s_varIdx].labelKey);
            sideCol = ImGui::GetColorU32(ImGuiCol_Text);
        }
        side = Ellipsize(side, kW * 0.35f);
        const float sideW = ImGui::CalcTextSize(side.c_str()).x;
        float nameW = rowW - textIndent - sideW - 3.f * pad;
        if (nameW < 40.f) nameW = 40.f;
        const std::string name = Ellipsize(r.isMeMote ? r.m.Name : r.e.Name, nameW);
        const float textY = rowMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(rowMin.x + textIndent + pad, textY),
                    ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        dl->AddText(ImVec2(rowMax.x - sideW - pad, textY),
                    sideCol, side.c_str());

        // Send-alternatives menu: the Library's shared variant body (target /
        // sync for emotes; You / All for /me-motes - Default stays the Enter /
        // left-click default). A successful variant send closes the palette
        // like Enter does; a refusal keeps it open with the overlay reason.
        if (ImGui::BeginPopup("palctx")) {
            anyCtx = true;
            // ImGui's own popup-Esc-close needs keyboard nav enabled, which
            // Nexus doesn't guarantee - handle it here so Esc reliably closes
            // the MENU first (the next Esc then closes the palette).
            if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape), false))
                ImGui::CloseCurrentPopup();
            SetSendModeOverride(sendMode);  // the menu items send through it
            const bool sent = r.isMeMote ? MeMoteVariantItems(r.m)
                                         : RenderSendVariants(r.e);
            SetSendModeOverride(ESendModeOverride::None);
            if (sent) s_open = false;  // finishes this frame, gone the next
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Context-menu lifecycle: when the menu closes (Esc, outside click, item
    // click), re-arm the query field so the palette is type-ready again - and
    // only flip s_ctxOpen AFTER the loop so the checks at the top of this
    // function stand down for the popup's entire lifetime.
    if (s_ctxOpen && !anyCtx) s_takeFocus = true;
    s_ctxOpen = anyCtx;

    // Everything BELOW the list: the query field at the pinned bottom edge
    // when growing up; the cap line + footer when growing down.
    if (growUp) {
        ImGui::Spacing();
        queryField();
    } else {
        capLine();
        if (g_Settings.PaletteShowFooter) { ImGui::Spacing(); footerLine(); }
    }

    if (enter && !rows.empty()) activate = s_sel;
    if (activate >= 0 && activate < (int)rows.size()) {
        const PalRow& r = rows[activate];
        // A keyboard-driven send has no meaningful cursor position - anchor a
        // refusal pill at the SELECTED ROW (where the eye is) instead of
        // wherever the mouse happens to be parked. Click sends keep the
        // default cursor anchor. One-shot + frame-scoped (see Feedback.h).
        if (enter)
            SetNextFeedbackAnchor(selAnchor.x, selAnchor.y);
        // Default = left-click Library semantics: targetable honors the
        // user's send-on-target setting, never sync'd. An ARMED Tab variant
        // (only ever on the selected row - clicking a different row sends
        // that row's default) sends its menu-equivalent instead. The full
        // gate applies; on a refusal the palette stays open (the overlay
        // names the reason) and the query field re-takes focus so the typing
        // flow survives Enter.
        SetSendModeOverride(sendMode);
        bool sent;
        if (activate == s_sel && s_varIdx > 0 && s_varIdx < varCount) {
            const PalVariant& v = vars[s_varIdx];
            sent = r.isMeMote ? SendOrFillMeMote(r.m, v.mv)
                              : SendOrFillEmote(r.e, v.useTarget, v.useSync);
        } else {
            sent = r.isMeMote
                ? SendOrFillMeMote(r.m, EMeMoteVariant::Default)
                : SendOrFillEmote(r.e, /*useTarget=*/g_Settings.SendTargetableOnTarget,
                                  /*useSync=*/false);
        }
        SetSendModeOverride(ESendModeOverride::None);
        if (sent) s_open = false;
        else if (enter) s_takeFocus = true;
    }

    // Refusal overlay on THIS surface (the palette is open + focused, so the
    // in-window sink is right - SendAlert is for surfaces that may be closed).
    DrawFeedbackOverlay(FeedbackSurface::Palette, /*highContrast=*/false,
                        ImGui::GetWindowDrawList());
    ImGui::End();
}
