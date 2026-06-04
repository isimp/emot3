#pragma once
//
// Type-checked, never-throw accessors for nlohmann/json.
//
// The loaders wrap the *parse* (`f >> j`) in try/catch, but nlohmann's own
// `j.value(key, default)` THROWS `type_error` when the key is present but holds
// the wrong JSON type (e.g. settings.json hand-edited to `"show": 1` or
// `"icon_scale": "big"`), and `j.value("block", json::object())` throws on a
// present-but-non-object block. That throw escapes the loader into AddonLoad,
// uncaught. These helpers check the stored type first and fall back to the
// caller's default on any mismatch, so a single mistyped field degrades to its
// default instead of detonating the whole load — preserving the "partial /
// hand-edited files load cleanly" contract per-field.

#include <nlohmann/json.hpp>

#include <string>

namespace jsonutil {

using json = nlohmann::json;

// A child object, or an empty object when the key is missing / not an object.
// Lets nested-block access stay a cheap chained call that never throws.
// Named GetObj (not GetObject) to dodge the Win32 GDI macro
// `#define GetObject GetObjectA` in wingdi.h, which would otherwise rewrite
// every call site that transitively includes <Windows.h>.
inline const json& GetObj(const json& j, const char* key) {
    static const json kEmpty = json::object();
    if (!j.is_object()) return kEmpty;
    auto it = j.find(key);
    if (it != j.end() && it->is_object()) return *it;
    return kEmpty;
}

// A child array, or an empty array when the key is missing / not an array.
inline const json& GetArray(const json& j, const char* key) {
    static const json kEmpty = json::array();
    if (!j.is_object()) return kEmpty;
    auto it = j.find(key);
    if (it != j.end() && it->is_array()) return *it;
    return kEmpty;
}

inline bool GetBool(const json& j, const char* key, bool def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) return it->get<bool>();
    return def;
}

inline int GetInt(const json& j, const char* key, int def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) return it->get<int>();
    return def;
}

// Accepts any JSON number (integer or float), so `"icon_scale": 1` reads fine.
inline float GetFloat(const json& j, const char* key, float def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) return it->get<float>();
    return def;
}

inline std::string GetString(const json& j, const char* key, const std::string& def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    return def;
}

} // namespace jsonutil
