#!/usr/bin/env bash
# Convenience wrapper so the ESP-IDF build can be driven from git-bash.
# idf.py must be launched via cmd.exe (see scripts/idf.cmd for why).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cmd.exe //c "$(cygpath -w "$here/idf.cmd")" "$@"
