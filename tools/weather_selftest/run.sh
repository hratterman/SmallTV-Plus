#!/usr/bin/env bash
# Build and run the weather-logic self-test (no device needed).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/weather_selftest"
mkdir -p "$out"
g++ -O2 -Wall -Wextra -std=c++17 -o "$out/selftest" "$here/selftest.cpp"
"$out/selftest"
