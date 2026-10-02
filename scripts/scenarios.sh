#!/usr/bin/env bash
# Run scenarios side by side and say which passed. Options: tools/ghc.py.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/ghc.py" scenarios "$@"
