#pragma once

#include <string>
#include <unordered_map>

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

// Shared text input + hint, used by the catalog and /me-mote editors so every
// editable field renders identically: ONE unified frame (consistent hover/active
// drawn from the theme's FrameBg* colors), an optional invalid red frame+border,
// and an optional in-frame "n / budget" char counter. Pure rendering at the
// current cursor - the caller owns its label/layout/commit. Returns InputText's
// edited-this-frame bool; outActive/outHovered report state over the WHOLE frame
// (incl. the counter zone) for commit-on-defocus + hover tooltips.
struct InputFieldOpts {
    bool  invalid    = false;     // red frame + 2px border
    bool  enabled    = true;      // false: skip hover/active highlight (caller dims via alpha)
    int   charBudget = 0;         // > 0: draw "n / budget" counter inside the frame
    float width      = 0.f;       // input width; 0 fills the available region
    int   flags      = 0;         // ImGuiInputTextFlags forwarded to the input
                                  // (Password, EnterReturnsTrue, ...); the return
                                  // value is InputText's verbatim, so EnterReturnsTrue
                                  // makes InputFieldWithHint return "Enter pressed".
};
bool InputFieldWithHint(const char* id, const char* hintKey,
                        char* buf, size_t bufSize, const InputFieldOpts& opts,
                        bool* outActive = nullptr, bool* outHovered = nullptr);

// Off-screen row culling for a vertical list of VARIABLE-height rows inside a
// scrolling child (the catalog / me-mote editors). Every per-row buffer is keyed
// by stable Id, so skipping a row's RENDER never touches its state - this only
// avoids the per-frame widget + string work for rows the user can't see. Usage,
// once per list (the instance is static so heights persist across frames):
//
//   static RowCuller cull;  cull.Begin();          // at the top of the child
//   for (const std::string& id : view) {
//       if (!cull.BeginRow(id)) continue;          // off-screen -> spacer drawn, skip
//       ImGui::PushID(id.c_str());
//       ... render the row (header + optional body + trailing separator) ...
//       ImGui::PopID();
//       cull.EndRow(id, rowHadActiveField);        // measure height; flag active edit
//   }
//
// The actively-edited row (rowHadActiveField == true this frame) is exempted from
// culling next frame, so typing and commit-on-defocus are never interrupted by a
// wheel-scroll. A row's height is learned on its first real render, then reused as
// a spacer while it's off-screen so the scrollbar geometry stays correct.
class RowCuller {
public:
    void Begin();                            // capture the visible scroll band
    bool BeginRow(const std::string& id);    // true: render for real; false: skipped
    void EndRow(const std::string& id, bool active);
    void Forget(const std::string& id);      // drop a deleted row's cached height
private:
    std::unordered_map<std::string, float> h_;   // measured start-to-start row heights
    std::string active_, activeNext_;            // active-edit row (this/next frame)
    float top_ = 0.f, bot_ = 0.f, y0_ = 0.f;
};

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
void RenderAutoMotesTab();   // auto-motes tab (defined in OptionsAutoMotes.cpp)
