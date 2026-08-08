#!/usr/bin/env bash
# Build and run the TinyLlm self-test on this machine (no device needed).
#
# The expected text comes from a Python implementation of the .tll format that
# shares no code with the C++ under test — same file, two independent readers,
# and they have to agree token for token. That is what catches a wrong tensor
# offset, which otherwise still produces fluent-looking nonsense.
#
# Needs a model: pass one, or let it build story.tll from the upstream
# checkpoint (a download the first time).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${TMPDIR:-/tmp}/story_selftest"
mkdir -p "$out"

model="${1:-$out/story.tll}"
if [ ! -f "$model" ]; then
  echo "building $model from the upstream checkpoint..."
  python3 "$root/tools/tinyllm_export.py" --out "$model"
fi

# Independent decode of the same file, in Python.
TOKENS=40
expected="$(python3 "$here/reference.py" "$model" $TOKENS)"
echo "reference: \"$expected\""

c++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -o "$out/selftest" "$here/selftest.cpp" "$root/src/features/story/TinyLlm.cpp" -lm

"$out/selftest" "$model" $TOKENS "$expected"
