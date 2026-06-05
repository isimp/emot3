#include "DevSettings.h"

#ifdef EMOT3_PLUS

#include "JsonUtil.h"
#include "Logging.h"

#include <nlohmann/json.hpp>
#include <fstream>

DevSettings g_DevSettings;

namespace {
std::string s_path;  // stashed by LoadDevSettings, reused by SaveDevSettings
}

void LoadDevSettings(const std::string& path) {
    s_path = path;
    std::ifstream f(path);
    if (!f.is_open()) return;  // first run - keep struct defaults

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::parse_error& e) {
        LOG_WARNING("dev.json parse error at byte %zu: %s - using defaults",
                    (size_t)e.byte, e.what());
        return;
    }
    if (!j.is_object()) return;

    using namespace jsonutil;
    g_DevSettings.QbClickThroughWheel =
        GetBool(j, "qb_click_through_wheel", g_DevSettings.QbClickThroughWheel);
    g_DevSettings.SwallowInputOnSend =
        GetBool(j, "swallow_input_on_send", g_DevSettings.SwallowInputOnSend);
}

void SaveDevSettings() {
    if (s_path.empty()) return;  // never loaded (no addon dir) - nothing to write
    std::ofstream f(s_path);
    if (!f.is_open()) {
        LOG_WARNING("Could not open %s for writing - dev settings not persisted",
                    s_path.c_str());
        return;
    }
    // Tiny dev-only file: nlohmann's pretty dump is fine (no hand-rolled writer
    // needed, unlike settings.json where we control the layout).
    nlohmann::json j;
    j["qb_click_through_wheel"] = g_DevSettings.QbClickThroughWheel;
    j["swallow_input_on_send"]  = g_DevSettings.SwallowInputOnSend;
    f << j.dump(2) << "\n";
}

#endif  // EMOT3_PLUS
