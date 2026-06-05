#include "QuickbarWheel.h"

#ifdef EMOT3_PLUS

#include "Globals.h"      // g_GameHwnd
#include "PlusSettings.h"  // g_PlusSettings

#include <windowsx.h>     // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM
#include <atomic>

// Render thread <-> WndProc thread hand-off. Single definitions here (not in a
// header) so both threads share one instance. Render publishes the capture
// rect; WndProc accumulates raw deltas (multiples of WHEEL_DELTA = 120); render
// drains. memory_order_relaxed: a one-frame-stale rect is harmless for a wheel
// hit-test, and there's no other state to order against.
namespace {
std::atomic<bool>  s_active     { false };
std::atomic<float> s_x          { 0.f };
std::atomic<float> s_y          { 0.f };
std::atomic<float> s_w          { 0.f };
std::atomic<float> s_h          { 0.f };
std::atomic<int>   s_pendingRaw { 0 };
}

bool QbWheelConsume(HWND /*hWnd*/, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != WM_MOUSEWHEEL || !s_active.load(std::memory_order_relaxed))
        return false;
    // WM_MOUSEWHEEL carries SCREEN coords in lParam (unlike client-relative
    // mouse-move messages); convert to client == ImGui display space.
    POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (g_GameHwnd) ScreenToClient(g_GameHwnd, &pt);
    float x = s_x.load(std::memory_order_relaxed);
    float y = s_y.load(std::memory_order_relaxed);
    float w = s_w.load(std::memory_order_relaxed);
    float h = s_h.load(std::memory_order_relaxed);
    if (pt.x < x || pt.x > x + w || pt.y < y || pt.y > y + h)
        return false;
    // GET_WHEEL_DELTA_WPARAM: signed multiple of 120, positive = wheel up.
    s_pendingRaw.fetch_add(GET_WHEEL_DELTA_WPARAM(wParam), std::memory_order_relaxed);
    return true;
}

void QbWheelPublish(bool wantWheel, float x, float y, float w, float h) {
    bool active = wantWheel && g_PlusSettings.QbClickThroughWheel;
    if (active) {
        s_x.store(x, std::memory_order_relaxed);
        s_y.store(y, std::memory_order_relaxed);
        s_w.store(w, std::memory_order_relaxed);
        s_h.store(h, std::memory_order_relaxed);
    }
    s_active.store(active, std::memory_order_relaxed);
}

float QbWheelDrain() {
    return s_pendingRaw.exchange(0, std::memory_order_relaxed) / 120.f;
}

#else  // ---- base build: no input swallow, no capture ----

bool  QbWheelConsume(HWND, UINT, WPARAM, LPARAM) { return false; }
void  QbWheelPublish(bool, float, float, float, float) {}
float QbWheelDrain() { return 0.f; }

#endif  // EMOT3_PLUS
