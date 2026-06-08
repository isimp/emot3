#pragma once

// UTF-8 <-> wide (UTF-16) <-> system-codepage (CP_ACP) conversions for the Win32
// file-API boundary. The icon code stores / displays / JSON-serializes paths as
// UTF-8 (what ImGui + nlohmann expect), but the Win32 file APIs need wide (for
// correctness on any name) or ANSI (for Nexus' GetOrCreateFromFile, which has no
// wide variant). Windows.h is included only in the .cpp so it stays out of the
// widely-included headers (keeps the data/ layer imgui/Windows-light).

#include <string>

// UTF-8 -> UTF-16. Empty in -> empty out.
std::wstring Utf8ToWide(const std::string& utf8);

// UTF-16 -> UTF-8.
std::string WideToUtf8(const std::wstring& wide);

// UTF-8 -> the system ANSI codepage (CP_ACP). Sets *lossy = true if any character
// is not representable in the codepage (no best-fit substitution is used, so a
// non-representable char would yield '?' - the caller skips a doomed file open
// instead). If the system ANSI codepage IS UTF-8 (the Win10 opt-in), the input is
// returned unchanged.
std::string Utf8ToAcp(const std::string& utf8, bool* lossy = nullptr);
