#!/usr/bin/env bash
# Run scenarios side by side and say which passed.
#
#   scripts/scenarios.sh [-j 4] [--speed 4] [name ...]
#
# Names are files in scenarios/ without .dta; none means all of them. Each
# runs through arm.sh into runs/<name>. A run passes when it quits on its
# own with no FAIL or STALL in its log. Exits 1 if any failed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS=4
SPEED=4
NAMES=()
while [ $# -gt 0 ]; do
  case "$1" in
    -j)      JOBS="$2"; shift ;;
    --speed) SPEED="$2"; shift ;;
    -*) echo "unknown argument: $1" >&2; exit 2 ;;
    *)  NAMES+=("$1") ;;
  esac
  shift
done
if [ ${#NAMES[@]} -eq 0 ]; then
  for file in "$ROOT"/scenarios/*.dta; do
    NAMES+=("$(basename "$file" .dta)")
  done
fi

run() {
  local name="$1" out="$ROOT/runs/$1" start
  start=$(date +%s)
  "$ROOT/scripts/arm.sh" "$out" --scenario "$ROOT/scenarios/$name.dta" --speed "$SPEED" --secs 600 \
    > "$out.summary" 2>&1 || true
  local verdict=pass
  grep -q '^exited' "$out.summary" || verdict=FAIL
  grep -qE '\[scenario\] (FAIL|STALL)' "$out/run.log" 2> /dev/null && verdict=FAIL
  printf '%-4s %-16s %4ss  %s\n' "$verdict" "$name" "$(($(date +%s) - start))" \
    "$(grep -E '\[scenario\] (FAIL|STALL)' "$out/run.log" 2> /dev/null | head -1 | sed 's/^\[scenario\] //')"
  rm -f "$out.summary"
}
export -f run
export ROOT SPEED

mkdir -p "$ROOT/runs"
printf '%s\n' "${NAMES[@]}" | xargs -P "$JOBS" -I{} bash -c 'run "$@"' _ {} | tee "$ROOT/runs/scenarios.txt"
! grep -q '^FAIL' "$ROOT/runs/scenarios.txt"
