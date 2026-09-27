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

# The game layer: GH2's hooks, installed through the runtime's override
# registry.
set(GHRECOMP_GAME_SOURCES
    src/game.cpp
    src/null_movie.cpp
    src/null_rnd.cpp
    src/null_synth.cpp
)

add_executable(ghrecomp
    ${GHRECOMP_GENERATED_SOURCES}
    ${GHRECOMP_GAME_SOURCES}
    "${PS2X_RUNTIME_DIR}/src/main.cpp"
)

# Unity batches are for the generated code; game files stay separate units.
set_source_files_properties(${GHRECOMP_GAME_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

target_include_directories(ghrecomp PRIVATE "${GHRECOMP_GENERATED_DIR}")
target_link_libraries(ghrecomp PRIVATE ps2_runtime)

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
