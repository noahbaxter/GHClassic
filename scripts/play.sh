#!/usr/bin/env bash
# Run the game in a window.
#
#   scripts/play.sh [--no-build] [disc] [GHClassic options, e.g. --res native]
#
# The first run does the whole build (scripts/build.sh) from the disc. After
# that it brings the binary up to date first (the C++ build only, not the
# recompile; a no-op when nothing changed), unless --no-build. The memory
# card lives in the user data directory and persists between runs. The disc
# defaults to the GH2 image in game/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/game/GHClassic.app/Contents/MacOS/GHClassic"
ELF="$ROOT/build/recomp/gh2.elf"
DATA="$HOME/Library/Application Support/GHClassic"
CARD="$DATA/mc0"

# The project was ghrecomp: its data directory moves over, as the game
# itself does it (settings.cpp), before the card below is looked for.
OLD_DATA="$HOME/Library/Application Support/ghrecomp"
[ -d "$OLD_DATA" ] && [ ! -e "$DATA" ] && mv "$OLD_DATA" "$DATA"

BUILD=1
if [ "${1:-}" = "--no-build" ]; then
  BUILD=0
  shift
fi

DISC=""
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
  DISC="$1"
  shift
fi
[ -n "$DISC" ] || DISC="$(python3 "$ROOT/tools/disc.py" find SLUS-21447 "$ROOT"/game/*)"

if [ ! -f "$ELF" ] || [ ! -x "$BIN" ]; then
  # First run: the whole build, recompile included.
  [ "$BUILD" = 1 ] || { echo "no build; run scripts/build.sh" >&2; exit 1; }
  "$ROOT/scripts/build.sh" "$DISC"
elif [ "$BUILD" = 1 ]; then
  LOG="$(mktemp)"
  if ! cmake --build "$ROOT/build/game" > "$LOG" 2>&1; then
    cat "$LOG" >&2
    rm -f "$LOG"
    exit 1
  fi
  rm -f "$LOG"
fi

mkdir -p "$CARD"

exec "$BIN" "$ELF" "$DISC" --mc "$CARD" "$@"
