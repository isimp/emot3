#define NOMINMAX
#include <Windows.h>

#include <cstring>
#include <fstream>
#include <mutex>

#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "imgui/imgui.h"

#include "Globals.h"
#include "CharacterState.h" // RTAPI integration + can't-emote/combat state
#include "EmoteAction.h"     // NoteKeyEvent/Clear/Reseed held-key tracking (WndProc-fed gate)
#include "UnlockScan.h"      // GW2-API emote-unlock sync (Hoard & Seek / own key)
#include "UpdateCheck.h"     // Plus-only "update available" check (no-op stub otherwise)
#include "PlusSettings.h"   // +plus persisted settings (plus.json; +plus flavor only)
#include "QuickbarWheel.h"  // click-through wheel routing (+plus flavor only)
#include "SendSuppress.h"   // keyboard swallow during emote injection (+plus flavor only)
#include "Keybinds.h"
#include "Logging.h"
#include "I18n.h"
#include "Settings.h"
#include "QuickbarPresets.h"
#include "EmoteData.h"
#include "Favorites.h"
#include "MainPanel.h"
#include "NexusShortcut.h"
#include "Quickbar.h"
#include "Options.h"
#include "DevTools.h"     // dev-tools framework (overlays; only in EMOT3_DEVTOOLS builds)

// ---- DLL lifecycle ----------------------------------------------------

AddonDefinition AddonDef = {};
HMODULE         hSelf    = nullptr;

// Forward declarations — GetAddonDef references the load/unload symbols
// before they're defined below.
void AddonLoad(AddonAPI* aApi);
void AddonUnload();
static UINT WndProcCallback(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) hSelf = hModule;
    return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition* GetAddonDef() {
    AddonDef.APIVersion  = NEXUS_API_VERSION;
    AddonDef.Version     = { 1, 0, 0, 0 };
    AddonDef.Author      = "Morlaed";
    AddonDef.Description = "Clickable emote panel with unlock tracking.";
    AddonDef.Load        = AddonLoad;
    AddonDef.Unload      = AddonUnload;
    AddonDef.Flags       = EAddonFlags_None;
#if !defined(EMOT3_PLUS) && !defined(EMOT3_DEVTOOLS)
    // Public build (emot3.dll, the base Distribution config) - the ONLY build that
    // auto-updates. In-game updates come via Nexus' GitHub provider, which offers
    // one when a release's tag-version outranks AddonDef.Version above; keep the
    // release tag in lockstep with that version. Signature is a unique negative
    // int (self-hosted; no Raidcore listing).
    AddonDef.Signature   = -135791;
    AddonDef.Name        = "emot3";
    AddonDef.Provider    = EUpdateProvider_GitHub;
    AddonDef.UpdateLink  = "https://github.com/isimp/emot3";
#else
    // Every flavored build (Plus, DevTools, PlusDevTools, Debug) is a manual,
    // opt-in variant: a DISTINCT Signature + Name so it can sit in addons/ next to
    // the public emot3.dll (enable one at a time), and Provider=None so Nexus
    // NEVER auto-updates or clobbers it. That keeps the input-swallow (+plus)
    // conveniences and the dev tools strictly opt-in - they only arrive by manually
    // dropping the DLL in and never change underneath you. All variants share the
    // "emot3" config directory (GetAddonDirectory below) - same settings/catalog.
    AddonDef.Provider    = EUpdateProvider_None;
#ifdef EMOT3_DEVTOOLS
    // Any +devtools build (DevTools / PlusDevTools / Debug).
    AddonDef.Signature   = -135793;
    AddonDef.Name        = "emot3 (Dev)";
#else
    // Plus: input-swallow conveniences, no dev tools.
    AddonDef.Signature   = -135792;
    AddonDef.Name        = "emot3 (Plus)";
#endif
#endif
    return &AddonDef;
}

// ---- Keybinds ---------------------------------------------------------
// Identifiers declared in Keybinds.h so NexusShortcut.cpp can reference
// the same strings when wiring the quick-access icon's left-click
// action. Definitions live here so the linker has exactly one copy.

// Nexus groups keybinds by addon in its UI already, so the "emot3 - "
// prefix was redundant noise. Stripping it. Side note: these strings
// are also the persistent storage key for each binding — anyone who
// had a key assigned under the old identifiers will see those slots
// unbound after this rename and need to set them again.
const char* const KB_TOGGLE_MAIN = "Toggle Library";
const char* const KB_TOGGLE_QB   = "Toggle Quickbar";

// Nexus calls this when the user (or another addon) triggers one of our
// registered keybinds. We act on press only — release is ignored so
// holding the key doesn't fire repeatedly.
static void OnKeybind(const char* identifier, bool isRelease) {
    if (isRelease || !identifier) return;
    if (std::strcmp(identifier, KB_TOGGLE_MAIN) == 0) {
        g_Settings.ShowWindow = !g_Settings.ShowWindow;
        LOG_DEBUG("Keybind: Library %s",
                  g_Settings.ShowWindow ? "shown" : "hidden");
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    } else if (std::strcmp(identifier, KB_TOGGLE_QB) == 0) {
        g_Settings.ShowQuickbar = !g_Settings.ShowQuickbar;
        LOG_DEBUG("Keybind: Quickbar %s",
                  g_Settings.ShowQuickbar ? "shown" : "hidden");
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
}

// ---- Addon load / unload ----------------------------------------------

// Build flavor, derived from the two additive gating macros, logged at load so a
// shared log identifies which DLL is running: "base" = public build (no input
// swallows), "plus" = the +plus swallow conveniences (EMOT3_PLUS) compiled in;
// "+devtools" = the diagnostic dev tools (EMOT3_DEVTOOLS) compiled in. Signature/
// Name vary by flavor (see GetAddonDef) - only base auto-updates.
static const char* Emot3BuildTag() {
#if defined(EMOT3_PLUS) && defined(EMOT3_DEVTOOLS)
    return "plus+devtools";
#elif defined(EMOT3_DEVTOOLS)
    return "devtools";
#elif defined(EMOT3_PLUS)
    return "plus";
#else
    return "base";
#endif
}

void AddonLoad(AddonAPI* aApi) {
    APIDefs = aApi;
    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions(
        (void*(*)(size_t, void*))APIDefs->ImguiMalloc,
        (void(*)(void*, void*))APIDefs->ImguiFree
    );

    // Clear the unload flag. Nexus reloads (disable -> enable) keep the DLL in
    // the process, so this global persists from the previous AddonUnload (which
    // set it true). Without resetting it, every emote-send worker spawned after
    // a reload sees g_Unloading == true and bails immediately - "can't send any
    // emote after a reload". (g_InflightWorkers self-heals: bailing workers still
    // run their RAII decrement, so it nets back to 0.)
    g_Unloading.store(false);

    NexusLink  = (NexusLinkData*)APIDefs->DataLink.Get("DL_NEXUS_LINK");
    MumbleLink = (Mumble::Data*)APIDefs->DataLink.Get("DL_MUMBLE_LINK");

    // Resolve the optional GW2 RealTime API DataLink + subscribe to addon
    // load/unload so precise can't-emote detection (swimming/gliding/downed/...)
    // works when that addon is present and degrades to mounted-only when it isn't.
    InitCharacterState();

    // Subscribe to the optional Hoard & Seek proxy's response event for the
    // GW2-API emote-unlock sync. Inert when H&S isn't installed / API mode off.
    InitUnlockScan();

    // Plus build only: arm the once-per-session "newer release available" check
    // (the public build auto-updates via Nexus instead). No-op stub elsewhere.
    InitUpdateCheck();

    // Register UI translations with Nexus before anything draws. Cheap;
    // parses the bundled i18n tables once. The active language is applied
    // from settings just below (after LoadSettings).
    InitI18n();

    const char* addonDir = APIDefs->Paths.GetAddonDirectory("emot3");
    if (addonDir) {
        CreateDirectoryA(addonDir, nullptr);
        g_SettingsPath   = std::string(addonDir) + "\\settings.json";
        g_EmotesJsonPath = std::string(addonDir) + "\\emotes.json";
        // Create the icons subfolder (and ui/ inside it) so users can drop
        // their own PNG overrides for both emote icons and UI decorations.
        std::string iconsDir = std::string(addonDir) + "\\icons";
        std::string uiDir    = iconsDir + "\\ui";
        CreateDirectoryA(iconsDir.c_str(), nullptr);
        CreateDirectoryA(uiDir.c_str(),    nullptr);
        g_IconsDir = iconsDir;

        // Quickbar presets live one-JSON-per-preset under presets/. The folder
        // is created (and seeded with the included defaults) lazily by
        // LoadQuickbarPresets below, the first time it's missing.
        g_PresetsDir = std::string(addonDir) + "\\presets";

#ifdef EMOT3_PLUS
        // +plus settings live in their own plus.json (never touched by a base
        // build, so it can't be dropped by one). See PlusSettings.h.
        LoadPlusSettings(std::string(addonDir) + "\\plus.json");
#endif

        // First-run README in icons/ui/ — explains what users can drop
        // here, what each file replaces, and that the Nexus shortcut
        // icons (icon.png / icon_hover.png) are intentionally locked
        // to the bundled artwork. Only written if missing, so any user
        // edits to the file (or a deletion to acknowledge they've read
        // it) survive subsequent launches.
        {
            std::string readmePath = uiDir + "\\README.txt";
            DWORD attr = GetFileAttributesA(readmePath.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                std::ofstream rf(readmePath);
                if (rf.is_open()) {
                    rf <<
"emot3 - UI icon overrides\n"
"=========================\n"
"\n"
"Drop a PNG with the matching name into this folder to replace one\n"
"of the small UI icons emot3 draws around emotes and category\n"
"headers. Changes take effect on the next game launch (Nexus' texture\n"
"cache has no runtime eviction).\n"
"\n"
"Files you can override\n"
"----------------------\n"
"  star.png        Gold star drawn next to user-created favorite\n"
"                  category headers in the Library.\n"
"  paperclip.png   Paperclip drawn next to the built-in section\n"
"                  headers (Core Emotes, Unlockable Emotes).\n"
"  lock.png        Lock overlay drawn on top of locked emote cells.\n"
"  target_dot.png  Small marker dot in the top-right corner of\n"
"                  targetable emote cells.\n"
"\n"
"Format\n"
"------\n"
"  - PNG with alpha. A transparent background is recommended -\n"
"    the icon is drawn on top of whatever's behind it.\n"
"  - Square source artwork at 32-64 px renders crisp at the addon's\n"
"    rendered sizes. The addon scales to fit either way.\n"
"\n"
"Restore the bundled artwork\n"
"---------------------------\n"
"Delete (or rename) your override PNG and restart the game. The\n"
"bundled image is used again on next load.\n";
                    LOG_DEBUG("Wrote default README to %s", readmePath.c_str());
                } else {
                    LOG_WARNING("Could not write %s - users won't see UI override docs",
                                readmePath.c_str());
                }
            }
        }
        LOG_INFO("Addon directory: %s", addonDir);
        // LoadSettings returns true when its sanitize pass corrected an
        // out-of-range / invalid value (e.g. a hand-edited settings.json); heal
        // the file on disk immediately rather than waiting for the next mutation.
        if (LoadSettings(g_SettingsPath)) {
            LOG_INFO("settings.json: rewriting to heal sanitized values");
            SaveSettings(g_SettingsPath);
        }
        SetUiLanguage(g_Settings.UiLanguage);  // "auto" follows Nexus
        LoadQuickbarPresets();  // creates + seeds presets/ on first run
        // The icons/ folder is created empty on purpose: bundled artwork
        // is served directly from the DLL's resource section, so the only
        // reason for a file to appear in icons/ is a user-supplied
        // override. Anything the user drops there wins over the bundle.
    } else {
        LOG_CRITICAL("Failed to obtain addon directory - settings won't persist");
    }

    // No auto-seed: a fresh install starts with an empty catalog. The
    // main window shows a first-run dialog (see AddonRender) that lets
    // the user seed the bundled emotes in one of the four GW2 client
    // languages. If emotes.json exists we trust whatever's on disk; the
    // Options > Emotes tab has "Restore bundled" / "Clear catalog" too.
    ClearEmotes();
    if (!g_EmotesJsonPath.empty()) {
        LoadEmotesJson(g_EmotesJsonPath);  // missing/empty -> dialog prompts
    }
    EnsureDefaultCategory();  // always at least one favorites category
    // Now that both settings (favorites/unlocks) and the catalog are loaded,
    // surface any favorite/unlock id that no longer resolves. Log-only — stale
    // ids are kept so re-seeding the catalog restores them.
    ReconcileFavoritesWithCatalog();

    // New-bundled-emote notifier: diff the embedded table against the user's
    // snapshot and stage the first-run-style prompt (opened by MainPanel's
    // AddonRender) when warranted. Logic + setting gating live in one place so
    // the dev notifier tool can re-use the exact path. See emot3.md.
    DetectNewBundledEmotes();

    // Compact one-line snapshot of the effective high-signal settings, so a
    // shared log opens with the user's actual config (mirrors the dev-state
    // inspector's "Settings (key flags)"). TRACE: diagnostic, off by default.
    LOG_TRACE("settings: window=%d quickbar=%d send_on_click=%d grey=%d precise=%d "
              "unusable_behavior=%d qb_cat_idx=%d autosync=%d key_source=%d ui_lang=%s",
              g_Settings.ShowWindow, g_Settings.ShowQuickbar, g_Settings.SendOnClick,
              g_Settings.QuickbarGreyUnusable, g_Settings.QuickbarPreciseStateDetection,
              (int)g_Settings.QuickbarUnusableBehavior, g_Settings.QuickbarCategoryIdx,
              g_Settings.UnlockAutoSync, g_Settings.UnlockApiKeySource,
              g_Settings.UiLanguage.c_str());

    // Prime the texture cache up-front so the Quickbar has icons the
    // moment it draws — without this it would render blank until the
    // main window was opened at least once, since the loader used to
    // live inside AddonRender. Both calls are idempotent.
    LoadEmoteTextures();
    LoadUiIconOverrides();

    // Nexus quick-access icon — only registers if the user has it
    // enabled in General Options. ApplyNexusShortcut is also called
    // from Options whenever the toggle / swap setting changes.
    ApplyNexusShortcut();

    APIDefs->Renderer.Register(ERenderType_Render,        AddonRender);
    APIDefs->Renderer.Register(ERenderType_Render,        QuickbarRender);
    APIDefs->Renderer.Register(ERenderType_OptionsRender, AddonOptions);
#ifdef EMOT3_DEVTOOLS
    // Dev tools: build the registry once, then one render callback draws every
    // enabled tool (perf overlay, QB sizing, runtime state inspector, ...).
    // See DevTools.h. Compiled out entirely in non-EMOT3_DEVTOOLS builds.
    RegisterBuiltinDevTools();
    APIDefs->Renderer.Register(ERenderType_Render,        RenderDevToolOverlays);
#endif
    APIDefs->WndProc.Register(WndProcCallback);

    // ESC closes the main window like other Nexus windows. The QB hook is
    // gated on the setting (default off — it's a HUD, not modal). The
    // window-name string must exactly match what we pass to ImGui::Begin.
    APIDefs->UI.RegisterCloseOnEscape("emot3 Library##wnd", &g_Settings.ShowWindow);
    ApplyQbCloseOnEsc();

    // Keybinds registered with empty default — users assign their own via
    // the Nexus keybind UI without risking conflicts.
    APIDefs->InputBinds.RegisterWithString(KB_TOGGLE_MAIN, OnKeybind, "");
    APIDefs->InputBinds.RegisterWithString(KB_TOGGLE_QB,   OnKeybind, "");

    LOG_INFO("emot3 v%d.%d.%d.%d (%s) loaded",
             AddonDef.Version.Major, AddonDef.Version.Minor,
             AddonDef.Version.Build, AddonDef.Version.Revision, Emot3BuildTag());
}

void AddonUnload() {
    // Signal detached workers BEFORE deregistering anything Nexus-owned.
    // SendOrFillEmote's emote-injection thread and IconBrowse's file-
    // picker thread both check this flag and bail before their next
    // APIDefs deref; without it, a worker mid-Sleep would deref a
    // dangling APIDefs after Nexus tears the addon down. See
    // nexus-addon-dev.md "Things to avoid" (detached threads + Nexus
    // pointers).
    g_Unloading.store(true);

    RemoveNexusShortcut();
    APIDefs->InputBinds.Deregister(KB_TOGGLE_MAIN);
    APIDefs->InputBinds.Deregister(KB_TOGGLE_QB);
    APIDefs->UI.DeregisterCloseOnEscape("emot3 Library##wnd");
    APIDefs->UI.DeregisterCloseOnEscape("emot3 Quickbar##qb");
    APIDefs->WndProc.Deregister(WndProcCallback);
    APIDefs->Renderer.Deregister(AddonRender);
    APIDefs->Renderer.Deregister(QuickbarRender);
    APIDefs->Renderer.Deregister(AddonOptions);
    ShutdownCharacterState();  // unsubscribe the RTAPI addon load/unload events
    ShutdownUnlockScan();      // unsubscribe the Hoard & Seek response event
#ifdef EMOT3_DEVTOOLS
    APIDefs->Renderer.Deregister(RenderDevToolOverlays);  // dev-tools framework
#endif
    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);

    // Give in-flight workers up to ~500 ms to drain. SendOrFillEmote
    // tops out at ~250-400 ms per emote; IconBrowse blocks on the
    // OPENFILENAME dialog which the user might leave open forever, so
    // this is a best-effort wait, not a hard guarantee. After the
    // timeout we ship anyway - the workers will see g_Unloading on
    // their next check and bail without touching APIDefs.
    for (int i = 0; i < 50 && g_InflightWorkers.load() > 0; ++i) {
        Sleep(10);
    }

    APIDefs->Log(ELogLevel_INFO, "emot3", "Unloaded.");
}

// ---- WndProc hook -----------------------------------------------------

static UINT WndProcCallback(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // First reason we hook WndProc: capture the game's HWND on the first
    // message so EmoteAction can target it without polling FindWindowA. (The
    // old keystroke-swallow during emote sends was replaced by the click-time
    // refusal in SendOrFillEmote - see EmoteAction.cpp's ShouldSkipEmoteSend.)
    if (!g_GameHwnd) g_GameHwnd = hWnd;

    // Observe held printable keys for the send / grey-while-moving gate, so it
    // doesn't poll the keyboard every frame (EmoteAction's held-key counter).
    // Observe-only: we never consume these messages (no return 0).
    switch (uMsg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
        case WM_KEYUP:   case WM_SYSKEYUP:
            NoteKeyEvent(uMsg, (unsigned)wParam);
            break;
        case WM_KILLFOCUS:   ClearHeldKeys();  break;  // no key-ups arrive unfocused
        case WM_SETFOCUS:    ReseedHeldKeys(); break;  // re-sync after an alt-tab
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) ClearHeldKeys();
            else                               ReseedHeldKeys();
            break;
        default: break;
    }

#ifdef EMOT3_PLUS
    // +plus only: route the mouse wheel to the Quickbar under click-through and
    // consume it so the game doesn't also zoom. The input-swallow (return 0) is
    // compiled in only for the +plus flavor - see QuickbarWheel.h. The whole
    // WM_MOUSEWHEEL handling lives in the module; this is just the seam.
    if (QbWheelConsume(hWnd, uMsg, wParam, lParam)) return 0;
    // +plus only: while an emote is being injected in swallow mode, consume the
    // user's real keyboard so held keys can't garble the command. Absent from
    // base builds (no keyboard-consume reachable in the public binary).
    if (SendSuppressConsume(uMsg)) return 0;
#endif
    return uMsg;
}
