#include "SettingsMigrations.h"

#include "Logging.h"

using json = nlohmann::json;

namespace emot3::migrations {

namespace {

// Read the DOM's stored schema version, tolerating a missing / wrong-typed /
// out-of-range value - any of which means "predates versioning" = baseline 1.
int ReadVersion(const json& dom) {
    auto it = dom.find("version");
    if (it == dom.end() || !it->is_number_integer()) return 1;
    int v = it->get<int>();
    return v < 1 ? 1 : v;
}

}  // namespace

bool RunSettingsMigrations(json& dom) {
    if (!dom.is_object()) return false;  // caller guarantees this; belt + braces

    const int from = ReadVersion(dom);
    if (from >= kCurrentSchemaVersion) {
        // Already current, or a file from a future build - leave it as-is; the
        // tolerant field parser still reads whatever it understands.
        return false;
    }

    // Apply every step from the stored version onward. The fallthrough chain
    // keeps the order explicit and forces a compile-time reminder (add a case)
    // whenever kCurrentSchemaVersion grows.
    switch (from) {
        case 1:
            Migrate_001_NormalizeLegacyKeys(dom);
            [[fallthrough]];
        case 2:
            Migrate_002_UnusableSplit(dom);
            [[fallthrough]];
        default:
            break;
    }

    dom["version"] = kCurrentSchemaVersion;
    LOG_INFO("settings: migrated schema v%d -> v%d", from, kCurrentSchemaVersion);
    return true;
}

}  // namespace emot3::migrations
