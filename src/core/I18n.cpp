#include "I18n.h"
#include "Globals.h"
#include "Logging.h"
#include "Resources.h"

#include "imgui/imgui.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// Nexus' Localization store is a single namespace shared by every addon,
// so our short keys ("common.clear", "filter.core", ...) would collide
// with any other addon that registered the same id - last writer wins,
// and in Auto mode we'd then serve their string. We namespace every
// id we hand to Nexus with this prefix; our JSON files and all L("...")
// call sites keep using the short keys (s_english / s_cache are keyed by
// the short form). Only the Nexus-facing identifier carries the prefix.
constexpr const char* kNsPrefix = "emot3.";
std::string NsId(const char* key) { return std::string(kNsPrefix) + key; }

// English ground truth: every key -> English text. Populated from
// en.json at init and used as the guaranteed fallback whenever Nexus has
// no translation for the active language.
std::map<std::string, std::string> s_english;

// code -> human display name (from each file's "_lang" field).
std::map<std::string, std::string> s_displayNames;

// Available language codes (file stems), in the order kI18nFiles lists
// them. Includes "en".
std::vector<std::string> s_available;

// Active mode: "auto" or a concrete code. Default follows Nexus.
std::string s_mode = "auto";

// Per-key cache of the resolved string for the active language. Gives L()
// a stable, distinct pointer per key so several L() calls in one
// expression (e.g. a combo's item array) never alias a shared buffer -
// which Nexus' Translate might otherwise hand back. Node-based map, so
// inserting new keys never invalidates other keys' c_str().
std::map<std::string, std::string> s_cache;

// Parse one bundled i18n file's bytes and register its entries with
// Nexus. Keys beginning with '_' are metadata (e.g. "_lang", "_note")
// and are not translation entries.
void LoadOneLanguage(const std::string& code, const void* data, size_t size) {
    json j;
    try {
        j = json::parse(static_cast<const char*>(data),
                        static_cast<const char*>(data) + size);
    } catch (const json::parse_error& e) {
        LOG_WARNING("i18n: %s.json parse error at byte %zu: %s",
                    code.c_str(), (size_t)e.byte, e.what());
        return;
    }
    if (!j.is_object()) {
        LOG_WARNING("i18n: %s.json is not a JSON object - skipped", code.c_str());
        return;
    }

    int entries = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        if (!key.empty() && key.front() == '_') {
            if (key == "_lang" && it.value().is_string())
                s_displayNames[code] = it.value().get<std::string>();
            continue;
        }
        if (!it.value().is_string()) continue;
        std::string val = it.value().get<std::string>();
        // Register with Nexus under the namespaced id so Translate/
        // TranslateTo can resolve it without colliding with other addons.
        if (APIDefs && APIDefs->Localization.Set)
            APIDefs->Localization.Set(NsId(key.c_str()).c_str(),
                                      code.c_str(), val.c_str());
        if (code == "en") s_english[key] = std::move(val);
        ++entries;
    }
    LOG_DEBUG("i18n: loaded %d entries for '%s'", entries, code.c_str());
}

} // namespace

void InitI18n() {
    if (!APIDefs) return;
    s_english.clear();
    s_displayNames.clear();
    s_available.clear();

    for (int i = 0; i < kI18nFilesCount; ++i) {
        const char* stem = kI18nFiles[i].name;
        if (!stem) continue;
        std::string code = stem;
        const void* data = nullptr; size_t size = 0;
        if (!TryLoadBundledData(kI18nFiles, kI18nFilesCount, code, data, size)) {
            LOG_WARNING("i18n: bundled file for '%s' not found", code.c_str());
            continue;
        }
        LoadOneLanguage(code, data, size);
        s_available.push_back(code);
    }
    LOG_INFO("i18n: %d language(s) available, %d English key(s)",
             (int)s_available.size(), (int)s_english.size());
}

void SetUiLanguage(const std::string& code) {
    std::string next = code.empty() ? "auto" : code;
    if (next != s_mode) LOG_INFO("ui language -> %s", next.c_str());
    s_mode = next;
}

const std::string& GetUiLanguage() { return s_mode; }

const char* L(const char* key) {
    if (!key) return "";

    auto englishOr = [&]() -> std::string {
        auto it = s_english.find(key);
        if (it != s_english.end()) return it->second;
        return key;  // last resort: the key itself
    };

    std::string resolved;
    if (s_mode == "en" || !APIDefs || !APIDefs->Localization.Translate) {
        // English (or no Nexus / no localization VT): our ground truth.
        resolved = englishOr();
    } else {
        // Resolve against the namespaced id we registered under.
        std::string nsId = NsId(key);
        const char* t = (s_mode == "auto")
            ? APIDefs->Localization.Translate(nsId.c_str())
            : (APIDefs->Localization.TranslateTo
                   ? APIDefs->Localization.TranslateTo(nsId.c_str(), s_mode.c_str())
                   : nullptr);
        // Nexus returns the identifier unchanged when it has no
        // translation for the active language - fall back to English.
        resolved = (!t || std::strcmp(t, nsId.c_str()) == 0) ? englishOr()
                                                             : std::string(t);
    }

    std::string& slot = s_cache[key];  // stable node; distinct per key
    slot = std::move(resolved);
    return slot.c_str();
}

const std::vector<std::string>& AvailableUiLanguages() { return s_available; }

std::string UiLanguageDisplayName(const std::string& code) {
    auto it = s_displayNames.find(code);
    if (it != s_displayNames.end()) return it->second;
    return code;
}

// ---- Localized tooltip helpers ----------------------------------------

namespace {
// Wrap width as a multiple of the font size. Generous on purpose: normal lines
// never hit it, only runaway translations soft-wrap instead of overflowing.
constexpr float kTipWrapEm = 35.f;
// Description-column width (em) for the label|description tooltip layout. Wraps
// here, hang-indented under the description, with the label column to its left.
constexpr float kTipDescEm = 26.f;
// "(default)" with a leading space, built from the localized fragment.
std::string DefaultTag() { return std::string(" ") + L("tt.default"); }

// Render one "label | wrapped description" row. The label sits in a column
// `labelColW` wide; the description starts to its right and hang-indents (its
// wrapped continuation lines line up under the description, not back under the
// label) because ImGui draws every wrapped line at the x where the text began.
//
// Deliberately NOT an ImGui table: a table needs a frame to resolve its column
// widths, so on the FIRST hover the description column had no width to wrap
// against and the text stacked into a tall column - a giant box for one frame
// before it settled. Here every width comes from CalcTextSize / the font size,
// which are window-independent, so the layout is correct on frame one.
void TipRow(const std::string& label, const char* descKey, float labelColW) {
    const float startX = ImGui::GetCursorPosX();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(startX + labelColW + ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * kTipDescEm);
    ImGui::TextUnformatted(L(descKey));
    ImGui::PopTextWrapPos();
}

// Pixel width of the label column = the widest composed label, so the
// descriptions line up. Measured now (CalcTextSize is window-independent).
float TipLabelWidth(const std::vector<std::string>& labels) {
    float w = 0.f;
    for (const auto& s : labels) {
        const float sw = ImGui::CalcTextSize(s.c_str()).x;
        if (sw > w) w = sw;
    }
    return w;
}

// Halve the inter-row vertical gap for the tighter look; rows are plain Text
// items separated by ItemSpacing.y. Caller pops.
void PushTipRowSpacing() {
    const ImVec2 sp = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp.x, sp.y * 0.5f));
}
}  // namespace

void TooltipText(const char* key) {
    if (!key) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * kTipWrapEm);
    ImGui::TextUnformatted(L(key));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void TooltipOnOff(const char* onKey, const char* offKey, bool defaultIsOn) {
    ImGui::BeginTooltip();
    const std::string def = DefaultTag();
    // On above Off; the two state descriptions align in the second column.
    const std::string onLabel  = std::string(L("tt.on"))  + (defaultIsOn  ? def : std::string()) + ":";
    const std::string offLabel = std::string(L("tt.off")) + (!defaultIsOn ? def : std::string()) + ":";
    const float labelW = TipLabelWidth({onLabel, offLabel});
    PushTipRowSpacing();
    TipRow(onLabel, onKey, labelW);
    TipRow(offLabel, offKey, labelW);
    ImGui::PopStyleVar();
    ImGui::EndTooltip();
}

void TooltipOptions(const char* introKey, const TooltipOption* options, int count) {
    ImGui::BeginTooltip();
    if (introKey) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * kTipWrapEm);
        ImGui::TextUnformatted(L(introKey));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
    const std::string def = DefaultTag();
    std::vector<std::string> labels;
    labels.reserve(count);
    for (int i = 0; i < count; ++i)
        labels.push_back(std::string(L(options[i].labelKey))
                             + (options[i].isDefault ? def : std::string()) + ":");
    const float labelW = TipLabelWidth(labels);
    PushTipRowSpacing();
    for (int i = 0; i < count; ++i)
        TipRow(labels[i], options[i].descKey, labelW);
    ImGui::PopStyleVar();
    ImGui::EndTooltip();
}

// --- Dev-tool introspection -------------------------------------------------
// s_cache is file-private so the accessors live here.

size_t TranslationCacheSize() { return s_cache.size(); }

size_t TranslationCacheApproxBytes() {
    auto strHeap = [](const std::string& s) -> size_t {
        return s.capacity() > 15 ? s.capacity() + 1 : 0;
    };
    // std::map<string, string>: 32 = rough MSVC x64 RB-tree node header
    // (3 ptrs + color + padding). Two strings per entry (key + value).
    size_t bytes = s_cache.size() * (2 * sizeof(std::string) + 32);
    for (const auto& kv : s_cache) {
        bytes += strHeap(kv.first);
        bytes += strHeap(kv.second);
    }
    return bytes;
}

// The always-resident ground-truth tables (the bundled English table + the
// per-language display names + the available-codes list), distinct from the
// L() result cache above. The English table is loaded once at init and stays
// for the process; the cache is only a subset that's been looked up. Surfaced
// as its own memory-monitor row so the table's footprint isn't conflated with
// the (smaller, lazily-grown) cache.
size_t TranslationTableSize() { return s_english.size(); }

size_t TranslationTableApproxBytes() {
    auto strHeap = [](const std::string& s) -> size_t {
        return s.capacity() > 15 ? s.capacity() + 1 : 0;
    };
    size_t bytes = s_english.size() * (2 * sizeof(std::string) + 32);
    for (const auto& kv : s_english) { bytes += strHeap(kv.first) + strHeap(kv.second); }
    bytes += s_displayNames.size() * (2 * sizeof(std::string) + 32);
    for (const auto& kv : s_displayNames) { bytes += strHeap(kv.first) + strHeap(kv.second); }
    bytes += s_available.capacity() * sizeof(std::string);
    for (const auto& s : s_available) bytes += strHeap(s);
    return bytes;
}
