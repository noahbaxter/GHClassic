cmake_minimum_required(VERSION 3.21)

project(ghrecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output of ps2_recomp, written by scripts/build.sh. The target below only
# exists once it is populated, so a fresh tree can configure and build the
# tools first.
set(GHRECOMP_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/recomp/output"
    CACHE PATH "Directory holding the recompiler's generated C++")

# Relinking 12k functions with LTO costs about 90s. Off while iterating.
option(GHRECOMP_ENABLE_LTO "Link-time optimisation for release builds" ON)

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
# build, leaving ghrecomp and what it links.
add_subdirectory(lib/PS2Recomp EXCLUDE_FROM_ALL)

file(GLOB GHRECOMP_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${GHRECOMP_GENERATED_DIR}/*.cpp"
)

if(NOT GHRECOMP_GENERATED_SOURCES)
    message(STATUS "No generated code in ${GHRECOMP_GENERATED_DIR}; ghrecomp target skipped")
    return()
endif()

set(PS2X_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/PS2Recomp/ps2xRuntime")

# The host: SDL3 for the window, Vulkan (MoltenVK on macOS) for the picture.
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
include(FetchContent)
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.4.357)
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0)
FetchContent_MakeAvailable(vk-bootstrap VulkanMemoryAllocator)

# The game layer: GH2's hooks, installed through the runtime's override
# registry, and the host window and renderer they draw through.
set(GHRECOMP_GAME_SOURCES
    src/main.cpp
    src/game.cpp
    src/meta_music.cpp
    src/null_movie.cpp
    src/null_rnd.cpp
    src/pad.cpp
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

add_executable(ghrecomp
    ${GHRECOMP_GENERATED_SOURCES}
    ${GHRECOMP_GAME_SOURCES}
)

# Shaders compile to SPIR-V headers, each a uint32_t array named after it
# (mesh.vert -> kMeshVert in mesh_vert.h).
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(GHRECOMP_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${GHRECOMP_SHADER_DIR}")
set(GHRECOMP_SHADER_HEADERS)
foreach(SHADER mesh.vert mesh.frag)
    string(REPLACE "." "_" SHADER_ID "${SHADER}")
    string(REPLACE "mesh_vert" "kMeshVert" SHADER_VAR "${SHADER_ID}")
    string(REPLACE "mesh_frag" "kMeshFrag" SHADER_VAR "${SHADER_VAR}")
    set(SHADER_HEADER "${GHRECOMP_SHADER_DIR}/${SHADER_ID}.h")
    add_custom_command(
        OUTPUT "${SHADER_HEADER}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --vn "${SHADER_VAR}" -o "${SHADER_HEADER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/host/shaders/${SHADER}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/shaders/${SHADER}"
        VERBATIM)
    list(APPEND GHRECOMP_SHADER_HEADERS "${SHADER_HEADER}")
endforeach()
add_custom_target(ghrecomp_shaders DEPENDS ${GHRECOMP_SHADER_HEADERS})
add_dependencies(ghrecomp ghrecomp_shaders)
target_include_directories(ghrecomp PRIVATE "${GHRECOMP_SHADER_DIR}")

# The window icons, next to the binary where SDL_GetBasePath finds them.
configure_file(assets/icon.png "${CMAKE_CURRENT_BINARY_DIR}/icon.png" COPYONLY)
configure_file(assets/icon_macos.png "${CMAKE_CURRENT_BINARY_DIR}/icon_macos.png" COPYONLY)

# Unity batches are for the generated code; game files stay separate units.
set_source_files_properties(${GHRECOMP_GAME_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

target_include_directories(ghrecomp PRIVATE "${GHRECOMP_GENERATED_DIR}" src)
target_link_libraries(ghrecomp PRIVATE
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
target_include_directories(ghrecomp PRIVATE ${PS2X_DIR_INCLUDE_DIRECTORIES})
target_compile_definitions(ghrecomp PRIVATE ${PS2X_DIR_COMPILE_DEFINITIONS})
target_compile_options(ghrecomp PRIVATE ${PS2X_DIR_COMPILE_OPTIONS})

# Same batching and precompiled headers upstream uses for its runner. Without
# them a cold build of the generated code takes several times longer.
set_target_properties(ghrecomp PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 32
)
target_precompile_headers(ghrecomp PRIVATE
    "${PS2X_RUNTIME_DIR}/include/ps2_runtime_macros.h"
    "${PS2X_RUNTIME_DIR}/include/ps2_runtime.h"
    "${GHRECOMP_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${GHRECOMP_GENERATED_DIR}/ps2_recompiled_stubs.h"
    "${PS2X_RUNTIME_DIR}/include/ps2_syscalls.h"
    "${PS2X_RUNTIME_DIR}/include/ps2_stubs.h"
)

if(GHRECOMP_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # EnableFastReleaseMode reads IPO_SUPPORTED, which upstream sets only in
    # its own directory scope. Without this check LTO is silently skipped.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED LANGUAGES CXX)
    EnableFastReleaseMode(ghrecomp)
endif()
