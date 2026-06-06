#pragma once

// File-picker plumbing for the "Browse..." button in the Options emote
// editor. The Win32 OPENFILENAME dialog must NOT run on the render thread
// — it pumps messages, and parenting it to the game's HWND while we own
// the swapchain locks the game up. Instead we spawn a detached worker
// thread for the dialog and the render thread polls for completion.

#include <atomic>
#include <mutex>
#include <string>

// Which catalog the in-flight browse will write back to. Stored in
// IconBrowseState so DrainIconBrowse routes the picked path to the right
// place (the official Emote catalog or the /me-mote catalog — separate
// domains, separate persistence). Adding a third kind later means appending
// a value + extending the switch in DrainIconBrowse.
enum class EIconTargetKind { Emote, MeMote };

struct IconBrowseState {
    std::mutex        m;
    std::atomic<bool> active { false };  // worker is currently displaying the dialog
    std::atomic<bool> ready  { false };  // result available, not yet consumed
    EIconTargetKind   targetKind { EIconTargetKind::Emote };
    std::string       targetId;          // catalog Id awaiting the result
    std::string       result;            // picked path; empty on Cancel
};
extern IconBrowseState g_IconBrowse;

// Called from the render thread when the Browse button is clicked. `kind`
// names which catalog the picked path goes to. No-op if a dialog is already
// in flight or its result hasn't been drained.
void StartIconBrowse(EIconTargetKind kind,
                     const std::string& targetId,
                     const std::string& currentValue);

// Backwards-compat overload — Emote-typed targets (the existing call surface).
inline void StartIconBrowse(const std::string& emoteId,
                            const std::string& currentValue) {
    StartIconBrowse(EIconTargetKind::Emote, emoteId, currentValue);
}

// Called once per frame at the top of AddonOptions. Picks up any completed
// pick and writes it to the matching catalog entry's IconPath, persists the
// affected catalog's JSON, and bumps its dirty flag. Returns true if a pick
// was applied (mostly for caller logging — persistence is handled here so
// the Emote vs /me-mote routing stays in one place).
bool DrainIconBrowse();
