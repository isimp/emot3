#include "MainPanel.h"
#include "Globals.h"
#include "Logging.h"
#include "I18n.h"
#include "OptionsCommon.h"  // InputFieldWithHint (shared text-field standard)
#include "Settings.h"
#include "SaveScheduler.h"  // RequestSave / PumpSaves (debounced, off-thread saves)
#include "EmoteData.h"
#include "MeMotes.h"           // /me-motes Library section + favorites mixing
#include "EmoteAction.h"
#include "CharacterState.h" // TickCharacterState (per-frame falling check)
#include "ChatWatch.h"      // DrainAutoMotes (per-frame auto-mote fire drain)
#include "UnlockScan.h"   // DrainUnlockSync (drives auto-sync + applies results)
#include "UpdateCheck.h"  // DrainUpdateCheck (Plus update hint; no-op stub otherwise)
#include "Favorites.h"
#include "Palette.h"          // TogglePalette / IsPaletteOpen (toolbar "P" button)
#include "SearchMatch.h"      // MatchEmoteSearch / MatchMeMoteSearch (shared with the palette)
#include "StringUtil.h"       // TrimWhitespace (new-category entry) + ToLower (search needle)
#include "Icons.h"
#include "IconDrawing.h"      // DrawStarIcon / DrawPaperclipIcon / DrawCollapseArrow
#include "IconCacheConfig.h"  // g_IconCache.poolBudgetMB (dev pool-budget readout)
#include "Layout.h"
#include "Cells.h"
#include "Resources.h"
#include "Feedback.h"    // SetActiveFeedbackSurface / DrawFeedbackOverlay
#include "Profiling.h"   // dev perf overlay

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // BeginDragDropTargetCustom + ImRect (trash-zone drop target)

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Persistent search-box buffer for the main panel. Local to this TU — no
// other module reads it, so it doesn't belong in Globals.
static char g_SearchBuf[64] = {};

// Collapse / expand every Library section at once: each user favorites category
// plus the three built-in sections (Core / Unlockable / /me-motes). These collapse
// flags are pure navigation state — updated in memory only; they ride along the
// next genuine settings write and the unload flush, so toggling them costs no disk
// I/O (see SaveScheduler.h).
static void SetAllSectionsCollapsed(bool c) {
    for (auto& cat : g_Settings.FavoriteCategories) cat.Collapsed = c;
    g_Settings.MainCoreCollapsed     = c;
    g_Settings.MainUnlockedCollapsed = c;
    g_Settings.MainMeMotesCollapsed  = c;
}

// Category-reorder drop zone spanning a whole section (header + its grid), from
// screen Y `secTop` to `secBot`. Submitted as an overlapping invisible button
// ONLY while a CAT_DRAG is in flight, so it never interferes with emote drops or
// clicks. The big Y span makes it easy to aim (the old one-line header target was
// hard to pin down); top half drops BEFORE this category, bottom half AFTER it,
// and the blue bar shows in the real gap above/below the section. No-op slots
// (the dragged category's own neighbours) draw nothing and deliver nothing.
static void CategoryReorderZone(int ci, float secTop, float secBot) {
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    if (!drag || !drag->IsDataType("CAT_DRAG")) return;

    float left  = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
    float width = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
    ImVec2 saved = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(left, secTop));
    ImGui::PushID(ci);
    ImGui::InvisibleButton("##cat_reorder",
        ImVec2(std::max(1.f, width), std::max(1.f, secBot - secTop)));
    ImGui::SetItemAllowOverlap();
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
            "CAT_DRAG", ImGuiDragDropFlags_AcceptPeekOnly);
        if (pl) {
            int  from  = *(const int*)pl->Data;
            bool after = ImGui::GetMousePos().y > (secTop + secBot) * 0.5f;
            int  slot  = ci + (after ? 1 : 0);
            int  to    = (from < slot) ? slot - 1 : slot;  // final index after removing `from`
            if (to != from) {
                float gap = ImGui::GetStyle().ItemSpacing.y;
                DrawCategoryDropLine((after ? secBot : secTop) + gap * 0.5f);
                if (pl->IsDelivery()) MoveFavoriteCategory(from, to);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(saved);
}

// "Drop here to remove from favorites" zone. A subtle bar anchored to the bottom
// of the Library window, shown ONLY while an emote dragged FROM a user favorites
// category is in flight (payload.categoryIdx >= 0). Catalog-sourced drags (Core /
// Unlockable, categoryIdx < 0) aren't in favorites, so the zone stays hidden for
// them. Dropping calls RemoveRefFromCategories with the payload's type - exactly
// the right-click "Remove from favorites" path (which is type-aware, so a
// /me-mote favorite drops correctly instead of routing to the Emote namespace).
//
// Drawn on the FOREGROUND draw list (always on top) with a custom drop rect
// (BeginDragDropTargetCustom) rather than a separate window: a real overlay window
// fell BEHIND the main panel in z-order once you clicked a cell to start the next
// drag (the panel came to front and covered it), so it "stopped showing" after the
// first removal. A foreground bar + custom target has no window z-order at all.
// Call INSIDE the Library window (so the custom target's RootWindow matches the
// hovered child) before its End.
static void RenderRemoveTrashZone() {
    const ImGuiPayload* dp = ImGui::GetDragDropPayload();
    if (!dp || !dp->IsDataType("FAV_DRAG")) return;
    const EmoteDragPayload* src = (const EmoteDragPayload*)dp->Data;
    if (!src || src->categoryIdx < 0) return;  // not a favorite: nothing to remove

    const char* label = L("mp.remove_zone");
    float pad    = 10.f;
    float iconSz = ImGui::GetFontSize();
    ImVec2 textSz = ImGui::CalcTextSize(label);
    float barW = pad + iconSz + 8.f + textSz.x + pad;
    float barH = std::max(iconSz, textSz.y) + pad;

    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImVec2 p0(wp.x + (ws.x - barW) * 0.5f, wp.y + ws.y - barH - 12.f);
    ImVec2 p1(p0.x + barW, p0.y + barH);

    bool hovering = false;
    if (ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), ImGui::GetID("##emot3_remove_zone"))) {
        const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
            "FAV_DRAG", ImGuiDragDropFlags_AcceptPeekOnly);
        if (pl) {
            hovering = true;
            const EmoteDragPayload* s = (const EmoteDragPayload*)pl->Data;
            if (pl->IsDelivery() && s) RemoveRefFromCategories(s->type, std::string(s->id));
        }
        ImGui::EndDragDropTarget();
    }

    // Subtle red backing that brightens when the drag hovers it.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 bg  = hovering ? IM_COL32(150, 55, 50, 235) : IM_COL32(64, 38, 38, 200);
    ImU32 brd = hovering ? IM_COL32(232, 112, 96, 255) : IM_COL32(170, 86, 78, 180);
    ImU32 fg  = hovering ? IM_COL32(255, 236, 232, 255) : IM_COL32(220, 176, 170, 235);
    dl->AddRectFilled(p0, p1, bg, 6.f);
    dl->AddRect(p0, p1, brd, 6.f, 0, hovering ? 2.f : 1.5f);
    DrawTrashIcon(ImVec2(p0.x + pad + iconSz * 0.5f, (p0.y + p1.y) * 0.5f), iconSz * 0.5f, fg, dl);
    dl->AddText(ImVec2(p0.x + pad + iconSz + 8.f, (p0.y + p1.y) * 0.5f - textSz.y * 0.5f),
                fg, label);
}

// The addon DLL's own HMODULE — captured in entry.cpp's DllMain. Needed
// here so GetOrCreateFromResource looks inside our DLL rather than the
// host process when loading AI fallback icons.
extern HMODULE hSelf;

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
// Runtime state inspector section: texture/resource health. Iterates the
// catalog on the open frame only (read-only, no disk I/O), so it's safe per
// the no-blocking-I/O-in-render rule. Self-registers via the Layer-2 standard.
static DevStateRegistrar s_texStateSection(DevStateCat::Content, "Icon textures", [] {
    // Deduped content-pool accounting: every distinct icon texture we've loaded,
    // split into in-use (drawn this/last frame) vs idle (off-screen or orphaned
    // by an edit). Shared icons count once. The idle bucket is the lazy/churn
    // cost made visible. Reads icon-cache state only - no catalog lock, no I/O.
    // (Catalog sizes intentionally not shown here: the emote / me-mote counts
    // are in the memory monitor with bytes, and the /me-mote count in the
    // "/me-motes catalog" section - no need to triple-show them.)
    // Once per addon load, sweep in any content textures that survived a
    // hot-reload but whose cells are off-screen (lazy per-cell recording can't
    // see them). Probe-only, so a cold start records nothing here.
    static bool s_poolReconciled = false;
    if (!s_poolReconciled) { s_poolReconciled = true; ReconcileResidentPool(); }
    IconPoolUsage u; IconPoolStats(u);
    auto mb = [](size_t b){ return (double)b / (1024.0 * 1024.0); };
    const int budget = g_IconCache.poolBudgetMB;
    if (budget > 0)
        DevStateRow("icon pool used", "%.2f / %d MB (%.0f%%), %llu icons",
                    mb(u.totalBytes), budget, mb(u.totalBytes) / (double)budget * 100.0,
                    (unsigned long long)u.totalCount);
    else
        DevStateRow("icon pool used", "%.2f MB, %llu icons (no budget)",
                    mb(u.totalBytes), (unsigned long long)u.totalCount);
    DevStateRow("  in use (on screen)", "%llu (%.2f MB)", (unsigned long long)u.inUseCount, mb(u.inUseBytes));
    DevStateRow("  loaded, off-screen", "%llu (%.2f MB)", (unsigned long long)u.idleCount,  mb(u.idleBytes));
});
#endif  // EMOT3_DEVTOOLS

// Per-emote / per-/me-mote textures now load lazily on first show via
// EnsureEmoteTexture / EnsureMeMoteTexture (ui/Icons.cpp). The old
// LoadEmoteTextures bulk loader (whole catalog on every catalog change,
// pumped per-frame from AddonRender + the Quickbar) is gone; only UI artwork
// is still primed up-front below.
void LoadUiIconOverrides() {
    // UI artwork (decorations + the Nexus shortcut icons). For each
    // binding we either look at icons/ui/<stem>.png first and fall back
    // to the bundled PNG embedded in the DLL, or skip the disk check
    // entirely. Idempotent - Nexus' texture cache no-ops on hits, so
    // calling this from settings changes is safe.
    if (!APIDefs) return;

    struct UiBinding {
        const char* cacheKey;       // texture identifier used at draw time
        const char* stem;           // filename stem in both icons/ui/ and resources/ui/
        bool        allowOverride;  // false -> disk file (if any) is ignored
    };
    // The Nexus quick-access icon and its hover state are locked to the
    // bundled artwork so the shortcut stays recognisable in the Nexus
    // icon row regardless of what users drop into icons/ui/. The
    // README.txt we write next to this folder calls out the same rule.
    static const UiBinding kBindings[] = {
        { "EMOT3_UI_STAR",           "star",        true  },
        { "EMOT3_UI_PAPERCLIP",      "paperclip",   true  },
        { "EMOT3_UI_LOCK",           "lock",        true  },
        { "EMOT3_UI_TARGET",         "target_dot",  true  },
        { "EMOT3_UI_ME_MOTE",        "me_mote_dot", true  },
        { "EMOT3_UI_SHORTCUT",       "icon",        false },
        { "EMOT3_UI_SHORTCUT_HOVER", "icon_hover",  false },
    };

    for (const auto& b : kBindings) {
        // 1. User override at icons/ui/<stem>.png - only consulted for
        //    bindings that opted into overriding. Locked bindings skip
        //    straight to step 2 even if a same-named file exists on
        //    disk (it's intentionally ignored).
        if (b.allowOverride && !g_IconsDir.empty()) {
            std::string file = g_IconsDir + "\\ui\\" + b.stem + ".png";
            DWORD attr = GetFileAttributesA(file.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES &&
                !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                APIDefs->Textures.GetOrCreateFromFile(b.cacheKey, file.c_str());
                continue;
            }
        }
        // 2. Bundled PNG. Same memory-loader path the emote bundle uses;
        //    avoids the GetOrCreateFromResource RT_RCDATA-vs-string trap.
        const void* data = nullptr; size_t size = 0;
        if (TryLoadBundledIconBytes(kUiIcons, kUiIconsCount,
                                    b.stem, data, size)) {
            APIDefs->Textures.GetOrCreateFromMemory(
                b.cacheKey, const_cast<void*>(data), size);
        }
    }

}

// First-run / empty-catalog dialog. Shown in place of the toolbar +
// sections whenever g_Emotes is empty (fresh install, or after "Clear
// catalog"). Lets the user seed the bundled emote set in one of the four
// GW2 command languages. The pick is just a wording preference (every
// command variant works in chat regardless of the game's language); the
// copy frames it that way rather than as a requirement.
static void RenderEmptyCatalogDialog() {
    std::vector<std::string> langs = AvailableEmoteLanguages();

    // Default the selection: prefer the catalog's last language, else the
    // forced UI language if it's an available emote language, else English.
    static int  sel       = -1;
    static bool selInited = false;
    if (!selInited) {
        std::string want = !g_EmoteLanguage.empty() ? g_EmoteLanguage
                         : (g_Settings.UiLanguage != "auto" ? g_Settings.UiLanguage
                                                            : std::string("en"));
        for (size_t i = 0; i < langs.size(); ++i)
            if (langs[i] == want) { sel = (int)i; break; }
        if (sel < 0) {
            for (size_t i = 0; i < langs.size(); ++i)
                if (langs[i] == "en") { sel = (int)i; break; }
        }
        if (sel < 0 && !langs.empty()) sel = 0;
        selInited = true;
    }
    if (sel >= (int)langs.size()) sel = langs.empty() ? -1 : 0;

    // --- Centered first-run card ------------------------------------
    // This is the user's first impression, so the content is laid out as
    // a tidy centered column (max width clamped) sitting in the upper
    // third, rather than hugging the top-left corner.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float availH = ImGui::GetContentRegionAvail().y;
    const float cardW  = std::min(availW, 420.f);
    const float indent = std::max(0.f, (availW - cardW) * 0.5f);

    // Center the next item (of width w) within the card; call at line start.
    auto centerX = [&](float w) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent +
                             std::max(0.f, (cardW - w) * 0.5f));
    };

    ImGui::Dummy(ImVec2(0.f, std::max(8.f, availH * 0.16f)));

    // Heading - larger and in the addon gold so it reads as a title.
    const ImVec4 gold(0.92f, 0.78f, 0.32f, 1.f);
    ImGui::SetWindowFontScale(1.5f);
    centerX(ImGui::CalcTextSize(L("empty.heading")).x);
    ImGui::TextColored(gold, "%s", L("empty.heading"));
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Spacing();
    ImGui::Spacing();

    // Body - each line (split on '\n') centered within the card.
    {
        std::string body = L("empty.body");
        size_t pos = 0;
        while (true) {
            size_t nl = body.find('\n', pos);
            std::string line = body.substr(
                pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
            centerX(ImGui::CalcTextSize(line.c_str()).x);
            ImGui::TextUnformatted(line.c_str());
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Language label + combo, as one centered row.
    const float comboW = 150.f;
    float rowW = ImGui::CalcTextSize(L("empty.language")).x +
                 style.ItemSpacing.x + comboW;
    centerX(rowW);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L("empty.language"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboW);
    const char* preview = (sel >= 0 && sel < (int)langs.size())
        ? L(("lang." + langs[sel]).c_str()) : "";
    if (ImGui::BeginCombo("##seedlang", preview)) {
        for (size_t i = 0; i < langs.size(); ++i) {
            bool s = ((int)i == sel);
            if (ImGui::Selectable(L(("lang." + langs[i]).c_str()), s)) sel = (int)i;
            if (s) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // Optional secondary language: folds its commands in as searchable aliases.
    // Tracked by code (not index) so it survives a primary change; excludes the
    // primary, and never equals it.
    static std::string s_secondary;  // "" = none
    const std::string primaryCode =
        (sel >= 0 && sel < (int)langs.size()) ? langs[sel] : std::string();
    if (s_secondary == primaryCode) s_secondary.clear();
    {
        float rowW2 = ImGui::CalcTextSize(L("empty.secondary_language")).x +
                      style.ItemSpacing.x + comboW;
        centerX(rowW2);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(L("empty.secondary_language"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(comboW);
        const char* preview2 = s_secondary.empty()
            ? L("common.none") : L(("lang." + s_secondary).c_str());
        if (ImGui::BeginCombo("##seedlang2", preview2)) {
            if (ImGui::Selectable(L("common.none"), s_secondary.empty()))
                s_secondary.clear();
            for (size_t i = 0; i < langs.size(); ++i) {
                if (langs[i] == primaryCode) continue;  // can't alias the primary
                bool s = (langs[i] == s_secondary);
                if (ImGui::Selectable(L(("lang." + langs[i]).c_str()), s))
                    s_secondary = langs[i];
                if (s) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Primary action - bumped padding so it reads as the main button.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.f, 8.f));
    float addW = ImGui::CalcTextSize(L("empty.add")).x + style.FramePadding.x * 2.f;
    centerX(addW);
    bool addClicked = ImGui::Button(L("empty.add"), ImVec2(addW, 0));
    ImGui::PopStyleVar();
    if (addClicked && sel >= 0 && sel < (int)langs.size()) {
        SeedDefaultEmotes(langs[sel], s_secondary);
        RequestSave(SaveKind::Emotes);
        MarkEmotesDirty();  // lazy: visible cells load their icons on next render
        // First-run: seed bundled /me-motes too so the catalog isn't an empty
        // "Add some under Options" hint. ClampMeMoteLanguage maps the emote
        // bundle's choice down to the /me-mote bundle's supported set —
        // picks like "fr" / "es" land at "en" so g_MeMoteLanguage is always
        // a value SeedBundledMeMotes can resolve. Idempotent by Id (a
        // returning user who already hit Restore won't see duplicates).
        g_MeMoteLanguage = ClampMeMoteLanguage(langs[sel]);
        int added = SeedBundledMeMotes(g_MeMoteLanguage);
        if (added > 0) {
            RequestSave(SaveKind::MeMotes);
            MarkMeMotesDirty();
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    centerX(ImGui::CalcTextSize(L("empty.custom_hint")).x);
    ImGui::TextDisabled("%s", L("empty.custom_hint"));

    // Friendly pointer to the addon's options, matching the muted hint tone.
    ImGui::Spacing();
    centerX(ImGui::CalcTextSize(L("empty.settings_hint")).x);
    ImGui::TextDisabled("%s", L("empty.settings_hint"));
}

void DetectNewBundledEmotes() {
    std::vector<std::string> bundle = AllBundledEmoteIds();
    // Start clean each call: turning notifications off (then re-detecting) must
    // be able to clear a previously-staged prompt.
    g_PromptNewBundledEmotes = false;
    g_NewBundledEmoteIds.clear();

    if (g_Settings.KnownBundledEmotes.empty()) {
        // First run with the feature (or a fresh install): adopt the current
        // bundle silently so existing users aren't nagged about emotes that
        // predate the feature.
        g_Settings.KnownBundledEmotes = bundle;
        RequestSave(SaveKind::Settings);
        return;
    }
    if (!g_Settings.NotifyNewBundledEmotes) {
        // Notifications off: suppress the prompt but do NOT advance the snapshot,
        // so re-enabling later still surfaces whatever was added while off.
        return;
    }
    std::vector<std::string> toOffer = ComputeNewBundledEmotes(g_Settings.KnownBundledEmotes);
    if (!toOffer.empty()) {
        // Stage the prompt; the snapshot advances when the user acts on the
        // dialog (so an unacted prompt re-shows next launch).
        g_NewBundledEmoteIds     = std::move(toOffer);
        g_PromptNewBundledEmotes = true;
        LOG_INFO("notifier: %d new bundled emote(s) available since last run",
                 (int)g_NewBundledEmoteIds.size());
    } else {
        // Nothing to offer: catch the snapshot up so it stays tidy.
        g_Settings.KnownBundledEmotes = bundle;
        RequestSave(SaveKind::Settings);
    }
}

// New-bundled-emote notifier dialog. A first-run-style modal shown over the
// (populated) Library the first time a version update added emotes the user
// doesn't have - g_PromptNewBundledEmotes staged in AddonLoad, the ids in
// g_NewBundledEmoteIds. The user can add them, skip for now, or turn the
// notifications off. Add uses AddBundledEmotesByIds so ONLY the new ids are
// added (a bundled emote the user deleted is never resurrected). Every choice
// advances the snapshot (g_Settings.KnownBundledEmotes) to the current bundle so
// the prompt doesn't repeat for the same emotes; an Escape dismiss leaves the
// snapshot un-advanced so it reappears next launch. See emot3.md.
static void RenderNewEmotesDialog() {
    static bool s_opened = false;
    const char* kPopupId = "###emot3newemotes";

    if (g_PromptNewBundledEmotes && !s_opened) {
        ImGui::OpenPopup(kPopupId);
        s_opened = true;
    }
    if (!s_opened) return;

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    std::string title = std::string(L("newemotes.heading")) + kPopupId;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        // Dismissed without a button (e.g. Escape): stop showing this session,
        // but leave the snapshot un-advanced so it reappears next launch.
        g_PromptNewBundledEmotes = false;
        s_opened = false;
        return;
    }

    // Shared close path for all three buttons: advance the snapshot to the full
    // bundle (so this batch never re-prompts), clear the staged state, close.
    auto finish = [&]() {
        g_Settings.KnownBundledEmotes = AllBundledEmoteIds();
        RequestSave(SaveKind::Settings);
        g_PromptNewBundledEmotes = false;
        g_NewBundledEmoteIds.clear();
        s_opened = false;
        ImGui::CloseCurrentPopup();
    };

    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
    ImGui::TextWrapped(L("newemotes.body"), (int)g_NewBundledEmoteIds.size());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // Scrollable list of the new emote ids (height grows with the count up to a
    // cap so a big batch still fits in a tidy box).
    float listH = std::min(160.f,
        (float)g_NewBundledEmoteIds.size() * ImGui::GetTextLineHeightWithSpacing() + 8.f);
    ImGui::BeginChild("##newemoteslist", ImVec2(0, listH), true);
    for (const auto& id : g_NewBundledEmoteIds)
        ImGui::BulletText("%s", id.c_str());
    ImGui::EndChild();
    ImGui::Spacing();

    if (ImGui::Button(L("newemotes.add"))) {
        std::string lang = g_EmoteLanguage.empty() ? std::string("en")
                                                    : g_EmoteLanguage;
        int n = AddBundledEmotesByIds(g_NewBundledEmoteIds, lang);
        if (n > 0) {
            RequestSave(SaveKind::Emotes);
            MarkEmotesDirty();  // lazy: new emotes load their icons on next render
        }
        LOG_INFO("notifier: user added %d new bundled emote(s)", n);
        finish();
    }
    ImGui::SameLine();
    if (ImGui::Button(L("newemotes.not_now"))) finish();
    ImGui::SameLine();
    if (ImGui::Button(L("newemotes.dont_ask"))) {
        g_Settings.NotifyNewBundledEmotes = false;  // saved by finish() below
        finish();
    }

    ImGui::EndPopup();
}

void AddonRender() {
    // Flush any debounced config writes whose changes have settled (the actual
    // disk I/O runs on the SaveScheduler's writer thread). First thing each frame,
    // before the gameplay/visibility early-returns below, so a pending save still
    // lands while the window is hidden. Cheap no-op when nothing is dirty.
    PumpSaves();

    // Tag this render pass so any refusal raised from a main-panel cell shows
    // its feedback line in the main panel (not the Quickbar). See Feedback.h.
    SetActiveFeedbackSurface(FeedbackSurface::MainPanel);
    if (!NexusLink || !NexusLink->IsGameplay) return;
    // Drive the GW2-API unlock sync every gameplay frame (drives auto-sync +
    // applies completed results) - before the visibility early-returns below so
    // it ticks even when the window is hidden. Cheap no-op when idle.
    DrainUnlockSync();
    // (usage.json now flushes via PumpSaves above, like every other config.)
    // Per-frame character-state update (the falling check = avatar height
    // velocity). Before the visibility early-returns below so the Quickbar's
    // grey/hide sees a fresh value even when the main window is hidden.
    TickCharacterState();
    // Auto-motes: fire any emote queued by the chat watch (core/ChatWatch) since
    // last frame, through the normal send gate. Before the visibility early-returns
    // below so it works even when the Library window is hidden. No-op when idle.
    DrainAutoMotes();
    // Plus-only: tick the "newer release available" check (no-op stub otherwise).
    DrainUpdateCheck();
    // Auto-hide while the fullscreen world map is open. It covers the screen,
    // so an emote panel drawn on top is just clutter. Same shape as the
    // IsGameplay gate: a per-frame suppression, not a persisted hide - the
    // window reappears when the map closes. Only the fullscreen map sets
    // IsMapOpen; the minimap does not, so the HUD Quickbar/panel stay during
    // normal play. (The minimap is the only other native UI rect we could
    // detect at all, via Compass.Width/Height - everything else GW2 keeps to
    // itself, which is why this map check is as far as auto-hide can go.)
    if (MumbleLink && MumbleLink->Context.IsMapOpen) return;
    if (!g_Settings.ShowWindow) return;
    PROFILE_SCOPE("mp.frame");  // dev perf overlay

    // Emote/me-mote icons load lazily per visible cell (EnsureEmoteTexture in
    // RenderEmoteCell), so there's no per-frame catalog reload here anymore.

    // Min size: width fits 8 Icon-mode cells (8*(56+8)-8 + padding ≈ 540); height 300.
    // Min width fits the toolbar comfortably: All / Core / Unlocked /
    // Locked radios + view-mode combo + scale slider + Quickbar button
    // + search box. Bumped from 540 → 590 when the Core radio was
    // added.
    ImGui::SetNextWindowSizeConstraints(ImVec2(590.f, 300.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("emot3 Library##wnd", &g_Settings.ShowWindow)) { ImGui::End(); return; }

    // Captured after the scrollable grid child begins (below): the refusal-line
    // overlay draws into THIS list so it lands on top of the cells while staying
    // in the panel's window z-order. nullptr if no child this frame (early
    // dialogs) -> DrawFeedbackOverlay falls back to the foreground list.
    ImDrawList* mpContentDrawList = nullptr;

    // Empty catalog (fresh install or after "Clear catalog") -> first-run
    // dialog instead of the normal toolbar/sections. Checked under the
    // lock; the dialog's seed button repopulates g_Emotes.
    {
        bool empty;
        { std::lock_guard<std::mutex> lk(g_EmotesMutex); empty = g_Emotes.empty(); }
        if (empty) {
            RenderEmptyCatalogDialog();
            ImGui::End();
            return;
        }
    }

    // New-bundled-emote notifier (only over a populated catalog; the empty-state
    // dialog above already covers a fresh install). Opens once when staged.
    RenderNewEmotesDialog();

    // Competitive modes (PvP / WvW): sends are hard-refused (gate step 0) and the
    // Quickbar is hidden outright, with no setting - so without a visible reason
    // the addon just looks broken the first time someone enters a match. One
    // amber line, same notice styling as the Plus update banner in Options >
    // General; clears by itself when the player leaves (live MumbleLink bit).
    if (InCompetitiveMode()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", L("mp.competitive_banner"));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ---- Toolbar ----
    // Three independent class filters as blue-tint toggle pills (Core /
    // Unlocked / Locked), replacing the old single-select All/Core/
    // Unlocked/Locked radio. All-on (the default) shows everything, so
    // there's no separate "All". An emote shows only when its class
    // toggle is on - see passes() below. Collapsing the radio's mutually-
    // exclusive modes into independent toggles is what lets the empty-
    // state messaging drop its per-mode "switch to All to see them" lines.
    bool filterChanged = false;
    filterChanged |= ToggleButton(L("filter.core"),     &g_Settings.FilterShowCore);
    if (ImGui::IsItemHovered()) TooltipOnOff("filter.core.on", "filter.core.off", /*defaultIsOn=*/true);
    ImGui::SameLine();
    filterChanged |= ToggleButton(L("filter.unlocked"), &g_Settings.FilterShowUnlocked);
    if (ImGui::IsItemHovered()) TooltipOnOff("filter.unlocked.on", "filter.unlocked.off", /*defaultIsOn=*/true);
    ImGui::SameLine();
    filterChanged |= ToggleButton(L("filter.locked"),   &g_Settings.FilterShowLocked);
    if (ImGui::IsItemHovered()) TooltipOnOff("filter.locked.on", "filter.locked.off", /*defaultIsOn=*/true);
    ImGui::SameLine();
    filterChanged |= ToggleButton(L("filter.me_motes"), &g_Settings.FilterShowMeMotes);
    if (ImGui::IsItemHovered()) TooltipOnOff("filter.me_motes.on", "filter.me_motes.off", /*defaultIsOn=*/true);
    if (filterChanged) {
        LOG_TRACE("setting main.filter core=%d unlocked=%d locked=%d me_motes=%d",
                  g_Settings.FilterShowCore, g_Settings.FilterShowUnlocked,
                  g_Settings.FilterShowLocked, g_Settings.FilterShowMeMotes);
        // View filters are navigation state — no eager save; ride along the next
        // real settings write / unload flush (see SaveScheduler.h).
    }

    ImGui::SameLine();
    // Slightly wider than before because the longest label is now "Text only".
    ImGui::SetNextItemWidth(105.f);
    // Display order — descending information density (Full → Compact →
    // Icon → Text only). Decoupled from the numeric enum values via the
    // lookup below, so we can reorder the dropdown without breaking
    // existing settings.json files (which store the raw enum integers).
    static const EViewMode kViewOrder[] = {
        EViewMode::Full,
        EViewMode::Compact,
        EViewMode::Icon,
        EViewMode::TextOnly,
    };
    // Rebuilt each frame from L() so it tracks the active language. The
    // pointers L() returns stay valid for the duration of the Combo call.
    const char* viewNames[] = { L("view.full"), L("view.compact"),
                                L("view.icon"), L("view.textonly") };
    int displayIdx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kViewOrder); ++i) {
        if (kViewOrder[i] == g_Settings.ViewMode) { displayIdx = i; break; }
    }
    if (ImGui::Combo("##vm", &displayIdx, viewNames, IM_ARRAYSIZE(viewNames))) {
        g_Settings.ViewMode = kViewOrder[displayIdx];
        LOG_TRACE("setting main.view_mode = %d", (int)g_Settings.ViewMode);
        // View mode is navigation state — rides along (no eager save).
    }

    // Icon-scale slider sits next to the view combo. Per-mode bounds:
    //   Full, Text-only, Compact — minimum 1× (the two-line label / single
    //                              text line / alpha label strip each need
    //                              vertical room; below 1× they break).
    //   Icon                     — free scaling, 0.5× to 2.5×.
    //
    // All four modes share the 2.5× cap.
    {
        float mScaleMin = MinIconScaleForMode(g_Settings.ViewMode);
        if (g_Settings.MainIconScale < mScaleMin) g_Settings.MainIconScale = mScaleMin;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.f);
        ImGui::SliderFloat("##mainscale", &g_Settings.MainIconScale, mScaleMin, 2.5f, "%.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            LOG_TRACE("setting main.icon_scale = %.2f", g_Settings.MainIconScale);
            // Icon scale is navigation state — rides along (no eager save).
        }
        // Right-click resets to the default scale. 1.0 is always within the
        // clamp range (mScaleMin is 0.5 or 1.0; max is 2.5).
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            g_Settings.MainIconScale = 1.0f;
            LOG_TRACE("setting main.icon_scale = %.2f (reset)", g_Settings.MainIconScale);
            // Icon scale is navigation state — rides along (no eager save).
        }
        if (ImGui::IsItemHovered())
            TooltipText("mp.icon_scale_tooltip");
    }

    // Quickbar + Palette toggles, right-aligned on this same row. Short "QB" /
    // "P" labels so the pair still fits beside the scale slider at the
    // window's min width; the tooltips carry the full names.
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        std::string qbLabel  = L("mp.quickbar");   // "QB"
        std::string palLabel = L("mp.palette");    // "P"
        float qbBtnW  = ImGui::CalcTextSize(qbLabel.c_str()).x  + style.FramePadding.x * 2.f;
        float palBtnW = ImGui::CalcTextSize(palLabel.c_str()).x + style.FramePadding.x * 2.f;
        float rightX = ImGui::GetWindowContentRegionMax().x - qbBtnW - palBtnW
                     - style.ItemSpacing.x - style.ItemInnerSpacing.x;
        if (rightX > ImGui::GetCursorPosX()) {
            ImGui::SameLine(rightX);
        } else {
            ImGui::SameLine();
        }
        // Window visibility is navigation state — ToggleButton flips ShowQuickbar
        // in place; no eager save (rides along / unload flush).
        ToggleButton(qbLabel.c_str(), &g_Settings.ShowQuickbar);
        if (ImGui::IsItemHovered())
            TooltipText("mp.quickbar_tooltip");
        ImGui::SameLine();
        // The palette's open state is transient (nothing persisted): the
        // ToggleButton runs on a local copy for the lit-while-open look, and
        // a change routes through TogglePalette (which owns the real state +
        // the open guards).
        bool palOpen = IsPaletteOpen();
        if (ToggleButton(palLabel.c_str(), &palOpen))
            TogglePalette();
        if (ImGui::IsItemHovered())
            TooltipText("mp.palette_tooltip");
    }

    // Search with an X clear button on the right. Filters from the first
    // character (the old 2-char activation rule + its one-more-character
    // hint were removed - the palette filters from the first keystroke and
    // the two surfaces should feel the same).
    const float clearW = 24.f;
    {
        InputFieldOpts o;   // explicit width leaves room for the trailing X button
        o.width = ImGui::GetContentRegionAvail().x - clearW - ImGui::GetStyle().ItemSpacing.x;
        if (o.width < 60.f) o.width = 60.f;
        InputFieldWithHint("##search", "mp.search_hint", g_SearchBuf, sizeof(g_SearchBuf), o);
    }
    if (ImGui::IsItemHovered())
        TooltipText("mp.search_tooltip");
    ImGui::SameLine();
    {
        bool hasText = (g_SearchBuf[0] != '\0');
        if (!hasText) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                           ImGui::GetStyle().Alpha * 0.30f);
        if (ImGui::SmallButton("X##clrsearch") && hasText) g_SearchBuf[0] = '\0';
        if (!hasText) ImGui::PopStyleVar();
        if (ImGui::IsItemHovered() && hasText) TooltipText("mp.clear_search");
    }

    ImGui::Separator();

    // ---- Build display lists (lock held) ----
    const std::string search = ToLower(g_SearchBuf);

    // Single source of truth for "is the search filter actually
    // running this frame?" - read by both passes() below and the
    // empty-state-message lambdas further down.
    const bool searchActive = !search.empty();

    // Per-frame lookup tables, populated once under the lock below. Building
    // these turns the list-build from O(N^2 log N) - it used to call the
    // linear-scan FindEmote (via IsEmoteUnlocked) per emote, per favorite, and
    // O(N log N) times inside the unlockables sort - into O(N + M + favs).
    // They're rebuilt every frame (no cross-frame caching, so nothing to
    // invalidate when the catalog mutates).
    CatalogIndex idx;                              // byId + unlockedIds, filled below
    std::unordered_set<std::string> favoritedIds;        // every Emote Id sitting in any category
    std::unordered_set<std::string> favoritedMeMoteIds;  // every /me-mote Id sitting in any category

    auto isUnlk = [&](const Emote& e) { return idx.unlocked(e); };

    auto passes = [&](const Emote& e, std::string& note) {
        note.clear();
        // Class filter: every emote belongs to exactly one of three
        // classes, and shows only when that class's toggle is on. Core
        // emotes are always usable; an unlockable counts as Unlocked when
        // the user has marked it so (it's in unlockedIds), Locked
        // otherwise.
        if (e.IsCore) {
            if (!g_Settings.FilterShowCore) return false;
        } else if (idx.unlockedIds.count(e.Id)) {
            if (!g_Settings.FilterShowUnlocked) return false;
        } else {
            if (!g_Settings.FilterShowLocked) return false;
        }
        if (searchActive) {
            // Match the display name, the slash-command (e.g. "/bow"), or any
            // alias ("/bbq" finds Barbecue) - shared predicate (SearchMatch.h)
            // so the palette and this box can't drift. An alias-only hit gets
            // the explanatory note (the name shows in the label, the command in
            // the tooltip - an alias shows nowhere, which is the confusing case).
            SearchHit h = MatchEmoteSearch(e, search);
            if (!h.hit) return false;
            if (h.aliasOnly) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), L("mp.search_match_alias"), h.aliasOnly->c_str());
                note = buf;
            }
        }
        return true;
    };

    // /me-mote search predicate. No class filters here (FilterShowMeMotes
    // gates the section/favorites build before this is consulted) and no
    // Command field (doesn't exist). Match shape mirrors passes() above so
    // an alias-only hit surfaces the same "matched alias: X" tooltip line.
    auto passesMeMote = [&](const MeMote& m, std::string& note) {
        note.clear();
        if (!searchActive) return true;
        SearchHit h = MatchMeMoteSearch(m, search);
        if (!h.hit) return false;
        if (h.aliasOnly) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), L("mp.search_match_alias"), h.aliasOnly->c_str());
            note = buf;
        }
        return true;
    };

    std::vector<std::vector<CellInfo>> catItems(g_Settings.FavoriteCategories.size());
    // Unfiltered count per category - counts only "real" emotes (skips
    // stale references to deleted emotes). Lets us distinguish "category
    // is truly empty" from "category has emotes but the active
    // filter/search hides them all" when rendering the empty-state line.
    std::vector<int> catTotal(g_Settings.FavoriteCategories.size(), 0);
    std::vector<CellInfo> coreItems, unlockItems, meMoteItems;
    // Totals of non-favorited entries ignoring filter/search - lets us tell
    // "all favorited" apart from "filter/search hides everything".
    int coreUnfavCount = 0, unlockUnfavCount = 0, meMoteUnfavCount = 0;
    // Totals over the whole catalog (favorited or not), so an empty section
    // can say "none of this class exist" vs "they're hidden by the filter".
    int coreTotal = 0, unlockTotal = 0;
    int meMoteTotal = 0;

    // Snapshot a /me-mote Id -> ptr map under g_MeMotesMutex so we don't have
    // to nest with g_EmotesMutex below. Pointers stay stable while no one is
    // mutating g_MeMotes (the editor runs on the same render thread).
    // meMoteTotal is set later, after the half-filled-entry filter, so it
    // represents renderable /me-motes (not raw catalog size).
    std::unordered_map<std::string, const MeMote*> meMotesById;
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        meMotesById.reserve(g_MeMotes.size());
        for (const auto& m : g_MeMotes) meMotesById[m.Id] = &m;
    }
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);

        { PROFILE_SCOPE("mp.build");  // dev perf overlay
        // Populate the per-frame lookup tables (rebuilt each frame, never cached).
        BuildCatalogIndex(g_Settings.ManuallyUnlocked, idx);
        // favoritedIds tracks which Emote IDs are currently favorited (drives
        // the "skip favorited" logic for the Core / Unlockable sections so the
        // same emote doesn't appear in both the favorites section and the
        // built-in one). favoritedMeMoteIds does the same for the /me-motes
        // section. The two are NOT merged: an Emote and a /me-mote can share
        // an Id (the catalogs are namespace-distinct), so the per-type sets
        // keep the skip-check unambiguous.
        for (const auto& c : g_Settings.FavoriteCategories) {
            for (const auto& r : c.Refs) {
                if (r.Type == EFavoriteRefType::Emote)       favoritedIds.insert(r.Id);
                else if (r.Type == EFavoriteRefType::MeMote) favoritedMeMoteIds.insert(r.Id);
            }
        }

        // One filtered list per favorites category, preserving user order.
        // Each Ref is resolved to its catalog by Type — Emote-typed refs go
        // through idx.byId (g_Emotes); /me-mote-typed refs go through the
        // snapshot map above. CellInfo carries the right pointer (e XOR m)
        // and RenderEmoteCell dispatches accordingly. Both the FilterShowMeMotes
        // pill and the search predicate apply to /me-mote favorites, mirroring
        // how passes() gates Emote favorites.
        for (size_t ci = 0; ci < g_Settings.FavoriteCategories.size(); ++ci) {
            const auto& cat = g_Settings.FavoriteCategories[ci];
            for (size_t i = 0; i < cat.Refs.size(); ++i) {
                const auto& ref = cat.Refs[i];
                if (ref.Type == EFavoriteRefType::Emote) {
                    auto it = idx.byId.find(ref.Id);
                    if (it == idx.byId.end()) continue;  // stale reference
                    const Emote* e = it->second;
                    ++catTotal[ci];
                    std::string note;
                    if (passes(*e, note))
                        catItems[ci].push_back({ e, nullptr, (int)i, isUnlk(*e), std::move(note) });
                } else {  // MeMote
                    auto it = meMotesById.find(ref.Id);
                    if (it == meMotesById.end()) continue;
                    const MeMote* m = it->second;
                    // A half-filled /me-mote (empty Name or empty
                    // TextDefault) is invisible until the user finishes it.
                    // The ref stays in storage so re-completing it pops back
                    // into view here.
                    if (!IsMeMoteRenderable(*m)) continue;
                    ++catTotal[ci];
                    // /me-motes filter pill, mirrors how passes() gates Emote
                    // refs above. Counted into catTotal first so the empty-
                    // state still distinguishes "no entries" from "filter
                    // hides them".
                    if (!g_Settings.FilterShowMeMotes) continue;
                    std::string note;
                    if (!passesMeMote(*m, note)) continue;
                    catItems[ci].push_back({ nullptr, m, (int)i, /*unlocked=*/true, std::move(note) });
                }
            }
        }

        // Core / Unlockable from g_Emotes, alphabetical. Skip anything favorited.
        for (const auto& e : g_Emotes) {
            if (e.IsCore) ++coreTotal; else ++unlockTotal;
            if (favoritedIds.count(e.Id)) continue;
            if (e.IsCore) ++coreUnfavCount; else ++unlockUnfavCount;
            std::string note;
            if (!passes(e, note)) continue;
            (e.IsCore ? coreItems : unlockItems).push_back({ &e, nullptr, -1, isUnlk(e), std::move(note) });
        }
        // /me-mote built-in section. Surfaces every renderable /me-mote that
        // ISN'T already favorited — same skip-when-favorited rule the Core /
        // Unlockable sections follow so an entry never appears in both the
        // favorites section and the built-in one. meMoteTotal +
        // meMoteUnfavCount drive the empty-state branching so we can
        // distinguish "none exist" / "all favorited" / "filter hid them".
        // Both counts are post-renderable (half-filled entries don't count
        // toward either) and BEFORE the FilterShowMeMotes pill — meMoteTotal
        // and meMoteUnfavCount stay accurate when the pill is off so the
        // empty-state message can name the right reason.
        for (const auto& kv : meMotesById) {
            const MeMote* m = kv.second;
            if (!IsMeMoteRenderable(*m)) continue;
            ++meMoteTotal;
            if (favoritedMeMoteIds.count(m->Id)) continue;
            ++meMoteUnfavCount;
            if (!g_Settings.FilterShowMeMotes) continue;
            std::string note;
            if (!passesMeMote(*m, note)) continue;
            meMoteItems.push_back({ nullptr, m, -1, /*unlocked=*/true, std::move(note) });
        }

        auto byName = [](const CellInfo& a, const CellInfo& b) {
            // Both sides are Emote cells in coreItems/unlockItems (built
            // above); the comparison reads a->e safely.
            return a.e->Name < b.e->Name;
        };
        std::sort(coreItems.begin(), coreItems.end(), byName);
        // Sort /me-motes by Name as well (or Id when Name empty).
        std::sort(meMoteItems.begin(), meMoteItems.end(),
            [](const CellInfo& a, const CellInfo& b) {
                const std::string& na = !a.m->Name.empty() ? a.m->Name : a.m->Id;
                const std::string& nb = !b.m->Name.empty() ? b.m->Name : b.m->Id;
                return na < nb;
            });

        // Unlockable: unlocked first, then locked, alphabetical within each.
        // CellInfo.unlocked was precomputed above (covers cores internally -
        // they always count as unlocked), so the sort is a plain field read,
        // not an O(N) lookup per comparison.
        std::sort(unlockItems.begin(), unlockItems.end(),
            [](const CellInfo& a, const CellInfo& b) {
                if (a.unlocked != b.unlocked) return a.unlocked;
                return a.e->Name < b.e->Name;
            });
        }  // end TEMP mp.build scope

        // ---- Pinned header: "+ Category" bar (stays put when scrolling) ----
        static char newCatBuf[64] = {};
        std::string trimmedNewCat = TrimWhitespace(newCatBuf);
        bool        newCatInvalid = !trimmedNewCat.empty() &&
                                    CategoryNameExists(trimmedNewCat);

        bool addPressed = false;
        {
            InputFieldOpts o; o.width = 180.f; o.invalid = newCatInvalid;
            o.flags = ImGuiInputTextFlags_EnterReturnsTrue;
            bool hov = false;
            addPressed = InputFieldWithHint("##newcat_main", nullptr, newCatBuf,
                                            sizeof(newCatBuf), o, nullptr, &hov);
            if (newCatInvalid && hov) {
                char m[160]; std::snprintf(m, sizeof m, L("mp.cat_exists"), trimmedNewCat.c_str());
                TooltipTextRaw(m);
            }
        }

        // X clear button next to the input.
        ImGui::SameLine();
        {
            bool hasText = (newCatBuf[0] != '\0');
            if (!hasText) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                               ImGui::GetStyle().Alpha * 0.30f);
            if (ImGui::SmallButton("X##clrnewcat") && hasText) newCatBuf[0] = '\0';
            if (!hasText) ImGui::PopStyleVar();
            if (ImGui::IsItemHovered() && hasText) TooltipText("common.clear");
        }
        ImGui::SameLine();
        addPressed |= ImGui::Button(L("mp.add_category"));

        if (addPressed && !trimmedNewCat.empty() && !newCatInvalid) {
            LOG_DEBUG("favorites: created category \"%s\"", trimmedNewCat.c_str());
            FavoriteCategory c;
            c.Name = trimmedNewCat;
            g_Settings.FavoriteCategories.push_back(std::move(c));
            // Keep the parallel locals in sync. catN below uses
            // min(catItems.size(), FavoriteCategories.size()), so missing
            // either of these pushes lets the render loop reach an index
            // that's out of range in the un-grown vector and trip MSVC's
            // debug bounds check.
            catItems.push_back({});
            catTotal.push_back(0);
            newCatBuf[0] = '\0';
            RequestSave(SaveKind::Settings);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            TooltipText("mp.category_help");
        }

        // Collapse / expand every section (user categories + the built-ins),
        // right-aligned on the category-management row.
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            std::string colLbl = L("mp.collapse_all");
            std::string expLbl = L("mp.expand_all");
            float colW = ImGui::CalcTextSize(colLbl.c_str()).x + style.FramePadding.x * 2.f;
            float expW = ImGui::CalcTextSize(expLbl.c_str()).x + style.FramePadding.x * 2.f;
            float rightX = ImGui::GetWindowContentRegionMax().x
                         - colW - expW - style.ItemSpacing.x - style.ItemInnerSpacing.x;
            if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);
            else                                 ImGui::SameLine();
            if (ImGui::Button(colLbl.c_str())) SetAllSectionsCollapsed(true);
            ImGui::SameLine();
            if (ImGui::Button(expLbl.c_str())) SetAllSectionsCollapsed(false);
        }

        // ---- Scrollable content begins here ----
        { PROFILE_SCOPE("mp.draw");  // dev perf overlay (cell-draw, separate from mp.build)
        ImGui::BeginChild("##content");
        mpContentDrawList = ImGui::GetCurrentWindow()->DrawList;  // for the refusal overlay (see below)

        // The empty-state lambdas below pull searchActive from the
        // outer scope (defined alongside `search`) so "is the user
        // actually searching?" matches the test passes() uses.

        // Picks the right line for a favorites category that has no
        // currently-visible items. With the class filters as independent
        // toggles the per-mode messaging is gone: a category either holds
        // nothing real, the search hid everything, or the class toggles
        // hid everything.
        auto catEmptyMsg = [&](int totalInCat) -> const char* {
            if (totalInCat == 0)
                return L("mp.cat_empty");
            if (searchActive)
                return L("mp.cat_no_search");
            return L("mp.cat_filtered");
        };

        bool anyShown = false;
        // Iterate by catItems.size() — always parallel with FavoriteCategories.
        // If a category gets deleted from a section-header right-click during
        // the loop, the bound shrinks the same frame and we stop safely.
        size_t catN = std::min(catItems.size(), g_Settings.FavoriteCategories.size());
        // Defensive render-time clamp mirroring the toolbar slider's
        // per-mode bounds (same MinIconScaleForMode floor).
        float mainScale = std::max(g_Settings.MainIconScale,
                                   MinIconScaleForMode(g_Settings.ViewMode));

        for (size_t ci = 0; ci < catN; ++ci) {
            anyShown = true;
            float secTop = ImGui::GetCursorScreenPos().y;   // section top (for reorder zone)
            bool catCollapsed = RenderCategoryHeader((int)ci,
                g_Settings.FavoriteCategories[ci].Name.c_str(), searchActive);
            // Collapsed categories show only their header; skip the grid /
            // empty-state body. The if/else below is one guarded statement.
            if (!catCollapsed)
            if (catItems[ci].empty()) {
                // Empty category: the only insertion slot is gap 0. Draw the
                // placeholder text, then lay a full-width invisible button over
                // it as a forgiving drop target (the old version hit-tested the
                // text run itself, which was easy to miss). Reuses the same
                // blue insertion bar and ApplyEmoteDrop logic as the grid so
                // the two paths look and behave identically.
                ImVec2 textStart = ImGui::GetCursorPos();
                float  availW    = ImGui::GetContentRegionAvail().x;
                ImGui::PushTextWrapPos(0.0f);   // keep the placeholder inside the panel
                ImGui::TextDisabled("%s", catEmptyMsg(catTotal[ci]));
                ImGui::PopTextWrapPos();
                ImVec2 afterText = ImGui::GetCursorPos();

                ImGui::SetCursorPos(textStart);
                ImGui::PushID((int)ci);
                ImGui::InvisibleButton("##empty_drop",
                    ImVec2(std::max(1.f, availW), ImGui::GetTextLineHeight()));
                if (ImGui::BeginDragDropTarget()) {
                    // Only slot is gap 0; AcceptEmoteDropInto handles the no-op
                    // guard + delivery (same path the collapsed header uses).
                    if (AcceptEmoteDropInto((int)ci, 0)) {
                        ImVec2 mn = ImGui::GetItemRectMin();
                        ImVec2 mx = ImGui::GetItemRectMax();
                        DrawDropInsertionLine(mn.x + 1.5f, mn.y, mx.y);
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
                ImGui::SetCursorPos(afterText);
            } else {
                RenderEmoteSection(catItems[ci], /*allowReorder=*/true, (int)ci, g_Settings.ViewMode, mainScale);
            }
            catN = std::min(catItems.size(), g_Settings.FavoriteCategories.size());
            // Reorder target spanning this whole section (only while dragging a
            // category). Covers the gap below the last section too, so "drop at
            // the end" needs no separate zone.
            CategoryReorderZone((int)ci, secTop, ImGui::GetCursorScreenPos().y);
        }
        // Contextual empty-state line for a built-in section with nothing
        // visible. Four cases, checked in order; with the class filters as
        // independent toggles the last one is simply "hidden by the filter"
        // (if the relevant toggle were on and unfavorited emotes existed,
        // the section wouldn't be empty), so the old per-mode lines are gone.
        auto emptyMsgFor = [&](bool core, int total, int unfavTotal) -> const char* {
            if (total == 0)
                return core ? L("mp.core_none_flagged") : L("mp.unlock_none");
            if (unfavTotal == 0)
                return core ? L("mp.core_all_fav") : L("mp.unlock_all_fav");
            if (searchActive)
                return core ? L("mp.core_no_search") : L("mp.unlock_no_search");
            return core ? L("mp.core_filtered") : L("mp.unlock_filtered");
        };

        // Core / Unlockable sections always render their header (now collapsible)
        // so the user can see them as the built-in categories. anyShown tracks
        // content existence, not visibility, so collapsing a non-empty section
        // doesn't trip the "nothing matches" line below.
        bool coreCollapsed = SectionHeader(L("mp.core_emotes"), /*isUser=*/false,
                                           &g_Settings.MainCoreCollapsed, searchActive);
        if (!coreItems.empty()) anyShown = true;
        if (!coreCollapsed) {
            if (coreItems.empty())
                ImGui::TextDisabled("%s", emptyMsgFor(/*core=*/true, coreTotal, coreUnfavCount));
            else
                RenderEmoteSection(coreItems, /*allowReorder=*/false, -1, g_Settings.ViewMode, mainScale);
        }
        bool unlockCollapsed = SectionHeader(L("mp.unlockable_emotes"), /*isUser=*/false,
                                             &g_Settings.MainUnlockedCollapsed, searchActive);
        if (!unlockItems.empty()) anyShown = true;
        if (!unlockCollapsed) {
            if (unlockItems.empty())
                ImGui::TextDisabled("%s", emptyMsgFor(/*core=*/false, unlockTotal, unlockUnfavCount));
            else
                RenderEmoteSection(unlockItems, /*allowReorder=*/false, -1, g_Settings.ViewMode, mainScale);
        }
        // /me-motes built-in section ("/me-motes"). Header always renders so
        // users see the section even before they've added any /me-motes
        // (matches Core/Unlockable behavior). Empty-state branches mirror
        // the Core/Unlockable triad: no entries → "create some"; all
        // favorited → "all favorited" so users don't think they vanished.
        bool meCollapsed = SectionHeader(L("cat.me_motes"), /*isUser=*/false,
                                         &g_Settings.MainMeMotesCollapsed, searchActive);
        if (!meMoteItems.empty()) anyShown = true;
        if (!meCollapsed) {
            if (meMoteItems.empty()) {
                // Mirrors the Core/Unlockable empty-state quartet: none
                // exist → "create some"; everything exists but is favorited
                // → "all favorited"; search active and nothing matches →
                // "no /me-motes match the search"; the pill hid them →
                // "filtered out". Most-specific cause first.
                const char* msg;
                if (meMoteTotal == 0)           msg = L("mp.me_motes_none");
                else if (meMoteUnfavCount == 0) msg = L("mp.me_motes_all_fav");
                else if (searchActive)          msg = L("mp.me_motes_no_search");
                else                            msg = L("mp.me_motes_filtered");
                ImGui::TextDisabled("%s", msg);
            } else {
                RenderEmoteSection(meMoteItems, /*allowReorder=*/false, -1, g_Settings.ViewMode, mainScale);
            }
        }
        if (!anyShown) {
            ImGui::TextDisabled("%s", L("mp.no_match_filter"));
        }

        ImGui::EndChild();
        }  // end mp.draw scope
    }

    // Refusal line (replaces Nexus toasts): pops up at the click, drawn into the
    // content child's draw list (on top of the cells, but in the panel's window
    // z-order). Gated to main-panel-origin messages. The panel always has a
    // background, so no high-contrast strengthening needed.
    DrawFeedbackOverlay(FeedbackSurface::MainPanel, /*highContrast=*/false, mpContentDrawList);

    // Bottom-anchored "drop to remove from favorites" zone — only while a favorite
    // is being dragged. Drawn on the foreground list with a custom drop rect, so
    // it can't fall behind the panel in z-order. Must be inside the window.
    RenderRemoveTrashZone();

    ImGui::End();
}
