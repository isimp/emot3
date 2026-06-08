#include "Options.h"
#include "OptionsCommon.h"
#include "Globals.h"
#include "Logging.h"
#include "I18n.h"
#include "Settings.h"
#include "SaveScheduler.h"  // RequestSave (debounced, off-thread catalog/settings writes)
#include "QuickbarPresets.h"
#include "EmoteData.h"
#include "EmoteAction.h"
#include "Favorites.h"
#include "Usage.h"        // usage::RemoveRef / RemoveAllOfType on delete / clear
#include "StringUtil.h"     // TrimWhitespace / IsAbsolutePath (shared helpers)
#include "Icons.h"          // ResolveIconPath for icon-source status
#include "Layout.h"         // ToggleButton, PushHighContrastButtonStyles
#include "IconBrowse.h"
#include "IconPicker.h"     // OpenIconPicker ("Library..." button)
#include "EmoteBinds.h"     // SyncEmoteBinds on a keybind toggle
#include "NexusShortcut.h"  // ApplyNexusShortcut on settings changes
#include "Resources.h"      // kOfficialIcons / kAIIcons for icon-source status
#include "Profiling.h"      // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

#define NOMINMAX
#include <Windows.h>     // GetFileAttributesA for icon-source status

#include <algorithm>
#include <cctype>
#include <cfloat>        // FLT_MIN for stretch-to-fill input widths
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
// ---- Emotes editor state ----------------------------------

// Persistent per-row edit buffer for the Emotes editor. Carries the row's
// editable fields - display name, the localized slash command, and the
// space-separated aliases - so each can be edited and committed on
// click-away. (The Id is the only read-only field; it isn't buffered here.)
//
// Living outside the InputText call means the buffer survives between
// frames independently of ImGui's internal state, so:
//   - click-away reliably commits (no fragile IsItemDeactivatedAfterEdit
//     dependency on a stable user buffer);
//   - validation can run every frame against current g_Emotes state, so a
//     hint that became stale (because another row's name changed) clears
//     automatically without the user touching this row.
struct RowBuffer {
    std::string name;
    std::string command;   // editable localized command (was read-only before)
    std::string aliases;   // alternate commands, space-separated edit buffer
    bool        initialized = false;
};

// Returns true if `value` collides with any *other* emote's stable Id.
// Id is the catalog's unique key, so this is the hard constraint enforced
// when adding an emote. Pass nullptr for the "new emote" input.
static bool IdCollidesWith(const std::string& value, const Emote* selfPtr) {
    for (const auto& other : g_Emotes) {
        if (&other == selfPtr) continue;
        if (other.Id == value) return true;
    }
    return false;
}

// Returns true if `value` matches any *other* emote's command. Command is
// no longer the key (Id is), so two emotes sharing a command is harmless
// and merely redundant — this drives a soft, non-blocking hint, not a
// hard block.
static bool CommandCollidesWith(const std::string& value, const Emote* selfPtr) {
    for (const auto& other : g_Emotes) {
        if (&other == selfPtr) continue;
        if (other.Command == value) return true;
    }
    return false;
}

// Title-case the stem of a slash command for use as a default display
// name when the user creates a new emote — "/bow" → "Bow",
// "/playdead" → "Playdead". Cheap and predictable; user can edit
// further in the Name field.
static std::string TitleCaseStem(const std::string& command) {
    std::string s = command;
    if (!s.empty() && s.front() == '/') s.erase(0, 1);
    if (s.empty()) return s;
    s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

// Describes where an emote's icon currently resolves from. Used by the
// row's status line so the user can see at a glance whether the icon
// they're looking at is custom, a folder override, bundled official,
// bundled AI fallback, or a letter-button fallback.
//
// Resolution order mirrors ResolveIconSource (ui/Icons.cpp), the same chain the
// lazy loader uses; if the two ever drift, this label will lie and so will the
// actual icon.
static std::string DescribeIconSource(const Emote& e) {
    // 0. Icon picker: a "bundled:<bucket>:<name>" ref names a specific bundled
    //    icon (resolves regardless of UseAIIconFallback). Show the chosen name.
    //    Checked before the path branch below, which would otherwise mislabel
    //    the ref as a "Custom" file path.
    {
        const BundledIcon* tbl = nullptr; int cnt = 0; std::string nm;
        if (ParseBundledIconRef(e.IconPath, tbl, cnt, nm))
            return "Built-in: " + nm;
    }
    // 1. Explicit custom path always wins - surface what the user set
    //    even if it doesn't currently exist on disk (so they can spot a
    //    typo). Distinguish the two storage forms so the user can tell
    //    at a glance whether the file lives in the addon folder
    //    (relative path, IconBrowse collapses these automatically when
    //    picked) or somewhere arbitrary on disk (absolute path).
    if (!e.IconPath.empty()) {
        const std::string& p = e.IconPath;
        bool isAbs = IsAbsolutePath(p);

        std::string resolved = ResolveIconPath(e);
        DWORD attr = GetFileAttributesA(resolved.c_str());
        bool exists = (attr != INVALID_FILE_ATTRIBUTES) &&
                      !(attr & FILE_ATTRIBUTE_DIRECTORY);

        if (isAbs) {
            return (exists ? std::string("Custom (external): ")
                           : std::string("Custom (external, missing): ")) + p;
        }
        // Relative form lives under <addonDir>/icons/. Prefixing the
        // shown path with "icons\" makes that storage location visible
        // - the user sees exactly the file the addon resolves to, not
        // just a bare stem with no context.
        return (exists ? std::string("Custom (addon folder): icons\\")
                       : std::string("Custom (addon folder, missing): icons\\")) + p;
    }

    // No explicit path: the rest of the order (folder override -> official ->
    // AI -> text) comes from the shared ResolveIconSource, the same resolver the
    // texture loader uses, so this label can't drift from what actually loads.
    // (ResolveIconSource only returns Custom when IconPath is set, which the
    // branch above already handled.)
    switch (ResolveIconSource(e)) {
        case IconSource::FolderOverride: {
            std::string stem = e.Id;
            if (!stem.empty() && stem.front() == '/') stem.erase(0, 1);
            std::transform(stem.begin(), stem.end(), stem.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            return "Folder override (icons/" + stem + ".png)";
        }
        case IconSource::BundledOfficial: return "Bundled (ArenaNet official)";
        case IconSource::BundledAI:       return "Bundled (AI fallback)";
        default:                          return "Text fallback (no icon)";
    }
}

// ---- Tab: Emotes ------------------------------------------------------

void RenderEmotesTab() {
    PROFILE_SCOPE("opt.emotes");  // dev perf overlay
    ImGui::TextWrapped("%s", L("opt.em.intro"));

    // Collapsible explainer for the icon resolution chain (where each
    // row's icon comes from). Closed by default; opens cheaply when
    // curiosity strikes. Its order must match ResolveIconSource
    // (ui/Icons.cpp) and DescribeIconSource - if they drift, the user
    // sees the lie before we do.
    if (ImGui::CollapsingHeader(L("opt.em.icon_header"))) {
        ImGui::Indent();
        ImGui::TextWrapped("%s", L("opt.em.icon_explainer"));
        ImGui::Unindent();
    }

    // ---- "Add to catalog" inline input ----
    //
    // The everyday action (add one emote), above the list. Mirrors the
    // "+ Category" UX in MainPanel: a slash-prefixed command with live
    // validation. The stable Id is derived from the command stem
    // (lowercased, no slash) and is the uniqueness constraint - the red
    // border / hard block fires on an Id collision. A command that
    // duplicates another emote's is allowed (Id is the key), surfaced
    // only as a soft note.
    OptionsSection(L("opt.sec.add_emote"));
    static char newCmdBuf[64] = {};

    // Normalize via the shared helper (trim, force leading slash, lowercase) so
    // every command ingress - here, the per-row edit below, and emotes.json
    // load - is identical; derive the stable id from the normalized stem.
    std::string rawNew = newCmdBuf;
    std::string trimmedNew = TrimWhitespace(rawNew);
    std::string normalizedNew = NormalizeEmoteCommand(trimmedNew);
    std::string newId = normalizedNew.size() > 1 ? normalizedNew.substr(1)
                                                 : std::string();

    bool newHasInput = !trimmedNew.empty();
    bool newIdDup = false, newCmdDup = false;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        if (newHasInput && !newId.empty()) {
            newIdDup  = IdCollidesWith(newId, nullptr);
            newCmdDup = CommandCollidesWith(normalizedNew, nullptr);
        }
    }
    // Hard-invalid (blocks add): empty stem or Id collision.
    bool newInvalid = newHasInput && (newId.empty() || newIdDup);

    ImGui::SetNextItemWidth(180.f);
    if (newInvalid) PushInvalidInputStyle();
    ImGui::InputTextWithHint("##newcmd", L("opt.em.cmd_hint"),
                             newCmdBuf, sizeof(newCmdBuf));
    if (newInvalid) {
        PopInvalidInputStyle();
        DrawInvalidInputBorder();
    }
    if (newInvalid && ImGui::IsItemHovered()) {
        if (newId.empty())
            TooltipText("opt.em.cmd_min");
        else
            ImGui::SetTooltip(L("opt.em.id_exists"), newId.c_str());
    }

    ImGui::SameLine();
    bool addEnabled = newHasInput && !newInvalid;
    if (!addEnabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                             ImGui::GetStyle().Alpha * 0.45f);
    }
    bool addPressed = ImGui::Button(L("opt.em.add_button"));
    if (!addEnabled) {
        ImGui::PopStyleVar();
        ImGui::PopItemFlag();
    }
    // Soft, non-blocking note: another emote already sends this command.
    if (addEnabled && newCmdDup) {
        ImGui::TextDisabled(L("opt.em.shared_note"), normalizedNew.c_str());
    }
    if (addPressed && addEnabled) {
        // Default display name is the stem title-cased; de-duped against
        // existing names with a " 2"/" 3" suffix so a new row never ships
        // with a duplicate display name.
        std::string baseName = TitleCaseStem(normalizedNew);
        std::string finalName = baseName;
        {
            std::lock_guard<std::mutex> lk(g_EmotesMutex);
            for (int n = 2; n < 1000; ++n) {
                bool dup = false;
                for (const auto& other : g_Emotes) {
                    if (other.Name == finalName) { dup = true; break; }
                }
                if (!dup) break;
                finalName = baseName + " " + std::to_string(n);
            }
            Emote e;
            e.Id           = newId;
            e.Command      = normalizedNew;
            e.Name         = finalName;
            e.IsCore       = false;
            e.IsTargetable = false;
            e.IsMadKing    = false;  // user emotes aren't part of the bundled set
            g_Emotes.push_back(std::move(e));
        }
        LOG_INFO("Added emote id=%s command=%s (\"%s\")",
                 newId.c_str(), normalizedNew.c_str(), finalName.c_str());
        RequestSave(SaveKind::Emotes);
        MarkEmotesDirty();
        newCmdBuf[0] = '\0';  // Clear input on commit.
    }

    // Persistent per-row edit buffers keyed by Id (stable). New rows
    // initialize lazily.
    static std::map<std::string, RowBuffer> s_rowBufs;

    // Per-row icon-source status cache, keyed by Id. DescribeIconSource()
    // stats the disk (GetFileAttributesA, usually on a non-existent override
    // file) and scans the bundled tables; doing that for all 67 bundled
    // emotes every frame is what stalled the whole overlay when this tab was
    // open. Recompute a row's status only when an input that actually affects
    // it changed - the explicit IconPath or the AI-fallback toggle. (A PNG
    // dropped into icons/ while the tab is open still needs a tab reopen to
    // show, same expectation as the texture cache's restart-to-apply note.)
    struct IconStatusCache {
        std::string iconPath;
        bool        aiFallback = false;
        bool        valid      = false;
        bool        isDiskFile = false;  // Custom / FolderOverride -> offer Refresh
        std::string text;
    };
    static std::map<std::string, IconStatusCache> s_iconStatus;

    // Per-row expand/collapse state, keyed by Id. Default-missing reads as
    // false, so rows are collapsed by default - a fresh 67-row catalog opens
    // as a scannable list of Ids, and only the rows the user expands pay for
    // the editor body (and its icon-status disk lookup). This is also why the
    // list no longer uses ImGuiListClipper: collapsible rows are variable
    // height, which 1.80's clipper can't handle (it assumes uniform rows). The
    // collapsed-by-default body-skip recovers the same "don't process what
    // isn't shown" win without the uniform-height constraint.
    static std::map<std::string, bool> s_rowOpen;
    // One-shot from the Expand all / Collapse all buttons: 0 = no action,
    // 1 = open every row this frame, -1 = close every row. Applied in the loop
    // and reset after it.
    static int s_setAllOpen = 0;

    std::string deleteId;      // empty = nothing to delete this frame
    bool        emoteEdited = false;
    bool        keybindEdited = false;  // a UserKeybind toggle -> re-sync Nexus binds

    // List toolbar: a muted emote count on the left, the Expand all /
    // Collapse all bulk toggles pinned to the right (right-aligned by
    // measuring the two buttons' widths against the available width).
    // The buttons drive s_setAllOpen, applied per row in the loop below.
    {
        int emoteCount = 0;
        { std::lock_guard<std::mutex> lk(g_EmotesMutex); emoteCount = (int)g_Emotes.size(); }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(L("opt.em.count"), emoteCount);

        // Rescan / Expand all / Collapse all, right-aligned together. Rescan
        // re-checks addons/emot3/icons for added/changed/removed files for EVERY
        // row at once (the per-row Refresh only shows for already-disk-backed rows,
        // so a PNG newly dropped at icons/<id>.png for a letter/bundled entry has
        // no per-row button - this is its path).
        const ImGuiStyle& st = ImGui::GetStyle();
        float wRescan   = ImGui::CalcTextSize(L("opt.icon.rescan")).x     + st.FramePadding.x * 2.f;
        float wExpand   = ImGui::CalcTextSize(L("opt.em.expand_all")).x   + st.FramePadding.x * 2.f;
        float wCollapse = ImGui::CalcTextSize(L("opt.em.collapse_all")).x + st.FramePadding.x * 2.f;
        float total     = wRescan + wExpand + wCollapse + st.ItemSpacing.x * 2.f;
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > total)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total));
        if (ImGui::SmallButton(L("opt.icon.rescan"))) {
            s_iconStatus.clear();   // every row re-stats its icon status next render
            MarkEmotesDirty();      // every visible cell re-resolves (memo drops on the epoch)
            LOG_INFO("Rescan icons: cleared the icon-status cache + bumped the catalog epoch");
        }
        if (ImGui::IsItemHovered())
            TooltipText("opt.icon.rescan_tooltip");
        ImGui::SameLine();
        if (ImGui::SmallButton(L("opt.em.expand_all")))   s_setAllOpen =  1;
        ImGui::SameLine();
        if (ImGui::SmallButton(L("opt.em.collapse_all"))) s_setAllOpen = -1;
    }

    ImGui::BeginChild("##emotelist", ImVec2(0, 320.f), true);
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);

        // Sort indices alphabetically by Id so the visible order is
        // stable regardless of insertion order. g_Emotes itself stays in
        // its on-disk order — only the display walks via this index list.
        std::vector<size_t> order;
        order.reserve(g_Emotes.size());
        for (size_t i = 0; i < g_Emotes.size(); ++i) {
            order.push_back(i);  // every emote shows now — IsCore is editable
        }
        std::sort(order.begin(), order.end(), [](size_t a, size_t b) {
            return g_Emotes[a].Id < g_Emotes[b].Id;
        });

        // Gold matches the user-category headers in MainPanel — pulls the
        // Id into the same "this is the identifier" visual language the
        // user is already used to.
        const ImVec4 kCmdColor(0.92f, 0.78f, 0.32f, 1.f);

        // Per-frame collision counts over the stored commands/names. The
        // per-row "another emote shares this command/name" checks used to call
        // CommandCollidesWith / NameCollidesWith, each an O(N) scan of
        // g_Emotes - O(N^2) once many rows are expanded. Counting once is O(N)
        // and the per-row check becomes an O(1) map lookup.
        //
        // Exactly behavior-preserving: `sharesValue(value, selfCounted)` is
        // true iff some *other* emote has the value, computed as count(value)
        // minus (1 if this row is itself counted under value). `selfCounted` is
        // just `e.Field == value` at the check site. To stay identical to the
        // old live g_Emotes scan even when a row auto-commits mid-loop, the
        // commit sites update these counts (--old, ++new) before mutating the
        // emote, so a later row this frame sees the change just as the scan
        // would have.
        std::unordered_map<std::string, int> cmdCounts, nameCounts;
        cmdCounts.reserve(g_Emotes.size());
        nameCounts.reserve(g_Emotes.size());
        for (const auto& em : g_Emotes) {
            ++cmdCounts[em.Command];
            ++nameCounts[em.Name];
        }
        auto sharesValue = [](const std::unordered_map<std::string, int>& counts,
                              const std::string& value, bool selfCounted) {
            auto it = counts.find(value);
            int n = (it == counts.end()) ? 0 : it->second;
            return (n - (selfCounted ? 1 : 0)) > 0;
        };

        // Width of the expanded-row form's label column. Shared by every row so
        // the field inputs line up; the widest of the four field labels plus
        // frame padding. The field column stretches to fill whatever's left, so
        // inputs scale with the panel and can't overflow its right edge.
        float labelColW = 0.f;
        for (const char* k : { "opt.em.lbl_command", "opt.em.lbl_name",
                               "opt.em.lbl_aliases", "opt.em.lbl_flags",
                               "opt.em.lbl_icon" })
            labelColW = std::max(labelColW, ImGui::CalcTextSize(L(k)).x);
        labelColW += ImGui::GetStyle().FramePadding.x * 2.f;

        for (size_t idx : order) {
            Emote& e = g_Emotes[idx];
            const std::string rowKey = e.Id;

            RowBuffer& rb = s_rowBufs[rowKey];
            if (!rb.initialized) {
                rb.name        = e.Name;
                rb.command     = e.Command;
                rb.aliases.clear();   // join the emote's aliases, space-separated
                for (const auto& a : e.Aliases) {
                    if (!rb.aliases.empty()) rb.aliases += ' ';
                    rb.aliases += a;
                }
                rb.initialized = true;
            }

            ImGui::PushID(rowKey.c_str());

            // Apply a queued Expand all / Collapse all before reading state.
            bool& rowOpen = s_rowOpen[rowKey];
            if (s_setAllOpen != 0) rowOpen = (s_setAllOpen > 0);

            // ============================================================
            //  Row header — a disclosure arrow + gold Id on the left (the
            //  stable, read-only anchor), soft red Delete on the right.
            //  Collapsed by default; the editable command/name/icon body
            //  below only renders when expanded. Separate widgets (not a
            //  CollapsingHeader) so the Delete button can't overlap the
            //  toggle's hit-test.
            // ============================================================
            if (ImGui::ArrowButton("##expand", rowOpen ? ImGuiDir_Down : ImGuiDir_Right))
                rowOpen = !rowOpen;
            ImGui::SameLine();
            ImGui::TextColored(kCmdColor, "%s", e.Id.c_str());
            if (ImGui::IsItemHovered())
                TooltipText("opt.em.id_tooltip");

            // Display name in grey next to the Id, mirroring the /me-motes
            // editor so the two catalog tabs read the same. Skipped when Name
            // is empty or equals the Id (no redundant "bow — bow").
            const std::string& dispName = e.Name.empty() ? e.Id : e.Name;
            if (dispName != e.Id) {
                ImGui::SameLine();
                ImGui::TextDisabled("— %s", dispName.c_str());
            }

            // Right-align the Delete button on the same line. SmallButton
            // + warm-red styling — readable as "destructive" without
            // shouting in the default state.
            //
            // Positioning bug fix: GetContentRegionAvail.x must be
            // queried *after* SameLine. Before SameLine the cursor is
            // at column 0 of the next line and the "remaining" width
            // covers the whole row, so adding it as an offset overshoots
            // the right edge by the width of the command text.
            {
                const char* lbl = L("common.delete");
                float btnW = ImGui::CalcTextSize(lbl).x +
                             ImGui::GetStyle().FramePadding.x * 2.f;
                ImGui::SameLine();
                float remainingW = ImGui::GetContentRegionAvail().x;
                if (remainingW > btnW)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (remainingW - btnW));
                int destrPushes = PushDestructiveButtonStyles(/*includeText=*/true);
                if (ImGui::SmallButton(lbl)) deleteId = e.Id;
                ImGui::PopStyleColor(destrPushes);
            }

            // ============================================================
            //  Body — only when expanded. Slight left indent so the rows
            //  visually "hang" under their Id title.
            // ============================================================
            if (rowOpen) {
            ImGui::Indent(10.f);

            // Two-column form: a fixed label column (labelColW, shared by every
            // row) + a stretch field column. Inputs use -FLT_MIN so they fill
            // the field column and scale with the panel; nothing can run past
            // the child's right edge or clip at narrow Options widths.
            if (ImGui::BeginTable("##fields", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                                        labelColW);
                ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);

                // ---- Command ----
                // Editable: the command is just the text sent to chat (the
                // user's preference), not the key. Empty commits are ignored;
                // a command that duplicates another emote's is allowed (Id is
                // the key) and only flagged as a soft note.
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(L("opt.em.lbl_command"));
                ImGui::TableSetColumnIndex(1);
                std::string trimmedCmd = TrimWhitespace(rb.command);
                bool cmdDirty = (trimmedCmd != e.Command);
                char cmdBuf[64];
                strncpy_s(cmdBuf, sizeof(cmdBuf), rb.command.c_str(), _TRUNCATE);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputTextWithHint("##command", L("opt.em.cmd_hint"),
                                              cmdBuf, sizeof(cmdBuf))) {
                    rb.command = cmdBuf;
                }
                bool cmdActive = ImGui::IsItemActive();
                if (ImGui::IsItemHovered())
                    TooltipText("opt.em.cmd_field_tooltip");
                if (cmdDirty && !trimmedCmd.empty() && !cmdActive) {
                    // Normalize on commit (leading '/' etc.) so an edited command
                    // can't break the send path - the add path does the same.
                    std::string normCmd = NormalizeEmoteCommand(trimmedCmd);
                    --cmdCounts[e.Command]; ++cmdCounts[normCmd];  // keep counts live
                    e.Command  = normCmd;
                    rb.command = normCmd;
                    emoteEdited = true;
                }
                // Soft, non-blocking note below the input (not SameLine) so it
                // can't widen the field column.
                if (!trimmedCmd.empty() && sharesValue(cmdCounts, trimmedCmd, e.Command == trimmedCmd)) {
                    ImGui::TextDisabled("%s", L("opt.em.shared_command"));
                    if (ImGui::IsItemHovered())
                        TooltipText("opt.em.shared_command_tooltip");
                }

                // ---- Name ----
                // Validation runs every frame against current g_Emotes, so a
                // row's red overlay clears the instant another row's edit
                // resolves the conflict — no need for the user to refocus.
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(L("opt.em.lbl_name"));
                ImGui::TableSetColumnIndex(1);
                std::string trimmedName = TrimWhitespace(rb.name);
                bool nameDirty   = (trimmedName != e.Name);
                bool nameInvalid = nameDirty && (trimmedName.empty() ||
                                                 sharesValue(nameCounts, trimmedName,
                                                             e.Name == trimmedName));
                char nameBuf[64];
                strncpy_s(nameBuf, sizeof(nameBuf), rb.name.c_str(), _TRUNCATE);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (nameInvalid) PushInvalidInputStyle();
                if (ImGui::InputTextWithHint("##name", L("opt.em.name_hint"),
                                              nameBuf, sizeof(nameBuf))) {
                    rb.name = nameBuf;
                }
                bool nameActive = ImGui::IsItemActive();
                if (nameInvalid) {
                    PopInvalidInputStyle();
                    DrawInvalidInputBorder();
                }
                if (nameInvalid && ImGui::IsItemHovered()) {
                    if (trimmedName.empty()) {
                        TooltipText("common.name_empty");
                    } else {
                        ImGui::SetTooltip(L("opt.em.name_dup"), trimmedName.c_str());
                    }
                }
                // Auto-commit: dirty + valid + not currently being edited.
                if (nameDirty && !nameInvalid && !nameActive) {
                    --nameCounts[e.Name]; ++nameCounts[trimmedName];  // keep counts live
                    e.Name  = trimmedName;
                    rb.name = trimmedName;
                    emoteEdited = true;
                }

                // ---- Aliases ----
                // Alternate commands that also identify this emote (used by the
                // GW2-API unlock sync + main-panel search; not for sending).
                // Space/comma-separated; normalized + deduped on commit.
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(L("opt.em.lbl_aliases"));
                ImGui::TableSetColumnIndex(1);
                {
                    char aliasBuf[256];
                    strncpy_s(aliasBuf, sizeof(aliasBuf), rb.aliases.c_str(), _TRUNCATE);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputTextWithHint("##aliases", L("opt.em.aliases_hint"),
                                                 aliasBuf, sizeof(aliasBuf))) {
                        rb.aliases = aliasBuf;
                    }
                    bool aliasActive = ImGui::IsItemActive();
                    if (ImGui::IsItemHovered())
                        TooltipText("opt.em.aliases_tooltip");
                    // Commit when not actively editing: parse the buffer into a
                    // normalized, deduped list (drop empties + the primary command)
                    // and canonicalize the buffer back to the parsed form.
                    if (!aliasActive) {
                        std::vector<std::string> parsed;
                        std::string tok;
                        auto flush = [&]() {
                            if (tok.empty()) return;
                            std::string na = NormalizeEmoteCommand(tok);
                            tok.clear();
                            if (na.empty() || na == e.Command) return;
                            if (std::find(parsed.begin(), parsed.end(), na) == parsed.end())
                                parsed.push_back(na);
                        };
                        for (char ch : rb.aliases) {
                            if (ch == ' ' || ch == '\t' || ch == ',') flush();
                            else tok += ch;
                        }
                        flush();
                        if (parsed != e.Aliases) { e.Aliases = parsed; emoteEdited = true; }
                        std::string joined;
                        for (const auto& a : parsed) {
                            if (!joined.empty()) joined += ' ';
                            joined += a;
                        }
                        rb.aliases = joined;
                    }
                }

                // ---- Flags ----
                // Targetable / Core / Mad King, flowed left-to-right and wrapped
                // to a new line within the cell when the next one wouldn't fit —
                // so the cluster never overflows a narrow Options width (replaces
                // the old forced SameLine that pushed Mad King onto its own line).
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(L("opt.em.lbl_flags"));
                ImGui::TableSetColumnIndex(1);
                {
                    const ImGuiStyle& st = ImGui::GetStyle();
                    float cellW = ImGui::GetContentRegionAvail().x;
                    float used  = 0.f;
                    bool  first = true;
                    // onOff: -1 = prose tooltip from tipKey; 0/1 = On/Off
                    // tooltip from <labelKey>.on/.off, default Off (0) / On (1).
                    auto flowCheckbox = [&](const char* labelKey, bool* value,
                                            const char* tipKey, int onOff) {
                        const char* lbl = L(labelKey);
                        float w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                                  ImGui::CalcTextSize(lbl).x;
                        if (first) {
                            first = false; used = w;
                        } else if (used + st.ItemSpacing.x + w <= cellW) {
                            ImGui::SameLine(); used += st.ItemSpacing.x + w;
                        } else {
                            used = w;  // wrap to a new line in this cell
                        }
                        if (ImGui::Checkbox(lbl, value)) emoteEdited = true;
                        if (ImGui::IsItemHovered()) {
                            if (onOff < 0) {
                                TooltipText(tipKey);
                            } else {
                                std::string on  = std::string(labelKey) + ".on";
                                std::string off = std::string(labelKey) + ".off";
                                TooltipOnOff(on.c_str(), off.c_str(), onOff == 1);
                            }
                        }
                    };
                    flowCheckbox("opt.em.targetable", &e.IsTargetable,
                                 "opt.em.targetable_tooltip", -1);
                    flowCheckbox("opt.em.core", &e.IsCore, nullptr, /*onOff default Off=*/0);
                    flowCheckbox("opt.em.mad_king", &e.IsMadKing,
                                 "opt.em.mad_king_tooltip", -1);
                    // Opt-in Nexus keybind (also makes it radial-exportable).
                    // Snapshot to detect the change so we re-sync the binds.
                    bool kbBefore = e.UserKeybind;
                    flowCheckbox("opt.em.keybind", &e.UserKeybind, nullptr, /*Off*/0);
                    if (e.UserKeybind != kbBefore) keybindEdited = true;
                    // Already bound for any radial wheel it's in (no key needed).
                    RadialMembershipNote(EFavoriteRefType::Emote, e.Id);
                }

                // ---- Icon ----
                // Read-only status describing where the icon currently resolves
                // from. Cached per row (s_iconStatus) — DescribeIconSource stats
                // the disk, so recompute only when IconPath or the AI-fallback
                // toggle changed. Library/Refresh/Clear pick, reload, or remove it.
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(L("opt.em.lbl_icon"));
                ImGui::TableSetColumnIndex(1);
                IconStatusCache& ics = s_iconStatus[rowKey];
                if (!ics.valid || ics.iconPath != e.IconPath ||
                    ics.aiFallback != g_Settings.UseAIIconFallback) {
                    ics.iconPath   = e.IconPath;
                    ics.aiFallback = g_Settings.UseAIIconFallback;
                    ics.text       = DescribeIconSource(e);
                    IconSource src = ResolveIconSource(e);   // disk tiers -> offer Refresh
                    ics.isDiskFile = (src == IconSource::Custom ||
                                      src == IconSource::FolderOverride);
                    ics.valid      = true;
                }
                // The label column already says "Icon", so the status drops the
                // old "Icon: " prefix. Ellipsized to the field width; full text
                // on hover. The Library/Refresh/Clear buttons sit on their own
                // line below so a long path can't push them past the row's edge.
                {
                    float availW = ImGui::GetContentRegionAvail().x;
                    if (availW < 40.f) availW = 40.f;
                    std::string statusFit = Ellipsize(ics.text, availW);
                    ImGui::TextDisabled("%s", statusFit.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", ics.text.c_str());
                }
                // In-app icon picker (visual grid). The OS "From file..." dialog
                // was removed once the picker landed: it pulls from every bundled
                // bucket AND the user's icons/ folder, so a drop-in PNG is the
                // disk-side self-service path and the Library button covers the
                // in-app side. Hand-edit `emotes.json` "icon" for an arbitrary
                // absolute path.
                if (ImGui::SmallButton((std::string(L("opt.pick.button")) + "##iconlib").c_str()))
                    OpenIconPicker(EIconTargetKind::Emote, e.Id, e.IconPath);  // under g_EmotesMutex; pass path (picker must not re-lock)
                if (ImGui::IsItemHovered())
                    TooltipText("opt.pick.button_tooltip");
                // Refresh: only for a disk-file icon (bundled/letter can't change
                // on disk). Forces a full re-stat of this row - clears the icon
                // memo (so the cell reloads) AND this row's status cache (so the
                // status line + this button's own isDiskFile visibility re-resolve;
                // both are otherwise cached until IconPath/AI changes, which a bare
                // file edit/delete does NOT do). A file replaced in place reloads
                // (mtime/size keyed, no churn when unchanged); a file DELETED resets
                // the cell to the fallback, flips the status to "missing", and hides
                // this button.
                if (ics.isDiskFile) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton((std::string(L("opt.icon.refresh")) + "##iconrefresh").c_str())) {
                        InvalidateEmoteIcon(e.Id);   // cell re-resolves next render
                        ics.valid = false;           // status line + isDiskFile recompute next frame
                    }
                    if (ImGui::IsItemHovered())
                        TooltipText("opt.icon.refresh_tooltip");
                }
                ImGui::SameLine();
                {
                    bool hasOverride = !e.IconPath.empty();
                    if (!hasOverride) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                                          ImGui::GetStyle().Alpha * 0.30f);
                    if (ImGui::SmallButton((std::string(L("common.clear")) + "##iconclr").c_str()) && hasOverride) {
                        e.IconPath.clear();
                        emoteEdited = true;
                    }
                    if (!hasOverride) ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered() && hasOverride)
                        TooltipText("opt.em.clear_icon_tooltip");
                }

                ImGui::EndTable();
            }

            ImGui::Unindent(10.f);
            ImGui::Spacing();
            }  // end if (rowOpen)

            // Divider after every row (collapsed or not) so the list reads
            // as discrete entries.
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // The Expand/Collapse all request was applied to every row this frame;
    // clear it so per-row toggles work again next frame.
    s_setAllOpen = 0;

    if (!deleteId.empty()) {
        DeleteEmote(deleteId);       // erase + favorites/manual-unlock cascade + persist + dirty
        usage::RemoveRef(EFavoriteRefType::Emote, deleteId);  // drop it from Recently/Frequently used
        s_rowBufs.erase(deleteId);   // UI-only: drop the edit buffer for the now-gone row
    }

    // Prune buffers whose emote no longer exists. Bounded scan; map is small.
    // Keyed by Id, and covers every emote (core included) so an in-progress
    // edit isn't pruned out from under the user.
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        std::set<std::string> live;
        for (const auto& e : g_Emotes) live.insert(e.Id);
        for (auto it = s_rowBufs.begin(); it != s_rowBufs.end(); ) {
            if (live.find(it->first) == live.end())
                it = s_rowBufs.erase(it);
            else
                ++it;
        }
        for (auto it = s_iconStatus.begin(); it != s_iconStatus.end(); ) {
            if (live.find(it->first) == live.end())
                it = s_iconStatus.erase(it);
            else
                ++it;
        }
        for (auto it = s_rowOpen.begin(); it != s_rowOpen.end(); ) {
            if (live.find(it->first) == live.end())
                it = s_rowOpen.erase(it);
            else
                ++it;
        }
    }

    if (emoteEdited) {
        RequestSave(SaveKind::Emotes);
        MarkEmotesDirty();
    }
    if (keybindEdited) SyncEmoteBinds();  // register/deregister the toggled bind

    // ===== Bundled emotes (uncommon: reseed) =====
    // Bottom of the tab, next to Clear catalog: both are catalog-wide
    // maintenance actions, distinct from the everyday "add one emote".
    OptionsSection(L("opt.sec.bundled"));
    ImGui::TextWrapped("%s", L("opt.em.bundled_explainer"));
    ImGui::Spacing();
    {
        std::vector<std::string> langs = AvailableEmoteLanguages();
        std::string current = g_EmoteLanguage.empty() ? std::string("en")
                                                       : g_EmoteLanguage;
        int curIdx = 0;
        for (size_t i = 0; i < langs.size(); ++i)
            if (langs[i] == current) { curIdx = (int)i; break; }

        ImGui::Text("%s", L("opt.em.emote_language"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.f);
        std::string emPreview = (curIdx < (int)langs.size())
            ? std::string(L(("lang." + langs[curIdx]).c_str())) : std::string("en");
        if (ImGui::BeginCombo("##emotelang", emPreview.c_str())) {
            for (size_t i = 0; i < langs.size(); ++i) {
                bool sel = ((int)i == curIdx);
                if (ImGui::Selectable(L(("lang." + langs[i]).c_str()), sel) && !sel) {
                    // Preference only: the language NEW bundled emotes are
                    // added in (below). It does NOT relocalize emotes already
                    // in the catalog - that would clobber the user's edits and
                    // retroactively change what they see, which is surprising.
                    LOG_INFO("emote language (for new bundled emotes) -> %s",
                             langs[i].c_str());
                    g_EmoteLanguage = langs[i];
                    RequestSave(SaveKind::Emotes);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            TooltipText("opt.em.emote_language_tooltip");

        // Optional secondary language, right under the primary: folds its
        // commands in as searchable aliases on the emotes this Restore adds.
        // Excludes the primary; tracked by code so it survives a primary change.
        static std::string s_secondaryRestore;  // "" = none
        if (s_secondaryRestore == current) s_secondaryRestore.clear();
        ImGui::Text("%s", L("opt.em.secondary_language"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.f);
        const char* secPreview = s_secondaryRestore.empty()
            ? L("common.none") : L(("lang." + s_secondaryRestore).c_str());
        if (ImGui::BeginCombo("##emotelang2", secPreview)) {
            if (ImGui::Selectable(L("common.none"), s_secondaryRestore.empty()))
                s_secondaryRestore.clear();
            for (size_t i = 0; i < langs.size(); ++i) {
                if (langs[i] == current) continue;  // can't alias the primary
                bool sel2 = (langs[i] == s_secondaryRestore);
                if (ImGui::Selectable(L(("lang." + langs[i]).c_str()), sel2))
                    s_secondaryRestore = langs[i];
                if (sel2) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            TooltipText("opt.em.secondary_language_tooltip");

        // The language above only governs NEWLY added bundled emotes; it does
        // not relocalize the catalog. Spell out the clear-then-restore route
        // for a user who actually wants to switch the language of what they
        // have. Muted - it's a footnote to the language controls above.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", L("opt.em.switch_language_note"));
        ImGui::PopStyleColor();

        ImGui::Spacing();
        if (ImGui::Button(L("opt.em.restore_bundled"))) {
            SeedDefaultEmotes(current, s_secondaryRestore);
            RequestSave(SaveKind::Emotes);
            MarkEmotesDirty();
        }
        if (ImGui::IsItemHovered())
            TooltipText("opt.em.restore_bundled_tooltip");
    }

    // ===== Clear catalog (uncommon: destructive) =====
    OptionsSection(L("opt.sec.clear"));
    int clearPushes = PushDestructiveButtonStyles(/*includeText=*/false);
    if (ImGui::Button(L("opt.em.clear_catalog")))
        ImGui::OpenPopup("###clearcatalog");
    ImGui::PopStyleColor(clearPushes);
    if (ImGui::IsItemHovered())
        TooltipText("opt.em.clear_catalog_tooltip");

    // Confirmation modal - destructive, gated behind an explicit second
    // click. "title###id" keeps the popup id stable while the title
    // localizes. Centered on the screen, body wrapped to a sane width so
    // it doesn't stretch into one ultra-wide line.
    std::string clearTitle = std::string(L("opt.em.clear_modal_title")) + "###clearcatalog";
    // Center on screen. This ImGui (1.80 via Nexus) doesn't expose
    // GetMainViewport, so use the display size from the IO.
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(clearTitle.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        int total = 0;
        { std::lock_guard<std::mutex> lk(g_EmotesMutex); total = (int)g_Emotes.size(); }
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextWrapped(L("opt.em.clear_modal_body"), total);
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", L("opt.em.cannot_undo"));
        ImGui::Spacing();

        // Both buttons sized to the longer label so neither clips.
        float btnW = std::max(ImGui::CalcTextSize(L("opt.em.remove_all")).x,
                              ImGui::CalcTextSize(L("common.cancel")).x)
                   + ImGui::GetStyle().FramePadding.x * 2.f + 8.f;
        if (ImGui::Button(L("opt.em.remove_all"), ImVec2(btnW, 0))) {
            ClearEmotes();
            g_EmoteLanguage.clear();
            // Clearing the catalog must also drop the now-dangling Emote-typed
            // refs from favorites and the manual-unlock list - otherwise the
            // General-tab category counters keep counting ghosts. Keep the
            // category names (user structure) and any /me-mote-typed refs
            // (a separate catalog, untouched by this clear).
            for (auto& cat : g_Settings.FavoriteCategories) {
                cat.Refs.erase(std::remove_if(cat.Refs.begin(), cat.Refs.end(),
                                              [](const FavoriteRef& r) {
                                                  return r.Type == EFavoriteRefType::Emote;
                                              }),
                               cat.Refs.end());
            }
            g_Settings.ManuallyUnlocked.clear();
            usage::RemoveAllOfType(EFavoriteRefType::Emote);  // drop emote usage history
            RequestSave(SaveKind::Emotes);
            RequestSave(SaveKind::Settings);
            MarkEmotesDirty();
            LOG_INFO("Catalog cleared by user (%d emote(s) removed)", total);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L("common.cancel"), ImVec2(btnW, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}