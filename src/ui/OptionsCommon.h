#pragma once

#include <string>

enum class EFavoriteRefType;  // data/Settings.h (RadialMembershipNote param)

// Shared helpers + per-tab entry points for the Options module. The Options
// panel is split across Options.cpp (the AddonOptions dispatcher + lifecycle)
// and OptionsGeneral / OptionsQuickbar / OptionsEmotes / OptionsUnlocks.cpp
// (one tab each).
// These two helpers are project-aware (they touch g_Settings / SaveSettings /
// L), so they live here rather than in the pure-ImGui Layout module.

// Checkbox + auto-save + tooltip — the trio almost every settings checkbox
// repeats. Returns the Checkbox's "changed this frame" so callers can chain
// extra on-change work; the auto-save runs regardless. See nexus-addon-dev.md
// "On-change save discipline". Callers needing a conditional tooltip or a custom
// save predicate call ImGui::Checkbox directly.
bool CheckboxWithSaveAndTooltip(const char* labelKey, bool* state,
                                const char* tooltipKey);

// On/Off-tooltip overload: the tooltip is composed from `<labelKey>.on` and
// `<labelKey>.off` via TooltipOnOff - no `_tooltip` key, no embedded `\n`.
// defaultIsOn marks which state is the default. Use this for the common
// two-state checkbox; the const-char* overload above is for a prose tooltip.
bool CheckboxWithSaveAndTooltip(const char* labelKey, bool* state, bool defaultIsOn);

// Checkbox that greys out (and refuses to change) when `enabled` is false, the
// disabled-aware sibling of CheckboxWithSaveAndTooltip (no hand-rolled
// PushItemFlag/revert dance). When enabled the tooltip is the On/Off two-liner
// from `<labelKey>.on` / `.off` (defaultIsOn marks the default state); when
// disabled it's the prose `disabledTipKey` explaining why. Auto-saves; returns
// true only on an enabled change so the caller can chain extra work.
bool DisabledCheckbox(const char* labelKey, bool* state, bool enabled,
                      bool defaultIsOn, const char* disabledTipKey);

#ifdef EMOT3_PLUS
// Gold "Plus" tag rendered SameLine after a control, marking it an emot3 (Plus)
// feature (hover explains the marker). Used by the Plus* checkboxes below and by the
// RadialMenus tab's Deploy button.
void PlusBadge();

// +plus settings checkboxes - same look as the g_Settings helpers above, but
// backed by g_PlusSettings / SavePlusSettings (plus.json) instead of g_Settings.
// Used for the two shipped input-swallow toggles (Quickbar wheel routing,
// General "send while moving"). The tooltip is the On/Off two-liner from
// `<labelKey>.on` / `.off` (defaultIsOn marks the default). Returns the
// "changed this frame" so callers can chain extra work.
bool PlusCheckbox(const char* labelKey, bool* state, bool defaultIsOn);
// Disabled-aware sibling (greys out + reverts a click when `enabled` is false,
// with a prose `disabledTipKey` explaining why) - mirrors DisabledCheckbox but
// persists to plus.json. Returns true only on an enabled change.
bool PlusDisabledCheckbox(const char* labelKey, bool* state, bool enabled,
                          bool defaultIsOn, const char* disabledTipKey);
#endif  // EMOT3_PLUS

// Muted section header + hairline that groups a tab's controls ("Layout",
// "Window", "Look", ...) without the weight of a CollapsingHeader.
void OptionsSection(const char* title);

// Inline radial-membership note for the catalog editors' keybind row, SameLine after
// the checkbox. When (type,id) is in >= 1 staged wheel it reads "- active via radial
// 'X'" (no personal key needed) when the personal keybind is OFF, or "- also in radial
// 'X'" when it's ON - so ticking the box visibly changes the note (it isn't dead).
// No-op when the entry is in no wheel. Shared by the Emote and /me-mote editors.
void RadialMembershipNote(EFavoriteRefType type, const std::string& id,
                          bool hasPersonalKeybind);

// The Options tabs, one per TU, invoked by AddonOptions() in Options.cpp.
void RenderGeneralOptionsTab();
void RenderQuickbarOptionsTab();
void RenderPaletteOptionsTab();
void RenderEmotesTab();
void RenderUnlocksTab();
