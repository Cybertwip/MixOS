#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# flashtools matrix test: build-flashtools.sh must build every LK variant
# (both power modes) and assert each variant's deliverables afterwards --
# narrowing the matrix or dropping the check silently un-builds an LK a
# developer is about to pick and flash.
set -u

ROOT="$(cd -- "$(dirname -- "$0")/../../.." && pwd)"
fail=0
if ! grep -q 'MODES="battery without-battery"' "$ROOT/build-flashtools.sh"; then
    echo "FAIL: power-mode matrix narrowed"; fail=1
else
    echo "  modes: battery + without-battery ok"
fi
if ! grep -q 'after build' "$ROOT/build-flashtools.sh"; then
    echo "FAIL: per-variant deliverable check missing"; fail=1
else
    echo "  per-variant check: ok"
fi
[ "$fail" -eq 0 ] && echo "PASS: flashtools matrix"
exit "$fail"
