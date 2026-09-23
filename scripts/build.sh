#!/usr/bin/env bash
# GH2 debug ELF -> generated C++ -> native binary.
#
#   scripts/build.sh               analyse, recompile, build
#   scripts/build.sh --skip-recomp rebuild from the existing generated code
#   scripts/build.sh --no-lto      skip link-time optimisation, for iterating
#
# Reads game/ (GH2_debug.elf with GEN/ beside it) and never writes to it.
# Everything it produces lands in build/, which is safe to delete.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="$ROOT/game/GH2_debug.elf"
TOOLS="$ROOT/build/tools"
RECOMP_DIR="$ROOT/build/recomp"
BUILD="$ROOT/build/game"
JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)"

RECOMP=1
LTO=ON
for arg in "$@"; do
  case "$arg" in
    --skip-recomp) RECOMP=0 ;;
    --no-lto)      LTO=OFF ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

[ -f "$ELF" ] || { echo "missing $ELF" >&2; exit 1; }

if [ "$RECOMP" = 1 ]; then
  cmake -S "$ROOT/lib/PS2Recomp" -B "$TOOLS" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF > /dev/null
  cmake --build "$TOOLS" --target ps2_analyzer ps2_recomp -j "$JOBS"

  # The analyzer writes the recompiler config, and points the generated code
  # at output/ beside it.
  rm -rf "$RECOMP_DIR"
  mkdir -p "$RECOMP_DIR"
  "$TOOLS/ps2xAnalyzer/ps2_analyzer" "$ELF" "$RECOMP_DIR/gh2.toml"
  "$TOOLS/ps2xRecomp/ps2_recomp" "$RECOMP_DIR/gh2.toml" > "$RECOMP_DIR/recomp.log" 2>&1 ||
    { tail -20 "$RECOMP_DIR/recomp.log"; exit 1; }
  echo "generated $(find "$RECOMP_DIR/output" -name '*.cpp' | wc -l | tr -d ' ') files"
fi

# Configured after recompiling, so the glob in CMakeLists.txt sees the output.
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGHRECOMP_GENERATED_DIR="$RECOMP_DIR/output" -DGHRECOMP_ENABLE_LTO="$LTO" > /dev/null
cmake --build "$BUILD" --target ghrecomp -j "$JOBS"
echo "run: $BUILD/ghrecomp $ELF"
