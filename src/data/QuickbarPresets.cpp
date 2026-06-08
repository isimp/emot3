#include "QuickbarPresets.h"
#include "JsonUtil.h"
#include "Globals.h"   // g_PresetsDir (+ Windows.h)
#include "QuickbarGeometry.h"  // g_QbWin*/g_QbApply*/g_QbGeometryValid (preset capture/apply)
#include "Logging.h"
#include "Resources.h" // kPresets bundled default-preset table
#include "StringUtil.h" // ToLower (shared helper)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

std::vector<QuickbarPreset> g_QuickbarPresets;

namespace {

std::string StemOf(const std::string& fn) {
    size_t dot = fn.find_last_of('.');
    return dot == std::string::npos ? fn : fn.substr(0, dot);
}

bool DirExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsurePresetsDir() {
    if (g_PresetsDir.empty()) return;
    if (!DirExists(g_PresetsDir)) CreateDirectoryA(g_PresetsDir.c_str(), nullptr);
}

// Find "<slug>.json" not colliding with another preset's file or an existing
// on-disk file. selfFile is excluded so re-saving in place doesn't rename. The
// `slug` comes from SanitizeFilename (core/StringUtil) - already non-empty,
// reserved-name-safe, [a-z0-9_]; here we just pick a non-colliding stem.
std::string UniqueFileName(const std::string& slug, const std::string& selfFile) {
    auto stemTaken = [&](const std::string& stem) -> bool {
        const std::string fn = stem + ".json";
        if (fn == selfFile) return false;
        for (const auto& p : g_QuickbarPresets)
            if (p.File == fn && p.File != selfFile) return true;
        DWORD attr = GetFileAttributesA((g_PresetsDir + "\\" + fn).c_str());
        return attr != INVALID_FILE_ATTRIBUTES;
    };
    return MakeUniqueSlug(slug, stemTaken) + ".json";
}

json PresetToJson(const QuickbarPreset& p) {
    json s;
    s["view_mode"]                      = (int)p.ViewMode;
    s["icon_scale"]                     = p.IconScale;
    s["use_dropdown"]                   = p.UseDropdown;
    s["show_category_bar"]              = p.ShowCategoryBar;
    s["horizontal_scroll"]              = p.HorizontalScroll;
    s["snap_window"]                    = p.SnapWindow;
    // snap_scroll: enum int (0=Off/1=Cells/2=Pages). Older bool-typed presets
    // still read via GetIntOrBool on parse (true -> 1, false -> 0).
    s["snap_scroll"]                    = (int)p.SnapScroll;
    s["allow_resize"]                   = p.AllowResize;
    s["allow_move"]                     = p.AllowMove;
    s["show_background"]                = p.ShowBg;
    s["show_title"]                     = p.ShowTitle;
    s["click_through"]                  = p.ClickThrough;
    s["high_contrast"]                  = p.HighContrast;
    s["scroll_indicator"]               = (int)p.ScrollIndicator;
    s["show_tooltips"]                  = p.ShowTooltips;
    s["wheel_cycles_category"]          = (int)p.WheelCycle;
    s["wheel_cycle_wrap"]               = p.WheelCycleWrap;
    s["scroll_wrap"]                    = p.ScrollWrap;
    s["close_on_esc"]                   = p.CloseOnEsc;
    s["combat_behavior"]                = (int)p.CombatBehavior;

    json j;
    j["version"]  = 1;   // on-disk preset protocol version (parser is tolerant)
    j["name"]     = p.Name;
    j["settings"] = std::move(s);
    if (p.HasGeometry) {
        json g;
        g["x"] = p.X; g["y"] = p.Y; g["w"] = p.W; g["h"] = p.H;
        j["geometry"] = std::move(g);
    }
    return j;
}

// Parse one preset object. Starts from struct defaults so a partial / hand-
// edited file fills missing keys sanely (same .value() pattern as Settings).
QuickbarPreset ParsePresetJson(const json& j) {
    using namespace jsonutil;
    QuickbarPreset p;
    p.Name = TruncateUtf8(GetString(j, "name", std::string()), kMaxNameBytes);

    // Typed getters fall back to the struct default on a missing OR wrong-typed
    // key (and never throw out of the loader, unlike nlohmann's own .value()).
    const json& s = GetObj(j, "settings");
    p.ViewMode         = NormalizeViewMode(GetInt(s, "view_mode", (int)p.ViewMode));
    p.IconScale        = GetFloat(s, "icon_scale",                     p.IconScale);
    p.UseDropdown      = GetBool (s, "use_dropdown",                   p.UseDropdown);
    p.ShowCategoryBar  = GetBool (s, "show_category_bar",              p.ShowCategoryBar);
    p.HorizontalScroll = GetBool (s, "horizontal_scroll",             p.HorizontalScroll);
    p.SnapWindow       = GetBool (s, "snap_window",                   p.SnapWindow);
    // Migrate the old bool form (true -> Cells, false -> Off) alongside reading
    // the new int form. NormalizeScrollSnap runs in SanitizeSettings on apply.
    p.SnapScroll       = (EQbScrollSnap)GetIntOrBool(s, "snap_scroll", (int)p.SnapScroll);
    p.AllowResize      = GetBool (s, "allow_resize",                  p.AllowResize);
    p.AllowMove        = GetBool (s, "allow_move",                    p.AllowMove);
    p.ShowBg           = GetBool (s, "show_background",               p.ShowBg);
    p.ShowTitle        = GetBool (s, "show_title",                    p.ShowTitle);
    p.ClickThrough     = GetBool (s, "click_through",                 p.ClickThrough);
    p.HighContrast     = GetBool (s, "high_contrast",                 p.HighContrast);
    p.ScrollIndicator  = NormalizeScrollIndicator(
                             GetInt(s, "scroll_indicator", (int)p.ScrollIndicator));
    p.ShowTooltips     = GetBool (s, "show_tooltips",                 p.ShowTooltips);
    // Value-bearing field: normalize so a hand-edited / out-of-range preset
    // can't carry a bad enum into g_Settings on apply (same discipline as
    // ViewMode/IconScale).
    p.WheelCycle       = NormalizeWheelCycle(GetInt(s, "wheel_cycles_category", (int)p.WheelCycle));
    p.WheelCycleWrap   = GetBool (s, "wheel_cycle_wrap",              p.WheelCycleWrap);
    p.ScrollWrap       = GetBool (s, "scroll_wrap",                   p.ScrollWrap);
    p.CloseOnEsc       = GetBool (s, "close_on_esc",                  p.CloseOnEsc);
    p.CombatBehavior   = NormalizeQbCombat(GetInt(s, "combat_behavior", (int)p.CombatBehavior));

    // Same [0.5, 2.5] envelope settings.json uses, so an applied preset can't
    // carry an out-of-range scale into g_Settings.
    if (p.IconScale < 0.5f) p.IconScale = 0.5f;
    else if (p.IconScale > 2.5f) p.IconScale = 2.5f;

    const json& g = GetObj(j, "geometry");
    if (!g.empty()) {
        p.HasGeometry = true;
        p.X = GetFloat(g, "x", 0.f);
        p.Y = GetFloat(g, "y", 0.f);
        p.W = GetFloat(g, "w", 0.f);
        p.H = GetFloat(g, "h", 0.f);
    }
    return p;
}

// === Included presets ==================================================
// Embedded defaults seeded to presets/ on first run (when the folder doesn't
// exist yet). After seeding they are ordinary editable/deletable files; we
// never re-seed while the folder exists, so a default the user deleted stays
// deleted.
//
// The defaults are bundled JSON files (resources/presets/*.json), exactly like
// the i18n and emote-locale tables - gen_rc.py packs them into the DLL as the
// kPresets RCDATA bucket. We load each, parse it, and run it through
// ParsePresetJson so the baked defaults get the SAME parse/clamp/normalize as a
// hand-edited file. To retune or add a default, edit/drop a file under
// resources/presets/ and rebuild - no code change here.
std::vector<QuickbarPreset> BuildDefaultPresets() {
    std::vector<QuickbarPreset> out;
    for (int i = 0; i < kPresetsCount; ++i) {
        const void* data = nullptr; size_t size = 0;
        if (!TryLoadBundledData(kPresets, kPresetsCount, kPresets[i].name,
                                data, size)) {
            LOG_WARNING("preset: bundled default '%s' missing from resources",
                        kPresets[i].name);
            continue;
        }
        try {
            json j = json::parse(static_cast<const char*>(data),
                                 static_cast<const char*>(data) + size);
            out.push_back(ParsePresetJson(j));
        } catch (const std::exception& e) {
            // A malformed bundled default is a packaging bug, not user data -
            // skip it (don't abort the whole seed) and log so it's caught in
            // testing.
            LOG_WARNING("preset: bundled default '%s' failed to parse: %s",
                        kPresets[i].name, e.what());
        }
    }
    return out;
}

void SeedDefaultPresets() {
    for (QuickbarPreset p : BuildDefaultPresets()) {
        p.File.clear();            // force a fresh slug-based filename
        WriteQuickbarPreset(p);
    }
}

} // namespace

void CaptureQuickbarPreset(QuickbarPreset& p) {
    const Settings& s = g_Settings;
    p.ViewMode         = s.QuickbarViewMode;
    p.IconScale        = s.QuickbarIconScale;
    p.UseDropdown      = s.QuickbarUseDropdown;
    p.ShowCategoryBar  = s.ShowQuickbarCategoryBar;
    p.HorizontalScroll = s.QuickbarHorizontalScroll;
    p.SnapWindow       = s.QuickbarSnapWindow;
    p.SnapScroll       = s.QuickbarSnapScroll;
    p.AllowResize      = s.QuickbarAllowResize;
    p.AllowMove        = s.QuickbarAllowMove;
    p.ShowBg           = s.ShowQuickbarBg;
    p.ShowTitle        = s.ShowQuickbarTitle;
    p.ClickThrough     = s.QuickbarClickThrough;
    p.HighContrast     = s.QuickbarHighContrast;
    p.ScrollIndicator  = s.QuickbarScrollIndicator;
    p.ShowTooltips     = s.ShowQuickbarTooltips;
    p.WheelCycle       = s.QuickbarWheelCycle;
    p.WheelCycleWrap   = s.QuickbarWheelCycleWrap;
    p.ScrollWrap       = s.QuickbarScrollWrap;
    p.CloseOnEsc       = s.QuickbarCloseOnEsc;
    p.CombatBehavior   = s.QuickbarCombatBehavior;
    if (g_QbGeometryValid) {
        p.HasGeometry = true;
        p.X = g_QbWinX; p.Y = g_QbWinY; p.W = g_QbWinW; p.H = g_QbWinH;
    }
}

void ApplyQuickbarPreset(const QuickbarPreset& p) {
    Settings& s = g_Settings;
    s.QuickbarViewMode                  = p.ViewMode;
    s.QuickbarIconScale                 = p.IconScale;
    s.QuickbarUseDropdown               = p.UseDropdown;
    s.ShowQuickbarCategoryBar           = p.ShowCategoryBar;
    s.QuickbarHorizontalScroll          = p.HorizontalScroll;
    s.QuickbarSnapWindow                = p.SnapWindow;
    s.QuickbarSnapScroll                = p.SnapScroll;
    s.QuickbarAllowResize               = p.AllowResize;
    s.QuickbarAllowMove                 = p.AllowMove;
    s.ShowQuickbarBg                    = p.ShowBg;
    s.ShowQuickbarTitle                 = p.ShowTitle;
    s.QuickbarClickThrough              = p.ClickThrough;
    s.QuickbarHighContrast              = p.HighContrast;
    s.QuickbarScrollIndicator           = p.ScrollIndicator;
    s.ShowQuickbarTooltips              = p.ShowTooltips;
    s.QuickbarWheelCycle                = p.WheelCycle;
    s.QuickbarWheelCycleWrap            = p.WheelCycleWrap;
    s.QuickbarScrollWrap                = p.ScrollWrap;
    s.QuickbarCloseOnEsc                = p.CloseOnEsc;
    s.QuickbarCombatBehavior            = p.CombatBehavior;
    if (p.HasGeometry) {
        g_QbApplyGeometry = true;
        g_QbApplyX = p.X; g_QbApplyY = p.Y; g_QbApplyW = p.W; g_QbApplyH = p.H;
    }
    // Applying a preset rewrites ~18 Quickbar settings at once - record which
    // one, since the individual setting-change TRACE lines don't fire here.
    LOG_INFO("preset: applied \"%s\" (geometry: %s)",
             p.Name.c_str(), p.HasGeometry ? "yes" : "no");
}

bool QuickbarPresetMatchesCurrent(const QuickbarPreset& p) {
    const Settings& s = g_Settings;
    bool settingsMatch =
        p.ViewMode         == s.QuickbarViewMode &&
        p.IconScale        == s.QuickbarIconScale &&
        p.UseDropdown      == s.QuickbarUseDropdown &&
        p.ShowCategoryBar  == s.ShowQuickbarCategoryBar &&
        p.HorizontalScroll == s.QuickbarHorizontalScroll &&
        p.SnapWindow       == s.QuickbarSnapWindow &&
        p.SnapScroll       == s.QuickbarSnapScroll &&
        p.AllowResize      == s.QuickbarAllowResize &&
        p.AllowMove        == s.QuickbarAllowMove &&
        p.ShowBg           == s.ShowQuickbarBg &&
        p.ShowTitle        == s.ShowQuickbarTitle &&
        p.ClickThrough     == s.QuickbarClickThrough &&
        p.HighContrast     == s.QuickbarHighContrast &&
        p.ScrollIndicator  == s.QuickbarScrollIndicator &&
        p.ShowTooltips     == s.ShowQuickbarTooltips &&
        p.WheelCycle       == s.QuickbarWheelCycle &&
        p.WheelCycleWrap   == s.QuickbarWheelCycleWrap &&
        p.ScrollWrap       == s.QuickbarScrollWrap &&
        p.CloseOnEsc       == s.QuickbarCloseOnEsc &&
        p.CombatBehavior   == s.QuickbarCombatBehavior;
    if (!settingsMatch) return false;

    // Geometry only participates when the preset has it AND the live window is
    // actually on screen to compare against. ~1px tolerance avoids sub-pixel
    // jitter flagging a freshly-applied preset as modified.
    if (p.HasGeometry && g_QbGeometryValid) {
        const float tol = 1.0f;
        if (std::fabs(p.X - g_QbWinX) > tol) return false;
        if (std::fabs(p.Y - g_QbWinY) > tol) return false;
        if (std::fabs(p.W - g_QbWinW) > tol) return false;
        if (std::fabs(p.H - g_QbWinH) > tol) return false;
    }
    return true;
}

bool WriteQuickbarPreset(QuickbarPreset& p) {
    if (g_PresetsDir.empty()) return false;
    EnsurePresetsDir();
    if (p.File.empty())
        p.File = UniqueFileName(SanitizeFilename(p.Name, "preset"), "");

    std::string full = g_PresetsDir + "\\" + p.File;
    try {
        std::ofstream f(full, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            LOG_WARNING("preset: cannot open %s for writing", full.c_str());
            return false;
        }
        f << PresetToJson(p).dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    } catch (const std::exception& e) {
        LOG_WARNING("preset: write failed for %s: %s", full.c_str(), e.what());
        return false;
    }
    LOG_INFO("preset: saved \"%s\" -> %s", p.Name.c_str(), p.File.c_str());
    return true;
}

bool RenameQuickbarPreset(QuickbarPreset& p, const std::string& newName) {
    if (p.Name != newName)
        LOG_INFO("preset: renamed \"%s\" -> \"%s\"", p.Name.c_str(), newName.c_str());
    p.Name = newName;
    // UniqueFileName excludes p.File, so an unchanged slug keeps the same file.
    std::string newFile = UniqueFileName(SanitizeFilename(newName, "preset"), p.File);
    if (newFile != p.File && !p.File.empty()) {
        std::string oldFile = p.File;
        p.File = newFile;
        if (!WriteQuickbarPreset(p)) return false;
        DeleteFileA((g_PresetsDir + "\\" + oldFile).c_str());
        return true;
    }
    p.File = newFile;          // covers the first-save (was empty) case
    return WriteQuickbarPreset(p);
}

bool DeleteQuickbarPresetFile(const QuickbarPreset& p) {
    if (p.File.empty() || g_PresetsDir.empty()) return false;
    std::string full = g_PresetsDir + "\\" + p.File;
    if (!DeleteFileA(full.c_str())) {
        // Treat an already-missing file as success; a real failure logs.
        if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
            LOG_WARNING("preset: delete failed for %s", full.c_str());
            return false;
        }
    }
    LOG_INFO("preset: deleted \"%s\" (%s)", p.Name.c_str(), p.File.c_str());
    return true;
}

bool QuickbarPresetNameExists(const std::string& name, int excludeIndex) {
    std::string lower = ToLower(name);
    for (int i = 0; i < (int)g_QuickbarPresets.size(); ++i) {
        if (i == excludeIndex) continue;
        if (ToLower(g_QuickbarPresets[i].Name) == lower) return true;
    }
    return false;
}

void LoadQuickbarPresets() {
    g_QuickbarPresets.clear();
    if (g_PresetsDir.empty()) return;

    bool firstRun = !DirExists(g_PresetsDir);
    EnsurePresetsDir();
    if (firstRun) SeedDefaultPresets();

    std::string pattern = g_PresetsDir + "\\*.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string fn   = fd.cFileName;
            std::string full = g_PresetsDir + "\\" + fn;
            std::ifstream f(full, std::ios::binary);
            if (!f.is_open()) continue;
            json j;
            try { f >> j; }
            catch (const json::parse_error& e) {
                LOG_WARNING("preset: %s parse error at byte %zu: %s",
                            fn.c_str(), (size_t)e.byte, e.what());
                continue;
            }
            if (!j.is_object()) {
                LOG_WARNING("preset: %s is not a JSON object - skipped", fn.c_str());
                continue;
            }
            QuickbarPreset p = ParsePresetJson(j);
            p.File = fn;
            if (p.Name.empty()) p.Name = StemOf(fn);  // fall back to the filename
            g_QuickbarPresets.push_back(std::move(p));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    std::sort(g_QuickbarPresets.begin(), g_QuickbarPresets.end(),
              [](const QuickbarPreset& a, const QuickbarPreset& b) {
                  return ToLower(a.Name) < ToLower(b.Name);
              });

    // Warn (don't mutate) on duplicate names: the NexusShortcut "apply preset by
    // name" submenu and the Update-matches guard key on the name, so two files
    // sharing one name make apply-by-name ambiguous. Sorted above, so any
    // duplicates are adjacent. Left to the user to resolve (rename one file).
    for (size_t i = 1; i < g_QuickbarPresets.size(); ++i) {
        if (ToLower(g_QuickbarPresets[i].Name) == ToLower(g_QuickbarPresets[i - 1].Name)) {
            LOG_WARNING("presets: duplicate name \"%s\" (%s, %s) - apply-by-name is ambiguous; rename one",
                        g_QuickbarPresets[i].Name.c_str(),
                        g_QuickbarPresets[i - 1].File.c_str(),
                        g_QuickbarPresets[i].File.c_str());
        }
    }

    LOG_INFO("presets: loaded %d", (int)g_QuickbarPresets.size());
}
