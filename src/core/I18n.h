#pragma once
//
// UI string localization.
//
// Strings route through Nexus' Localization API: at load we parse the
// bundled resources/i18n/<code>.json tables and push every (key, text)
// pair into Nexus via Localization.Set. At draw time L(key) resolves the
// string for the active UI language:
//
//   - "en"   -> our English ground-truth text, returned directly.
//   - "auto" -> Localization.Translate(key)  (follows Nexus' language).
//   - "<code>" -> Localization.TranslateTo(key, code)  (forced).
//
// English is always the fallback: if Nexus has no translation for the
// active language (it returns the key unchanged), L() returns the
// English ground-truth text instead. So a Nexus language we don't ship
// (most of them) degrades cleanly to English, never to a raw key.
//
// Keys are dotted identifiers (e.g. "main.search_hint"). en.json is the
// source of truth - every key used anywhere must exist there.

#include <string>
#include <vector>

// Parse the bundled i18n tables and register them with Nexus. Builds the
// English fallback table and the available-language list. Call once from
// AddonLoad after APIDefs is set.
void InitI18n();

// Set the active UI language: "auto" (follow Nexus) or a concrete code
// ("en", "de", ...). Persisted by the caller (Settings). Cheap; just
// swaps a module-local string read by L().
void SetUiLanguage(const std::string& code);
const std::string& GetUiLanguage();

// Translate a UI string key (see file header for resolution order).
// The returned pointer is valid for the duration of the current frame's
// use - copy it if you need to hold it across frames. Never returns null.
const char* L(const char* key);

// UI language codes discovered from the bundled i18n files (includes
// "en"). The empty-catalog dialog and the Options dropdown enumerate
// this rather than hardcoding a list, so dropping in a new <code>.json
// adds a language with no code change.
const std::vector<std::string>& AvailableUiLanguages();

// Display name for a code, read from the file's "_lang" field
// ("en" -> "English", "de" -> "Deutsch"); falls back to the code itself.
std::string UiLanguageDisplayName(const std::string& code);

// ---- Localized tooltip helpers ----------------------------------------
//
// Wrapping tooltips so the i18n VALUES carry no '\n': each helper opens a
// tooltip, pushes a generous text-wrap width, and renders. ImGui still honours
// any hard '\n' a translator leaves AND soft-wraps long lines, so translators
// write natural sentences and never hand-place breaks just to avoid overflow.
// Call from inside an `if (ImGui::IsItemHovered())` like SetTooltip.

// Single wrapped paragraph - for prose tooltips.
void TooltipText(const char* key);

// Same as TooltipText but for an already-resolved / composed string (a filename,
// a printf-formatted message, a runtime value) rather than an i18n key. Wraps the
// same way so these never run off-screen. Call from inside `if (IsItemHovered())`.
void TooltipTextRaw(const char* text);

// Two state lines: "On[ (default)]: <onText>" / "Off[ (default)]: <offText>".
// The "On"/"Off"/"(default)" words come from the shared tt.on/tt.off/tt.default
// fragments (localized once), so a per-setting value is just the state's
// description - no embedded prefix, no '\n'. defaultIsOn marks the default state.
void TooltipOnOff(const char* onKey, const char* offKey, bool defaultIsOn);

// One labeled line per option, for a combo/dropdown with 3+ choices:
// "<label>[ (default)]: <desc>". labelKey reuses the combo's own item key; descKey
// is the per-option explanation; introKey (may be null) is a wrapped lead line.
struct TooltipOption { const char* labelKey; const char* descKey; bool isDefault; };
void TooltipOptions(const char* introKey, const TooltipOption* options, int count);

// --- Dev-tool introspection (used by devtools/MemoryMonitor) -----------
// Ungated (not behind EMOT3_DEVTOOLS). The L() cache grows with the unique
// set of keys actually translated this session - useful as a leak proxy if
// it ever grew unbounded. ApproxBytes sums map-node overhead + per-entry
// string capacities. Within ~2x; the shape over time matters, not the
// absolute number.
size_t TranslationCacheSize();
size_t TranslationCacheApproxBytes();
// The always-resident ground-truth tables (bundled English + display names +
// available codes), distinct from the L() cache above. Size = English key count.
size_t TranslationTableSize();
size_t TranslationTableApproxBytes();
