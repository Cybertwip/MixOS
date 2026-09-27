#!/usr/bin/env bash
# Build the J36 Ultra flash tooling: the LK images plus the flash CLI.
#
# This script builds nothing itself. It runs tools/mediatek/build.sh once per
# power mode -- battery, then without-battery -- which is what keeps it correct
# when the firmware gains flags: build.sh owns the table, this script only
# enumerates it.
#
# Usage: ./build-flashtools.sh [--battery-only | --without-battery] [--tests]
#
#   --battery-only     build only build/mediatek/j36-ultra/battery/boot
#   --without-battery  build only build/mediatek/j36-ultra/without-battery/boot
#   --tests            also run the flash-tool Go suite and the LK bootmenu C test
#
# After a build, flash from the matching boot dir, e.g.:
#   cd build/mediatek/j36-ultra/without-battery/boot
#   ./flash -root ./ -upload release -device /dev/cu.usbmodemXXXX -yes
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODES="battery without-battery"
RUN_TESTS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --battery-only) MODES="battery"; shift ;;
        --without-battery) MODES="without-battery"; shift ;;
        --tests) RUN_TESTS=1; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d'; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

export GOFLAGS="${GOFLAGS:--mod=mod}"
for mode in $MODES; do
    if [[ "$mode" == "without-battery" ]]; then
        "$ROOT/tools/mediatek/build.sh" --without-battery
    else
        "$ROOT/tools/mediatek/build.sh"
    fi
done

if [[ "$RUN_TESTS" == 1 ]]; then
    (cd "$ROOT/tools/mediatek/mvii-flash" && go test ./)
    cc -std=c99 -Wall -Wextra -Werror \
        "$ROOT/tools/mediatek/firmware/tests/test-bootmenu.c" \
        -o /tmp/j36-bootmenu-test && /tmp/j36-bootmenu-test
fi
