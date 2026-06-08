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

// Write radials/<slug>/<slug>.json + radials/<slug>/icons/* for one wheel.
bool WriteOneWheel(const std::string& slug, int id, const std::string& name,
                   const std::string& sourceCategory, bool partial,
                   const std::vector<RadialItemRef>& items,
                   const RadialWheelOptions& opt) {
    if (g_RadialsDir.empty()) return false;
    std::string wheelDir = g_RadialsDir + "\\" + slug;
    std::string iconsDir = wheelDir + "\\icons";
    CreateDirectoryA(g_RadialsDir.c_str(), nullptr);
    CreateDirectoryA(wheelDir.c_str(), nullptr);
    CreateDirectoryA(iconsDir.c_str(), nullptr);

    json j;
    j["FormatRevision"]        = kFormatRevision;                          // required
    j["ID"]                    = id;                                       // required
    j["Name"]                  = std::string(kRadialPackNamePrefix) + name; // required
    j["Type"]                  = opt.Small ? kMenuTypeSmall : kMenuTypeNormal;  // required: default None=0 -> 0 capacity
    j["SelectionMode"]         = opt.SelectionMode;                        // required: default None -> no commit path
    j["emot3_source_category"] = sourceCategory;                          // drift marker
    j["emot3_gate"]            = opt.GateByState;                         // recover the gate toggle on rescan
    if (partial) j["emot3_partial"] = true;                              // subset / split: judge drift leniently
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
    std::string packPath = wheelDir + "\\" + slug + ".json";
    if (!AtomicWriteFile(packPath, body, /*binary=*/true)) return false;
    LOG_INFO("radials: wrote wheel \"%s\" (id %d, %d items) -> %s",
             name.c_str(), id, (int)nItems, slug.c_str());
    return true;
}

// Allocate a unique slug from a name, skipping on-disk subfolders and slugs already
// reserved in this batch.
std::string AllocSlug(const std::string& name, const std::vector<std::string>& reserved) {
    std::string base = SanitizeFilename(name, "wheel");
    return MakeUniqueSlug(base, [&](const std::string& s) {
        if (RadialSlugInUse(s)) return true;
        for (const auto& r : reserved) if (r == s) return true;
        return false;
    });
}

}  // namespace

RadialExportResult ExportCategoryAsWheels(const std::string& sourceCategory,
                                          const std::string& baseName,
                                          const std::vector<RadialItemRef>& items,
                                          const RadialWheelOptions& options,
                                          bool autoSplit) {
    RadialExportResult res;
    if (g_RadialsDir.empty()) { res.error = "radials dir unset"; return res; }
    const int cap = CapacityFor(options);

    // Is this a full mirror of the category, or a deliberate subset? Full = exactly
    // one item per category ref, all Default variant. Anything else (fewer items, a
    // non-Default /me-mote variant, a duplicate, or an auto-split below) is partial,
    // which the status drift-check then judges leniently.
    int fullCount = 0;
    for (const auto& fc : g_Settings.FavoriteCategories)
        if (fc.Name == sourceCategory) { fullCount = (int)fc.Refs.size(); break; }
    bool partialSel = ((int)items.size() != fullCount);
    if (!partialSel)
        for (const auto& it : items)
            if (it.Variant != EMeMoteVariant::Default) { partialSel = true; break; }

    // Partition into wheels: split into cap-sized chunks when over capacity and
    // autoSplit, else a single (defensively truncated) wheel.
    std::vector<std::vector<RadialItemRef>> chunks;
    if (autoSplit && (int)items.size() > cap) {
        for (size_t i = 0; i < items.size(); i += cap)
            chunks.emplace_back(items.begin() + i,
                                items.begin() + std::min(items.size(), i + (size_t)cap));
    } else {
        std::vector<RadialItemRef> one = items;
        if ((int)one.size() > cap) one.resize(cap);  // defensive; wizard prevents this
        chunks.push_back(std::move(one));
    }

    const bool multi = chunks.size() > 1;
    std::vector<int>         reservedIds;
    std::vector<std::string> reservedSlugs;
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        const std::string name = multi ? baseName + " (" + std::to_string(ci + 1) + ")"
                                       : baseName;
        const std::string slug = AllocSlug(name, reservedSlugs);
        const int         id   = NextFreeRadialId(reservedIds);
        const bool        partial = multi || partialSel;  // each split piece is partial too
        if (!WriteOneWheel(slug, id, name, sourceCategory, partial, chunks[ci], options)) {
            res.error = "write failed";
            break;
        }
        reservedSlugs.push_back(slug);
        reservedIds.push_back(id);
        res.ids.push_back(id);
        res.names.push_back(std::string(kRadialPackNamePrefix) + name);
        ++res.wheelsWritten;
    }

    LoadRadialExports();  // rescan to pick up the new subfolder(s)
    SyncEmoteBinds();     // register the new wheel refs' binds
    res.ok = res.wheelsWritten > 0 && res.error.empty();
    return res;
}

bool ReExportWheel(const std::string& slug, int id, const std::string& name,
                   const std::string& sourceCategory, bool partial,
                   const std::vector<RadialItemRef>& items,
                   const RadialWheelOptions& options) {
    // Clear stale pack + icons (a removed ref's icon would otherwise linger), then
    // rewrite from scratch under the same slug + id.
    RemoveRadialDir(slug);
    bool ok = WriteOneWheel(slug, id, name, sourceCategory, partial, items, options);
    LoadRadialExports();
    SyncEmoteBinds();
    return ok;
}

bool RenameWheel(const std::string& oldSlug, const std::string& newName) {
    // Snapshot the existing wheel from the in-memory record.
    RadialExport w;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
        for (const auto& e : g_RadialExports)
            if (e.Slug == oldSlug) { w = e; found = true; break; }
    }
    if (!found) return false;

    const std::string newSlug = MakeUniqueSlug(
        SanitizeFilename(newName, "wheel"),
        [&](const std::string& s) { return s != oldSlug && RadialSlugInUse(s); });

    if (newSlug == oldSlug) {
        // Same slug -> rewrite the pack Name in place (refs unchanged, no bind sync).
        bool ok = WriteOneWheel(oldSlug, w.Id, newName, w.SourceCategory, w.Partial,
                                w.Items, w.Options);
        LoadRadialExports();
        return ok;
    }
    bool ok = WriteOneWheel(newSlug, w.Id, newName, w.SourceCategory, w.Partial,
                            w.Items, w.Options);
    if (ok) RemoveRadialDir(oldSlug);
    LoadRadialExports();
    SyncEmoteBinds();
    return ok;
}

bool RemoveWheel(const std::string& slug) {
    bool ok = RemoveRadialDir(slug);
    LoadRadialExports();
    SyncEmoteBinds();  // drop the wheel's radial-only binds (user binds persist)
    return ok;
}
