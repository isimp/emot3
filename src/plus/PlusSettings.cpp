#include "PlusSettings.h"

#ifdef EMOT3_PLUS

#include "JsonUtil.h"
#include "Logging.h"
#include "AtomicFile.h"  // AtomicWriteFile (crash-safe save - no live-file truncation)

#include <nlohmann/json.hpp>
#include <fstream>

PlusSettings g_PlusSettings;

namespace {
std::string s_path;  // stashed by LoadPlusSettings, reused by SavePlusSettings
}

void LoadPlusSettings(const std::string& path) {
    s_path = path;
    std::ifstream f(path);
    if (!f.is_open()) return;  // first run - keep struct defaults

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::parse_error& e) {
        LOG_WARNING("plus.json parse error at byte %zu: %s - using defaults",
                    (size_t)e.byte, e.what());
        return;
    }
    if (!j.is_object()) return;

    using namespace jsonutil;
    g_PlusSettings.QbClickThroughWheel =
        GetBool(j, "qb_click_through_wheel", g_PlusSettings.QbClickThroughWheel);
    g_PlusSettings.SwallowInputOnSend =
        GetBool(j, "swallow_input_on_send", g_PlusSettings.SwallowInputOnSend);
}

void SavePlusSettings() {
    if (s_path.empty()) return;  // never loaded (no addon dir) - nothing to write
    // Tiny file: nlohmann's pretty dump is fine (no hand-rolled writer needed,
    // unlike settings.json where we control the layout).
    nlohmann::json j;
    j["qb_click_through_wheel"] = g_PlusSettings.QbClickThroughWheel;
    j["swallow_input_on_send"]  = g_PlusSettings.SwallowInputOnSend;
    AtomicWriteFile(s_path,
                    j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
}

#endif  // EMOT3_PLUS
