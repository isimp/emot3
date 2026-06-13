# Contributing to emot3

Thanks for helping out! This file covers building from source on **Windows and
Linux**, the build flavors, and how to test for Wine/Proton compatibility.

For non-code contributions (emote catalog fixes, /me-mote and UI translations),
see [Translations & catalog fixes](README.md#translations--catalog-fixes) in the
README — those don't need a build.

## What emot3 is, build-wise

emot3 is a single **Windows x64 DLL** loaded by [Nexus](https://raidcore.gg/Nexus).
Linux players run it under Proton/Wine exactly like they run GW2. The build is
driven by **CMake**, which targets two toolchains:

- **MSVC on Windows** — produces the shipped binaries.
- **MinGW-w64 cross-compile on Linux** — for local Linux development and the CI
  Wine-portability gate. (Releases are *not* built this way.)

The five build flavors are modelled as CMake configurations. A build is a
**base** plus two independent, additive flavor macros:

| Configuration | Output DLL | `EMOT3_PLUS` | `EMOT3_DEVTOOLS` | Use |
|---|---|:---:|:---:|---|
| **Distribution** | `emot3.dll` | — | — | The clean public base build. CI publishes this. |
| **Plus** | `emot3_plus.dll` | ✓ | — | Input-swallow conveniences (mid-movement send, click-through wheel). |
| **DevTools** | `emot3_devtools.dll` | — | ✓ | Dev tools on a base-shaped binary. |
| **PlusDevTools** | `emot3_plusdevtools.dll` | ✓ | ✓ | Local diagnostic build — the default for development. |
| **Debug** | `emot3_debug.dll` | ✓ | ✓ | Unoptimized + debug CRT, for stepping. |

`EMOT3_PLUS` adds the AV-sensitive input-swallows (`src/plus/`); `EMOT3_DEVTOOLS`
adds the diagnostic dev tools (`src/devtools/`). Every shipped flavor links the
CRT **statically** so the DLL loads in a stock Proton/Wine prefix with no runtime
redist — CI enforces this (see [Wine/Proton compatibility](#wineproton-compatibility)).

## Prerequisites

Clone with submodules (the build `#include`s vendored headers):

```sh
git clone --recurse-submodules https://github.com/isimp/emot3
# already cloned?  git submodule update --init --recursive
```

- **Windows:** Visual Studio 2022 or newer (C++ desktop workload + a Windows 10
  SDK; bundles CMake ≥ 3.21 and Ninja) and Python 3 (for the resource codegen).
- **Linux:** `mingw-w64`, `cmake` (≥ 3.21), `ninja-build`, and `python3`. On
  Debian/Ubuntu:
  ```sh
  sudo apt-get install -y mingw-w64 cmake ninja-build python3
  ```

## Build — Windows (MSVC)

Multi-config generator: configure once, then build any flavor with `--config`.

```sh
cmake -B build-vs -A x64        # auto-detects the newest installed VS
cmake --build build-vs --config PlusDevTools      # or Distribution / Plus / DevTools / Debug
```

Omitting `-G` lets CMake pick whichever Visual Studio is installed (2022, 2026,
…); pass `-G "Visual Studio 18 2026"` to pin a specific one. Output lands in
`build-vs/<Config>/emot3*.dll`. `tools/gen_rc.py` runs automatically as a build
dependency — no manual codegen step.

## Build — Linux (MinGW-w64 cross-compile)

Single-config generator: one build dir per flavor, picked with `CMAKE_BUILD_TYPE`.

```sh
cmake -B build-mingw/Distribution -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
      -DCMAKE_BUILD_TYPE=Distribution
cmake --build build-mingw/Distribution
```

Swap `Distribution` for `Plus` / `DevTools` / `PlusDevTools` as needed. Output is
`build-mingw/<Config>/emot3*.dll` — a real Windows DLL you can drop into a GW2
Proton prefix's `addons/` for testing. (If your distro names the toolchain
differently, pass `-DMINGW_PREFIX=<prefix>`.)

## Wine/Proton compatibility

The DLL must load in a **stock** Proton/Wine prefix — no MSVC/MinGW runtime
redist. The guard is `tools/check_imports.py`, which parses the PE import table
(pure Python, runs anywhere) and fails if the DLL imports a dynamic runtime.

```sh
# MSVC build — must import only core Win32 (+ WINHTTP):
python tools/check_imports.py build-vs/Distribution/emot3.dll

# MinGW build — also rejects libgcc/libstdc++/libwinpthread (static-link check):
python3 tools/check_imports.py --mingw build-mingw/Distribution/emot3.dll
```

A healthy DLL imports only `KERNEL32.dll`, `USER32.dll`, and `WINHTTP.dll`.

**Keep it Wine-safe** when adding code:
- Reach the game through the Nexus API (input, paths, textures), not raw Win32
  where avoidable.
- Include Windows headers in **lowercase** (`#include <windows.h>`, not
  `<Windows.h>`). MSVC is case-insensitive so it forgives mis-casing; MinGW on a
  case-sensitive Linux filesystem does not, and it'll fail to find the header.
- File I/O on user paths is UTF-8 internally, wide (`...W` APIs /
  `std::filesystem::path`) at the OS boundary — never the MSVC-only
  `std::ifstream(const wchar_t*)` overload (it won't compile under MinGW). The
  MSVC "secure CRT" helpers (`strncpy_s`, ...) are fine — MinGW-w64 provides them.
- Don't link new system libs via `#pragma comment(lib, ...)` alone — GCC ignores
  it. Add the library in `CMakeLists.txt` (`target_link_libraries`) too.

**The one thing source can't prove:** emote *injection timing* under Wine — the
`Sleep` granularity and synthetic scancode/`WM_CHAR` delivery into GW2's input
pipeline (`src/core/EmoteAction.cpp`). No static build or import check exercises
it. If you touch that path, smoke-test sending an emote **live in-game**, ideally
both on Windows and under Proton. Keep those Sleeps generous; don't retune them
blind.

## CI

- **`build`** (Windows, MSVC): builds the flavors via CMake and runs the static-CRT
  import guard. On `main` it builds all four and uploads the public artifact that
  a `v*` tag later ships byte-for-byte.
- **`cross-mingw`** (Linux): cross-compiles the **PlusDevTools** flavor (the
  maximal-code superset) with MinGW-w64 and runs `check_imports.py --mingw`. This
  is a **portability gate** — it never ships anything; it just proves the code
  still compiles under libstdc++ and stays Wine-clean.

Open a PR and both must be green. Shipped binaries always come from the MSVC job.
