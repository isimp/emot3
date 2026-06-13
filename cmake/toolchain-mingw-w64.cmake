# Cross-compile emot3 to a Windows x64 DLL on Linux with MinGW-w64.
#
#   cmake -B build-mingw/Distribution -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#         -DCMAKE_BUILD_TYPE=Distribution
#   cmake --build build-mingw/Distribution
#
# The resulting DLL is for the Wine-portability CI gate (and local Linux dev),
# NOT for release — shipped binaries come from the MSVC build. The static link
# flags below keep the import table core-Win32-only so the gate's
# `check_imports.py --mingw` stays green (no libgcc/libstdc++/libwinpthread).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Debian/Ubuntu package "mingw-w64" installs this prefix; override with
# -DMINGW_PREFIX=... if your distro names it differently.
if(NOT DEFINED MINGW_PREFIX)
  set(MINGW_PREFIX x86_64-w64-mingw32)
endif()

# Prefer the POSIX threading variant: libstdc++ built for the win32 thread model
# has NO std::thread / std::mutex / std::condition_variable (the addon uses all
# three). On Debian/Ubuntu that's the `-posix`-suffixed driver; on distros where
# the default is already posix, fall back to the plain name. -static (below)
# folds winpthread in, so the DLL still imports no libwinpthread-1.dll.
find_program(EMOT3_MINGW_CC  NAMES ${MINGW_PREFIX}-gcc-posix ${MINGW_PREFIX}-gcc)
find_program(EMOT3_MINGW_CXX NAMES ${MINGW_PREFIX}-g++-posix ${MINGW_PREFIX}-g++)
set(CMAKE_C_COMPILER   ${EMOT3_MINGW_CC})
set(CMAKE_CXX_COMPILER ${EMOT3_MINGW_CXX})
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}-windres)

# Static GCC/C++/pthread runtimes -> Wine-clean import table.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

# Find libraries/headers in the MinGW sysroot, but run host tools (python, etc.).
set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
