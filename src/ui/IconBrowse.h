#pragma once

// File-picker plumbing for the "Browse..." button in the Options emote
// editor. The Win32 OPENFILENAME dialog must NOT run on the render thread
// — it pumps messages, and parenting it to the game's HWND while we own
// the swapchain locks the game up. Instead we spawn a detached worker
// thread for the dialog and the render thread polls for completion.

#include <atomic>
#include <mutex>
#include <string>

struct IconBrowseState {
    std::mutex        m;
    std::atomic<bool> active { false };  // worker is currently displaying the dialog
    std::atomic<bool> ready  { false };  // result available, not yet consumed
    std::string       targetId;          // emote command awaiting the result
    std::string       result;            // picked path; empty on Cancel
};
extern IconBrowseState g_IconBrowse;

// Called from the render thread when the Browse button is clicked.
// No-op if a dialog is already in flight or its result hasn't been drained.
void StartIconBrowse(const std::string& emoteCommand,
                     const std::string& currentValue);

// Called once per frame at the top of AddonOptions. Picks up any completed
// pick and writes it to the matching emote's IconPath. Returns true if a
// pick was applied so the caller can persist + flag the texture cache dirty.
bool DrainIconBrowse();
