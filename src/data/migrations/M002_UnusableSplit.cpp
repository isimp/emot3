#include "Migrations.h"

#include "Settings.h"  // EUnusableBehavior::Normal enum value

using json = nlohmann::json;

namespace emot3::migrations {

// v2 -> v3: split the old single "block unusable emotes" Quickbar setting into
// a GLOBAL send-gate concern (the master + its detection sub-toggles, which
// govern the send refusal on every surface and the radial export) and a
// PER-PRESET display choice (how the Quickbar shows a blocked emote).
//
// Old (general):
//   quickbar_grey_unusable      (bool) - master: refuse sends + drive QB display
//   quickbar_airborne           (bool) - detection sub-toggle
//   quickbar_precise_state      (bool) - detection sub-toggle
//   quickbar_unusable_behavior  (int)  - QB display Grey(0) / Hide(1)
//
// New:
//   general.block_unusable           (bool) <- quickbar_grey_unusable
//   general.block_while_airborne     (bool) <- quickbar_airborne
//   general.precise_state_detection  (bool) <- quickbar_precise_state
//   quickbar.look.unusable_display   (int)  <- master ? old_behavior : Normal(2)
//
// The display value lands in quickbar.look as the ACTIVE copy; it is now also a
// per-preset field. Old presets (no key) inherit this migrated active value via
// ParsePresetJson's default, so no preset files are rewritten here. Mapping the
// old "master off" state to Normal(2) ("show as usual") preserves behaviour: no
// blocking, no greying.
void Migrate_002_UnusableSplit(json& dom) {
    auto general = dom.find("general");
    if (general == dom.end() || !general->is_object()) return;
    json& g = *general;

    // Tolerant readers for the old keys; defaults match the struct defaults
    // (blocking on, behaviour Grey) so a partial file migrates sanely.
    auto oldBool = [&](const char* k, bool def) -> bool {
        auto it = g.find(k);
        return (it != g.end() && it->is_boolean()) ? it->get<bool>() : def;
    };
    auto oldInt = [&](const char* k, int def) -> int {
        auto it = g.find(k);
        return (it != g.end() && it->is_number_integer()) ? it->get<int>() : def;
    };

    // Don't clobber a DOM that already carries the new shape (idempotent re-run).
    if (!g.contains("block_unusable")) {
        const bool master   = oldBool("quickbar_grey_unusable", true);
        const bool airborne = oldBool("quickbar_airborne",      true);
        const bool precise  = oldBool("quickbar_precise_state", true);
        const int  behavior = oldInt ("quickbar_unusable_behavior", 0);  // 0 Grey / 1 Hide

        g["block_unusable"]          = master;
        g["block_while_airborne"]    = airborne;
        g["precise_state_detection"] = precise;

        const int display = master ? behavior : (int)EUnusableBehavior::Normal;
        json& qb = dom["quickbar"];
        if (!qb.is_object()) qb = json::object();
        json& look = qb["look"];
        if (!look.is_object()) look = json::object();
        if (!look.contains("unusable_display"))
            look["unusable_display"] = display;
    }

    // Retire the old keys regardless - dead in v3+.
    g.erase("quickbar_grey_unusable");
    g.erase("quickbar_airborne");
    g.erase("quickbar_precise_state");
    g.erase("quickbar_unusable_behavior");
}

}  // namespace emot3::migrations
