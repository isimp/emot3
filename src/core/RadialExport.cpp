#include "RadialExport.h"

#include "Globals.h"      // g_RadialsDir (+ Windows.h)
#include "Settings.h"     // g_Settings (QuickbarPreciseStateDetection)
#include "EmoteData.h"    // g_Emotes, g_EmotesMutex, FindEmote
#include "MeMotes.h"      // g_MeMotes, g_MeMotesMutex, FindMeMote, EMeMoteVariant
#include "EmoteBinds.h"   // EmoteBindIdentifier, SyncEmoteBinds
#include "StringUtil.h"   // SanitizeFilename, MakeUniqueSlug, ToLower
#include "AtomicFile.h"   // AtomicWriteFile (crash-safe temp+rename)
#include "Logging.h"
#include "Icons.h"        // ResolveEmoteIcon / ResolveMeMoteIcon + ResolvedIcon
#include "Resources.h"    // TryLoadBundledIconBytes (raw bundled PNG bytes)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// ============================================================================
// RadialMenus on-disk format constants. Validated against GW2-RadialMenus `main`
// (FormatRevision 2). These ints are POSITIONAL enum values in an undocumented
// format: if RadialMenus reorders an existing enumerator, packs written here point
// at the wrong thing with NO error. Re-run the schema spot-check (diff this writer's
// output against a freshly generated default pack) on every RadialMenus update. All
// format knowledge lives in THIS file so a format change is a one-file fix.
// ============================================================================
constexpr int      kFormatRevision   = 2;   // required: omit -> rev-1 migration mangles IsMounted=-1
constexpr int      kMenuTypeNormal   = 1;   // EMenuType: None=0, Normal=1, Small=2
constexpr int      kMenuTypeSmall    = 2;   // (verify against RadialMenus source on update)
constexpr int      kActionTypeBind   = 1;   // action Type: InputBind
constexpr int      kIconTypeFile     = 1;   // IconType: File
constexpr int      kIconTypeNone     = 0;   // IconType: None -> letter-fallback items
constexpr unsigned kColorOpaque      = 4294967295u;  // packed ABGR white (default 0 = transparent -> icon vanishes)
constexpr int      kObserveFalse     = 1;   // EObserveBoolean: Either=0, False=1, True=2
constexpr int      kObserveNotMounted = -1; // EObserveMount: NotMounted=-1, Either=0

int CapacityFor(const RadialWheelOptions& o) {
    return o.Small ? kRadialCapSmall : kRadialCapNormal;
}

// Write one resolved icon's bytes into the wheel's icons/ folder. Returns the bare
// filename (e.g. "emot3_greetings_emote_wave.png") to embed in IconValue, or "" for
// a letter-fallback entry (-> IconType:0). cat is "emote" / "memote".
std::string ExportItemIcon(const std::string& iconsDir, const std::string& slug,
                           const char* cat, const std::string& refId,
                           const ResolvedIcon& r) {
    if (r.from == ResolvedIcon::From::None) return "";

    std::string bytes;
    std::string ext = ".png";
    if (r.from == ResolvedIcon::From::BundledMem) {
        const void* data = nullptr; size_t size = 0;
        if (!TryLoadBundledIconBytes(r.table, r.count, r.name, data, size) || !data || !size)
            return "";
        bytes.assign(static_cast<const char*>(data), size);
    } else {  // DiskFile: copy bytes verbatim, preserving the source extension
        std::ifstream in(r.path, std::ios::binary);
        if (!in.is_open()) return "";
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (bytes.empty()) return "";
        size_t dot = r.path.find_last_of('.');
        if (dot != std::string::npos && r.path.size() - dot <= 5)
            ext = ToLower(r.path.substr(dot));
    }

    // Filenames embed <slug> so they stay self-identifying once copied into
    // RadialMenus' single shared icons/ folder (and don't collide across wheels).
    std::string fn = std::string("emot3_") + slug + "_" + cat + "_" +
                     SanitizeFilename(refId, "icon") + ext;
    std::string dest = iconsDir + "\\" + fn;
    if (!AtomicWriteFile(dest, bytes, /*binary=*/true)) {
        LOG_WARNING("radials: failed to write icon %s", dest.c_str());
        return "";  // fall back to a letter item rather than a dangling IconValue
    }
    return fn;
}

// Build one pack item: required Name/Color/ColorHover/IconType(+IconValue)/Actions,
// plus the gating Visibility when enabled. We emit ONLY required-or-changed keys;
// RadialMenus .value()-defaults everything else.
json BuildItem(const std::string& itemName, const std::string& identifier,
               const std::string& iconFile, const RadialWheelOptions& opt) {
    json it;
    it["Name"]       = itemName;
    it["Color"]      = kColorOpaque;   // required: default 0 = transparent -> icon vanishes
    it["ColorHover"] = kColorOpaque;
    if (iconFile.empty()) {
        it["IconType"] = kIconTypeNone;
    } else {
        it["IconType"]  = kIconTypeFile;
        it["IconValue"] = std::string("addons\\RadialMenus\\icons\\") + iconFile;  // deploy destination, game-root-relative
    }
    if (opt.GateByState) {
        json vis;
        vis["IsMounted"] = kObserveNotMounted;  // always (mounted is reliable everywhere)
        if (g_Settings.QuickbarPreciseStateDetection) {
            // Positional states are only reliable under precise (RTAPI) detection,
            // mirroring emot3's own gating.
            vis["IsUnderwater"]     = kObserveFalse;
            vis["IsOnWaterSurface"] = kObserveFalse;
            vis["IsAirborne"]       = kObserveFalse;
        }
        it["Visibility"] = std::move(vis);
    }
    json act;
    act["Type"]       = kActionTypeBind;
    act["Identifier"] = identifier;  // the Nexus bind name == our contract (carries variant)
    it["Actions"]     = json::array({ std::move(act) });
    return it;
}

// Write radials/packs/<slug>.json + radials/icons/emot3_<slug>_* for one wheel
// (the shared packs/ + icons/ layout that mirrors RadialMenus).
// `name` is the per-page pack Name shown in RadialMenus ("Greetings (1)"); `baseName`
// is the logical export name ("Greetings") stored in emot3_name so the grouped UI can
// recover it without parsing the "(N)" suffix.
bool WriteOneWheel(const std::string& slug, int id, const std::string& name,
                   const std::string& baseName,
                   const std::string& sourceCategory, const std::string& group,
                   int page, bool partial,
                   const std::vector<RadialItemRef>& items,
                   const RadialWheelOptions& opt) {
    if (g_RadialsDir.empty()) return false;
    std::string packsDir = RadialPacksDir();
    std::string iconsDir = RadialIconsDir();
    CreateDirectoryA(g_RadialsDir.c_str(), nullptr);
    CreateDirectoryA(packsDir.c_str(), nullptr);
    CreateDirectoryA(iconsDir.c_str(), nullptr);

    json j;
    j["FormatRevision"]        = kFormatRevision;                          // required
    j["ID"]                    = id;                                       // required
    j["Name"]                  = std::string(kRadialPackNamePrefix) + name; // required
    j["Type"]                  = opt.Small ? kMenuTypeSmall : kMenuTypeNormal;  // required: default None=0 -> 0 capacity
    j["SelectionMode"]         = opt.SelectionMode;                        // required: default None -> no commit path
    j["emot3_source_category"] = sourceCategory;                          // drift marker
    j["emot3_gate"]            = opt.GateByState;                         // recover the gate toggle on rescan
    j["emot3_group"]           = group;                                  // shared across a split's pages
    j["emot3_page"]            = page;                                   // 1-based page index
    j["emot3_name"]            = baseName;                               // logical export name (no "(N)")
    if (partial) j["emot3_partial"] = true;                              // subset: judge drift leniently
    if (opt.Scale != 1.0f)       j["Scale"]               = opt.Scale;      // user-changed only
    if (opt.IconScale != 1.0f)   j["IconScale"]           = opt.IconScale;
    if (opt.ShowItemNameTooltip) j["ShowItemNameTooltip"] = true;

    json arr = json::array();
    for (const auto& ref : items) {
        const std::string identifier = EmoteBindIdentifier(ref.Type, ref.Id, ref.Variant);
        std::string itemName, iconFile;
        if (ref.Type == EFavoriteRefType::Emote) {
            Emote e;
            {
                std::lock_guard<std::mutex> lk(g_EmotesMutex);
                const Emote* p = FindEmote(ref.Id);
                if (!p) continue;
                e = *p;
            }
            itemName = e.Name.empty() ? e.Id : e.Name;
            iconFile = ExportItemIcon(iconsDir, slug, "emote", ref.Id, ResolveEmoteIcon(e));
        } else {
            MeMote m;
            {
                std::lock_guard<std::mutex> lk(g_MeMotesMutex);
                const MeMote* p = FindMeMote(ref.Id);
                if (!p) continue;
                m = *p;
            }
            itemName = m.Name.empty() ? m.Id : m.Name;
            if      (ref.Variant == EMeMoteVariant::You) itemName += " (you)";
            else if (ref.Variant == EMeMoteVariant::All) itemName += " (all)";
            iconFile = ExportItemIcon(iconsDir, slug, "memote", ref.Id, ResolveMeMoteIcon(m));
        }
        arr.push_back(BuildItem(itemName, identifier, iconFile, opt));
    }
    const size_t nItems = arr.size();
    j["Items"] = std::move(arr);

    std::string body;
    try { body = j.dump(2, ' ', false, json::error_handler_t::replace) + "\n"; }
    catch (const std::exception& e) {
        LOG_WARNING("radials: serialize failed for %s: %s", slug.c_str(), e.what());
        return false;
    }
    std::string packPath = packsDir + "\\" + slug + ".json";
    if (!AtomicWriteFile(packPath, body, /*binary=*/true)) return false;
    LOG_INFO("radials: wrote wheel \"%s\" (id %d, %d items) -> %s",
             name.c_str(), id, (int)nItems, slug.c_str());
    return true;
}

// Allocate a unique page slug, skipping packs on disk and slugs reserved in this
// batch (page slugs are <group> for one page, <group>_<n> for a split).
std::string AllocSlug(const std::string& desired, const std::vector<std::string>& reserved) {
    std::string base = SanitizeFilename(desired, "wheel");
    return MakeUniqueSlug(base, [&](const std::string& s) {
        if (RadialSlugInUse(s)) return true;
        for (const auto& r : reserved) if (r == s) return true;
        return false;
    });
}

// Allocate a unique GROUP id from a name (free as both a group and a pack stem).
// `exclude` (the group being renamed) is treated as free, so a rename that re-slugs
// to the same id keeps it.
std::string AllocGroupSlug(const std::string& name, const std::string& exclude = "") {
    return MakeUniqueSlug(SanitizeFilename(name, "wheel"),
        [&](const std::string& s) {
            if (s == exclude) return false;
            return RadialGroupInUse(s) || RadialSlugInUse(s);
        });
}

// Is `pages`' union a full 1:1 default mirror of the category? If not it's a subset
// (drift judged leniently). Counts the distinct refs across all pages.
bool ComputePartial(const std::string& category,
                    const std::vector<std::vector<RadialItemRef>>& pages) {
    int fullCount = 0;
    for (const auto& fc : g_Settings.FavoriteCategories)
        if (fc.Name == category) { fullCount = (int)fc.Refs.size(); break; }
    size_t total = 0;
    for (const auto& pg : pages) {
        total += pg.size();
        for (const auto& it : pg)
            if (it.Variant != EMeMoteVariant::Default) return true;  // non-default variant
    }
    return (int)total != fullCount;
}

// Write all pages of a group `group` (already allocated/free). Rescans + binds.
RadialExportResult WriteGroupPages(const std::string& group, const std::string& baseName,
                                   const std::string& category,
                                   const std::vector<std::vector<RadialItemRef>>& pages,
                                   const RadialWheelOptions& options, bool partial) {
    RadialExportResult res;
    const bool multi = pages.size() > 1;
    std::vector<int>         reservedIds;
    std::vector<std::string> reservedSlugs;
    for (size_t p = 0; p < pages.size(); ++p) {
        const int page = (int)p + 1;
        const std::string desired = multi ? group + "_" + std::to_string(page) : group;
        const std::string slug    = AllocSlug(desired, reservedSlugs);
        const int         id      = NextFreeRadialId(reservedIds);
        const std::string name    = multi ? baseName + " (" + std::to_string(page) + ")"
                                          : baseName;
        if (!WriteOneWheel(slug, id, name, baseName, category, group, page, partial,
                           pages[p], options)) {
            res.error = "write failed";
            break;
        }
        reservedSlugs.push_back(slug);
        reservedIds.push_back(id);
        res.ids.push_back(id);
        res.names.push_back(std::string(kRadialPackNamePrefix) + name);
        ++res.wheelsWritten;
    }
    LoadRadialExports();  // rescan to pick up the new packs
    SyncEmoteBinds();     // register the new wheel refs' binds
    res.ok = res.wheelsWritten == (int)pages.size() && res.error.empty();
    return res;
}

}  // namespace

RadialExportResult ExportGroup(const std::string& sourceCategory,
                               const std::string& baseName,
                               const std::vector<std::vector<RadialItemRef>>& pages,
                               const RadialWheelOptions& options) {
    RadialExportResult res;
    if (g_RadialsDir.empty() || pages.empty()) { res.error = "nothing to export"; return res; }
    const bool partial = ComputePartial(sourceCategory, pages);
    const std::string group = AllocGroupSlug(baseName);
    RadialExportResult r = WriteGroupPages(group, baseName, sourceCategory, pages, options, partial);
    r.group = group;
    return r;
}

RadialExportResult EditGroup(const std::string& group, const std::string& sourceCategory,
                             const std::string& baseName,
                             const std::vector<std::vector<RadialItemRef>>& pages,
                             const RadialWheelOptions& options) {
    RadialExportResult res;
    if (g_RadialsDir.empty() || group.empty() || pages.empty()) {
        res.error = "nothing to export";
        return res;
    }
    const bool partial = ComputePartial(sourceCategory, pages);
    // Clear the group's current packs + icons, then rewrite fresh under the same group
    // (page menu ids reassigned). RemoveRadialGroup reads the page set in memory, so do
    // it before the rescan inside WriteGroupPages.
    RemoveRadialGroup(group);
    RadialExportResult r = WriteGroupPages(group, baseName, sourceCategory, pages, options, partial);
    r.group = group;
    return r;
}

bool RenameGroup(const std::string& group, const std::string& newName) {
    // Rewrite each page pack IN PLACE - same slug, id, group, items, options - only
    // the Name / emot3_name change. Keeping the slug + id stable means the staged
    // files don't move (no orphaning) and a deployed copy stays addressable by the
    // same filename + KB_RADIAL id, so it can be re-synced cleanly.
    std::vector<RadialExport> pagesIn = WheelsInGroup(group);
    if (pagesIn.empty()) return false;
    const bool multi = pagesIn.size() > 1;
    bool ok = true;
    for (const auto& pg : pagesIn) {
        const std::string name = multi ? newName + " (" + std::to_string(pg.Page) + ")"
                                       : newName;
        if (!WriteOneWheel(pg.Slug, pg.Id, name, newName, pg.SourceCategory, group,
                           pg.Page, pg.Partial, pg.Items, pg.Options))
            ok = false;
    }
    LoadRadialExports();  // refs unchanged -> no SyncEmoteBinds needed
    return ok;
}

void RetargetExportsCategory(const std::string& oldCategory,
                             const std::string& newCategory) {
    if (oldCategory.empty() || oldCategory == newCategory) return;
    // Page slugs whose pack points at the old category name.
    std::vector<std::string> slugs;
    {
        std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
        for (const auto& w : g_RadialExports)
            if (w.SourceCategory == oldCategory) slugs.push_back(w.Slug);
    }
    if (slugs.empty()) return;
    // Minimal field rewrite: just update emot3_source_category in each pack (the
    // marker is emot3-only, ignored by RadialMenus, so deployed copies need no change).
    const std::string packsDir = RadialPacksDir();
    int n = 0;
    for (const auto& slug : slugs) {
        std::string path = packsDir + "\\" + slug + ".json";
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) continue;
        json j;
        try { f >> j; } catch (const std::exception&) { f.close(); continue; }
        f.close();
        j["emot3_source_category"] = newCategory;
        std::string body = j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
        if (AtomicWriteFile(path, body, /*binary=*/true)) ++n;
    }
    LoadRadialExports();
    LOG_INFO("radials: retargeted %d pack(s) from category \"%s\" to \"%s\"",
             n, oldCategory.c_str(), newCategory.c_str());
}

bool RemoveGroup(const std::string& group) {
    bool ok = RemoveRadialGroup(group);
    LoadRadialExports();
    SyncEmoteBinds();  // drop the group's radial-only binds (personal binds persist)
    return ok;
}
