#!/usr/bin/env bash
# Run the game once, unattended, and keep what it did.
#
#   scripts/arm.sh <out> [--secs 30] [--disc <image>] [--shot-every 60]
#
# The window stays hidden, no audio device opens, the memory card is a
# throwaway copy of the player's, and only the process this script started
# is ever killed.
# <out> gets run.log, frames/ (the guest picture every --shot-every presented
# frames) and stack.txt, a 3s sample taken just before the stop. The summary
# names where the game thread spends its time, by recompiled function.
#
# Frames are 640x448 PNGs named by seconds since launch (frame_0012.3s.png).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$HOME/Library/Application Support/GHClassic"
OUT=""
SECS=30
DISC=""
EVERY=60
while [ $# -gt 0 ]; do
  case "$1" in
    --secs)       SECS="$2"; shift ;;
    --disc)       DISC="$2"; shift ;;
    --shot-every) EVERY="$2"; shift ;;
    -*) echo "unknown argument: $1" >&2; exit 2 ;;
    *)  OUT="$1" ;;
  esac
  shift
done
[ -n "$OUT" ] || { echo "usage: scripts/arm.sh <out> [--secs N] [--disc image] [--shot-every N]" >&2; exit 2; }

rm -rf "$OUT"
mkdir -p "$OUT/frames"
OUT="$(cd "$OUT" && pwd)"
CARD="$(mktemp -d "${TMPDIR:-/tmp}/arm-mc.XXXXXX")"
PID=""
RUNNER=""

cleanup() {
  [ -n "$RUNNER" ] && kill "$RUNNER" 2>/dev/null || true
  [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
  rm -rf "$CARD" "$CARD"_slot*
}
trap cleanup EXIT INT TERM

# Frames are named by seconds since here, read off the same clock as their
# own mtimes.
touch "$OUT/.start"
START="$(stat -f %Fm "$OUT/.start")"
rm "$OUT/.start"
since() { stat -f %Fm "$1" | awk -v s="$START" '{ printf "%07.2f", $1 - s }'; }

BIN="$ROOT/build/game/GHClassic.app/Contents/MacOS/GHClassic"
ELF="$ROOT/build/recomp/gh2.elf"
[ -x "$BIN" ] && [ -f "$ELF" ] || { echo "no build; run scripts/build.sh" >&2; exit 1; }
[ -n "$DISC" ] || DISC="$(python3 "$ROOT/tools/disc.py" find SLUS-21447 "$ROOT"/game/*)"
DISC="$(cd "$(dirname "$DISC")" && pwd)/$(basename "$DISC")"
# Copies of the player's card and settings, so a run plays like theirs and
# never writes to them.
[ -d "$DATA/mc0" ] && cp -Rp "$DATA/mc0/." "$CARD/"
[ -f "$DATA/settings.ini" ] && cp "$DATA/settings.ini" "$OUT/settings.ini"
(cd "$OUT" && exec "$BIN" "$ELF" "$DISC" --hidden --mute --mc "$CARD" --settings "$OUT/settings.ini" \
  --shots "$OUT/frames" --shot-every "$EVERY") > "$OUT/run.log" 2>&1 &
PID=$!
RUNNER=$PID

for _ in $(seq 1 "$SECS"); do
  kill -0 "$RUNNER" 2>/dev/null || break
  sleep 1
done

if [ -n "$RUNNER" ] && kill -0 "$RUNNER" 2>/dev/null; then
  sample "$RUNNER" 3 -file "$OUT/stack.txt" > /dev/null 2>&1 || true
  kill "$RUNNER"
  state="alive at ${SECS}s"
else
  state="exited before ${SECS}s"
fi
wait "$PID" 2>/dev/null || true
PID=""
RUNNER=""

for shot in "$OUT"/frames/frame_??????.ppm; do
  [ -e "$shot" ] || continue
  magick "$shot" "$OUT/frames/frame_$(since "$shot")s.png"
  rm "$shot"
done

echo "$state, $(find "$OUT/frames" -type f | wc -l | tr -d ' ') frames, log $OUT/run.log"
if [ -f "$OUT/stack.txt" ]; then
  # Samples per recompiled function, callers counting their callees' time.
  echo "game functions by samples:"
  awk 'match($0, /[0-9]+ [A-Za-z_][A-Za-z0-9_]*_0x[0-9a-f]+/) {
         split(substr($0, RSTART, RLENGTH), f, " "); n[f[2]] += f[1] }
       END { for (k in n) printf "%7d  %s\n", n[k], k }' "$OUT/stack.txt" | sort -rn | head -8
fi
