#!/usr/bin/env bash
# GH2 disc -> generated C++ -> native binary. Options: tools/ghc.py.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/ghc.py" build "$@"
