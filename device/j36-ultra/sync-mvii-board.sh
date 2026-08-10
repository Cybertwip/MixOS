#!/usr/bin/env bash
# Refresh device/j36-ultra/mvii-board/ from a PowerEngine MVII checkout.
#
# THIS SCRIPT IS NOT PART OF THE BUILD.  Nothing in dArkOS runs it, and the build
# does not need PowerEngine to be present -- mvii-board/ is committed, and the DTB
# generator reads it.  Run this by hand when the MVII drivers change and the DTB
# should follow.
#
# What is vendored is exactly the five files generate_dts.py opens and no more:
# 237 KB out of a 2.9 MB, 113-file driver tree.  The board facts the device tree
# is built from -- register bases, the JD9365 init sequence, the keypad pad mux
# and keymap, the framebuffer geometry -- live in those five files, and the
# generator parses them and asserts on what it finds.  Copying the parsed values
# into a JSON instead would be smaller still, but it would move the numbers one
# copy further from the code that drives the hardware, and those assertions are
# the only thing that turns an MVII pad-mux change into a build failure here
# rather than seven dead keys on the device.
#
# Usage:
#   ./device/j36-ultra/sync-mvii-board.sh                     # find PowerEngine beside dArkOS
#   POWERENGINE_ROOT=/path/to/PowerEngine ./...sync-mvii-board.sh
#   J36_DRIVERS_DIR=/path/to/J36Ultra/Drivers ./...sync-mvii-board.sh

set -Eeuo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "$HERE/../.." && pwd -P)"
POWERENGINE_ROOT="${POWERENGINE_ROOT:-$(dirname "$ROOT")/PowerEngineV3/PowerEngine}"
DRIVERS="${J36_DRIVERS_DIR:-$POWERENGINE_ROOT/OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers}"
DEST="$HERE/mvii-board"

# Keep this list identical to SOURCE_FILES in generate_dts.py.  If the generator
# learns to read a sixth file, this script must learn it too, or the build will
# fail on a file that only exists in the PowerEngine tree.
FILES=(
    mt6592_board_j36.h
    mt6592_disp_hw.h
    panel_bringup.h
    dsi_drv.c
    mt6592_keys.c
)

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -d "$DRIVERS" ]] || die "MVII J36 Drivers not found: $DRIVERS
set POWERENGINE_ROOT or J36_DRIVERS_DIR. This script is the only thing in dArkOS
that needs a PowerEngine checkout; the build itself does not."

# macOS ships shasum, Linux ships sha256sum, and this runs on both.
if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
else
    die "neither sha256sum nor shasum is available"
fi

mkdir -p "$DEST"
changed=0
for f in "${FILES[@]}"; do
    [[ -f "$DRIVERS/$f" ]] || die "$f is missing from $DRIVERS"
    if [[ ! -f "$DEST/$f" ]] || ! cmp -s "$DRIVERS/$f" "$DEST/$f"; then
        changed=$((changed + 1))
        printf '  updated  %s\n' "$f"
    else
        printf '  same     %s\n' "$f"
    fi
    cp "$DRIVERS/$f" "$DEST/$f"
done

# The provenance record exists so that a stale vendored copy is a visible fact
# rather than a mystery: the commit these came from, and a hash per file that
# build-j36-ultra.sh re-checks whenever a PowerEngine checkout happens to be
# sitting next to dArkOS.
commit="$(git -C "$DRIVERS" rev-parse HEAD 2>/dev/null || echo unknown)"
describe="$(git -C "$DRIVERS" describe --always --dirty 2>/dev/null || echo unknown)"
{
    echo "# Vendored MVII J36 Ultra board sources."
    echo "#"
    echo "# Copied by device/j36-ultra/sync-mvii-board.sh; do not edit here.  These are"
    echo "# read-only inputs to generate_dts.py, which parses the board constants,"
    echo "# the JD9365 panel init table and the keypad pad mux out of them."
    echo "#"
    echo "# Upstream: PowerEngine OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers"
    echo "source_commit=$commit"
    echo "source_describe=$describe"
    for f in "${FILES[@]}"; do
        echo "$(sha256 "$DEST/$f")  $f"
    done
} > "$DEST/PROVENANCE.txt"

if (( changed == 0 )); then
    printf 'mvii-board/ already matched %s\n' "$DRIVERS"
else
    printf '%d file(s) refreshed from %s\n' "$changed" "$DRIVERS"
    printf 'Rebuild the DTB to pick them up: ./build-j36-ultra-dtb.sh\n'
fi
