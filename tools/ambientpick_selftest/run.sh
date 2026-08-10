#!/usr/bin/env bash
# Build and run the ambient pattern-selection self-test (no device needed).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/ambientpick_selftest"
mkdir -p "$out"
g++ -O2 -Wall -Wextra -std=c++17 -I "$here/../../src" -o "$out/selftest" "$here/selftest.cpp"
"$out/selftest"
