#pragma once
// =====================================================================
//  Cross-addon deploy of staged RadialMenus wheels - the +plus convenience.
//
//  emot3 stages each exported wheel in its OWN folder (addons/emot3/radials/
//  <slug>/) and NEVER writes into another addon's directory by default. The
//  base build just hints the manual copy ("copy <slug>.json into
//  addons/RadialMenus/packs and its icons into ...\icons, then Reload radials").
//  The +plus build adds a one-click "Deploy to RadialMenus" that performs that
//  copy. Only the cross-addon copy is gated (EMOT3_PLUS) - matching the flavor
//  convention (src/plus/ holds +plus features; base links an inert stub).
//
//  Detection (IsRadialMenusInstalled / RadialMenusDir) is available in ALL builds
//  because the tab's status line and the manual hint use it.
// =====================================================================

#include <string>
#include <vector>

// Absolute path to addons/RadialMenus (whether or not it exists). Empty only if the
// Nexus path API is unavailable. Available in every build.
std::string RadialMenusDir();

// True if RadialMenus appears installed (its addon directory exists). Available in
// every build (drives the tab's detected/not-detected status).
bool IsRadialMenusInstalled();

// Outcome of a deploy. `available` is false in base builds (the copy is +plus only),
// which the UI uses to show the manual-copy hint instead of the Deploy button.
struct RadialDeployResult {
    bool        ok        = false;
    bool        available = false;  // false => base build, deploy not compiled in
    int         packs     = 0;      // pack files copied
    int         icons     = 0;      // icon files copied
    std::string error;              // short text on failure
};

// +plus: copy every staged wheel's pack into addons/RadialMenus/packs and its icons
// into addons/RadialMenus/icons (both are slug-named, so what lands in RadialMenus'
// flat folders stays identifiable). Base build: inert stub returning available=false.
// Either way the caller still shows the "Reload radials" reminder.
RadialDeployResult DeployToRadialMenus();

// +plus: is a pack with this page stem currently deployed in RadialMenus' packs
// folder? (Lets the remove flow offer "also delete from RadialMenus" only when there's
// something there.) Base build: always false (it never writes outside emot3's folder).
bool RadialMenusHasPack(const std::string& slug);

// +plus: delete the given page slugs' packs + their emot3_<slug>_* icons from
// RadialMenus' folders. Returns the count of files removed. Base build: no-op (0) -
// touching another addon's folder is a +plus-only action.
int RemoveFromRadialMenus(const std::vector<std::string>& slugs);

// +plus: (re)copy just the given page slugs' pack + icons into RadialMenus,
// overwriting. Returns files copied. Used to push a rename's new name onto an
// already-deployed wheel (same filenames). Base build: no-op (0).
int DeployGroupToRadialMenus(const std::vector<std::string>& slugs);
