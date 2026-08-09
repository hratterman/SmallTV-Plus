#!/usr/bin/env bash
# Build and run the captive-portal form parser self-test (no device needed).
#
# src/CaptiveForm.h is deliberately free of I/O so it can be compiled straight
# against a small host String shim in the test itself. What is exercised is the
# part that fails silently on a device: which fields get carried, and what a
# relative form action resolves to.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/captive_selftest"
mkdir -p "$out"

# CaptiveForm.h includes Arduino.h for String; the test brings its own String,
# so the shim only needs to satisfy the include and the C library it pulls in.
mkdir -p "$out/shim"
cat > "$out/shim/Arduino.h" <<'SHIM'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
SHIM

g++ -O2 -Wall -Wextra -std=c++17 -I "$out/shim" -o "$out/selftest" "$here/selftest.cpp"
"$out/selftest"
