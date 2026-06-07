#include "IconPicker.h"

#include "Globals.h"      // APIDefs, Texture, g_IconsDir, Windows.h (FindFirstFile)
#include "Icons.h"        // BundledBucket, MakeBundledIconRef, ProbeIconFile
#include "IconCacheConfig.h"  // g_IconCache.maxFolderIcons / maxIconDim
#include "Resources.h"    // bundled icon tables + TryLoadBundledIconBytes
#include "Settings.h"     // g_Settings.UseAIIconFallback
#include "I18n.h"         // L()
#include "Logging.h"      // LOG_WARNING (folder cap)
#include "Profiling.h"    // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
#endif

#include "imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>         // FLT_MIN
#include <string>
#include <vector>

namespace {

// One selectable thumbnail in the grid.
struct PickItem {
    std::string  name;       // display label (stem)
    std::string  nameLower;  // precomputed lowercase, so the search filter does
                             // no per-frame allocation while the modal renders
    ResolvedIcon icon;       // content key + load descriptor (SHARED with cells)
    std::string  writePath;  // what lands in IconPath on click (ref or filename)
};

const char* const kPopupId = "###emot3_icon_picker";

// Folder-bucket safety cap lives in icon_cache.json (g_IconCache.maxFolderIcons,
// default 512): priming is one-way (Nexus has no texture-evict), so a pathological
// icons/ folder would otherwise prime unbounded textures on open. Log once when we stop.
const float kThumb          = 48.f;  // thumbnail edge in px (at 1.0x zoom)
const float kPickScaleMin   = 0.5f;  // picker zoom range; mirrors the Quickbar
const float kPickScaleMax   = 2.5f;  // icon-scale slider (Options > Quickbar)

// Picker target + modal state. File-static: the picker is a single shared modal.
bool             s_pendingOpen   = false;   // OpenIconPicker requested; open next frame
bool             s_keepOpen      = true;    // BeginPopupModal p_open (title-bar X)
EIconTargetKind  s_kind          = EIconTargetKind::Emote;
std::string      s_targetId;
std::string      s_currentPath;             // target's IconPath at open (for highlight)
char             s_search[64]    = {};
float            s_pickerScale   = 1.0f;     // thumbnail zoom (scale slider; session-scoped)

std::vector<std::string> s_folderFiles;             // image filenames in g_IconsDir (refreshed on open)

// Built once per open from the tables + folder + AI setting, then reused every
// frame the modal renders (no per-frame allocation churn). A modal blocks other
// UI, so UseAIIconFallback can't change mid-open; rebuilding on open suffices.
std::vector<PickItem> s_official, s_ai, s_mmai, s_folder;

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Strip a supported-image extension (.png / .jpg / .jpeg, case-insensitive)
// from a filename or relative path. Used to derive the display label + cache
// key from a folder-relative path without falling out of sync with the
// extension list the scan filter accepts.
std::string StripImageExt(const std::string& path) {
    std::string out = path;
    auto endsWithI = [&](const char* suffix, size_t len) {
        if (out.size() < len) return false;
        std::string tail = ToLower(out.substr(out.size() - len));
        return tail == std::string(suffix);
    };
    if (endsWithI(".jpeg", 5)) out.erase(out.size() - 5);
    else if (endsWithI(".png", 4) || endsWithI(".jpg", 4)) out.erase(out.size() - 4);
    return out;
}

// Append every entry of a bundled table to `out`: a content-keyed ResolvedIcon
// (SHARED with cells via MakeBundledResolved) + the writable bundled ref that
// lands in IconPath when the item is picked.
void AddBundledBank(std::vector<PickItem>& out, const BundledIcon* tbl, int cnt,
                    BundledBucket bucket) {
    out.reserve(out.size() + (size_t)cnt);
    for (int i = 0; i < cnt; ++i)
        out.push_back({ tbl[i].command,
                        ToLower(tbl[i].command),
                        MakeBundledResolved(tbl, cnt, tbl[i].command),
                        MakeBundledIconRef(bucket, tbl[i].command) });
}

// (Re)scan addons/emot3/icons recursively for images and LIST each (filenames
// only; textures load lazily per visible grid cell). Run on every open so a
// freshly dropped-in icon appears without a restart. Capped at
// g_IconCache.maxFolderIcons so an enormous tree can't enumerate unbounded
// entries; each file is header-validated here, and over-cap / unreadable ones
// are logged + skipped so the grid only lists loadable icons.
//
// Recursion uses an explicit work stack instead of true recursion so a
// deeply-nested folder tree can't blow the render thread's stack. Each entry
// carries the running relative-prefix it traverses with (e.g. "subdir\\")
// and an isTop flag — the ui/ subfolder at the top level is skipped (UI
// overrides live there; surfacing them as emote-icon candidates would
// reassign star/paperclip/lock and break the addon's chrome). A deeper
// folder literally named "ui" is fine — only the top-level boundary is
// policed.
void ScanFolderTextures() {
    s_folderFiles.clear();
    if (g_IconsDir.empty() || !APIDefs) return;

    struct Frame { std::string prefix; std::string dir; bool isTop; int depth; };
    std::vector<Frame> stack;
    stack.push_back({ std::string(), g_IconsDir, true, 0 });

    // Traversal backstops: a reparse-point (junction/symlink) CYCLE under icons/
    // would otherwise recurse forever, and a junction into a huge tree would walk
    // the whole thing - both on the render thread, on picker open. Bound the depth
    // and the directory count rather than forbidding reparse points outright, so a
    // legitimate symlinked icon library still resolves.
    constexpr int kMaxDepth = 8;
    constexpr int kMaxDirs  = 4096;
    int  dirsVisited = 0;
    bool bounded     = false;

    bool capped = false;
    while (!stack.empty() && !capped) {
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (++dirsVisited > kMaxDirs) { bounded = true; break; }

        const std::string pattern = f.dir + "\\*";   // walk everything, filter below
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            // FindFirstFile names can't contain a path separator / ':' / wildcard
            // (illegal in Windows names), so `name` is always a safe relative leaf.
            // Guard anyway against an exotic name; skip anything weird.
            if (name.find_first_of("\\/:") != std::string::npos) continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Skip the UI-overrides subfolder at the top level only.
                if (f.isTop && ToLower(name) == "ui") continue;
                if (f.depth + 1 > kMaxDepth) { bounded = true; continue; }   // don't recurse deeper
                stack.push_back({ f.prefix + name + "\\",
                                  f.dir + "\\" + name,
                                  false, f.depth + 1 });
                continue;
            }

            // File. Filter to image types Nexus' texture loader decodes:
            // PNG and JPEG. Case-insensitive; matches the formats the old
            // (now-removed) OS file dialog accepted. Other extensions are
            // skipped here rather than loaded + rejected by the decoder
            // later, so the picker grid stays clean.
            std::string lowerName = ToLower(name);
            bool ok = false;
            if (lowerName.size() >= 4) {
                std::string ext4 = lowerName.substr(lowerName.size() - 4);
                if (ext4 == ".png" || ext4 == ".jpg") ok = true;
            }
            if (!ok && lowerName.size() >= 5 &&
                lowerName.substr(lowerName.size() - 5) == ".jpeg") ok = true;
            if (!ok) continue;

            // Relative path the picker writes back into IconPath when this entry
            // is chosen (e.g. "themes\\cool.png" for a subfolder pick, or
            // "mycustom.png" at the top level). Same shape the SanitizeIconPath
            // policy accepts; the resolver joins it to g_IconsDir on load.
            const std::string relPath  = f.prefix + name;
            const std::string fullPath = g_IconsDir + "\\" + relPath;

            // Validate the header before reserving a texture: skip + LOG any file
            // over the dimension cap or with an unreadable / wrong header, so the
            // grid only lists icons that will actually load and one giant PNG
            // can't eat a lot of VRAM. Invalid files don't count toward the cap.
            int pw = 0, ph = 0;
            IconProbe pv = ProbeIconFile(fullPath, pw, ph);
            if (pv == IconProbe::TooLarge) {
                LOG_WARNING("Icon picker: skipping %s (%dx%d over the %dpx cap)",
                            relPath.c_str(), pw, ph, g_IconCache.maxIconDim);
                continue;
            }
            if (pv == IconProbe::Unreadable) {
                LOG_WARNING("Icon picker: skipping %s (unreadable or not a PNG/JPEG)",
                            relPath.c_str());
                continue;
            }

            if ((int)s_folderFiles.size() >= g_IconCache.maxFolderIcons) {
                capped = true;
                break;   // out of the FindNext loop; outer while exits next check
            }
            // Lazy: just list it; the thumbnail loads on first show in the grid
            // (RenderBucket -> EnsureResolved), into the shared cell content pool.
            s_folderFiles.push_back(relPath);
        } while (FindNextFileA(h, &fd) && !capped);
        FindClose(h);
    }
    if (capped)
        LOG_WARNING("Icon picker: folder has more than %d icons; showing the first %d",
                    g_IconCache.maxFolderIcons, g_IconCache.maxFolderIcons);
    if (bounded)
        LOG_WARNING("Icon picker: folder scan hit its traversal bound (depth %d / %d dirs); "
                    "some subfolders were skipped (a symlink loop under icons/?)",
                    kMaxDepth, kMaxDirs);
}

// Rebuild the cached bucket item lists from the tables + folder scan + the
// current AI-fallback setting. Called once per open.
void RebuildItems() {
    s_official.clear(); s_ai.clear(); s_mmai.clear(); s_folder.clear();
    AddBundledBank(s_official, kOfficialIcons, kOfficialIconsCount, BundledBucket::Official);
    if (g_Settings.UseAIIconFallback) {
        AddBundledBank(s_ai,   kAIIcons,       kAIIconsCount,       BundledBucket::AI);
        AddBundledBank(s_mmai, kMeMoteAIIcons, kMeMoteAIIconsCount, BundledBucket::MeMoteAI);
    }
    s_folder.reserve(s_folderFiles.size());
    for (const std::string& relPath : s_folderFiles) {
        // Display label = the relative path minus its image extension (subfolder
        // picks read as "themes\\cool"; search matches the lowered label).
        // writePath = relPath verbatim (SanitizeIconPath accepts it; the resolver
        // joins it to g_IconsDir). The thumbnail shares the cell content pool via
        // the key MakeFileResolved builds from the absolute path.
        PickItem it;
        it.name      = StripImageExt(relPath);
        it.nameLower = ToLower(it.name);
        it.icon      = MakeFileResolved(g_IconsDir + "\\" + relPath);
        it.writePath = relPath;
        s_folder.push_back(std::move(it));
    }
}

// Render one labelled bucket as a wrapping grid. Sets *outPick to the chosen
// writePath when an icon is clicked. Returns the number of visible (matching)
// items so the caller can detect an all-empty search.
int RenderBucket(const char* headerKey, const std::vector<PickItem>& items,
                 const std::string& needle, std::string* outPick) {
    std::vector<const PickItem*> vis;
    vis.reserve(items.size());
    for (const auto& it : items)
        if (needle.empty() || it.nameLower.find(needle) != std::string::npos)
            vis.push_back(&it);
    if (vis.empty()) return 0;

    ImGui::TextDisabled("%s", L(headerKey));
    const ImGuiStyle& st = ImGui::GetStyle();
    const float thumb = kThumb * s_pickerScale;   // zoom (picker scale slider)
    const float visX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float stride = thumb + st.FramePadding.x * 2.f + st.ItemSpacing.x;
    for (size_t i = 0; i < vis.size(); ++i) {
        const PickItem& it = *vis[i];
        ImGui::PushID(it.icon.key.empty() ? it.name.c_str() : it.icon.key.c_str());
        // Lazy: load this thumbnail only if its cell is actually on-screen; an
        // off-screen item just looks up (null -> name-button placeholder). Shared
        // with the cell pool, so an already-shown icon is instant here.
        Texture* tex = ImGui::IsRectVisible(ImVec2(kThumb, kThumb))
                           ? EnsureResolved(it.icon)
                           : (it.icon.key.empty() ? nullptr
                                                  : APIDefs->Textures.Get(it.icon.key.c_str()));
        const bool isCurrent = !it.writePath.empty() && it.writePath == s_currentPath;
        if (isCurrent)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.f));
        bool clicked = false;
        if (tex && tex->Resource)
            clicked = ImGui::ImageButton((ImTextureID)tex->Resource,
                                         ImVec2(thumb, thumb),
                                         ImVec2(0, 0), ImVec2(1, 1), 2);
        else  // texture missing (e.g. a folder PNG that failed to decode)
            clicked = ImGui::Button(it.name.c_str(), ImVec2(thumb + 4.f, thumb + 4.f));
        if (isCurrent) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.name.c_str());
        if (clicked) *outPick = it.writePath;
        // Manual wrap: keep on the same line until the next thumbnail would
        // overflow the content edge (ImGui doesn't auto-wrap ImageButtons).
        const float nextX2 = ImGui::GetItemRectMax().x + stride;
        if (i + 1 < vis.size() && nextX2 < visX2) ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    return (int)vis.size();
}

} // namespace

void OpenIconPicker(EIconTargetKind kind, const std::string& targetId,
                    const std::string& currentIconPath) {
    s_kind        = kind;
    s_targetId    = targetId;
    s_currentPath = currentIconPath;  // for highlight; passed in (no catalog lock)
    s_pendingOpen = true;
    s_search[0]   = '\0';
}

// Picker thumbnails now share the cell content pool (EMOT3IC_*), so they're
// counted by the memory monitor's unified icon-pool rows (Icons.cpp
// IconPoolStats) - no separate picker-texture accounting here anymore.

void RenderIconPicker() {
    // Idle fast-path: when no open is pending and the modal isn't showing, do
    // NOTHING — not even SetNextWindow*/BeginPopupModal or a profiler sample —
    // so the picker has zero footprint while the Options window is open but the
    // modal is closed. (RenderIconPicker is only called from AddonOptions /
    // ERenderType_OptionsRender, which Nexus invokes solely while the addon's
    // options page is showing; it never runs while Options is closed.)
    if (!s_pendingOpen && !ImGui::IsPopupOpen(kPopupId)) return;
    PROFILE_SCOPE("opt.iconpicker");  // only ticks while opening / open
    if (s_pendingOpen) {
        s_pendingOpen = false;
        s_keepOpen    = true;
        ScanFolderTextures();   // lists files (validated); thumbnails load lazily
        RebuildItems();
        ImGui::OpenPopup(kPopupId);
    }

    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540.f, 580.f), ImGuiCond_Appearing);
    // Floor on user resize so the search row (input + X + zoom slider) and the
    // footer never collapse into an unusable sliver; height keeps a few thumbnail
    // rows visible. No upper bound.
    ImGui::SetNextWindowSizeConstraints(ImVec2(400.f, 380.f), ImVec2(FLT_MAX, FLT_MAX));

    const std::string title = std::string(L("opt.pick.title")) + kPopupId;
    if (!ImGui::BeginPopupModal(title.c_str(), &s_keepOpen, ImGuiWindowFlags_NoCollapse))
        return;

    // --- Search + clear (X) + zoom slider, one row ---
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float clearW  = ImGui::GetFrameHeight();   // square X, full height
        const float sliderW = 120.f;                     // zoom slider
        const float gap     = st.ItemSpacing.x;
        float searchW = ImGui::GetContentRegionAvail().x - clearW - sliderW - gap * 2.f;
        if (searchW < 120.f) searchW = 120.f;            // floor on a narrow modal
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##pick_search", L("opt.pick.search_hint"),
                                 s_search, sizeof(s_search));
        // Clear (X) - greyed when empty, mirrors the main-panel search clear.
        ImGui::SameLine(0, gap);
        const bool hasSearch = (s_search[0] != '\0');
        if (!hasSearch) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.Alpha * 0.30f);
        if (ImGui::Button("X##pick_clr", ImVec2(clearW, 0.f)) && hasSearch) s_search[0] = '\0';
        if (!hasSearch) ImGui::PopStyleVar();
        if (hasSearch && ImGui::IsItemHovered()) TooltipText("opt.pick.clear_search");
        // Zoom - mirrors the Quickbar icon-scale slider (range + right-click reset).
        ImGui::SameLine(0, gap);
        ImGui::SetNextItemWidth(sliderW);
        ImGui::SliderFloat("##pick_scale", &s_pickerScale,
                           kPickScaleMin, kPickScaleMax, "%.2fx");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) s_pickerScale = 1.0f;
        if (ImGui::IsItemHovered()) TooltipText("opt.pick.scale_tooltip");
    }
    const std::string needle = ToLower(s_search);

    // --- Grid (scrollable), from the cached bucket lists ---
    std::string pick;
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##pick_grid", ImVec2(0.f, -footer), true);
    int shown = 0;
    shown += RenderBucket("opt.pick.bucket_official",   s_official, needle, &pick);
    shown += RenderBucket("opt.pick.bucket_ai",         s_ai,       needle, &pick);
    shown += RenderBucket("opt.pick.bucket_memote_ai",  s_mmai,     needle, &pick);
    shown += RenderBucket("opt.pick.bucket_folder",     s_folder,   needle, &pick);
    if (shown == 0)
        ImGui::TextDisabled("%s", L("opt.pick.no_matches"));
    ImGui::EndChild();

    // --- Footer: user-icon count (lower-left) + use-default / cancel (right) ---
    {
        const ImGuiStyle& fst = ImGui::GetStyle();
        // Lower-left: how many of the user's own icons (addons/emot3/icons) are
        // loaded vs the count cap (max_folder_icons), plus the per-icon size cap
        // (max_icon_dim px) - so the user learns why a too-big or surplus file is
        // skipped. Both caps live in icon_cache.json.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(L("opt.pick.folder_count"),
                            (int)s_folderFiles.size(), g_IconCache.maxFolderIcons,
                            g_IconCache.maxIconDim, g_IconCache.maxIconDim);
        if (ImGui::IsItemHovered()) TooltipText("opt.pick.folder_count_tooltip");
        ImGui::SameLine();

        const char* defLbl = L("opt.pick.clear");   // "Use default" (revert to the chain)
        const char* canLbl = L("common.cancel");
        float wDef   = ImGui::CalcTextSize(defLbl).x + fst.FramePadding.x * 2.f;
        float wCan   = ImGui::CalcTextSize(canLbl).x + fst.FramePadding.x * 2.f;
        float total  = wDef + wCan + fst.ItemSpacing.x;
        float availW = ImGui::GetContentRegionAvail().x;
        if (availW > total) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - total));
        if (ImGui::Button(defLbl)) {
            pick.clear();
            ApplyIconPathToTarget(s_kind, s_targetId, std::string());  // revert to default chain
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(canLbl))
            ImGui::CloseCurrentPopup();
    }

    // A thumbnail was clicked this frame: apply + close.
    if (!pick.empty()) {
        ApplyIconPathToTarget(s_kind, s_targetId, pick);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

#ifdef EMOT3_DEVTOOLS
// Self-registering Runtime State Inspector section (Layer-2 dev-tools standard,
// like the catalog sections). Surfaces picker state (folder scan + target);
// thumbnail textures appear in the shared icon-pool rows ("Textures / resources").
static DevStateRegistrar s_pickerState("Icon picker", [] {
    DevStateRow("folder icons",    "%zu", s_folderFiles.size());
    DevStateRow("target",          "%s",
                s_targetId.empty() ? "(none)" : s_targetId.c_str());
    DevStateRow("target kind",     "%s",
                s_kind == EIconTargetKind::Emote ? "emote" : "me-mote");
});
#endif
