#!/usr/bin/env bash
# Run the game in a window.
#
#   scripts/play.sh [--no-build] [disc] [GHClassic options, e.g. --res native]
#
# Brings the binary up to date first (the C++ build only, not the recompile;
# a no-op when nothing changed), unless --no-build. The memory card lives in the user data directory and persists between
# runs. The first run seeds it from ghpc's card, so both boot the same save.
# The disc defaults to the GH2 image in game/.
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

if [ "$BUILD" = 1 ]; then
  LOG="$(mktemp)"
  if ! cmake --build "$ROOT/build/game" > "$LOG" 2>&1; then
    cat "$LOG" >&2
    rm -f "$LOG"
    exit 1
  fi
  rm -f "$LOG"
fi
[ -x "$BIN" ] && [ -f "$ELF" ] || { echo "no build; run scripts/build.sh" >&2; exit 1; }
DISC=""
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
  DISC="$1"
  shift
fi
[ -n "$DISC" ] || DISC="$(python3 "$ROOT/tools/disc.py" find SLUS-21447 "$ROOT"/game/*)"

if [ ! -d "$CARD" ]; then
  mkdir -p "$CARD"
  GHPC_CARD="$ROOT/../ghpc/work/mc0"
  [ -d "$GHPC_CARD" ] && cp -Rp "$GHPC_CARD/." "$CARD/"
fi

exec "$BIN" "$ELF" "$DISC" --mc "$CARD" "$@"
