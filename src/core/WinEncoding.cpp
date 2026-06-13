#include "WinEncoding.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

std::string Utf8ToAcp(const std::string& utf8, bool* lossy) {
    if (lossy) *lossy = false;
    if (utf8.empty()) return std::string();

    // System ANSI codepage IS UTF-8 (Windows 10 1903+ opt-in, or a UTF-8 Wine
    // prefix): no conversion needed, and CP_UTF8 doesn't accept the flags below.
    if (GetACP() == CP_UTF8) return utf8;

    const std::wstring w = Utf8ToWide(utf8);
    if (w.empty()) { if (lossy) *lossy = true; return std::string(); }

    // WC_NO_BEST_FIT_CHARS + lpUsedDefaultChar: a character not representable in
    // the codepage maps to the default '?' and flags `used` (so the caller can
    // skip a doomed open rather than look for a '?'-mangled name).
    BOOL used = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, &used);
    if (n <= 0) { if (lossy) *lossy = true; return std::string(); }
    std::string s((size_t)n, '\0');
    used = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(),
                        &s[0], n, nullptr, &used);
    if (lossy) *lossy = (used != FALSE);
    return s;
}
