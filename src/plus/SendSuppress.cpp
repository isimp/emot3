#include "SendSuppress.h"

#ifdef EMOT3_PLUS

#include <atomic>

// Worker thread (sets the scope) <-> WndProc thread (reads it). A nesting
// COUNTER, not a bool, so two overlapping sends don't have the first one's
// dtor clear suppression while the second is still injecting. relaxed order:
// the only invariant is "consume while depth > 0", and a one-message-stale
// read at the very edges of the window is harmless.
namespace { std::atomic<int> s_depth{ 0 }; }

SendSuppressScope::SendSuppressScope(bool active) : active_(active) {
    if (active_) s_depth.fetch_add(1, std::memory_order_relaxed);
}

SendSuppressScope::~SendSuppressScope() {
    if (active_) s_depth.fetch_sub(1, std::memory_order_relaxed);
}

bool SendSuppressConsume(UINT msg) {
    if (s_depth.load(std::memory_order_relaxed) <= 0) return false;
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_CHAR:
        case WM_DEADCHAR:
            return true;   // consume - keep it out of the game while we inject
        // Key-UPS pass through, always. An up can't type anything, so it can't
        // garble the injection - but EATING one wedges that key "down" in every
        // later stage of the dispatch chain: ImGui's shared io never sees the
        // release (a wedged Enter made ImGui's key-repeat auto-fire the
        // palette's EnterReturnsTrue the moment its field activated - the
        // machine-gun resend bug; the user's Enter release landed exactly in
        // this injection window), and the game misses it too (a movement key
        // released mid-window kept the character running). An up whose down was
        // consumed is harmless everywhere - unmatched releases are ignored.
        case WM_KEYUP:
        case WM_SYSKEYUP:
        default:
            return false;  // mouse and everything else pass through
    }
}

#else  // ---- base build: no input swallow at all ----

SendSuppressScope::SendSuppressScope(bool active) : active_(active) {}
SendSuppressScope::~SendSuppressScope() {}
bool SendSuppressConsume(UINT) { return false; }

#endif  // EMOT3_PLUS
