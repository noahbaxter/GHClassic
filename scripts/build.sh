#!/usr/bin/env bash
# GH2 disc -> generated C++ -> native binary.
#
#   scripts/build.sh [disc]        verify, symbolize, recompile, build
#   scripts/build.sh --skip-recomp rebuild from the existing generated code
#   scripts/build.sh --no-lto      skip link-time optimisation, for iterating
#
# The disc is a Guitar Hero II (USA) image (.chd, .iso or .bin). Without one,
# the GH2 image in game/ is used. game/ is only ever read; everything this
# produces lands in build/, which is safe to delete.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/build/tools"
RECOMP_DIR="$ROOT/build/recomp"
BUILD="$ROOT/build/game"
SERIAL="SLUS-21447"
SYMBOLS="$ROOT/config/gh2-retail.symbols"
JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)"

RECOMP=1
LTO=ON
DISC=""
for arg in "$@"; do
  case "$arg" in
    --skip-recomp) RECOMP=0 ;;
    --no-lto)      LTO=OFF ;;
    -*) echo "unknown argument: $arg" >&2; exit 2 ;;
    *)  DISC="$arg" ;;
  esac
done

if [ "$RECOMP" = 1 ]; then
  [ -n "$DISC" ] || DISC="$(python3 "$ROOT/tools/disc.py" find "$SERIAL" "$ROOT"/game/*)"

  rm -rf "$RECOMP_DIR"
  mkdir -p "$RECOMP_DIR"
  python3 "$ROOT/tools/disc.py" boot-elf "$DISC" "$RECOMP_DIR/retail.elf"
  python3 "$ROOT/tools/symbolize.py" "$RECOMP_DIR/retail.elf" "$SYMBOLS" "$RECOMP_DIR/gh2.elf"

  cmake -S "$ROOT/lib/PS2Recomp" -B "$TOOLS" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF > /dev/null
  cmake --build "$TOOLS" --target ps2_analyzer ps2_recomp -j "$JOBS"

  # The analyzer writes the recompiler config, and points the generated code
  # at output/ beside it. Both are verbose, so their output goes to logs.
  "$TOOLS/ps2xAnalyzer/ps2_analyzer" "$RECOMP_DIR/gh2.elf" "$RECOMP_DIR/gh2.toml" \
    > "$RECOMP_DIR/analyzer.log" 2>&1 || { tail -20 "$RECOMP_DIR/analyzer.log"; exit 1; }

  # The denylist goes into [general], the first table in the analyzer's output.
  DENY="$(grep -v -e '^#' -e '^$' "$ROOT/config/stub-denylist.txt" | sed 's/.*/"&"/' | paste -sd, -)"
  awk -v deny="stub_denylist = [$DENY]" '{ print } /^\[general\]$/ { print deny }' \
    "$RECOMP_DIR/gh2.toml" > "$RECOMP_DIR/gh2.toml.tmp"
  mv "$RECOMP_DIR/gh2.toml.tmp" "$RECOMP_DIR/gh2.toml"

  "$TOOLS/ps2xRecomp/ps2_recomp" "$RECOMP_DIR/gh2.toml" > "$RECOMP_DIR/recomp.log" 2>&1 ||
    { tail -20 "$RECOMP_DIR/recomp.log"; exit 1; }
  echo "generated $(find "$RECOMP_DIR/output" -name '*.cpp' | wc -l | tr -d ' ') files"
fi

# Configured after recompiling, so the glob in CMakeLists.txt sees the output.
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGHRECOMP_GENERATED_DIR="$RECOMP_DIR/output" -DGHRECOMP_ENABLE_LTO="$LTO" > /dev/null
cmake --build "$BUILD" --target ghrecomp -j "$JOBS"
echo "run: $BUILD/ghrecomp $RECOMP_DIR/gh2.elf"
