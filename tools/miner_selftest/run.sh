#!/usr/bin/env bash
# Build and run the miner job-math self-test on this machine (no device needed).
#
# The expected header and digest for the stratum-job case are computed here with
# Python's hashlib, so the C++ under test is checked against an implementation
# that shares no code with it.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${TMPDIR:-/tmp}/miner_selftest"
mkdir -p "$out"

# Host shims: the miner sources include Arduino.h and config.h, and mark the hot
# functions IRAM_ATTR/DRAM_ATTR. None of that means anything off-device.
mkdir -p "$out/shim"
cat > "$out/shim/Arduino.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
EOF
cat > "$out/shim/config.h" <<'EOF'
#pragma once
#define WITH_MINER 1
EOF

read -r -d '' PYGEN <<'PY' || true
import hashlib
def d(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()
version="20000000"
prev="000000000000000000024bead8df69990852c202db0e0097c1a12ea637d7e96d"
cb1="01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff20"
en1="1a2b3c4d"; en2="00000007"
cb2="ffffffff0100f2052a010000001976a914000000000000000000000000000000000000000088ac00000000"
ntime="5e0f1d20"; nbits="170f48e4"
branches=["57f0f8c8a1f3f9f0e7a54a5f5b1f0f3d2c9b8a7968574635241302f1e0d0c0b0",
          "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0"]

root=d(bytes.fromhex(cb1+en1+en2+cb2))
for b in branches:
    root=d(root+bytes.fromhex(b))

def swap_words(h):                      # reverse each 4-byte word
    b=bytearray.fromhex(h)
    for i in range(0,len(b),4):
        b[i:i+4]=b[i:i+4][::-1]
    return bytes(b)

header=swap_words(version)+swap_words(prev)+root+swap_words(ntime)+swap_words(nbits)
print((header+b"\x00\x00\x00\x00").hex())
print(d(header+(0x12345678).to_bytes(4,"little"))[::-1].hex())
PY

mapfile -t EXPECTED < <(python3 -c "$PYGEN")

g++ -O2 -Wall -Wno-unused-variable \
    -I "$out/shim" -I "$root/src/features/miner" \
    -o "$out/selftest" \
    "$here/selftest.cpp" \
    "$root/src/features/miner/MinerJob.cpp" \
    "$root/src/features/miner/NerdSha256.cpp"

"$out/selftest" "${EXPECTED[0]}" "${EXPECTED[1]}"
