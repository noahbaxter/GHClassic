#!/usr/bin/env bash
# Record a scenario as video and shots into review/<branch>/<date>_<scenario>/:
# scripts/record.sh <scenario name or path>. Options: tools/ghc.py.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/ghc.py" record "$@"
