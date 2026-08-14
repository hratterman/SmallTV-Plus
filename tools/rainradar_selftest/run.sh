#!/usr/bin/env bash
# Build and run the rain-radar logic self-test (no device needed).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/rainradar_selftest"
mkdir -p "$out"
for c in tinflate tinfzlib adler32 crc32; do
  gcc -O2 -c -o "$out/$c.o" "$here/../../src/vendor/uzlib/$c.c"
done
g++ -O2 -Wall -Wextra -std=c++17 -o "$out/selftest" "$here/selftest.cpp" \
    "$out"/tinflate.o "$out"/tinfzlib.o "$out"/adler32.o "$out"/crc32.o -lz
RR_FIXTURE="$here/fixture_storm.png" "$out/selftest"
