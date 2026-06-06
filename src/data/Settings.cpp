#include "Settings.h"
#include "JsonUtil.h"
#include "Favorites.h"   // TrimName, reused by the sanitize pass
#include "I18n.h"        // AvailableUiLanguages, for UiLanguage validation
#include "Logging.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_set>

Settings g_Settings;

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
// Runtime state inspector section: high-signal live settings flags (the ones
// that change visible behaviour), so a "why is it doing X?" can be checked
// against the actual in-memory state. Self-registers via the Layer-2 standard.
static DevStateRegistrar s_settingsSection("Settings (key flags)", [] {
    const Settings& s = g_Settings;
    DevStateRow("show window",        "%s", s.ShowWindow ? "on" : "off");
    DevStateRow("show quickbar",      "%s", s.ShowQuickbar ? "on" : "off");
    DevStateRow("send on click",      "%s", s.SendOnClick ? "on" : "off");
    DevStateRow("grey unusable",      "%s", s.QuickbarGreyUnusable ? "on" : "off");
    DevStateRow("precise detection",  "%s", s.QuickbarPreciseStateDetection ? "on" : "off");
    DevStateRow("unusable behavior",  "%d", (int)s.QuickbarUnusableBehavior);
    DevStateRow("qb category idx",    "%d", s.QuickbarCategoryIdx);
    DevStateRow("unlock auto-sync",   "%s", s.UnlockAutoSync ? "on" : "off");
    DevStateRow("unlock key source",  "%d", s.UnlockApiKeySource);
    DevStateRow("ui language",        "%s", s.UiLanguage.c_str());
});
#endif  // EMOT3_DEVTOOLS

// --- JSON I/O -----------------------------------------------------------
//
// settings.json is a nested JSON object whose top-level keys mirror
// the Options tabs (main / quickbar / general) and whose nested keys
// mirror the section headers within each tab (Layout / Window / Look /
// Interaction in the Quickbar tab; nexus_shortcut under General). The
// two collections (manually-unlocked emote list, favorites categories)
// live at the top level since they aren't owned by any one Options tab.
//
// Schema:
//
//   {
//     "version": 1,
//     "main": {
//       "show": true,
//       "show_core": true,
//       "show_unlocked": true,
//       "show_locked": true,
//       "view_mode": 0,
//       "icon_scale": 1.0,
//       "core_collapsed": false, "unlocked_collapsed": false
//     },
//     "quickbar": {
//       "show": false,
//       "active_category_index": 0,
//       "layout": {
//         "view_mode": 1,
//         "icon_scale": 1.0,
//         "show_category_bar": true,
//         "use_dropdown": false,
//         "horizontal_scroll": false,
//         "snap_window": true,
//         "snap_scroll": 1,  // 0 off, 1 snap to cells, 2 snap to pages
//         "scroll_wrap": false
//       },
//       "window": {
//         "allow_resize": true,
//         "allow_move": true,
//         "show_background": true,
//         "click_through": false,
//         "show_title": true
//       },
//       "look": {
//         "high_contrast": false,
//         "scroll_indicator": 1,        // 0 off, 1 edge hints, 2 scrollbar
//         "show_tooltips": true
//       },
//       "interaction": {
//         "wheel_cycles_category": 1,   // 0 off, 1 over bar, 2 anywhere
//         "wheel_cycle_wrap": true,
//         "close_on_esc": false,
//         "combat_behavior": 0          // 0 off, 1 grey out, 2 hide
//       }
//     },
//     "general": {
//       "send_on_click": true,
//       "send_targetable_on_target": false,
//       "use_ai_icon_fallback": false,
//       "show_target_dot": true,
//       "quickbar_grey_unusable": true,
//       "notify_new_bundled_emotes": true,
//       "quickbar_categories": {
//         "favorites": true, "core": false, "mad_king": false,
//         "unlocked": false, "unlocked_all": true
//       },
//       "nexus_shortcut": {
//         "show": true,
//         "left_click_opens_quickbar": false
//       }
//     },
//     "language": { "ui": "auto" },
//     "unlock_tracking": {
//       "key_source": 0,            // 0 Hoard & Seek, 1 own key
//       "api_key": "",
//       "auto_sync": false
//     },
//     "unlocks": ["bow", "cheer"],
//     "known_bundled_emotes": ["bow", "cheer"],
//     "favorites": [
//       { "name": "Combat", "emotes": ["bow", "cheer"], "collapsed": false }
//     ]
//   }
//
// (emot3.md carries the authoritative full schema if this ever drifts.)
//
// Design notes:
//
//   - Nested groups beat the old `quickbar_xxx` prefix soup: the
//     on-disk shape reads the same way the Options panel does, so a
//     user editing settings.json by hand finds the same groups
//     they'd see in the UI.
//
//   - Numeric enum values (EViewMode) are preserved across changes -
//     re-shuffling would invalidate existing settings.json files for
//     no real benefit.
//
//   - No migration from older schemas. Userbase is small enough that
//     a one-time reset is cheaper than carrying a legacy reader.
//
//   - On load failure (file missing, malformed JSON, missing keys),
//     we leave g_Settings at its struct defaults - the in-memory
//     state is always a valid Settings either way.
//
// Parsing runs through nlohmann/json (single-header form vendored at
// vendor/nlohmann_json/). The writer is hand-rolled so we control the
// on-disk layout: scalars one-per-line, primitive arrays inline,
// favorites one-category-per-line. nlohmann's default dump(N) would
// explode primitive arrays into multi-line lists.

using json = nlohmann::json;

namespace {

// Read a JSON array of strings into a std::vector<std::string>.
// Non-string entries are silently skipped.
std::vector<std::string> readStringArray(const json& arr) {
    std::vector<std::string> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (const auto& item : arr) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

// Sanitize g_Settings after load. Clamps out-of-range scalars, normalizes
// enums, and cleans up the favorites list (empty/duplicate names, duplicate
// ids). Catalog-independent - the stale-id cross-check against g_Emotes lives
// in ReconcileFavoritesWithCatalog (Favorites.cpp), run later in AddonLoad once
// the emote catalog is loaded. Returns true if it changed anything, so the
// caller can re-save and heal the on-disk file.
bool SanitizeSettings(Settings& s) {
    bool changed = false;

    // Each correction logs at WARNING: the file is healed + re-saved on load, so
    // a hand edit (or a value from an older build) that got fixed is recorded.
    auto clampScale = [&](float& v, const char* name) {
        float c = v < 0.5f ? 0.5f : (v > 2.5f ? 2.5f : v);
        if (c != v) {
            LOG_WARNING("settings: %s %.2f out of range -> clamped to %.2f", name, v, c);
            v = c; changed = true;
        }
    };
    clampScale(s.MainIconScale,     "main.icon_scale");
    clampScale(s.QuickbarIconScale, "quickbar.icon_scale");

    // Filter toggles are plain bools - any stored value is valid, nothing
    // to clamp. (The old EEmoteFilter clamp lived here.)
    {
        EViewMode v = NormalizeViewMode((int)s.ViewMode);
        if (v != s.ViewMode) {
            LOG_WARNING("settings: main.view_mode %d invalid -> %d", (int)s.ViewMode, (int)v);
            s.ViewMode = v; changed = true;
        }
    }
    {
        EViewMode v = NormalizeViewMode((int)s.QuickbarViewMode);
        if (v != s.QuickbarViewMode) {
            LOG_WARNING("settings: quickbar.view_mode %d invalid -> %d",
                        (int)s.QuickbarViewMode, (int)v);
            s.QuickbarViewMode = v; changed = true;
        }
    }
    {
        EWheelCycle v = NormalizeWheelCycle((int)s.QuickbarWheelCycle);
        if (v != s.QuickbarWheelCycle) {
            LOG_WARNING("settings: quickbar.wheel_cycle %d invalid -> %d",
                        (int)s.QuickbarWheelCycle, (int)v);
            s.QuickbarWheelCycle = v; changed = true;
        }
    }
    {
        EUnusableBehavior v = NormalizeUnusableBehavior((int)s.QuickbarUnusableBehavior);
        if (v != s.QuickbarUnusableBehavior) {
            LOG_WARNING("settings: quickbar.unusable_behavior %d invalid -> %d",
                        (int)s.QuickbarUnusableBehavior, (int)v);
            s.QuickbarUnusableBehavior = v; changed = true;
        }
    }
    {
        EQbCombat v = NormalizeQbCombat((int)s.QuickbarCombatBehavior);
        if (v != s.QuickbarCombatBehavior) {
            LOG_WARNING("settings: quickbar.combat_behavior %d invalid -> %d",
                        (int)s.QuickbarCombatBehavior, (int)v);
            s.QuickbarCombatBehavior = v; changed = true;
        }
    }
    {
        EQbScrollIndicator v = NormalizeScrollIndicator((int)s.QuickbarScrollIndicator);
        if (v != s.QuickbarScrollIndicator) {
            LOG_WARNING("settings: quickbar.scroll_indicator %d invalid -> %d",
                        (int)s.QuickbarScrollIndicator, (int)v);
            s.QuickbarScrollIndicator = v; changed = true;
        }
    }
    {
        EQbScrollSnap v = NormalizeScrollSnap((int)s.QuickbarSnapScroll);
        if (v != s.QuickbarSnapScroll) {
            LOG_WARNING("settings: quickbar.snap_scroll %d invalid -> %d",
                        (int)s.QuickbarSnapScroll, (int)v);
            s.QuickbarSnapScroll = v; changed = true;
        }
    }

    // Unlock key-source enum is 0/1; clamp any out-of-range hand edit to 0.
    if (s.UnlockApiKeySource < 0 || s.UnlockApiKeySource > 1) {
        LOG_WARNING("settings: unlock key_source %d invalid -> 0", s.UnlockApiKeySource);
        s.UnlockApiKeySource = 0; changed = true;
    }

    // Active category index: clamp into range (0 when there are no
    // categories). The index spans the COMBINED Quickbar list - the favorites
    // categories (when shown) plus the enabled built-in (synthetic) ones - so
    // the bound must match what the bar actually lists, or a saved selection
    // would get clamped wrongly on every load. Mirrors the render-time clamp
    // in Quickbar.cpp.
    {
        int n = (s.QuickbarShowFavoriteCategories ? (int)s.FavoriteCategories.size() : 0)
              + (s.QuickbarShowCoreCategory        ? 1 : 0)
              + (s.QuickbarShowMadKingCategory     ? 1 : 0)
              + (s.QuickbarShowUnlockedCategory    ? 1 : 0)
              + (s.QuickbarShowUnlockedAllCategory ? 1 : 0);
        int ci = s.QuickbarCategoryIdx;
        if (n == 0)            ci = 0;
        else { if (ci < 0) ci = 0; else if (ci >= n) ci = n - 1; }
        if (ci != s.QuickbarCategoryIdx) {
            LOG_WARNING("settings: quickbar category index %d out of range [0,%d) -> %d",
                        s.QuickbarCategoryIdx, n, ci);
            s.QuickbarCategoryIdx = ci; changed = true;
        }
    }

    // UI language: empty -> "auto"; an unknown concrete code -> "auto" (it would
    // otherwise degrade to English at L() but leave the Options dropdown with
    // nothing selected).
    if (s.UiLanguage.empty()) { s.UiLanguage = "auto"; changed = true; }
    if (s.UiLanguage != "auto") {
        const auto& langs = AvailableUiLanguages();
        if (std::find(langs.begin(), langs.end(), s.UiLanguage) == langs.end()) {
            LOG_WARNING("settings: unknown UI language '%s' - using auto",
                        s.UiLanguage.c_str());
            s.UiLanguage = "auto"; changed = true;
        }
    }

    // Favorites: fix empty/duplicate category names and drop empty/duplicate
    // emote ids. Each emote is meant to live in exactly one category, so a
    // repeated id (within or across categories, from a hand edit) keeps its
    // first occurrence only.
    std::unordered_set<std::string> seenNames;
    std::unordered_set<std::string> seenIds;
    for (auto& cat : s.FavoriteCategories) {
        std::string nm = TrimName(cat.Name);
        if (nm.empty()) nm = "Favorites";
        if (seenNames.count(nm)) {
            std::string base = nm;
            int k = 2;
            while (seenNames.count(base + " " + std::to_string(k))) ++k;
            nm = base + " " + std::to_string(k);
        }
        if (nm != cat.Name) {
            LOG_WARNING("settings: favorites category \"%s\" -> \"%s\" (empty/duplicate name)",
                        cat.Name.c_str(), nm.c_str());
            cat.Name = nm; changed = true;
        }
        seenNames.insert(nm);

        std::vector<std::string> kept;
        kept.reserve(cat.Emotes.size());
        for (const auto& id : cat.Emotes) {
            if (id.empty() || seenIds.count(id)) { changed = true; continue; }
            seenIds.insert(id);
            kept.push_back(id);
        }
        if (kept.size() != cat.Emotes.size()) {
            LOG_WARNING("settings: favorites \"%s\" dropped %d empty/duplicate id(s)",
                        cat.Name.c_str(), (int)(cat.Emotes.size() - kept.size()));
            cat.Emotes = std::move(kept);
        }
    }

    return changed;
}

} // namespace

EViewMode NormalizeViewMode(int raw) {
    return (raw >= 0 && raw <= 3) ? (EViewMode)raw : EViewMode::Full;
}

EWheelCycle NormalizeWheelCycle(int raw) {
    return (raw >= 0 && raw <= 2) ? (EWheelCycle)raw : EWheelCycle::OverBar;
}

EUnusableBehavior NormalizeUnusableBehavior(int raw) {
    return (raw >= 0 && raw <= 1) ? (EUnusableBehavior)raw : EUnusableBehavior::Grey;
}

EQbCombat NormalizeQbCombat(int raw) {
    return (raw >= 0 && raw <= 2) ? (EQbCombat)raw : EQbCombat::Off;
}

EQbScrollIndicator NormalizeScrollIndicator(int raw) {
    return (raw >= 0 && raw <= 2) ? (EQbScrollIndicator)raw : EQbScrollIndicator::Scrollbar;
}

EQbScrollSnap NormalizeScrollSnap(int raw) {
    return (raw >= 0 && raw <= 2) ? (EQbScrollSnap)raw : EQbScrollSnap::Cells;
}

float MinIconScaleForMode(EViewMode mode) {
    // Full / TextOnly / Compact reserve vertical room for a label and break
    // below 1×; Icon has no label and scales freely from 0.5×.
    return (mode == EViewMode::Full ||
            mode == EViewMode::TextOnly ||
            mode == EViewMode::Compact) ? 1.0f : 0.5f;
}

bool LoadSettings(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_DEBUG("settings.json not present at %s - using defaults",
                  path.c_str());
        return false;
    }

    json j;
    try {
        f >> j;
    } catch (const json::parse_error& e) {
        LOG_WARNING("settings.json parse error at byte %zu: %s - using defaults",
                    (size_t)e.byte, e.what());
        return false;  // leave the malformed file as-is for the user to fix
    }

    if (!j.is_object()) {
        LOG_WARNING("settings.json at %s is not a JSON object - using defaults",
                    path.c_str());
        return false;
    }

    Settings& s = g_Settings;
    using namespace jsonutil;

    // Each typed getter falls back to the current struct value when the key
    // (or its parent block) is missing OR holds the wrong JSON type, so
    // partial / hand-edited files still load cleanly with sane defaults
    // filling the blanks - and a wrong-typed value can never throw out of the
    // loader (nlohmann's own .value() would). Range/enum correctness is a
    // second concern handled by SanitizeSettings below.

    const json& main = GetObj(j, "main");
    s.ShowWindow         = GetBool (main, "show",         s.ShowWindow);
    s.FilterShowCore     = GetBool (main, "show_core",     s.FilterShowCore);
    s.FilterShowUnlocked = GetBool (main, "show_unlocked", s.FilterShowUnlocked);
    s.FilterShowLocked   = GetBool (main, "show_locked",   s.FilterShowLocked);
    s.ViewMode      = (EViewMode)   GetInt(main, "view_mode", (int)s.ViewMode);
    s.MainIconScale = GetFloat(main, "icon_scale", s.MainIconScale);
    s.MainCoreCollapsed     = GetBool(main, "core_collapsed",     s.MainCoreCollapsed);
    s.MainUnlockedCollapsed = GetBool(main, "unlocked_collapsed", s.MainUnlockedCollapsed);

    const json& qb = GetObj(j, "quickbar");
    s.ShowQuickbar        = GetBool(qb, "show",                  s.ShowQuickbar);
    s.QuickbarCategoryIdx = GetInt (qb, "active_category_index", s.QuickbarCategoryIdx);

    const json& qbLayout = GetObj(qb, "layout");
    s.QuickbarViewMode         = (EViewMode)GetInt(qbLayout, "view_mode", (int)s.QuickbarViewMode);
    s.QuickbarIconScale        = GetFloat(qbLayout, "icon_scale",        s.QuickbarIconScale);
    s.ShowQuickbarCategoryBar  = GetBool (qbLayout, "show_category_bar", s.ShowQuickbarCategoryBar);
    s.QuickbarUseDropdown      = GetBool (qbLayout, "use_dropdown",      s.QuickbarUseDropdown);
    s.QuickbarHorizontalScroll = GetBool (qbLayout, "horizontal_scroll", s.QuickbarHorizontalScroll);
    s.QuickbarSnapWindow       = GetBool (qbLayout, "snap_window",       s.QuickbarSnapWindow);
    // snap_scroll migrated from bool (true/false) to int enum (0=Off/1=Cells/2=Pages).
    // GetIntOrBool accepts either form; SanitizeSettings runs NormalizeScrollSnap to clamp.
    s.QuickbarSnapScroll       = (EQbScrollSnap)GetIntOrBool(qbLayout, "snap_scroll",
                                                              (int)s.QuickbarSnapScroll);
    s.QuickbarScrollWrap       = GetBool (qbLayout, "scroll_wrap",       s.QuickbarScrollWrap);

    const json& qbWindow = GetObj(qb, "window");
    s.QuickbarAllowResize  = GetBool(qbWindow, "allow_resize",    s.QuickbarAllowResize);
    s.QuickbarAllowMove    = GetBool(qbWindow, "allow_move",      s.QuickbarAllowMove);
    s.ShowQuickbarBg       = GetBool(qbWindow, "show_background", s.ShowQuickbarBg);
    s.QuickbarClickThrough = GetBool(qbWindow, "click_through",   s.QuickbarClickThrough);
    s.ShowQuickbarTitle    = GetBool(qbWindow, "show_title",      s.ShowQuickbarTitle);

    const json& qbLook = GetObj(qb, "look");
    s.QuickbarHighContrast  = GetBool(qbLook, "high_contrast",  s.QuickbarHighContrast);
    // Raw int; SanitizeSettings runs NormalizeScrollIndicator to clamp it.
    s.QuickbarScrollIndicator = (EQbScrollIndicator)GetInt(qbLook, "scroll_indicator",
                                                           (int)s.QuickbarScrollIndicator);
    s.ShowQuickbarTooltips  = GetBool(qbLook, "show_tooltips",  s.ShowQuickbarTooltips);

    const json& qbInter = GetObj(qb, "interaction");
    // Raw int; SanitizeSettings runs NormalizeWheelCycle to clamp it.
    s.QuickbarWheelCycle = (EWheelCycle)GetInt(qbInter, "wheel_cycles_category",
                                               (int)s.QuickbarWheelCycle);
    s.QuickbarCloseOnEsc                = GetBool(qbInter, "close_on_esc",
                                                  s.QuickbarCloseOnEsc);
    s.QuickbarWheelCycleWrap            = GetBool(qbInter, "wheel_cycle_wrap",
                                                  s.QuickbarWheelCycleWrap);
    // Raw int; SanitizeSettings runs NormalizeQbCombat to clamp it.
    s.QuickbarCombatBehavior = (EQbCombat)GetInt(qbInter, "combat_behavior",
                                                 (int)s.QuickbarCombatBehavior);

    const json& general = GetObj(j, "general");
    s.SendOnClick       = GetBool(general, "send_on_click",        s.SendOnClick);
    s.CloseChatOnSend   = GetBool(general, "close_chat_on_send",   s.CloseChatOnSend);
    s.SendTargetableOnTarget = GetBool(general, "send_targetable_on_target", s.SendTargetableOnTarget);
    s.UseAIIconFallback = GetBool(general, "use_ai_icon_fallback", s.UseAIIconFallback);
    s.ShowTargetDot     = GetBool(general, "show_target_dot",       s.ShowTargetDot);
    s.QuickbarGreyUnusable = GetBool(general, "quickbar_grey_unusable", s.QuickbarGreyUnusable);
    s.QuickbarPreciseStateDetection = GetBool(general, "quickbar_precise_state",
                                              s.QuickbarPreciseStateDetection);
    // Raw int; SanitizeSettings runs NormalizeUnusableBehavior to clamp it.
    s.QuickbarUnusableBehavior = (EUnusableBehavior)GetInt(general, "quickbar_unusable_behavior",
                                                           (int)s.QuickbarUnusableBehavior);
    s.NotifyNewBundledEmotes   = GetBool(general, "notify_new_bundled_emotes",  s.NotifyNewBundledEmotes);

    const json& qbCats = GetObj(general, "quickbar_categories");
    s.QuickbarShowFavoriteCategories  = GetBool(qbCats, "favorites",    s.QuickbarShowFavoriteCategories);
    s.QuickbarShowCoreCategory        = GetBool(qbCats, "core",         s.QuickbarShowCoreCategory);
    s.QuickbarShowMadKingCategory     = GetBool(qbCats, "mad_king",     s.QuickbarShowMadKingCategory);
    s.QuickbarShowUnlockedCategory    = GetBool(qbCats, "unlocked",     s.QuickbarShowUnlockedCategory);
    s.QuickbarShowUnlockedAllCategory = GetBool(qbCats, "unlocked_all", s.QuickbarShowUnlockedAllCategory);

    const json& shortcut = GetObj(general, "nexus_shortcut");
    s.ShowNexusShortcut        = GetBool(shortcut, "show",                      s.ShowNexusShortcut);
    s.SwapShortcutClickActions = GetBool(shortcut, "left_click_opens_quickbar", s.SwapShortcutClickActions);

    const json& language = GetObj(j, "language");
    s.UiLanguage = GetString(language, "ui", s.UiLanguage);

    const json& unlockTracking = GetObj(j, "unlock_tracking");
    s.UnlockApiKeySource = GetInt(unlockTracking, "key_source",  s.UnlockApiKeySource);
    s.Gw2ApiKey          = GetString(unlockTracking, "api_key",  s.Gw2ApiKey);
    s.UnlockAutoSync     = GetBool(unlockTracking, "auto_sync",  s.UnlockAutoSync);

    if (j.contains("unlocks"))
        s.ManuallyUnlocked = readStringArray(GetArray(j, "unlocks"));

    if (j.contains("known_bundled_emotes"))
        s.KnownBundledEmotes = readStringArray(GetArray(j, "known_bundled_emotes"));

    {
        const json& favs = GetArray(j, "favorites");
        if (!favs.empty()) {
            s.FavoriteCategories.clear();
            s.FavoriteCategories.reserve(favs.size());
            for (const auto& catJ : favs) {
                if (!catJ.is_object()) continue;
                FavoriteCategory cat;
                cat.Name      = GetString(catJ, "name", std::string());
                cat.Emotes    = readStringArray(GetArray(catJ, "emotes"));
                cat.Collapsed = GetBool(catJ, "collapsed", false);
                s.FavoriteCategories.push_back(std::move(cat));
            }
        }
    }

    return SanitizeSettings(s);
}

void SaveSettings(const std::string& path) {
    std::ofstream f(path);
    if (!f.is_open()) {
        LOG_WARNING("Could not open %s for writing - settings not persisted",
                    path.c_str());
        return;
    }

    const Settings& s = g_Settings;

    // We still lean on nlohmann::json for string escaping - json(str)
    // .dump() returns the properly-quoted JSON form including the
    // outer quotes. Saves reimplementing escape rules.
    auto quoted = [](const std::string& v) {
        return json(v).dump();
    };
    auto B = [](bool v) { return v ? "true" : "false"; };

    f << "{\n";
    f << "  \"version\": 1,\n";

    // --- main ----------------------------------------------------------
    f << "  \"main\": {\n";
    f << "    \"show\": "          << B(s.ShowWindow)          << ",\n";
    f << "    \"show_core\": "     << B(s.FilterShowCore)      << ",\n";
    f << "    \"show_unlocked\": " << B(s.FilterShowUnlocked)  << ",\n";
    f << "    \"show_locked\": "   << B(s.FilterShowLocked)    << ",\n";
    f << "    \"view_mode\": "     << (int)s.ViewMode          << ",\n";
    f << "    \"icon_scale\": "    << s.MainIconScale          << ",\n";
    f << "    \"core_collapsed\": "     << B(s.MainCoreCollapsed)     << ",\n";
    f << "    \"unlocked_collapsed\": " << B(s.MainUnlockedCollapsed) << "\n";
    f << "  },\n";

    // --- quickbar ------------------------------------------------------
    f << "  \"quickbar\": {\n";
    f << "    \"show\": "                  << B(s.ShowQuickbar)         << ",\n";
    f << "    \"active_category_index\": " << s.QuickbarCategoryIdx     << ",\n";

    f << "    \"layout\": {\n";
    f << "      \"view_mode\": "         << (int)s.QuickbarViewMode      << ",\n";
    f << "      \"icon_scale\": "        << s.QuickbarIconScale          << ",\n";
    f << "      \"show_category_bar\": " << B(s.ShowQuickbarCategoryBar) << ",\n";
    f << "      \"use_dropdown\": "      << B(s.QuickbarUseDropdown)     << ",\n";
    f << "      \"horizontal_scroll\": " << B(s.QuickbarHorizontalScroll) << ",\n";
    f << "      \"snap_window\": "       << B(s.QuickbarSnapWindow)      << ",\n";
    f << "      \"snap_scroll\": "       << (int)s.QuickbarSnapScroll    << ",\n";
    f << "      \"scroll_wrap\": "       << B(s.QuickbarScrollWrap)      << "\n";
    f << "    },\n";

    f << "    \"window\": {\n";
    f << "      \"allow_resize\": "    << B(s.QuickbarAllowResize)  << ",\n";
    f << "      \"allow_move\": "      << B(s.QuickbarAllowMove)    << ",\n";
    f << "      \"show_background\": " << B(s.ShowQuickbarBg)       << ",\n";
    f << "      \"click_through\": "   << B(s.QuickbarClickThrough) << ",\n";
    f << "      \"show_title\": "      << B(s.ShowQuickbarTitle)    << "\n";
    f << "    },\n";

    f << "    \"look\": {\n";
    f << "      \"high_contrast\": "  << B(s.QuickbarHighContrast)  << ",\n";
    f << "      \"scroll_indicator\": " << (int)s.QuickbarScrollIndicator << ",\n";
    f << "      \"show_tooltips\": "  << B(s.ShowQuickbarTooltips)    << "\n";
    f << "    },\n";

    f << "    \"interaction\": {\n";
    f << "      \"wheel_cycles_category\": " << (int)s.QuickbarWheelCycle << ",\n";
    f << "      \"wheel_cycle_wrap\": "               << B(s.QuickbarWheelCycleWrap)            << ",\n";
    f << "      \"close_on_esc\": "                   << B(s.QuickbarCloseOnEsc)                << ",\n";
    f << "      \"combat_behavior\": "                << (int)s.QuickbarCombatBehavior          << "\n";
    f << "    }\n";
    f << "  },\n";

    // --- general -------------------------------------------------------
    f << "  \"general\": {\n";
    f << "    \"send_on_click\": "         << B(s.SendOnClick)       << ",\n";
    f << "    \"close_chat_on_send\": "    << B(s.CloseChatOnSend)   << ",\n";
    f << "    \"send_targetable_on_target\": " << B(s.SendTargetableOnTarget) << ",\n";
    f << "    \"use_ai_icon_fallback\": "  << B(s.UseAIIconFallback) << ",\n";
    f << "    \"show_target_dot\": "        << B(s.ShowTargetDot)     << ",\n";
    f << "    \"quickbar_grey_unusable\": " << B(s.QuickbarGreyUnusable) << ",\n";
    f << "    \"quickbar_precise_state\": " << B(s.QuickbarPreciseStateDetection) << ",\n";
    f << "    \"quickbar_unusable_behavior\": " << (int)s.QuickbarUnusableBehavior << ",\n";
    f << "    \"notify_new_bundled_emotes\": "  << B(s.NotifyNewBundledEmotes)   << ",\n";
    f << "    \"quickbar_categories\": {\n";
    f << "      \"favorites\": "    << B(s.QuickbarShowFavoriteCategories)  << ",\n";
    f << "      \"core\": "         << B(s.QuickbarShowCoreCategory)        << ",\n";
    f << "      \"mad_king\": "     << B(s.QuickbarShowMadKingCategory)     << ",\n";
    f << "      \"unlocked\": "     << B(s.QuickbarShowUnlockedCategory)    << ",\n";
    f << "      \"unlocked_all\": " << B(s.QuickbarShowUnlockedAllCategory) << "\n";
    f << "    },\n";
    f << "    \"nexus_shortcut\": {\n";
    f << "      \"show\": "                      << B(s.ShowNexusShortcut)        << ",\n";
    f << "      \"left_click_opens_quickbar\": " << B(s.SwapShortcutClickActions) << "\n";
    f << "    }\n";
    f << "  },\n";

    // --- language (UI language; emote language lives in emotes.json) ---
    f << "  \"language\": {\n";
    f << "    \"ui\": " << quoted(s.UiLanguage) << "\n";
    f << "  },\n";

    // --- unlock_tracking (Unlocks tab) --------------------------------
    f << "  \"unlock_tracking\": {\n";
    f << "    \"key_source\": "    << s.UnlockApiKeySource      << ",\n";
    f << "    \"api_key\": "       << quoted(s.Gw2ApiKey)       << ",\n";
    f << "    \"auto_sync\": "     << B(s.UnlockAutoSync)       << "\n";
    f << "  },\n";

    // --- unlocks (inline array) ---------------------------------------
    f << "  \"unlocks\": [";
    for (size_t k = 0; k < s.ManuallyUnlocked.size(); ++k) {
        if (k) f << ", ";
        f << quoted(s.ManuallyUnlocked[k]);
    }
    f << "],\n";

    // --- known_bundled_emotes (notifier snapshot, inline array) -------
    f << "  \"known_bundled_emotes\": [";
    for (size_t k = 0; k < s.KnownBundledEmotes.size(); ++k) {
        if (k) f << ", ";
        f << quoted(s.KnownBundledEmotes[k]);
    }
    f << "],\n";

    // --- favorites (one category per line, emotes inline) -------------
    f << "  \"favorites\": [";
    for (size_t c = 0; c < s.FavoriteCategories.size(); ++c) {
        const auto& cat = s.FavoriteCategories[c];
        if (c) f << ",";
        f << "\n    { \"name\": " << quoted(cat.Name) << ", \"emotes\": [";
        for (size_t k = 0; k < cat.Emotes.size(); ++k) {
            if (k) f << ", ";
            f << quoted(cat.Emotes[k]);
        }
        f << "], \"collapsed\": " << B(cat.Collapsed) << " }";
    }
    if (!s.FavoriteCategories.empty()) f << "\n  ";
    f << "]\n";

    f << "}\n";
}
