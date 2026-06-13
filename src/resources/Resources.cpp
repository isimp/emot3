#include "Resources.h"
#include "StringUtil.h"  // ToLower (shared helper)

#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <string>

// The addon's own DLL handle, captured in DllMain (entry.cpp). Resource
// lookups must be qualified with this so FindResource searches inside
// our DLL rather than the host process's executable.
extern HMODULE hSelf;

namespace {

// Locks an RCDATA resource and returns its raw bytes. The returned
// pointer is into the addon DLL's read-only resource section — do not
// modify, do not free. Lifetime is the DLL's, which means "as long as
// the addon is loaded," so callers can hold it indefinitely.
//
// Note on the lpType argument: for predefined resource types (RCDATA,
// BITMAP, ICON, …), FindResource requires MAKEINTRESOURCE(RT_*) — the
// integer pseudo-pointer form. Passing the literal string "RCDATA" only
// matches *user-defined* type names; the .rc compiler stores predefined
// types as their integer ID (RT_RCDATA == 10), so a string lookup
// silently fails on every entry. Using RT_RCDATA / MAKEINTRESOURCE keeps
// us on the right side of that distinction.
bool LoadResourceBytes(int resourceId, const void*& outData, DWORD& outSize) {
    HRSRC rsrc = FindResource(hSelf,
                              MAKEINTRESOURCE(resourceId),
                              RT_RCDATA);
    if (!rsrc) return false;
    HGLOBAL h = LoadResource(hSelf, rsrc);
    if (!h) return false;
    LPVOID p = LockResource(h);
    DWORD  sz = SizeofResource(hSelf, rsrc);
    if (!p || sz == 0) return false;
    outData = p;
    outSize = sz;
    return true;
}

} // namespace

int LookupBundledResource(const BundledIcon* table, int count,
                          const std::string& command) {
    if (!table || count <= 0) return 0;
    // Normalize: strip leading slash, lowercase. The manifest stores
    // stems in the same shape.
    std::string key = command;
    if (!key.empty() && key.front() == '/') key.erase(0, 1);
    key = ToLower(key);
    for (int i = 0; i < count; ++i) {
        if (table[i].command && key == table[i].command)
            return table[i].resourceId;
    }
    return 0;
}

bool TryLoadBundledIconBytes(const BundledIcon* table, int count,
                             const std::string& command,
                             const void*& outData, size_t& outSize) {
    outData = nullptr;
    outSize = 0;
    int rid = LookupBundledResource(table, count, command);
    if (rid == 0) return false;
    DWORD sz = 0;
    if (!LoadResourceBytes(rid, outData, sz)) return false;
    outSize = static_cast<size_t>(sz);
    return true;
}

int LookupBundledData(const BundledData* table, int count,
                      const std::string& name) {
    if (!table || count <= 0) return 0;
    std::string key = ToLower(name);
    for (int i = 0; i < count; ++i) {
        if (table[i].name && key == table[i].name)
            return table[i].resourceId;
    }
    return 0;
}

bool TryLoadBundledData(const BundledData* table, int count,
                        const std::string& name,
                        const void*& outData, size_t& outSize) {
    outData = nullptr;
    outSize = 0;
    int rid = LookupBundledData(table, count, name);
    if (rid == 0) return false;
    DWORD sz = 0;
    if (!LoadResourceBytes(rid, outData, sz)) return false;
    outSize = static_cast<size_t>(sz);
    return true;
}
