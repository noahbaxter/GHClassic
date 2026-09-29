#!/usr/bin/env bash
# Run the game in a window.
#
#   scripts/play.sh [disc] [ghrecomp options, e.g. --res native]
#
# The memory card lives in the user data directory and persists between
# runs. The first run seeds it from ghpc's card, so both boot the same save.
# The disc defaults to the GH2 image in game/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/game/ghrecomp"
ELF="$ROOT/build/recomp/gh2.elf"
DATA="$HOME/Library/Application Support/ghrecomp"
CARD="$DATA/mc0"

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
