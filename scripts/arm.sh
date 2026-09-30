#!/usr/bin/env bash
# Run the game once, unattended, and keep what it did.
#
#   scripts/arm.sh <out> [--secs 30] [--disc <image>] [--shot-every 60] [--ref]
#
# The window stays hidden, no audio device opens, the memory card is a
# throwaway copy of ghpc's (both arms boot from the same save), and only the
# process this script started is ever killed.
# <out> gets run.log, frames/ (the guest picture every --shot-every presented
# frames) and stack.txt, a 3s sample taken just before the stop. The summary
# names where the game thread spends its time, by recompiled function.
#
# --ref runs ../ghpc instead, the reference: the debug ELF through play.sh,
# the software picture presented through Vulkan. It refuses to start while
# any ghpc runner is up, since that is someone playing. play.sh boot-skips
# to main_screen for scripted runs; --intro boots the reference through the
# title like ours.
#
# Frames from both arms are 640x448 PNGs named by seconds since launch
# (frame_0012.3s.png, the reference adding its screen name), since the two
# present at different rates and frame counts do not line up. The reference
# shoots its whole window, so its shots are cropped to the letterbox and
# scaled back to the guest size.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GHPC="$(cd "$ROOT/../ghpc" 2>/dev/null && pwd || true)"
OUT=""
SECS=30
DISC=""
EVERY=60
REF=0
INTRO=()
while [ $# -gt 0 ]; do
  case "$1" in
    --secs)       SECS="$2"; shift ;;
    --disc)       DISC="$2"; shift ;;
    --shot-every) EVERY="$2"; shift ;;
    --ref)        REF=1 ;;
    --intro)      INTRO=(--intro) ;;
    -*) echo "unknown argument: $1" >&2; exit 2 ;;
    *)  OUT="$1" ;;
  esac
  shift
done
[ -n "$OUT" ] || { echo "usage: scripts/arm.sh <out> [--secs N] [--disc image] [--shot-every N] [--ref]" >&2; exit 2; }

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

# Both arms start from a copy of ghpc's card, so they boot from the same save.
[ -n "$GHPC" ] && [ -d "$GHPC/work/mc0" ] && cp -Rp "$GHPC/work/mc0/." "$CARD/"

if [ "$REF" = 1 ]; then
  [ -n "$GHPC" ] || { echo "no ../ghpc beside this repo" >&2; exit 1; }
  if pgrep -x ps2EntryRunner > /dev/null; then
    echo "a ghpc runner is already up; not starting beside it" >&2
    exit 1
  fi
  find /tmp/ -maxdepth 1 -name 'ghpc_vk_*' -delete
  GHPC_HIDE_WINDOW=1 GHPC_AUDIO=0 GHPC_NO_FOCUS=1 GHPC_MC_ROOT="$CARD" \
    GHPC_RENDERER=vulkan GHPC_VK_SHOT="$EVERY" GHPC_VK_SHOT_LIMIT=100000 \
    "$GHPC/ghpc/scripts/play.sh" ${INTRO[@]+"${INTRO[@]}"} --log "$OUT/run.log" > /dev/null 2>&1 &
  PID=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    RUNNER="$(pgrep -x ps2EntryRunner -P "$PID" || true)"
    [ -n "$RUNNER" ] && break
    sleep 0.5
  done
else
  BIN="$ROOT/build/game/ghrecomp"
  ELF="$ROOT/build/recomp/gh2.elf"
  [ -x "$BIN" ] && [ -f "$ELF" ] || { echo "no build; run scripts/build.sh" >&2; exit 1; }
  [ -n "$DISC" ] || DISC="$(python3 "$ROOT/tools/disc.py" find SLUS-21447 "$ROOT"/game/*)"
  DISC="$(cd "$(dirname "$DISC")" && pwd)/$(basename "$DISC")"
  # A copy of the player's settings, so a run plays like theirs and never
  # writes to them.
  USER_SETTINGS="$HOME/Library/Application Support/ghrecomp/settings.ini"
  [ -f "$USER_SETTINGS" ] && cp "$USER_SETTINGS" "$OUT/settings.ini"
  (cd "$OUT" && exec "$BIN" "$ELF" "$DISC" --hidden --mute --mc "$CARD" --settings "$OUT/settings.ini" \
    --shots "$OUT/frames" --shot-every "$EVERY") > "$OUT/run.log" 2>&1 &
  PID=$!
  RUNNER=$PID
fi

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

if [ "$REF" = 1 ]; then
  # The letterbox is the tallest picture any shot trims to. One shot alone
  # is not enough: a dark loading screen trims into its own content.
  box="$(find /tmp/ -maxdepth 1 -name 'ghpc_vk_*.ppm' \
           -exec magick {} -define trim:edges=north,south -format '%@\n' info: \; 2>/dev/null |
         awk -F'[x+]' '$2 > h { w = $1; h = $2 } END { if (h > 0) printf "%dx%d+0+0", w, h }')"
  find /tmp/ -maxdepth 1 -name 'ghpc_vk_*.ppm' | while read -r shot; do
    screen="$(basename "$shot" .ppm | sed -E 's/^ghpc_vk_[0-9]+_//')"
    magick "$shot" ${box:+-gravity center -crop "$box" +repage} -resize '640x448!' \
      "$OUT/frames/frame_$(since "$shot")s_$screen.png"
    rm "$shot"
  done
else
  for shot in "$OUT"/frames/frame_??????.ppm; do
    [ -e "$shot" ] || continue
    magick "$shot" "$OUT/frames/frame_$(since "$shot")s.png"
    rm "$shot"
  done
fi

echo "$state, $(find "$OUT/frames" -type f | wc -l | tr -d ' ') frames, log $OUT/run.log"
if [ -f "$OUT/stack.txt" ]; then
  # Samples per recompiled function, callers counting their callees' time.
  echo "game functions by samples:"
  awk 'match($0, /[0-9]+ [A-Za-z_][A-Za-z0-9_]*_0x[0-9a-f]+/) {
         split(substr($0, RSTART, RLENGTH), f, " "); n[f[2]] += f[1] }
       END { for (k in n) printf "%7d  %s\n", n[k], k }' "$OUT/stack.txt" | sort -rn | head -8
fi
