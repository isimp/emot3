#!/usr/bin/env python3
"""
Linux/Wine readiness guard: verify a built emot3 DLL does NOT import the
dynamic MSVC runtime.

emot3 runs on Linux under Proton/Wine inside the GW2 prefix. The single
most important factor that keeps it loading there is that every SHIPPED
config links the CRT statically (/MT in src/emot3.vcxproj). A static-CRT
DLL is self-contained: it imports only core Win32 system DLLs that Wine
implements, and needs NO vcruntime140.dll / msvcp140.dll / UCRT redist in
the prefix. Flip a config to /MD (or link something that drags the dynamic
CRT in) and the DLL suddenly fails to load on a stock Proton prefix with a
cryptic "missing DLL" error - exactly the kind of regression nobody
notices on Windows.

This script makes that invariant enforceable in CI. It parses the PE
import table in pure Python (no dumpbin / no VC environment needed, so it
runs anywhere) and exits non-zero if any imported DLL name matches a
forbidden substring (the dynamic CRT). It also prints the full import list
so the CI log shows exactly what the DLL depends on.

Usage:
    py -3 tools/check_imports.py build/Distribution/emot3.dll [more.dll ...]

Exit codes: 0 = all clean, 1 = a forbidden import found, 2 = bad args /
unparseable file.
"""

import os
import struct
import sys

# Substrings (matched case-insensitively against imported DLL names) that
# indicate the dynamic CRT. A /MT build imports none of these; a /MD build
# pulls in vcruntime140 + msvcp140, and the UCRT shows up as ucrtbase.dll
# or the api-ms-win-crt-*.dll forwarder set.
_DEFAULT_FORBIDDEN = ("vcruntime", "msvcp", "ucrtbase", "api-ms-win-crt")

# Extra forbidden substrings can be appended via the EMOT3_FORBIDDEN_IMPORTS
# env var (comma-separated) - handy for an ad-hoc tighten in CI, or to
# self-test that the guard bites (e.g. EMOT3_FORBIDDEN_IMPORTS=winhttp).
_extra = [s.strip().lower() for s in
          os.environ.get("EMOT3_FORBIDDEN_IMPORTS", "").split(",") if s.strip()]
FORBIDDEN = tuple(_DEFAULT_FORBIDDEN) + tuple(_extra)


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def imported_dlls(path):
    """Return the list of DLL names in `path`'s import directory.

    Minimal PE walk: DOS header -> PE header -> optional header data
    directory[1] (imports) -> section table for RVA->file-offset mapping ->
    the IMAGE_IMPORT_DESCRIPTOR array, reading each descriptor's Name RVA.
    """
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"MZ":
        raise ValueError("not a PE file (no MZ signature)")
    pe = _u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file (no PE signature)")

    coff = pe + 4
    num_sections = _u16(data, coff + 2)
    size_opt = _u16(data, coff + 16)
    opt = coff + 20
    magic = _u16(data, opt)
    if magic == 0x20B:      # PE32+ (x64)
        dir_off = opt + 112
    elif magic == 0x10B:    # PE32 (x86)
        dir_off = opt + 96
    else:
        raise ValueError("unknown optional-header magic 0x%X" % magic)

    import_rva = _u32(data, dir_off + 8)   # data directory entry 1 = imports
    if import_rva == 0:
        return []                          # no imports at all (unusual)

    # Section table follows the optional header; build RVA->file-offset map.
    sect = opt + size_opt
    sections = []
    for i in range(num_sections):
        s = sect + i * 40
        vsize = _u32(data, s + 8)
        vaddr = _u32(data, s + 12)
        raw_size = _u32(data, s + 16)
        raw_ptr = _u32(data, s + 20)
        span = max(vsize, raw_size)
        sections.append((vaddr, span, raw_ptr))

    def rva_to_off(rva):
        for vaddr, span, raw_ptr in sections:
            if vaddr <= rva < vaddr + span:
                return raw_ptr + (rva - vaddr)
        raise ValueError("RVA 0x%X not in any section" % rva)

    def read_cstr(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("ascii", "replace")

    names = []
    desc = rva_to_off(import_rva)
    while True:                            # array ends at an all-zero descriptor
        name_rva = _u32(data, desc + 12)
        if name_rva == 0:
            break
        names.append(read_cstr(rva_to_off(name_rva)))
        desc += 20
    return names


def check(path):
    """Print imports for `path`; return list of offending DLL names."""
    names = imported_dlls(path)
    print("  imports (%d):" % len(names))
    for n in sorted(names, key=str.lower):
        flagged = any(bad in n.lower() for bad in FORBIDDEN)
        print("    %s %s" % ("FAIL ->" if flagged else "      ", n))
    return [n for n in names if any(bad in n.lower() for bad in FORBIDDEN)]


def main(argv):
    paths = argv[1:]
    if not paths:
        print("usage: check_imports.py <dll> [<dll> ...]", file=sys.stderr)
        return 2

    any_bad = False
    for path in paths:
        print("Checking %s" % path)
        try:
            bad = check(path)
        except (OSError, ValueError) as e:
            print("  ERROR: %s" % e, file=sys.stderr)
            return 2
        if bad:
            any_bad = True
            print("  -> FORBIDDEN dynamic-CRT import(s): %s" % ", ".join(bad))
            print("  -> This DLL needs an MSVC redist in the Wine/Proton prefix "
                  "and will fail to load on a stock Linux setup.")
            print("  -> Fix: ensure this config links the CRT statically "
                  "(RuntimeLibrary = MultiThreaded /MT) in src/emot3.vcxproj.")
        else:
            print("  -> OK: static CRT, no dynamic-runtime dependency.")
        print("")

    if any_bad:
        print("FAILED: at least one DLL imports the dynamic CRT.")
        return 1
    print("PASSED: all checked DLLs are static-CRT / Wine-safe.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
