#include "SettingsMigrations.h"

#include "Settings.h"  // EQbScrollSnap / EShortcutClick enum values

#include <string>

using json = nlohmann::json;

namespace emot3::migrations {

// v1 -> v2: fold the three formerly-inline tolerant readers in
// data/Settings.cpp into the migration layer, so the field parser reads only
// the current schema. Each transform is guarded so a DOM already in the new
// shape passes through untouched (these run on EVERY pre-v2 file, including
// current-schema files still stamped v1).
//
//   1. quickbar.layout.snap_scroll : bool -> int enum (true -> Cells(1),
//      false -> Off(0)).
//   2. general.nexus_shortcut       : legacy bool "left_click_opens_quickbar"
//      -> the 3-way int "left_click_opens" (true -> Quickbar, else Library);
//      the legacy key is dropped.
//   3. favorites[].emotes (string array, all Emote-typed) -> favorites[].refs
//      (array of {type:0, id}); only when "refs" is absent/empty. "emotes" is
//      dropped either way.
void Migrate_001_NormalizeLegacyKeys(json& dom) {
    // (1) snap_scroll bool -> int enum
    auto qb = dom.find("quickbar");
    if (qb != dom.end() && qb->is_object()) {
        auto layout = qb->find("layout");
        if (layout != qb->end() && layout->is_object()) {
            auto ss = layout->find("snap_scroll");
            if (ss != layout->end() && ss->is_boolean()) {
                (*layout)["snap_scroll"] = ss->get<bool>()
                    ? (int)EQbScrollSnap::Cells
                    : (int)EQbScrollSnap::Off;
            }
        }
    }

    // (2) left_click_opens_quickbar (legacy bool) -> left_click_opens (int)
    auto general = dom.find("general");
    if (general != dom.end() && general->is_object()) {
        auto sc = general->find("nexus_shortcut");
        if (sc != general->end() && sc->is_object()) {
            auto legacy = sc->find("left_click_opens_quickbar");
            if (legacy != sc->end()) {
                const bool wantsQb = legacy->is_boolean() && legacy->get<bool>();
                if (!sc->contains("left_click_opens"))
                    (*sc)["left_click_opens"] = wantsQb
                        ? (int)EShortcutClick::Quickbar
                        : (int)EShortcutClick::Library;
                sc->erase("left_click_opens_quickbar");
            }
        }
    }

    // (3) favorites[].emotes -> favorites[].refs
    auto favs = dom.find("favorites");
    if (favs != dom.end() && favs->is_array()) {
        for (auto& cat : *favs) {
            if (!cat.is_object()) continue;
            auto emotes = cat.find("emotes");
            if (emotes == cat.end() || !emotes->is_array()) continue;
            auto refs = cat.find("refs");
            const bool refsPresent =
                refs != cat.end() && refs->is_array() && !refs->empty();
            if (!refsPresent) {
                json arr = json::array();
                for (const auto& e : *emotes) {
                    if (!e.is_string()) continue;
                    std::string id = e.get<std::string>();
                    if (id.empty()) continue;
                    arr.push_back(json{ {"type", 0}, {"id", id} });  // 0 = Emote
                }
                cat["refs"] = std::move(arr);
            }
            cat.erase("emotes");
        }
    }
}

}  // namespace emot3::migrations
