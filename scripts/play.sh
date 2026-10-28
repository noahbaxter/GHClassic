#!/usr/bin/env bash
# Run the game in a window, building it from the disc the first time.
# A first argument naming a worktree in .worktrees/ runs that one's build
# instead: scripts/play.sh experimental --cheats. Options: tools/ghc.py.
# sandbox after it opens the hub's guitarist sandbox instead (ghc.py sandbox).
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -n "$1" ] && [ -d "$root/.worktrees/$1" ]; then
    root="$root/.worktrees/$1"
    shift
fi
if [ "$1" = sandbox ]; then
    shift
    exec python3 "$root/tools/ghc.py" sandbox "$@"
fi
exec python3 "$root/tools/ghc.py" play "$@"
