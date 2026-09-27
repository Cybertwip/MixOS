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
# --list names every family and its status without building anything.
listing="$("$ROOT/build-flashtools.sh" --list)" || { echo "FAIL: --list exits nonzero"; fail=1; }
for d in j36-ultra oppo-mt6877 lg-msm8917 oppo-mt6833 lg-mt6739; do
    printf '%s\n' "$listing" | grep -q "$d" \
        || { echo "FAIL: --list omits $d"; fail=1; }
done
printf '%s\n' "$listing" | grep -q "no LK sources" \
    || { echo "FAIL: --list hides the gap"; fail=1; }
for d in oppo-mt6833 lg-mt6739; do
    printf '%s\n' "$listing" | grep "$d" | grep -q "builds:" \
        || { echo "FAIL: --list does not build $d"; fail=1; }
done
echo "  --list matrix: ok"
# Families without sources fail loudly instead of building the wrong LK.
if "$ROOT/build-flashtools.sh" --device oppo-mt6877 >/dev/null 2>&1; then
    echo "FAIL: --device oppo-mt6877 built something"; fail=1
else
    echo "  --device oppo-mt6877: refused ok"
fi
# Phone LK families build ungated (building is harmless; flashing is the
# gated step, and it is manual). A real build is minutes + LLVM, so this
# stays static: both families must be in the default plan and routed
# through the per-device builder + verifier.
if ! grep -q 'DO_PHONES="oppo-mt6833 lg-mt6739"' "$ROOT/build-flashtools.sh"; then
    echo "FAIL: phone LKs missing from the default plan"; fail=1
else
    echo "  default plan builds phones: ok"
fi
if ! grep -q 'build_phone_lk "$matrix"' "$ROOT/build-flashtools.sh"; then
    echo "FAIL: phone builds bypass build_phone_lk"; fail=1
else
    echo "  phone builder wiring: ok"
fi
if ! grep -q 'lk.bin lk.elf FACTS.md build-info.txt' "$ROOT/build-flashtools.sh"; then
    echo "FAIL: phone per-variant check missing"; fail=1
else
    echo "  phone per-variant check: ok"
fi
# Power modes are j36-only: narrowing them for a phone must fail fast.
if "$ROOT/build-flashtools.sh" --device oppo-mt6833 --battery-only >/dev/null 2>&1; then
    echo "FAIL: --battery-only accepted for a phone"; fail=1
else
    echo "  phone mode guard: ok"
fi
if "$ROOT/build-flashtools.sh" --device bogus >/dev/null 2>&1; then
    echo "FAIL: --device bogus accepted"; fail=1
else
    echo "  --device bogus: refused ok"
fi
[ "$fail" -eq 0 ] && echo "PASS: flashtools matrix"
exit "$fail"
