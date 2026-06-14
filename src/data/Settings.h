#pragma once
#include <cstdint>   // uint32_t (SendMinIntervalMs + the interval-bound consts)
#include <string>
#include <vector>

// View modes:
//   Full      — icon with a two-line label below; widest cell.
//   Icon      — icon only; smallest cell with artwork.
//   TextOnly  — single-line text button, no icon (formerly named
//               Compact; kept at numeric value 2 so settings.json
//               files written before the rename still resolve).
//   Compact   — icon with a small alpha text strip overlaid on the bottom
//               edge of the icon. Same cell footprint as Icon.
enum class EViewMode    { Full = 0, Icon = 1, TextOnly = 2, Compact = 3 };

// When the mouse wheel cycles the active Quickbar category. Replaces the
// old two-boolean scheme (over-bar + anywhere), where "anywhere" silently
// superseded "over bar". Numeric values match the Options combo's item
// order so no display<->enum lookup table is needed.
//   Off      — wheel never cycles; always scrolls the icon list.
//   OverBar  — wheel cycles only when the cursor is over the tab/dropdown
//              row; over the icon list it still scrolls.
//   Anywhere — wheel cycles anywhere in the Quickbar, overriding icon scroll.
enum class EWheelCycle  { Off = 0, OverBar = 1, Anywhere = 2 };

// How the Quickbar presents a "can't emote right now" block (mounted, and -
// with the RealTime API - swimming / downed / etc.). Grey keeps the buttons
// visible but dimmed + unclickable; Hide removes the whole bar until usable.
enum class EUnusableBehavior { Grey = 0, Hide = 1 };

// What the Quickbar does while the player is in combat. Off ignores combat;
// Grey dims + blocks the buttons in place (Quickbar-only - emotes still work
// in combat everywhere else); Hide removes the whole bar until combat ends.
enum class EQbCombat { Off = 0, Grey = 1, Hide = 2 };

// What signals there is more scrollable content. Mutually exclusive so the
// scrollbar and the edge hints never fight for the same edge:
//   Off       — nothing; the wheel still scrolls.
//   Hints     — a lit accent edge line + soft glow (no draggable bar).
//   Scrollbar — the visible bar (custom slim-pill in owned mode, ImGui's
//               standard bar in pure free mode).
enum class EQbScrollIndicator { Off = 0, Hints = 1, Scrollbar = 2 };

// How the Quickbar snaps wheel + scrollbar-drag to a grid:
//   Off    — smooth pixel scrolling (ImGui's default), no snap.
//   Cells  — one notch = one cell, scroll rounds to whole cells. Scrolling
//            back up always lands on clean rows.
//   Pages  — one notch = one full viewport (g_QbRows * cell pitch vertical,
//            g_QbCols * cell pitch horizontal). Scroll rounds to whole pages.
// Either Cells or Pages takes scrolling ownership from ImGui (draws the
// custom slim scrollbar). Default Cells.
enum class EQbScrollSnap { Off = 0, Cells = 1, Pages = 2 };

// What the palette (ui/Palette.cpp) suggests while the query is
// empty: the frequently-used / recently-used emotes (the usage log's two
// views), or nothing.
enum class EPaletteEmptyQuery { Frequent = 0, Recent = 1, Off = 2 };

// What Enter / a row click does in the palette: follow the global
// send-on-click settings (SendOnClick / MeMoteSendOnClick), or force
// auto-send / fill-chat-only for sends from the palette. Numeric for JSON
// stability (settings.json palette "enter_mode").
enum class EPaletteEnterMode { Global = 0, Send = 1, Fill = 2 };

// What a left-click on the Nexus quick-access icon opens. Numeric for JSON
// stability (settings.json "left_click_opens"); right-click always opens the
// shortcut's context menu regardless.
enum class EShortcutClick { Library = 0, Quickbar = 1, Palette = 2 };

// Which catalog a FavoriteRef points into. Numeric for JSON stability —
// settings.json stores "type": 0 for an Emote, "type": 1 for a /me-mote
// (legacy "emote" / "me_mote" strings also accepted on load for forward
// compat with hand-edits). Adding a third kind later means appending an
// enum value + extending the load/save table — no migration of stored
// values.
enum class EFavoriteRefType { Emote = 0, MeMote = 1 };

// One entry in a favorite category. Type-tagged so the renderer + send
// pipeline can look the Id up in the right catalog. The Emote-id-only
// form this replaced couldn't distinguish a /me-mote Id from an Emote Id,
// so /me-motes couldn't be quick-accessed as favorites at all.
struct FavoriteRef {
    EFavoriteRefType Type = EFavoriteRefType::Emote;
    std::string      Id;
};

// Stable "<type-int>:<id>" key for a typed ref - the single source of truth for
// the string used to compare/dedup refs across subsystems (usage log dedup,
// radial drift snapshots). Inline so every caller shares one definition.
inline std::string FavoriteRefKey(EFavoriteRefType type, const std::string& id) {
    return std::to_string((int)type) + ":" + id;
}
inline std::string FavoriteRefKey(const FavoriteRef& r) {
    return FavoriteRefKey(r.Type, r.Id);
}

struct FavoriteCategory {
    std::string                Name;
    // Type-tagged refs in user order. Migrated from the legacy
    // `std::vector<std::string> Emotes` (interpreted as Emote-typed Ids) at
    // load — see Settings.cpp. Renamed from `Emotes` to `Refs` since the
    // collection now mixes Emotes and /me-motes.
    std::vector<FavoriteRef>   Refs;
    // Collapsed in the Library to just its header (the emote grid is hidden).
    // Per-category UI state, persisted in settings.json. Default expanded.
    bool                       Collapsed = false;
};

struct Settings {
    // User-marked "unlocked" emote IDs (lowercase). Without API key access we
    // can't auto-detect unlocks, so the user manages this list manually via
    // the emote context menu (Mark as unlocked / Mark as locked).
    std::vector<std::string>      ManuallyUnlocked;
    // Main-panel emote-class filters. Three independent toggles, all on by
    // default (all-on shows everything, the old "All" radio). An emote is
    // shown only when the toggle for its class is on: core emotes follow
    // FilterShowCore; unlockable emotes follow FilterShowUnlocked when the
    // user has marked them unlocked, FilterShowLocked otherwise. Replaced
    // the old single-select EEmoteFilter radio — see MainPanel.cpp.
    bool                          FilterShowCore       = true;
    bool                          FilterShowUnlocked   = true;
    bool                          FilterShowLocked     = true;
    // Class filter for /me-motes — the fourth Library toolbar pill, mirrors
    // the Core/Unlocked/Locked toggles. Hides /me-motes from both the
    // built-in /me-motes section and the user-favorite sections. Default on.
    bool                          FilterShowMeMotes    = true;
    EViewMode                     ViewMode             = EViewMode::Full;
    float                         MainIconScale        = 1.0f;
    // Per-section collapse state for the built-in Library sections (Core /
    // Unlockable / Text). User favorites categories store their own Collapsed
    // flag in FavoriteCategory; these cover the synthetic built-ins. Default
    // expanded. An active search renders every section expanded regardless.
    bool                          MainCoreCollapsed     = false;
    bool                          MainUnlockedCollapsed = false;
    // Library /me-motes section — surfaces /me-motes (data/MeMotes.h). See the
    // /me-motes Quickbar toggle further down.
    bool                          MainMeMotesCollapsed  = false;
    // ---- Emote palette (ui/Palette.cpp) ----
    // Result-row cap (5..15). The palette never scrolls — past the cap it shows
    // "+N more" and the answer is a longer query.
    int                           PaletteMaxResults    = 9;
    // What an empty query suggests (see EPaletteEmptyQuery above).
    EPaletteEmptyQuery            PaletteEmptyQuery    = EPaletteEmptyQuery::Frequent;
    // Open with the query cleared (default) instead of the last query
    // preselected.
    bool                          PaletteClearOnOpen   = true;
    // Palette size factor — window width + row/icon height (text stays the
    // shared-atlas size). 0.8..1.5.
    float                         PaletteScale         = 1.0f;
    // Vertical anchor of the palette's top edge, as a fraction of the screen
    // height (0.0..0.8).
    float                         PaletteYPos          = 0.2f;
    // Horizontal anchor of the palette's CENTER, as a fraction of the screen
    // width (0.1..0.9; 0.5 = centered).
    float                         PaletteXPos          = 0.5f;
    // Enter / row-click send mode (see EPaletteEnterMode above). Applies to
    // every send from the palette, context-menu variants included.
    EPaletteEnterMode             PaletteEnterMode     = EPaletteEnterMode::Global;
    // Opt-in: first Esc clears a non-empty query, second Esc closes. Off
    // (default) = Esc always closes immediately.
    bool                          PaletteEscClearsFirst = false;
    // Window background opacity (0.2..1.0). 1.0 keeps the theme's own
    // background untouched.
    float                         PaletteBgAlpha       = 1.0f;
    // Icon column in result rows. Off = compact text-only rows.
    bool                          PaletteShowIcons     = true;
    // The bottom key-help line (Enter sends - Esc closes).
    bool                          PaletteShowFooter    = true;
    // Grow direction: false = top edge anchored at PaletteYPos, grows down
    // (default); true = BOTTOM edge anchored there, grows up - the query
    // field stays put while the result list extends toward the screen top.
    bool                          PaletteGrowUp        = false;
    EViewMode                     QuickbarViewMode     = EViewMode::Icon;
    float                         QuickbarIconScale    = 1.0f;
    bool                          QuickbarUseDropdown  = false;  // tabs (false) or dropdown (true)
    bool                          ShowWindow           = true;
    bool                          SendOnClick          = true;
    // Independent of SendOnClick — users may want auto-send for one and
    // chat-fill-only for the other. /me-motes commit free-form text to chat,
    // which some users prefer to confirm-then-send rather than fire on click.
    // See SendOrFillMeMote / EmoteAction.cpp.
    bool                          MeMoteSendOnClick    = true;
    // When on, clicking an emote while a GW2 text box is focused closes it
    // (injects Escape, clearing the half-typed line) and then sends, instead of
    // refusing. Off by default. See SendOrFillEmote / ShouldSkipEmoteSend.
    bool                          CloseChatOnSend      = false;
    // When on, left-clicking a targetable emote sends it on the current
    // target (appends " @") instead of plain. Off by default. With it on, the
    // right-click menu offers "Send normally" and drops "Send on target"
    // (that's the click default now). See Cells.cpp.
    bool                          SendTargetableOnTarget = false;
    bool                          ShowQuickbar         = false;
    bool                          QuickbarCloseOnEsc   = false;  // off by default — QB is a HUD, not a modal
    // Positive-direction booleans throughout: `true` = the feature
    // named by the field is active. Defaults still match the historic
    // behaviour — resizable / movable / scrollbar visible / tooltips
    // visible by default; transparent body, hidden title, click-
    // through, and category-bar dropdown all opt-in.
    bool                          QuickbarAllowResize      = true;   // edges drag to resize
    bool                          QuickbarAllowMove        = true;   // title-bar / body drag to move
    bool                          ShowQuickbarBg           = true;   // window/child background fill
    bool                          ShowQuickbarTitle        = true;   // title bar (loses drag handle when off)
    // Punches up the alpha of the category-bar tabs / dropdown and the
    // Text-only mode emote buttons. Aimed at the "background off" use
    // case where ImGui's default ~0.40-alpha button fills disappear
    // against the game world. Doesn't touch icon/full/compact cells —
    // their artwork already carries enough contrast on its own.
    bool                          QuickbarHighContrast     = false;
    // Off / Hints / Scrollbar — mutually exclusive (see EQbScrollIndicator).
    // Default Scrollbar: the familiar visible bar.
    EQbScrollIndicator            QuickbarScrollIndicator  = EQbScrollIndicator::Scrollbar;
    bool                          QuickbarHorizontalScroll = false;  // column-major layout + horizontal scrollbar
    // Drag-snap the Quickbar window to whole-cell multiples on both axes so it
    // frames an exact grid (no partial cells, no premature scrollbar). On by
    // default - the bar looks best framing a clean grid. Residual: the snap
    // target shifts when the cell size (view mode / icon scale) or the chrome
    // (title / category bar wrap / scrollbar) changes, so it re-snaps with a
    // one-frame jump. See Quickbar.cpp.
    bool                          QuickbarSnapWindow       = true;
    // Snap scrolling mode: Off (smooth), Cells (one notch = one emote, lands on
    // row boundaries) or Pages (one notch = one full viewport's worth of cells).
    // Default Cells. Either snap mode takes scrolling ownership from ImGui and
    // draws the custom slim scrollbar. See EQbScrollSnap and Quickbar.cpp.
    EQbScrollSnap                 QuickbarSnapScroll       = EQbScrollSnap::Cells;
    // Wheel scroll-wrap: wheeling past the bottom row jumps to the top (and the
    // reverse). Mouse-wheel only - dragging the scrollbar still clamps at the
    // ends. Off by default. Works in every scroll mode (in smooth vertical mode
    // emot3 takes over the wheel while this is on). See Quickbar.cpp.
    bool                          QuickbarScrollWrap       = false;
    bool                          ShowQuickbarTooltips     = true;   // per-emote hover tooltips in the QB
    bool                          QuickbarClickThrough     = false;  // pass clicks through empty QB area
    bool                          ShowQuickbarCategoryBar = true;   // tab/dropdown row above the icons
    // Which categories the Quickbar's category bar lists. The controls live in
    // the Options Quickbar tab; the keys + settings.json nesting stay
    // general.quickbar_categories for back-compat (see Settings.cpp).
    //   Favorites    — the user's own favorites categories (all of them).
    //   Core         — every core emote.
    //   Unlocked     — unlockable emotes the user has marked unlocked.
    //   UnlockedAll  — core + unlocked = everything currently usable.
    // Favorites default on (the original behaviour). Among the built-ins only
    // Unlocked (all) defaults on - it's the single "everything usable" list
    // most users want; Core and the unlockables-only Unlocked are narrower
    // slices, opt-in to avoid crowding the bar with extra tabs.
    bool                          QuickbarShowFavoriteCategories  = true;
    bool                          QuickbarShowCoreCategory        = false;
    bool                          QuickbarShowUnlockedCategory    = false;
    bool                          QuickbarShowUnlockedAllCategory = true;
    // "Your Mad King Says..." Halloween event set (the mad_king-flagged
    // emotes). Opt-in like Core / Unlocked — a seasonal slice most users
    // only want during the event.
    bool                          QuickbarShowMadKingCategory     = false;
    // /me-motes (data/MeMotes.h) — user-defined free-form text emotes. Opt-in
    // (defaults off, matching the other built-in category posture). The
    // Library always shows the /me-motes section when /me-motes exist; this
    // toggle only gates the Quickbar's category-cycle inclusion.
    bool                          QuickbarShowMeMotesCategory     = false;
    // Synthetic usage categories (see data/Usage.h). Quickbar-only — they're
    // non-editable by nature (derived from a usage log), so there's no Library
    // section. Recently used = last distinct emotes/me-motes; Frequently used =
    // by frequency over the bounded log. Opt-in like the other built-ins.
    bool                          QuickbarShowRecentlyUsedCategory = false;
    bool                          QuickbarShowFrequentCategory     = false;
    // Exclude /me-motes from the usage log's Recently/Frequently used lists (some
    // users want those to surface only real emotes). Filters at READ time
    // (usage::RecentlyUsed/Frequent skip /me-mote refs) so toggling is immediate +
    // reversible - the log still records them. Affects the Quickbar usage categories
    // AND the Palette's zero-query list. Off by default.
    bool                          IgnoreMeMotesFromUsage = false;
    // When the mouse wheel cycles the active category (see EWheelCycle).
    // Defaults to OverBar - cycling when hovering the category bar is what
    // most users intuitively expect, while the icon list still scrolls.
    EWheelCycle                   QuickbarWheelCycle   = EWheelCycle::OverBar;
    // Wheel-cycle wrap: at the last category, cycling further jumps to the
    // first (and the reverse). On by default (the long-standing behaviour);
    // off clamps cycling at the first/last category. Only relevant while
    // QuickbarWheelCycle isn't Off.
    bool                          QuickbarWheelCycleWrap = true;
    // What the Quickbar does in combat (MumbleLink IsInCombat): Off / Grey out
    // the buttons / Hide the whole bar. Off by default. Emotes still work in
    // combat everywhere else - this is a Quickbar-only convenience, not a global
    // usability block - so it's a Quickbar setting and travels with presets.
    EQbCombat                     QuickbarCombatBehavior = EQbCombat::Off;
    int                           QuickbarCategoryIdx  = 0;
    // Opt-in fallback: when an emote has no official bundled icon AND no
    // user-supplied PNG in icons/, use the bundled AI-generated artwork
    // instead of the letter fallback. Off by default — the AI provenance
    // is something the user should knowingly enable.
    bool                          UseAIIconFallback    = false;
    // Show the small corner dot marking targetable emotes (those you can
    // suffix with " @"). On by default. Applies to both the main panel and
    // the Quickbar (drawn in RenderEmoteCell).
    bool                          ShowTargetDot        = true;
    // Show the small corner accent marking /me-motes (drawn top-right — the
    // same corner as the target dot, which is safe because /me-motes are never
    // IsTargetable so the two never co-occur). On by default — makes /me-motes
    // visually distinguishable from regular Emotes in mixed favourites /
    // Quickbar categories.
    bool                          ShowMeMoteIndicator  = true;
    // Block emotes that can't currently be used: refuse the send (toast) + grey
    // or hide the Quickbar. On by default. Always covers mounted (MumbleLink); the
    // transient refusals (typing, moving) ride it too. Airborne and the RTAPI states
    // are separate sub-toggles below. See core/CharacterState.
    bool                          QuickbarGreyUnusable = true;
    // Grey/refuse while AIRBORNE (jumps + falls) - MumbleLink-derived, needs no addon.
    // Its own sub-toggle (gated by QuickbarGreyUnusable); on by default.
    bool                          QuickbarAirborneDetection = true;
    // Extend the block to the RTAPI-only states (downed / swimming / underwater /
    // gliding / flying) - requires the optional GW2 RealTime API addon (a no-op
    // without it). Gated by QuickbarGreyUnusable. On by default; only does anything
    // once RTAPI loads.
    bool                          QuickbarPreciseStateDetection = true;
    // How a blocked state presents on the Quickbar: grey the buttons (default)
    // or hide the whole bar until the player can emote again. When active
    // (QuickbarGreyUnusable), this covers the transient send refusals too - a GW2
    // text box focused, or moving / a printable key held - matching the send gate,
    // with no separate opt-in (they're cheap + robust). The +plus "send while
    // moving" setting drops the movement case; "close chat on send" drops the
    // textbox case. See Quickbar.cpp.
    EUnusableBehavior             QuickbarUnusableBehavior = EUnusableBehavior::Grey;
    // Nexus quick-access shortcut (the little icon row at the top of the
    // screen). On by default — it's the main entry point for the addon.
    bool                          ShowNexusShortcut    = true;
    // What a left-click on the icon opens — Library (default), Quickbar, or
    // the emote palette. Replaced the old swap bool (legacy
    // "left_click_opens_quickbar" still maps on load). Right-click always
    // opens the context menu.
    EShortcutClick                ShortcutClickAction = EShortcutClick::Library;
    // UI language code, or "auto" to follow Nexus' active language. Empty
    // is treated as "auto". The set of valid concrete codes is discovered
    // from the bundled i18n tables (see I18n.h). Default follows Nexus.
    std::string                   UiLanguage = "auto";

    // ---- Unlock tracking (Unlocks tab; see core/UnlockScan) ----
    // Manual right-click lock management is always available; these govern the
    // optional GW2-API sync that fills in account unlock state on top.
    // Key source: 0 = Hoard & Seek proxy, 1 = own API key. Clamped on load.
    int                           UnlockApiKeySource = 0;
    // Own GW2 API key (needs account + unlocks + progression scope). Stored
    // plaintext in settings.json like other GW2 addons. Empty by default.
    std::string                   Gw2ApiKey;
    // Auto-sync once per session, a few seconds after the game starts, when a
    // usable source exists (own key set, or Hoard & Seek). Off by default.
    // The sync is always additive (only ADDS found unlocks, never locks) since
    // the API only covers a subset of emotes - we don't trust it to manage the
    // full lock state.
    bool                          UnlockAutoSync = false;

    // ---- New-bundled-emote notifier (see core notifier detection) ----
    // When a future addon version ships emotes not in KnownBundledEmotes, offer
    // them via a first-run-style dialog. On by default; the dialog's "Don't ask
    // again" (and an Options toggle) clears this.
    bool                          NotifyNewBundledEmotes = true;
    // Snapshot of bundled emote ids seen on a prior run. The notifier diffs the
    // current bundle against this; deliberately-deleted bundled emotes stay in
    // here so they're never re-offered. Empty (first run / fresh install) means
    // "initialize silently, don't nag". Stored as a top-level inline array
    // (known_bundled_emotes), like ManuallyUnlocked.
    std::vector<std::string>      KnownBundledEmotes;

    // ---- Auto-motes (Emote::AutoKeywords in data/EmoteData.h; core/ChatWatch) ----
    // Master switch. When on AND the "Events: Chat" addon is present, the user's
    // OWN sent chat lines are matched against the auto-mote rules and fire a
    // catalog emote. Off by default — it reads chat + auto-acts, so it's strictly
    // opt-in (and always competitive-locked via the send gate).
    bool                          AutoMotesEnabled     = false;
    // Watched chat channels (own messages only). Say/Local + Party on by default
    // (the channels most users chat casually in); Map/Squad/Guild/Whisper off.
    // The competitive channels (Team PvP/WvW) are never options. See ChatWatch.
    bool                          AutoMoteWatchLocal   = true;   // Say / Local
    bool                          AutoMoteWatchParty   = true;
    bool                          AutoMoteWatchMap     = false;
    bool                          AutoMoteWatchSquad   = false;
    bool                          AutoMoteWatchGuild   = false;
    bool                          AutoMoteWatchWhisper = false;
    // Where auto-motes may fire, by GW2 map type (MumbleLink Context.MapType).
    // Open world = Public + Public_Mini (cities, zones, Dry Top / Silverwastes /
    // Mistlock Sanctuary) + WvW_Lounge (Armistice Bastion social space); Instances
    // = Instance (dungeons, fractals, raids, strikes, story, guild/home). Open
    // world on, instances off by default. Every other map type (PvP/WvW
    // competitive - already send-gated, the unused BigBattle, and transient
    // redirect/charcreate/tutorial) never fires.
    bool                          AutoMoteInOpenWorld  = true;
    bool                          AutoMoteInInstances  = false;
    // ADDITIONAL minimum interval between two AUTO-fires specifically, on top of
    // the global SendMinIntervalMs below. Auto-motes fire passively from your own
    // typing, so they get a longer floor of their own (e.g. /laugh shouldn't
    // re-fire every couple seconds in a chatty map). Enforced in core/ChatWatch's
    // drain against a separate auto-only timestamp; the global gate throttle still
    // applies too, so the effective auto cadence is max(this, global). Clamped to
    // [kAutoMoteMinIntervalFloorMs, kAutoMoteMinIntervalCeilMs] on load.
    uint32_t                      AutoMoteMinIntervalMs = 15000;

    // Global minimum interval between any two emote/me-mote sends, across EVERY
    // surface (click, keybind, radial, auto-mote). Anti-spam throttle enforced at
    // the shared send gate (core/EmoteAction ShouldSkipEmoteSend chokepoint). A
    // manual send within the window is refused (with the in-window "slow down"
    // cue); an auto-fire within it is dropped silently. Clamped to
    // [kSendMinIntervalFloorMs, kSendMinIntervalCeilMs] on load.
    uint32_t                      SendMinIntervalMs    = 2000;

    std::vector<FavoriteCategory> FavoriteCategories;
};

// Global send-interval bounds + default — shared by the settings sanitize clamp,
// the General > Sending slider (incl. its right-click reset), and the gate
// throttle. constexpr at namespace scope = internal linkage (matches
// kMaxNameBytes), so each TU sees one value.
constexpr uint32_t kSendMinIntervalFloorMs   = 750;
constexpr uint32_t kSendMinIntervalCeilMs    = 5000;
constexpr uint32_t kSendMinIntervalDefaultMs = 2000;

// Auto-mote-specific interval bounds + default (the additional auto-only floor).
constexpr uint32_t kAutoMoteMinIntervalFloorMs   = 5000;
constexpr uint32_t kAutoMoteMinIntervalCeilMs    = 60000;
constexpr uint32_t kAutoMoteMinIntervalDefaultMs = 15000;

extern Settings g_Settings;

// Map a raw integer (e.g. a hand-edited settings.json view_mode) to a valid
// EViewMode, falling back to Full when out of range. Shared by the settings
// and Quickbar-preset loaders so the normalization lives in one place.
EViewMode NormalizeViewMode(int raw);

// Map a raw integer to a valid EWheelCycle, falling back to OverBar (the
// default) when out of range. Shared by the settings and Quickbar-preset
// loaders, same as NormalizeViewMode.
EWheelCycle NormalizeWheelCycle(int raw);

// Map a raw integer to a valid EUnusableBehavior, falling back to Grey (the
// default) when out of range. Same shape as NormalizeViewMode.
EUnusableBehavior NormalizeUnusableBehavior(int raw);

// Map a raw integer to a valid EQbCombat (Off/Grey/Hide), falling back to Off
// (the default) when out of range. Same shape as NormalizeViewMode.
EQbCombat NormalizeQbCombat(int raw);

// Map a raw integer to a valid EQbScrollIndicator (Off/Hints/Scrollbar),
// falling back to Scrollbar (the default) when out of range. Shared by the
// settings and Quickbar-preset loaders, same as NormalizeViewMode.
EQbScrollIndicator NormalizeScrollIndicator(int raw);

// Map a raw integer to a valid EQbScrollSnap (Off/Cells/Pages), falling back
// to Cells (the default) when out of range. Shared by the settings and
// Quickbar-preset loaders, same shape as NormalizeScrollIndicator.
EQbScrollSnap NormalizeScrollSnap(int raw);

// Minimum icon scale for a view mode. Full / TextOnly / Compact need a 1.0
// floor — their label area (two-line name / centered text line / alpha strip)
// needs vertical room and breaks below 1×; Icon has no label and scales freely
// from 0.5×. The 2.5× cap is uniform across modes. One source of truth for the
// per-mode floor that the main-panel, Quickbar and Options scale sliders + their
// render-time clamps all share, so they can't drift apart.
float MinIconScaleForMode(EViewMode mode);

// Load settings.json into g_Settings. Tolerant of a missing / malformed /
// partially-hand-edited file (falls back to struct defaults per field, then
// runs a sanitize pass). Returns true when the sanitize pass corrected an
// out-of-range value, so the caller can re-save to heal the file on disk.
bool LoadSettings(const std::string& path);
void SaveSettings(const std::string& path);
// Serialize g_Settings to the on-disk JSON string without touching disk. Lets the
// SaveScheduler build the bytes on the render thread and hand them to its writer
// thread; SaveSettings is the thin disk-writing wrapper.
std::string SerializeSettings();
