#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# reference/ isolation test: the vendor reference kernels are host-side
# reading material only, excluded from the VM sync. No wrapper, in-VM
# script, generator, shared helper or device table may read them at
# build time; the one legitimate mention is the documented exclusion in
# device/common/multipass.sh.
set -u

ROOT="$(cd -- "$(dirname -- "$0")/../../.." && pwd)"
hits="$(grep -rn "reference/" "$ROOT"/build-*.sh "$ROOT"/device/*/build-in-vm.sh \
    "$ROOT"/device/common/*.sh "$ROOT"/device/common/*.py \
    "$ROOT"/device/*/generate*.py "$ROOT"/device/*/devices.sh 2>/dev/null \
    | grep -v "device/common/multipass.sh" || true)"
if [ -n "$hits" ]; then
    echo "FAIL: build-time reads of reference/:"
    printf '%s\n' "$hits"
    exit 1
fi
echo "PASS: no build-time reads of reference/"
