#!/usr/bin/env bash
# Check the framing inside docs/public/tether.html against the C++ firmware's.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node "$here/selftest.mjs"
