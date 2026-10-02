#!/usr/bin/env bash
# Run the game once, unattended, and keep what it did. Options: tools/ghc.py.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/ghc.py" arm "$@"
