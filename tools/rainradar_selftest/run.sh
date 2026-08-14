#!/usr/bin/env bash
# Build and run the rain-radar logic self-test (no device needed).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/rainradar_selftest"
mkdir -p "$out"
g++ -O2 -Wall -Wextra -std=c++17 -o "$out/selftest" "$here/selftest.cpp" -lz
RR_FIXTURE="$here/fixture_storm.png" "$out/selftest"
