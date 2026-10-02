#!/usr/bin/env bash
# Run the game in a window, building it from the disc the first time.
# Options: tools/ghc.py.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/ghc.py" play "$@"
