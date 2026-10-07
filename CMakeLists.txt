cmake_minimum_required(VERSION 3.25)

# Else the build machine's macOS, which locks out every older one. MoltenVK's
# own floor.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Oldest macOS the game runs on")

project(GHClassic LANGUAGES C CXX)
# SDL's Cocoa backend is Objective-C, and a subproject cannot enable it.
if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output of ps2_recomp, written by scripts/build.sh. The target below only
# exists once it is populated, so a fresh tree can configure and build the
# tools first.
set(GHC_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/recomp/output"
    CACHE PATH "Directory holding the recompiler's generated C++")

# Relinking 12k functions with LTO costs about 90s. Off while iterating.
option(GHC_ENABLE_LTO "Link-time optimisation for release builds" ON)

# Runtime only. The analyzer and recompiler are host tools, built by
# scripts/build.sh as a top-level build of lib/PS2Recomp, which their CMake
# assumes.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE)
# Movies decode through libmpeg2 (below), so no FFmpeg to find or ship.
set(PS2X_ENABLE_FFMPEG OFF CACHE BOOL "" FORCE)

# The runtime's logs are on upstream. With them on, song select misses a
# press every run. Off unless asked for with -D.
set(PS2X_ENABLE_RUNTIME_LOGS OFF CACHE BOOL "")
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "")
set(PS2X_ENABLE_IOP_RPC_TRACE OFF CACHE BOOL "")

# A dev unit's 128MB rather than retail's 32MB. GH2 sizes its own heaps and
# leaves the rest to libc, so the extra room keeps later additions out of
# the out-of-memory paths retail runs close to.
set(PS2X_EE_RAM_MB 128 CACHE STRING "" FORCE)

# The runtime reads .chd images when chdr-static exists, so libchdr comes
# first. EXCLUDE_FROM_ALL builds only the static library it links.
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
# Its miniz also deflates save.bin, so it keeps the compressor.
set(MINIZ_DEFLATE_APIS ON CACHE BOOL "" FORCE)
add_subdirectory(lib/libchdr EXCLUDE_FROM_ALL)
target_include_directories(miniz INTERFACE lib/libchdr/deps/miniz-3.1.2)

# EXCLUDE_FROM_ALL keeps upstream's own ps2EntryRunner out of the default
# build, leaving GHClassic and what it links.
add_subdirectory(lib/PS2Recomp EXCLUDE_FROM_ALL)

# The runtime and the generated code use SSE4.1 intrinsics (_mm_extract_epi32),
# which x86_64 compilers other than MSVC leave off by default. Here, so the
# runtime builds without generated code too (CI).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$" AND NOT MSVC)
    set(GHC_X86_FLAGS -msse4.1)
    target_compile_options(ps2_runtime PRIVATE ${GHC_X86_FLAGS})
endif()

set(PS2X_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/PS2Recomp/ps2xRuntime")

# The host: SDL3 for the window, Vulkan (MoltenVK on macOS) for the picture.
# All fetched at pinned versions, so every platform builds the same libraries
# and nothing comes from the system. Vulkan is never linked: SDL loads the
# driver's loader (MoltenVK on macOS) and volk resolves the calls from it.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# SYSTEM: their headers' warnings are not ours to fix.
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.16
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.357
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.4.357
    SYSTEM)
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    SYSTEM)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 16.6.0
    GIT_SHALLOW TRUE
    SYSTEM)
# Headers first: vk-bootstrap and volk take Vulkan::Headers when it exists.
FetchContent_MakeAvailable(VulkanHeaders)
FetchContent_MakeAvailable(SDL3 volk vk-bootstrap VulkanMemoryAllocator glslang)
target_link_libraries(volk PUBLIC Vulkan::Headers)
if(APPLE)
    FetchContent_Declare(MoltenVK
        URL https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
        URL_HASH SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e)
    FetchContent_MakeAvailable(MoltenVK)
endif()

# MPEG-2 video for the movies: libmpeg2 (GPL-2.0-or-later), its portable C
# only, with no CPU detection or assembly. Upstream builds with autotools, so
# it is built here; the commit is 0.5.1 plus fixes, past the last tag.
FetchContent_Declare(libmpeg2
    GIT_REPOSITORY https://code.videolan.org/videolan/libmpeg2.git
    GIT_TAG 946bf4b518aacc224f845e73708f99e394744499)
FetchContent_MakeAvailable(libmpeg2)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libmpeg2/config.h"
    CONTENT "#define ATTRIBUTE_ALIGNED_MAX 64\n#define HAVE_BUILTIN_EXPECT 1\n")
add_library(mpeg2 STATIC)
foreach(MPEG2_SOURCE alloc cpu_accel cpu_state decode header idct motion_comp slice)
    target_sources(mpeg2 PRIVATE "${libmpeg2_SOURCE_DIR}/libmpeg2/${MPEG2_SOURCE}.c")
endforeach()
target_include_directories(mpeg2 SYSTEM PUBLIC "${libmpeg2_SOURCE_DIR}/include")
target_include_directories(mpeg2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/libmpeg2")
set_target_properties(mpeg2 PROPERTIES C_STANDARD 99)

# The save code's round trips, which need no disc and no game.
add_executable(save_test EXCLUDE_FROM_ALL
    tests/save_test.cpp
    src/save/gh2_stream.cpp
    src/save/gh2_text.cpp
    src/save/ps2_card.cpp
    src/save/store.cpp
)
target_include_directories(save_test PRIVATE src)
target_link_libraries(save_test PRIVATE miniz)

# Without a disc's generated code, everything above still builds: what CI
# checks (tools/ghc.py ci).
file(GLOB GHC_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${GHC_GENERATED_DIR}/*.cpp"
)
if(NOT GHC_GENERATED_SOURCES)
    message(STATUS "No generated code in ${GHC_GENERATED_DIR}; GHClassic target skipped")
    return()
endif()

# The game layer: GH2's hooks, installed through the runtime's override
# registry, and the host window and renderer they draw through.
set(GHC_GAME_SOURCES
    src/main.cpp
    src/game.cpp
    src/frame_step.cpp
    src/script.cpp
    src/sustain_release.cpp
    src/track_start.cpp
    src/whammy_hold.cpp
    src/dev/draw_dump.cpp
    src/dev/mesh_packet.cpp
    src/dev/null_rnd.cpp
    src/dev/scenario.cpp
    src/dev/seed.cpp
    src/dev/transplant.cpp
    src/disc/disc.cpp
    src/disc/sha1.cpp
    src/movie/movie.cpp
    src/movie/pss.cpp
    src/movie/screen.cpp
    src/save/card.cpp
    src/save/gh2_stream.cpp
    src/save/gh2_text.cpp
    src/save/ps2_card.cpp
    src/save/save.cpp
    src/save/store.cpp
    src/settings/latency.cpp
    src/settings/settings.cpp
    src/settings/video_options.cpp
    src/synth/module.cpp
    src/synth/reverb.cpp
    src/synth/spu.cpp
    src/synth/synth.cpp
    src/ui/fast_boot.cpp
    src/ui/locale.cpp
    src/ui/menus.cpp
    src/ui/meta_music.cpp
    src/host/audio.cpp
    src/host/bind.cpp
    src/host/bindings.cpp
    src/host/input.cpp
    src/host/pad.cpp
    src/host/profiles.cpp
    src/host/vulkan_frontend.cpp
    src/host/vma.cpp
    src/host/scene_renderer.cpp
    src/render/camera.cpp
    src/render/frame.cpp
    src/render/mesh_capture.cpp
    src/render/native_cull.cpp
    src/render/native_environ.cpp
    src/render/native_mat.cpp
    src/render/native_mesh.cpp
    src/render/native_particles.cpp
    src/render/native_points.cpp
    src/render/native_rect.cpp
    src/render/native_rnd.cpp
    src/render/texture_capture.cpp
)

add_executable(GHClassic
    ${GHC_GENERATED_SOURCES}
    ${GHC_GAME_SOURCES}
)

# Shaders compile to SPIR-V headers, each a uint32_t array named after it
# (mesh.vert -> kMeshVert in mesh_vert.h).
set(GHC_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${GHC_SHADER_DIR}")
set(GHC_SHADER_HEADERS)
foreach(SHADER mesh.vert mesh.frag)
    string(REPLACE "." "_" SHADER_ID "${SHADER}")
    string(REPLACE "mesh_vert" "kMeshVert" SHADER_VAR "${SHADER_ID}")
    string(REPLACE "mesh_frag" "kMeshFrag" SHADER_VAR "${SHADER_VAR}")
    set(SHADER_HEADER "${GHC_SHADER_DIR}/${SHADER_ID}.h")
    add_custom_command(
        OUTPUT "${SHADER_HEADER}"
        COMMAND glslang-standalone -V --vn "${SHADER_VAR}" -o "${SHADER_HEADER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/host/shaders/${SHADER}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/shaders/${SHADER}" glslang-standalone
        VERBATIM)
    list(APPEND GHC_SHADER_HEADERS "${SHADER_HEADER}")
endforeach()
add_custom_target(GHClassic_shaders DEPENDS ${GHC_SHADER_HEADERS})
add_dependencies(GHClassic GHClassic_shaders)
target_include_directories(GHClassic PRIVATE "${GHC_SHADER_DIR}")

# The port's own game scripts, compiled in as strings (dta.h); editing one
# reconfigures.
set(GHC_DTA_DIR "${CMAKE_CURRENT_BINARY_DIR}/dta")
file(READ src/ui/dta/lag_panel.dta GHC_LAG_PANEL_DTA)
file(READ src/ui/dta/highscore_panel.dta GHC_HIGHSCORE_PANEL_DTA)
file(READ src/ui/dta/main_menu.dta GHC_MAIN_MENU_DTA)
file(READ src/ui/dta/fast_boot.dta GHC_FAST_BOOT_DTA)
file(READ src/ui/dta/locale.dta GHC_LOCALE_DTA)
file(READ src/ui/dta/crowd.dta GHC_CROWD_DTA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    src/ui/dta/lag_panel.dta src/ui/dta/highscore_panel.dta src/ui/dta/main_menu.dta src/ui/dta/fast_boot.dta
    src/ui/dta/locale.dta src/ui/dta/crowd.dta)
configure_file(src/ui/dta/dta.h.in "${GHC_DTA_DIR}/dta.h" @ONLY)
target_include_directories(GHClassic PRIVATE "${GHC_DTA_DIR}")

# The window icons, where SDL_GetBasePath finds them. On macOS the game is an
# app bundle, so other apps (Finder, audio mixers) see its icon too, and the
# base path is the bundle's Resources.
if(APPLE)
    # MoltenVK rides in the bundle, where vulkan_frontend.cpp points SDL.
    set(GHC_MOLTENVK "${moltenvk_SOURCE_DIR}/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib")
    target_sources(GHClassic PRIVATE assets/icon.icns assets/icon_macos.png "${GHC_MOLTENVK}")
    set_source_files_properties(assets/icon.icns assets/icon_macos.png
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties("${GHC_MOLTENVK}" PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
    set_target_properties(GHClassic PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "GH Classic"
        MACOSX_BUNDLE_GUI_IDENTIFIER com.noahbaxter.ghclassic
        MACOSX_BUNDLE_ICON_FILE icon.icns)
else()
    configure_file(assets/icon.png "${CMAKE_CURRENT_BINARY_DIR}/icon.png" COPYONLY)
endif()

# Unity batches are for the generated code; game files stay separate units.
set_source_files_properties(${GHC_GAME_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

target_include_directories(GHClassic PRIVATE "${GHC_GENERATED_DIR}" src)
target_link_libraries(GHClassic PRIVATE
    ps2_runtime
    SDL3::SDL3
    volk::volk
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    mpeg2
    miniz # libchdr's, for save.bin
)
# Calls go through volk's pointers, never a linked loader.
target_compile_definitions(GHClassic PRIVATE VK_NO_PROTOTYPES)

# The game runs the executable off the player's disc, which must be the one
# the generated code came from (disc/disc.cpp).
set(GHC_RETAIL_ELF "${GHC_GENERATED_DIR}/../retail.elf" CACHE FILEPATH "The ELF the generated code came from")
if(NOT EXISTS "${GHC_RETAIL_ELF}")
    message(FATAL_ERROR "No ${GHC_RETAIL_ELF}; run tools/ghc.py build")
endif()
file(SHA1 "${GHC_RETAIL_ELF}" GHC_RETAIL_SHA1)
target_compile_definitions(GHClassic PRIVATE GHC_RETAIL_SHA1="${GHC_RETAIL_SHA1}")

# Upstream sets its platform setup (sse2neon on ARM) at directory scope, so it
# reaches only targets inside lib/PS2Recomp. Generated code needs it too.
foreach(PROP INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
    get_property(PS2X_DIR_${PROP} DIRECTORY lib/PS2Recomp PROPERTY ${PROP})
endforeach()
target_include_directories(GHClassic PRIVATE ${PS2X_DIR_INCLUDE_DIRECTORIES})
target_compile_definitions(GHClassic PRIVATE ${PS2X_DIR_COMPILE_DEFINITIONS})
target_compile_options(GHClassic PRIVATE ${PS2X_DIR_COMPILE_OPTIONS})

target_compile_options(GHClassic PRIVATE ${GHC_X86_FLAGS})
# Unity batches outgrow COFF's 32k sections. A static runtime leaves the exe
# needing nothing of llvm-mingw's beside it, and -mwindows no console window
# beside the game's (output goes to GHClassic.log, main.cpp).
if(MINGW)
    target_compile_options(GHClassic PRIVATE -Wa,-mbig-obj)
    target_link_options(GHClassic PRIVATE -static -mwindows)
endif()

# Same batching and precompiled headers upstream uses for its runner. Without
# them a cold build of the generated code takes several times longer.
set_target_properties(GHClassic PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 32
)
target_precompile_headers(GHClassic PRIVATE
    "${PS2X_RUNTIME_DIR}/include/ps2_runtime_macros.h"
    "${PS2X_RUNTIME_DIR}/include/ps2_runtime.h"
    "${GHC_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${GHC_GENERATED_DIR}/ps2_recompiled_stubs.h"
    "${PS2X_RUNTIME_DIR}/include/ps2_syscalls.h"
    "${PS2X_RUNTIME_DIR}/include/ps2_stubs.h"
)

if(GHC_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # EnableFastReleaseMode reads IPO_SUPPORTED, which upstream sets only in
    # its own directory scope. Without this check LTO is silently skipped.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED LANGUAGES CXX)
    EnableFastReleaseMode(GHClassic)
endif()
