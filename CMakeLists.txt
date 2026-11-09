cmake_minimum_required(VERSION 3.15)

# Project details. OBJC is only needed on Apple for the Metal-backed
# sokol_impl.m; enable it unconditionally where available -- CMake will
# just ignore OBJC if no .m files end up in the build on a given platform.
project(RevenantRevisited
  VERSION 1.0
  DESCRIPTION "Revenant PC 1998 built for modern machines."
  LANGUAGES C CXX
)
include(FetchContent)

# ----------------------------------------------------------------------------
# Annotated properties (docs/vfx/PREFAB_SYSTEM.md §2). Headers mark members
# with PROP(id); until our compilers read C++26 annotations, tools/reflect/
# gen.py emits the property tables + Set/Get switches into the build tree.
# rev_reflect(<target> <header>...) wires one generated .cpp per header into
# the target. Delete this block and the script when the compilers catch up.
# ----------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(REV_REFLECT_GEN "${CMAKE_SOURCE_DIR}/tools/reflect/gen.py")
function(rev_reflect target)
  foreach(header IN LISTS ARGN)
    get_filename_component(abs "${header}" ABSOLUTE)
    get_filename_component(stem "${header}" NAME_WE)
    set(out "${CMAKE_BINARY_DIR}/generated/reflect/${target}_${stem}.props.cpp")
    add_custom_command(
      OUTPUT "${out}"
      COMMAND ${Python3_EXECUTABLE} -I "${REV_REFLECT_GEN}" "${abs}" "${out}"
      DEPENDS "${abs}" "${REV_REFLECT_GEN}"
      COMMENT "reflect: ${header}"
      VERBATIM)
    target_sources(${target} PRIVATE "${out}")
  endforeach()
endfunction()
if(APPLE)
  enable_language(OBJC)
endif()

# C++ standard. C++20: the editor/UI layer uses designated initializers,
# which MSVC rejects under C++17 (C7555).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The 1998 code was written for MSVC on x86, where char is signed; the
# parser and the stat tables read bytes through plain char. GCC and Clang
# make char unsigned on ARM Linux, so pin it.
if(NOT MSVC)
  add_compile_options(-fsigned-char)
endif()

# MSVC: sources carry UTF-8 literals (e.g. the U+FFFD replacement char in
# textencoding.h) and some translation units exceed the 65,536 section
# limit; /permissive keeps the legacy char*-from-literal returns the
# original codebase relies on (std:c++20 turns /permissive- on).
if(MSVC)
  add_compile_options(/utf-8 /bigobj /permissive)
endif()

# clang-cl parses templates late for MSVC compatibility, which C++20
# deprecates; googletest builds itself with -Werror and fails on the
# deprecation. Two-phase lookup is what clang does everywhere else.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(/Zc:twoPhase)
endif()


# Windows: keep <windows.h> from defining min/max macros over revtypes.h's
# functions, accept the C runtime's classic string functions, and supply
# the POSIX headers the port uses (src/platform/windows/include).
if(WIN32)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
  include_directories(BEFORE "${CMAKE_SOURCE_DIR}/src/platform/windows/include")
endif()

# ----------------------------------------------------------------------------
# Sanitizer build (ASan + UBSan). Off by default — opt in by configuring
# with -DREVENANT_ASAN=ON. Use a separate build dir so the normal build
# stays unaffected:
#
#   cmake -S . -B build-asan -DREVENANT_ASAN=ON
#   cmake --build build-asan
#   ./build-asan/Revenant      # ASan runtime errors stream to stderr
#
# ASan overhead is ~2x runtime + ~3x memory; fine for catching crashes
# but not what you ship. Symbolized reports, headless runs and lldb:
# see docs/DEBUG_TOOLING.md.
# ----------------------------------------------------------------------------
option(REVENANT_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(REVENANT_ASAN)
  message(STATUS "Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer enabled")

  # Check that the sanitizer runtime can start on this OS before building
  # everything against it. A runtime older than the OS can fail inside its
  # own initializer, before main() -- Xcode 16's on macOS 26 aborts with
  # "CHECK failed: sanitizer_malloc_mac.inc ... asan_init_is_running".
  # Re-checked on every configure: an OS update can break a toolchain that
  # passed before.
  include(CheckCSourceRuns)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=address -fsanitize=undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address -fsanitize=undefined)
  unset(REVENANT_SANITIZER_RUNTIME_STARTS CACHE)
  check_c_source_runs("int main(void) { return 0; }" REVENANT_SANITIZER_RUNTIME_STARTS)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(NOT REVENANT_SANITIZER_RUNTIME_STARTS)
    message(FATAL_ERROR
      "The AddressSanitizer runtime of ${CMAKE_C_COMPILER_ID} "
      "${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER}) cannot start on "
      "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}. The toolchain is probably "
      "older than the OS: update Xcode / the Command Line Tools, then "
      "reconfigure. See docs/DEBUG_TOOLING.md.")
  endif()

  # vptr is disabled because several polymorphic effect classes
  # (TBloodEffect, TBurnEffect, TDripEffect, TPulpEffect, TParticle3DAnimator)
  # have all virtuals inline → no key function → no typeinfo emitted →
  # link error. Re-enable once those have an out-of-line virtual.
  # -fsanitize-recover=address only makes recovery possible: ASan still
  # stops at the first error unless the run sets
  # ASAN_OPTIONS=halt_on_error=0, which lets one run report past a known
  # bug (e.g. to reach shutdown).
  add_compile_options(-fsanitize=address -fsanitize=undefined -fno-sanitize=vptr
                      -fsanitize-recover=address
                      -fno-omit-frame-pointer -fno-optimize-sibling-calls -g -O1)
  add_link_options(-fsanitize=address -fsanitize=undefined -fno-sanitize=vptr)
endif()

# ----------------------------------------------------------------------------
# Sokol backend selection.
#   Apple  -> Metal        (SOKOL_METAL, sokol_impl.m)
#   Win32  -> Direct3D 11  (SOKOL_D3D11, sokol_impl.c)
#   Linux  -> OpenGL 3.3   (SOKOL_GLCORE33, sokol_impl.c)
# Override with -DSOKOL_BACKEND=METAL|D3D11|GLCORE33 on the CMake command
# line when cross-configuring for a non-default backend.
# ----------------------------------------------------------------------------
if(NOT DEFINED SOKOL_BACKEND)
  if(APPLE)
    set(SOKOL_BACKEND "METAL")
  elseif(WIN32)
    set(SOKOL_BACKEND "D3D11")
  elseif(UNIX)
    set(SOKOL_BACKEND "GLCORE33")
  else()
    message(FATAL_ERROR "Unsupported platform; set -DSOKOL_BACKEND manually.")
  endif()
endif()
message(STATUS "Sokol backend: ${SOKOL_BACKEND}")

# Define paths for third-party libraries
set(THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/thirdparty")
set(SOKOL_DIR "${THIRDPARTY_DIR}/sokol")
set(HANDMADE_MATH_DIR "${THIRDPARTY_DIR}/handmademath")
set(SIMPLEINI_DIR "${THIRDPARTY_DIR}/simpleini")
set(MINIZ_DIR "${THIRDPARTY_DIR}/miniz")
set(LOG_DIR "${THIRDPARTY_DIR}/log")
set(ARGH_DIR "${THIRDPARTY_DIR}/argh")
set(STB_DIR "${THIRDPARTY_DIR}/stb")
set(MINIAUDIO_DIR "${THIRDPARTY_DIR}/miniaudio")
set(IMGUI_DIR    "${THIRDPARTY_DIR}/imgui")
set(IMGUIZMO_DIR "${THIRDPARTY_DIR}/imguizmo")
set(IMPLOT_DIR   "${THIRDPARTY_DIR}/implot")
set(IMGUIFD_DIR  "${THIRDPARTY_DIR}/imguifiledialog")
set(IMGUITGL_DIR "${THIRDPARTY_DIR}/imgui_toggle")
set(FONTS_DIR    "${THIRDPARTY_DIR}/fonts")
set(XXHASH_DIR   "${THIRDPARTY_DIR}/xxhash")

# Dear ImGui core + first-party extensions (ImGuizmo, ImPlot, ImGuiFileDialog).
# No backends here -- sokol_imgui.h provides the backend.
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUIZMO_DIR}/ImGuizmo.cpp
  ${IMPLOT_DIR}/implot.cpp
  ${IMPLOT_DIR}/implot_items.cpp
  ${IMGUIFD_DIR}/ImGuiFileDialog.cpp
  ${IMGUITGL_DIR}/imgui_toggle.cpp
  ${IMGUITGL_DIR}/imgui_toggle_palette.cpp
  ${IMGUITGL_DIR}/imgui_toggle_presets.cpp
  ${IMGUITGL_DIR}/imgui_toggle_renderer.cpp
)
target_include_directories(imgui PUBLIC
  ${IMGUI_DIR} ${IMGUIZMO_DIR} ${IMPLOT_DIR} ${IMGUIFD_DIR} ${IMGUITGL_DIR} ${FONTS_DIR})
# ImGuiFileDialog lists directories through std::filesystem on every
# platform, rather than <dirent.h> (which Windows lacks).
target_compile_definitions(imgui PRIVATE USE_STD_FILESYSTEM)

# Gather C++ sources. Sokol TU is added explicitly per-backend below so
# the GLOB doesn't pick up the wrong one on a given platform.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/*.h")
list(REMOVE_ITEM SOURCES "${CMAKE_SOURCE_DIR}/src/countlines.cpp")

# PNG encoding, resizing and captured-byte cleanup dominate long A/B films
# in otherwise unoptimized builds. Keep game/renderer compilation unchanged.
option(REVENANT_OPTIMIZE_CAPTURE "Optimize screenshot export in non-Debug builds" ON)
if(REVENANT_OPTIMIZE_CAPTURE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/framesnap.cpp" APPEND PROPERTY
    COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-O2>")
endif()

if(SOKOL_BACKEND STREQUAL "METAL")
  list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/src/sokol_impl.m")
else()
  list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/src/sokol_impl.c")
endif()

# Pick up the editor's macOS Obj-C++ shim. .mm files aren't part of the
# default *.cpp glob so add it explicitly. Skipped on non-Apple builds
# where there's no Cocoa to call into.
if(APPLE)
  list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/src/platform/macosx/editorplatform.mm")
  list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/src/platform/macosx/renderer_readback.mm")
  list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/src/platform/macosx/cursor.mm")
  list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/src/platform/macosx/headless_window.mm")
endif()

add_executable(Revenant ${SOURCES} ${MINIZ_DIR}/miniz.c ${LOG_DIR}/log.c)
target_link_libraries(Revenant PRIVATE imgui revsmk)
# Annotated classes (PROP / PROP_FN). One line per header; see rev_reflect.
rev_reflect(Revenant
  src/nodeinst.h
  src/object.h
  src/sector.h
  src/particledef.h
  src/particlecomponent.h)

# buildinfo.h: the commit the binary is built from (the playtest recordings
# name it) and its version (the main menu shows it). Regenerated every
# build, rewritten only when it changes. CI passes the version it claimed;
# otherwise BuildInfo.cmake works one out from the git tags.
set(REVENANT_VERSION "" CACHE STRING "Version the build carries (vX.Y.Z); empty: from git tags")
set(BUILDINFO_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_target(buildinfo
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -DOUTPUT=${BUILDINFO_DIR}/buildinfo.h -DVERSION=${REVENANT_VERSION}
          -P ${CMAKE_SOURCE_DIR}/cmake/BuildInfo.cmake
  BYPRODUCTS ${BUILDINFO_DIR}/buildinfo.h
  COMMENT "Recording the build's git commit and version")
add_dependencies(Revenant buildinfo)
target_include_directories(Revenant PRIVATE ${BUILDINFO_DIR})

# Propagate the SOKOL_<BACKEND> macro to every TU that pulls in
# <sokol_gfx.h> / <sokol_app.h> / sokol_imgui.h.
target_compile_definitions(Revenant PRIVATE SOKOL_${SOKOL_BACKEND})

target_compile_definitions(Revenant PRIVATE REV_VFX_TEST_BG_DIR="${CMAKE_SOURCE_DIR}/tools/vfx/test_backgrounds")

# Include directories for third-party dependencies
target_include_directories(Revenant PRIVATE
  ${SOKOL_DIR}
  ${XXHASH_DIR}
  ${HANDMADE_MATH_DIR}
  ${SIMPLEINI_DIR}
  ${MINIZ_DIR}
  ${LOG_DIR}
  ${ARGH_DIR}
  ${STB_DIR}
  ${MINIAUDIO_DIR}
  ${IMGUI_DIR}
)

add_executable(test_defdoc tools/test_defdoc.cpp)
add_executable(test_vfx_review_schedule tools/test_vfx_review_schedule.cpp)
add_executable(test_vfx_review_layout tools/test_vfx_review_layout.cpp)
add_executable(test_vfx_review_preview tools/test_vfx_review_preview.cpp src/vfxreviewpreview.cpp src/time.cpp)
target_include_directories(test_vfx_review_preview PRIVATE ${HANDMADE_MATH_DIR} ${SOKOL_DIR})

add_executable(test_render_metadata
  tools/test_render_metadata.cpp
  src/render_metadata.cpp)
# The test reads the source tree's engine asset, whatever the cwd.
target_compile_definitions(test_render_metadata PRIVATE REV_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

add_executable(test_anim_system
  tools/test_anim_system.cpp
  src/animsystem.cpp)

add_executable(test_asset_cache
  tools/test_asset_cache.cpp
  src/assetcache.cpp)

add_executable(test_transform
  tools/test_transform.cpp
  src/transform.cpp
  src/animsystem.cpp
  src/math3d.cpp)
target_include_directories(test_transform PRIVATE
  ${HANDMADE_MATH_DIR})

add_executable(test_particlefx
  tools/test_particlefx.cpp
  src/particlefx.cpp)
target_include_directories(test_particlefx PRIVATE
  ${SOKOL_DIR}
  ${HANDMADE_MATH_DIR})
target_compile_definitions(test_particlefx PRIVATE REV_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

add_executable(test_props
  tools/test_props.cpp
  src/refl.cpp
  src/packed.cpp)
target_include_directories(test_props PRIVATE ${HANDMADE_MATH_DIR})
target_link_libraries(test_props PRIVATE GTest::gtest_main)
rev_reflect(test_props tools/test_props_types.h)

add_executable(test_defio
  tools/test_defio.cpp
  src/defio.cpp
  src/nodepack.cpp
  src/nodeinst.cpp
  src/transform.cpp
  src/animsystem.cpp
  src/math3d.cpp
  src/refl.cpp
  src/packed.cpp
  src/particledef.cpp)
target_include_directories(test_defio PRIVATE ${HANDMADE_MATH_DIR})
target_compile_definitions(test_defio PRIVATE REV_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_defio PRIVATE GTest::gtest_main)
rev_reflect(test_defio tools/test_defio_types.h src/nodeinst.h src/particledef.h)

add_executable(test_nodeinst
  tools/test_nodeinst.cpp
  src/nodeinst.cpp
  src/transform.cpp
  src/animsystem.cpp
  src/math3d.cpp
  src/refl.cpp
  src/packed.cpp)
target_include_directories(test_nodeinst PRIVATE ${HANDMADE_MATH_DIR})
target_link_libraries(test_nodeinst PRIVATE GTest::gtest_main)
rev_reflect(test_nodeinst src/nodeinst.h)

add_executable(test_savefile
  tools/test_savefile.cpp
  src/savefile.cpp)
target_include_directories(test_savefile PRIVATE ${XXHASH_DIR})

add_executable(test_defwidget tools/test_defwidget.cpp)
target_include_directories(test_defwidget PRIVATE ${HANDMADE_MATH_DIR} ${LOG_DIR} ${SOKOL_DIR})

add_executable(test_sectorstore
  tools/test_sectorstore.cpp
  src/sectorstore.cpp
  src/savefile.cpp
  ${LOG_DIR}/log.c)
target_include_directories(test_sectorstore PRIVATE ${XXHASH_DIR} ${HANDMADE_MATH_DIR} ${LOG_DIR})

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
target_link_libraries(test_particlefx PRIVATE GTest::gtest_main)
target_link_libraries(test_savefile PRIVATE GTest::gtest_main)
target_link_libraries(test_sectorstore PRIVATE GTest::gtest_main)
target_link_libraries(test_defwidget PRIVATE GTest::gtest_main)

add_executable(test_vfs_path_index tools/test_vfs_path_index.cpp)
add_executable(test_retail_mesh_lighting tools/test_retail_mesh_lighting.cpp)

add_executable(cpu_tile_perspective_test
  tools/cpu_tile_perspective_test.cpp)

# Pin parser behavior with regression tests. Only compiles parse.cpp
# (plus a tiny FatalError stub baked into the test) so we avoid pulling
# in the rest of the engine. parse.h includes revenant.h, which expects
# the same include dirs and SOKOL_<backend> define as the main target.
add_executable(test_parse
  tools/test_parse.cpp
  src/parse.cpp)
target_include_directories(test_parse PRIVATE
  ${SOKOL_DIR}
  ${HANDMADE_MATH_DIR}
  ${SIMPLEINI_DIR}
  ${MINIZ_DIR}
  ${LOG_DIR}
  ${ARGH_DIR}
  ${STB_DIR}
  ${IMGUI_DIR}
)
target_compile_definitions(test_parse PRIVATE SOKOL_${SOKOL_BACKEND})

# Classic (retail) light-model arithmetic. classiclighting.cpp is pure; the
# engine headers it includes need the same include dirs as test_parse.
add_executable(test_classiclighting
  tools/test_classiclighting.cpp
  src/classiclighting.cpp)
target_include_directories(test_classiclighting PRIVATE
  ${SOKOL_DIR}
  ${HANDMADE_MATH_DIR}
  ${SIMPLEINI_DIR}
  ${MINIZ_DIR}
  ${LOG_DIR}
  ${ARGH_DIR}
  ${STB_DIR}
  ${IMGUI_DIR}
)
target_compile_definitions(test_classiclighting PRIVATE SOKOL_${SOKOL_BACKEND})
target_link_libraries(test_classiclighting PRIVATE GTest::gtest_main)

# Player-stat rules (maxima, STATLEVEL, kill experience, STATLINE).
# playerstats.cpp needs only the tokenizer; the test stubs parse.cpp's
# engine hooks like test_parse does.
add_executable(test_playerstats
  tools/test_playerstats.cpp
  src/playerstats.cpp
  src/parse.cpp)
target_include_directories(test_playerstats PRIVATE
  ${SOKOL_DIR}
  ${HANDMADE_MATH_DIR}
  ${SIMPLEINI_DIR}
  ${MINIZ_DIR}
  ${LOG_DIR}
  ${ARGH_DIR}
  ${STB_DIR}
  ${IMGUI_DIR}
)
target_compile_definitions(test_playerstats PRIVATE SOKOL_${SOKOL_BACKEND})
target_link_libraries(test_playerstats PRIVATE GTest::gtest_main)

# Shadow Smoke's plan format (docs/shadow/ARCHITECTURE.md §9.1).
add_executable(test_shadowplan
  tools/test_shadowplan.cpp
  src/shadow/shadowplan.cpp)
target_link_libraries(test_shadowplan PRIVATE GTest::gtest_main)

# Item base meshes remain independent of unsupported particle templates.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/partsys_ownership_fixture.cpp
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/partsys_ownership_fixture.py
          --output ${CMAKE_CURRENT_BINARY_DIR}/partsys_ownership_fixture.cpp
  DEPENDS tools/partsys_ownership_fixture.py tools/test_partsys_ownership.cpp src/3dimage.cpp
  VERBATIM)
add_executable(test_partsys_ownership ${CMAKE_CURRENT_BINARY_DIR}/partsys_ownership_fixture.cpp)
target_link_libraries(test_partsys_ownership PRIVATE GTest::gtest_main)

if(WIN32)
  add_executable(test_d3d11_tiles tools/test_d3d11_tiles.cpp)
  target_link_libraries(test_d3d11_tiles PRIVATE GTest::gtest_main d3d11 d3dcompiler)
endif()

# Sound file decoding and lengths (audio_backend's decoders; no playback
# device). miniz reads the shipped voices when REVENANT_DATA_PATH is set.
add_executable(test_audio_decode
  tools/test_audio_decode.cpp
  src/audio_backend.cpp
  ${MINIZ_DIR}/miniz.c
  ${LOG_DIR}/log.c)
target_include_directories(test_audio_decode PRIVATE
  ${SOKOL_DIR}
  ${HANDMADE_MATH_DIR}
  ${SIMPLEINI_DIR}
  ${MINIZ_DIR}
  ${LOG_DIR}
  ${ARGH_DIR}
  ${STB_DIR}
  ${MINIAUDIO_DIR}
  ${IMGUI_DIR}
)
target_compile_definitions(test_audio_decode PRIVATE SOKOL_${SOKOL_BACKEND})
target_link_libraries(test_audio_decode PRIVATE GTest::gtest_main)

# ----------------------------------------------------------------------------
# revsmk — clean-room Smacker (SMK2) decoder. Self-contained (C++ stdlib only),
# so it's a standalone static lib the engine and the smkdump tool both link.
# ----------------------------------------------------------------------------
set(REVSMK_DIR "${THIRDPARTY_DIR}/revsmk")
add_library(revsmk STATIC
  ${REVSMK_DIR}/revsmk.cpp
  ${REVSMK_DIR}/huffman.cpp)
target_include_directories(revsmk PUBLIC ${REVSMK_DIR})

# smkdump — decode a .SMK to PNG frames + WAV audio for verification. Uses miniz
# for PNG output only (not part of the decoder).
add_executable(smkdump tools/smkdump.cpp ${MINIZ_DIR}/miniz.c)
target_include_directories(smkdump PRIVATE ${MINIZ_DIR})
target_link_libraries(smkdump PRIVATE revsmk)

# ----------------------------------------------------------------------------
# Platform libs / frameworks.
# ----------------------------------------------------------------------------
if(APPLE)
  target_link_libraries(Revenant PRIVATE
    "-framework Metal"
    "-framework MetalKit"
    "-framework Cocoa"
    "-framework AppKit"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework AudioToolbox")
elseif(WIN32)
  # D3D11 + Win32 + WASAPI (sokol_audio uses WASAPI on Windows).
  target_link_libraries(Revenant PRIVATE
    d3d11 dxgi d3dcompiler
    user32 gdi32 ole32 shell32 kernel32)
elseif(UNIX)
  # GL + X11 input + ALSA (sokol_audio) + dl + pthread.
  find_package(Threads REQUIRED)
  find_package(OpenGL REQUIRED)
  target_link_libraries(Revenant PRIVATE
    OpenGL::GL
    X11 Xi Xcursor Xrandr
    asound
    dl
    Threads::Threads)
endif()

# ----------------------------------------------------------------------------
# Tests: `ctest --test-dir build` runs them all; CI runs the same set on
# every platform as the merge gate (docs/CI.md). A test registered here must
# pass without the retail install: one that needs game data skips itself
# when REVENANT_DATA_PATH is unset. New test executables join a list below.
# ----------------------------------------------------------------------------
enable_testing()
include(GoogleTest)
if(TARGET test_d3d11_tiles)
  gtest_discover_tests(test_d3d11_tiles TEST_PREFIX "test_d3d11_tiles." DISCOVERY_MODE PRE_TEST)
endif()

foreach(test IN ITEMS
    test_anim_system
    test_asset_cache
    test_defdoc
    test_parse
    test_render_metadata
    test_retail_mesh_lighting
    test_transform
    test_vfs_path_index
    test_vfx_review_layout
    test_vfx_review_preview
    test_vfx_review_schedule)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# GoogleTest suites report each case as its own test, named
# <executable>.<Suite>.<Case>.
foreach(test IN ITEMS
    test_audio_decode
    test_classiclighting
    test_defio
    test_defwidget
    test_particlefx
    test_partsys_ownership
    test_nodeinst
    test_playerstats
    test_props
    test_savefile
    test_sectorstore
    test_shadowplan)
  gtest_discover_tests(${test} TEST_PREFIX "${test}." DISCOVERY_MODE PRE_TEST)
endforeach()

# Source-slice tests: each compiles a function cut from src/ into a small
# fixture with clang++ and runs it. POSIX hosts only.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND NOT WIN32)
  foreach(script IN ITEMS
      test_fx_capacity
      test_fx_source_alpha
      test_helper_source_alpha
      test_helper_uv_submission
      test_late_effect_submission
      test_map_texture_submission
      test_transient_map)
    add_test(NAME ${script}.py
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/${script}.py)
  endforeach()
  # Out of date with SubmitFxQuad since the prelit RGBA5 path (6c6c5d53; its
  # fixture lacks SQuadDrawItem::retail_prelit_rgba5 and EFxDepthMode).
  # Listed so reports show it; re-enable once the VFX track updates it.
  set_tests_properties(test_fx_source_alpha.py PROPERTIES DISABLED TRUE)
endif()
