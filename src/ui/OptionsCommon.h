#pragma once

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

// The four Options tabs, one per TU, invoked by AddonOptions() in Options.cpp.
void RenderGeneralOptionsTab();
void RenderQuickbarOptionsTab();
void RenderEmotesTab();
void RenderUnlocksTab();
