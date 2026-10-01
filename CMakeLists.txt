cmake_minimum_required(VERSION 3.25)

project(GHClassic LANGUAGES C CXX)

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

# A dev unit's 128MB rather than retail's 32MB. GH2 sizes its own heaps and
# leaves the rest to libc, so the extra room keeps later additions out of
# the out-of-memory paths retail runs close to.
set(PS2X_EE_RAM_MB 128 CACHE STRING "" FORCE)

# The runtime reads .chd images when chdr-static exists, so libchdr comes
# first. EXCLUDE_FROM_ALL builds only the static library it links.
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/libchdr EXCLUDE_FROM_ALL)

# EXCLUDE_FROM_ALL keeps upstream's own ps2EntryRunner out of the default
# build, leaving GHClassic and what it links.
add_subdirectory(lib/PS2Recomp EXCLUDE_FROM_ALL)

file(GLOB GHC_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${GHC_GENERATED_DIR}/*.cpp"
)

if(NOT GHC_GENERATED_SOURCES)
    message(STATUS "No generated code in ${GHC_GENERATED_DIR}; GHClassic target skipped")
    return()
endif()

set(PS2X_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/PS2Recomp/ps2xRuntime")

# The host: SDL3 for the window, Vulkan (MoltenVK on macOS) for the picture.
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
include(FetchContent)
# SYSTEM: their headers' warnings are not ours to fix.
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.4.357
    SYSTEM)
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    SYSTEM)
FetchContent_MakeAvailable(vk-bootstrap VulkanMemoryAllocator)

# The game layer: GH2's hooks, installed through the runtime's override
# registry, and the host window and renderer they draw through.
set(GHC_GAME_SOURCES
    src/main.cpp
    src/game.cpp
    src/frame_step.cpp
    src/latency.cpp
    src/menus.cpp
    src/meta_music.cpp
    src/null_movie.cpp
    src/null_rnd.cpp
    src/pad.cpp
    src/script.cpp
    src/settings.cpp
    src/synth/module.cpp
    src/synth/reverb.cpp
    src/synth/spu.cpp
    src/synth/synth.cpp
    src/video_options.cpp
    src/host/audio.cpp
    src/host/bind.cpp
    src/host/bindings.cpp
    src/host/input.cpp
    src/host/profiles.cpp
    src/host/vulkan_frontend.cpp
    src/host/vma.cpp
    src/host/scene_renderer.cpp
    src/render/camera.cpp
    src/render/frame.cpp
    src/render/mesh_capture.cpp
    src/render/native_environ.cpp
    src/render/native_mat.cpp
    src/render/native_mesh.cpp
    src/render/native_particles.cpp
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
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
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
        COMMAND "${GLSLANG_VALIDATOR}" -V --vn "${SHADER_VAR}" -o "${SHADER_HEADER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/host/shaders/${SHADER}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/shaders/${SHADER}"
        VERBATIM)
    list(APPEND GHC_SHADER_HEADERS "${SHADER_HEADER}")
endforeach()
add_custom_target(GHClassic_shaders DEPENDS ${GHC_SHADER_HEADERS})
add_dependencies(GHClassic GHClassic_shaders)
target_include_directories(GHClassic PRIVATE "${GHC_SHADER_DIR}")

# The port's own game scripts, compiled in as strings (dta.h); editing one
# reconfigures.
set(GHC_DTA_DIR "${CMAKE_CURRENT_BINARY_DIR}/dta")
file(READ src/dta/lag_panel.dta GHC_LAG_PANEL_DTA)
file(READ src/dta/highscore_panel.dta GHC_HIGHSCORE_PANEL_DTA)
file(READ src/dta/main_menu.dta GHC_MAIN_MENU_DTA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    src/dta/lag_panel.dta src/dta/highscore_panel.dta src/dta/main_menu.dta)
configure_file(src/dta/dta.h.in "${GHC_DTA_DIR}/dta.h" @ONLY)
target_include_directories(GHClassic PRIVATE "${GHC_DTA_DIR}")

# The window icons, where SDL_GetBasePath finds them. On macOS the game is an
# app bundle, so other apps (Finder, audio mixers) see its icon too, and the
# base path is the bundle's Resources.
if(APPLE)
    target_sources(GHClassic PRIVATE assets/icon.icns assets/icon_macos.png)
    set_source_files_properties(assets/icon.icns assets/icon_macos.png
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
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
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
)

# Upstream sets its platform setup (sse2neon on ARM) at directory scope, so it
# reaches only targets inside lib/PS2Recomp. Generated code needs it too.
foreach(PROP INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
    get_property(PS2X_DIR_${PROP} DIRECTORY lib/PS2Recomp PROPERTY ${PROP})
endforeach()
target_include_directories(GHClassic PRIVATE ${PS2X_DIR_INCLUDE_DIRECTORIES})
target_compile_definitions(GHClassic PRIVATE ${PS2X_DIR_COMPILE_DEFINITIONS})
target_compile_options(GHClassic PRIVATE ${PS2X_DIR_COMPILE_OPTIONS})

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
