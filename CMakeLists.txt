# emot3 — single build system for both MSVC (Windows, the shipped binaries) and
# MinGW-w64 (Linux cross-compile, the Wine-portability gate).
#
# A build is a "base" plus two independent, ADDITIVE flavor macros:
#   EMOT3_PLUS     - the +plus input-swallow conveniences (src/plus/). AV-sensitive,
#                    so the base build leaves them out.
#   EMOT3_DEVTOOLS - the +devtools diagnostic tools (src/devtools/).
# base = neither macro = the public build.
#
# Five configurations -> output DLL (NOT an orthogonal 2x2x2: only the four
# optimized flavors + one Debug = both-macros, unoptimized, debug CRT), so they
# are modelled as named CMake configurations driven by $<CONFIG:...> genexes:
#   Distribution -> emot3.dll               (base, optimized, shipped)
#   Plus         -> emot3_plus.dll          (base +plus)
#   DevTools     -> emot3_devtools.dll      (base +devtools)
#   PlusDevTools -> emot3_plusdevtools.dll  (base +plus +devtools, local diagnostic)
#   Debug        -> emot3_debug.dll         (+plus +devtools, /Od + debug CRT)
#
# x64 only (GW2 is x64). Static CRT on every shipped config (/MT under MSVC;
# -static* under MinGW) so the DLL loads in a stock Proton/Wine prefix with no
# MSVC/MinGW redist — enforced by tools/check_imports.py in CI.

cmake_minimum_required(VERSION 3.21)

set(EMOT3_CONFIGS Distribution Plus DevTools PlusDevTools Debug)

# Must be set BEFORE project() so multi-config generators (Visual Studio) bake the
# five flavors into the generated solution — including the ZERO_CHECK/ALL_BUILD
# helper projects. Single-config generators (Ninja/Make) ignore it; they use
# CMAKE_BUILD_TYPE, handled just after project().
set(CMAKE_CONFIGURATION_TYPES "${EMOT3_CONFIGS}" CACHE STRING "emot3 build flavors" FORCE)

project(emot3 LANGUAGES CXX)

get_property(EMOT3_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT EMOT3_MULTI_CONFIG)                          # single-config (Ninja/Make)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Distribution CACHE STRING "emot3 build flavor" FORCE)
  endif()
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${EMOT3_CONFIGS})
endif()
enable_language(RC)                                 # rc.exe (MSVC) / windres (MinGW), auto-selected
cmake_policy(SET CMP0091 NEW)                       # CRT selected via MSVC_RUNTIME_LIBRARY, not flags

set(CMAKE_CXX_STANDARD 17)                          # code uses C++17 sized-delete operators
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "emot3 is x64-only; configure a 64-bit toolchain (VS: -A x64).")
endif()

# Flavor predicates, reused across the genexes below. Distribution matches none.
set(EMOT3_IS_PLUS  "$<OR:$<CONFIG:Plus>,$<CONFIG:PlusDevTools>,$<CONFIG:Debug>>")
set(EMOT3_IS_DEV   "$<OR:$<CONFIG:DevTools>,$<CONFIG:PlusDevTools>,$<CONFIG:Debug>>")
set(EMOT3_IS_DEBUG "$<CONFIG:Debug>")
set(EMOT3_IS_OPT   "$<NOT:$<CONFIG:Debug>>")        # the four optimized/shipped configs

# ---------------------------------------------------------------------------
# Resource codegen: tools/gen_rc.py turns resources/*.{png,json} into
# src/generated/{emot3.rc,ResourcesGenerated.cpp}. Replaces the old vcxproj
# pre-build event; runs on both OSes via the discovered Python interpreter.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(EMOT3_GEN_DIR ${CMAKE_SOURCE_DIR}/src/generated)
set(EMOT3_GEN_RC  ${EMOT3_GEN_DIR}/emot3.rc)
set(EMOT3_GEN_CPP ${EMOT3_GEN_DIR}/ResourcesGenerated.cpp)

# CONFIGURE_DEPENDS so adding/removing an asset re-runs the glob at build time.
file(GLOB EMOT3_RES_DEPS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/resources/emotes_official/*.png
  ${CMAKE_SOURCE_DIR}/resources/emotes_ai/*.png
  ${CMAKE_SOURCE_DIR}/resources/ui/*.png
  ${CMAKE_SOURCE_DIR}/resources/i18n/*.json
  ${CMAKE_SOURCE_DIR}/resources/emote_data/*.json
  ${CMAKE_SOURCE_DIR}/resources/presets/*.json
  ${CMAKE_SOURCE_DIR}/resources/me_mote_data/*.json
  ${CMAKE_SOURCE_DIR}/resources/me_motes_ai/*.png)

add_custom_command(
  OUTPUT ${EMOT3_GEN_RC} ${EMOT3_GEN_CPP}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_rc.py
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_rc.py ${EMOT3_RES_DEPS}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Generating src/generated/{emot3.rc,ResourcesGenerated.cpp} from resources/"
  VERBATIM)
add_custom_target(emot3_codegen DEPENDS ${EMOT3_GEN_RC} ${EMOT3_GEN_CPP})

# ---------------------------------------------------------------------------
# The DLL. Source list mirrors src/emot3.vcxproj (the authoritative set) so the
# two builds compile the same TUs; vendored imgui TUs are built in.
# ---------------------------------------------------------------------------
add_library(emot3 SHARED
  src/core/AirborneDetect.cpp
  src/core/CharacterState.cpp
  src/core/ChatWatch.cpp
  src/core/UnlockScan.cpp
  src/plus/UpdateCheck.cpp
  src/plus/PlusSettings.cpp
  src/devtools/DevTools.cpp
  src/devtools/DevStateInspector.cpp
  src/devtools/NotifierDebug.cpp
  src/devtools/UpdateCheckDebug.cpp
  src/devtools/Simulators.cpp
  src/devtools/MemoryMonitor.cpp
  src/ui/Feedback.cpp
  src/core/EmoteAction.cpp
  src/core/EmoteBinds.cpp
  src/core/RadialExport.cpp
  src/core/entry.cpp
  src/core/Globals.cpp
  src/plus/QuickbarWheel.cpp
  src/plus/SendSuppress.cpp
  src/plus/RadialDeploy.cpp
  src/core/I18n.cpp
  src/core/Logging.cpp
  src/core/WinEncoding.cpp
  src/core/AtomicFile.cpp
  src/core/SaveScheduler.cpp
  src/data/EmoteData.cpp
  src/data/Favorites.cpp
  src/data/IconCacheConfig.cpp
  src/data/IconPath.cpp
  src/data/MeMotes.cpp
  src/data/QuickbarPresets.cpp
  src/data/Settings.cpp
  src/data/migrations/Migrations.cpp
  src/data/migrations/M001_NormalizeLegacyKeys.cpp
  src/data/migrations/M002_UnusableSplit.cpp
  src/data/Usage.cpp
  src/data/RadialExports.cpp
  ${EMOT3_GEN_CPP}
  src/resources/Resources.cpp
  src/ui/CatalogView.cpp
  src/ui/Cells.cpp
  src/ui/IconBrowse.cpp
  src/ui/IconPicker.cpp
  src/ui/Icons.cpp
  src/ui/IconDrawing.cpp
  src/ui/Layout.cpp
  src/ui/OwnedScroll.cpp
  src/ui/MainPanel.cpp
  src/ui/NexusShortcut.cpp
  src/ui/Palette.cpp
  src/ui/Options.cpp
  src/ui/OptionsAutoMotes.cpp
  src/ui/OptionsCommon.cpp
  src/ui/OptionsGeneral.cpp
  src/ui/OptionsPalette.cpp
  src/ui/OptionsQuickbar.cpp
  src/ui/OptionsEmotes.cpp
  src/ui/OptionsMeMotes.cpp
  src/ui/OptionsRadial.cpp
  src/ui/OptionsUnlocks.cpp
  src/ui/Quickbar.cpp
  src/ui/TextCache.cpp
  vendor/imgui/imgui.cpp
  vendor/imgui/imgui_draw.cpp
  vendor/imgui/imgui_tables.cpp
  vendor/imgui/imgui_widgets.cpp
  ${EMOT3_GEN_RC})

add_dependencies(emot3 emot3_codegen)

# Flat includes (sources #include "Settings.h", never a path). src/generated is
# listed so rc.exe/windres resolve the .rc's "../../resources/..." paths via the
# RC include path (windres resolves RCDATA relative to -I dirs, not the .rc dir).
target_include_directories(emot3 PRIVATE
  src/core src/devtools src/plus src/data src/ui src/resources ${EMOT3_GEN_DIR}
  vendor vendor/nlohmann_json/single_include)

# Per-config preprocessor matrix (exactly the vcxproj's defines).
target_compile_definitions(emot3 PRIVATE
  EMOT3_EXPORTS _WINDOWS _USRDLL
  $<${EMOT3_IS_PLUS}:EMOT3_PLUS>
  $<${EMOT3_IS_DEV}:EMOT3_DEVTOOLS>
  $<${EMOT3_IS_DEBUG}:_DEBUG>
  $<${EMOT3_IS_OPT}:NDEBUG>)

# Per-config output DLL name (the vcxproj's per-config TargetName). PREFIX ""
# stops MinGW emitting "libemot3.dll".
# No $<CONFIG> in the output dir: multi-config (VS) auto-appends the config
# subdir, while single-config (Ninja, one build dir per flavor) lands directly in
# it. Both resolve to <builddir>/<Config>/emot3*.dll without double-nesting.
set_target_properties(emot3 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "$<IF:$<CONFIG:Distribution>,emot3,$<IF:$<CONFIG:Plus>,emot3_plus,$<IF:$<CONFIG:DevTools>,emot3_devtools,$<IF:$<CONFIG:PlusDevTools>,emot3_plusdevtools,emot3_debug>>>>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(MSVC)
  # Static CRT on the four shipped configs (/MT); Debug keeps the debug CRT
  # (/MDd) so /RTC1 + /sdl work — matches the vcxproj exactly.
  set_target_properties(emot3 PROPERTIES
    MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Debug>,MultiThreadedDebug,MultiThreaded>")
  target_compile_options(emot3 PRIVATE
    /W3 /permissive- /MP
    $<${EMOT3_IS_OPT}:/O2> $<${EMOT3_IS_OPT}:/Gy> $<${EMOT3_IS_OPT}:/Oi> $<${EMOT3_IS_OPT}:/GL>
    $<${EMOT3_IS_DEBUG}:/Od> $<${EMOT3_IS_DEBUG}:/sdl>)
  target_link_options(emot3 PRIVATE
    /SUBSYSTEM:WINDOWS /MANIFESTUAC:NO /DEBUG
    # The four shipped configs link non-incrementally (LTCG ignores /INCREMENTAL
    # anyway -> LNK4075); Debug keeps incremental linking, as in the vcxproj.
    $<${EMOT3_IS_OPT}:/INCREMENTAL:NO>
    $<${EMOT3_IS_OPT}:/LTCG> $<${EMOT3_IS_OPT}:/OPT:REF> $<${EMOT3_IS_OPT}:/OPT:ICF>)
else()
  # MinGW-w64: target Win10 headers (WinHTTP flags etc.) and link the GCC/C++
  # runtimes statically so the import table stays core-Win32-only (Wine-clean).
  target_compile_definitions(emot3 PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  target_compile_options(emot3 PRIVATE
    -ffunction-sections -fdata-sections
    $<${EMOT3_IS_OPT}:-O2>
    $<${EMOT3_IS_DEBUG}:-O0> $<${EMOT3_IS_DEBUG}:-g>)
  target_link_options(emot3 PRIVATE
    -static -static-libgcc -static-libstdc++ -Wl,--subsystem,windows
    # Strip the symbol table from optimized builds (GCC keeps it in the binary by
    # default; MSVC offloads to a .pdb). Roughly halves the DLL. Debug keeps
    # symbols for stepping.
    $<${EMOT3_IS_OPT}:-Wl,--gc-sections> $<${EMOT3_IS_OPT}:-Wl,-s>)
endif()

# winhttp: UnlockScan (GW2-API) + UpdateCheck. psapi: MemoryMonitor (devtools).
# These are #pragma comment(lib)'d for MSVC but GCC ignores the pragma, so link
# them here for both toolchains (the pragmas stay as documentation).
target_link_libraries(emot3 PRIVATE winhttp)
target_link_libraries(emot3 PRIVATE $<${EMOT3_IS_DEV}:psapi>)
