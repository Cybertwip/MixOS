#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Build the mt68xx minimal LK for one device (a row in
# device/oppo-a77/devices.sh). Derived from tools/mediatek/mt65xx/build.sh: same
# host-side LLVM/CMake build, but no power modes (phones always have a
# battery) and --device is required (the codename is baked into the banner
# and build-info.txt, so a serial log names the image that printed it).
#
# Usage: tools/mediatek/mt68xx/build.sh --device cph2381 [--output DIR]
# Builds lk.bin, lk.elf, FACTS.md and build-info.txt under <output>/boot/.
# Requires LLVM (clang, ld.lld, llvm-objcopy), CMake and Python 3.
# Set MVII_LLVM_ROOT to override LLVM discovery.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$ROOT/../../.." && pwd -P)"
DEVICE=""
OUTPUT=""
JOBS="${BUILD_JOBS:-4}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --device) [[ $# -ge 2 ]] || { echo '--device needs a codename' >&2; exit 2; }; DEVICE="$2"; shift 2 ;;
        --output) [[ $# -ge 2 ]] || { echo '--output needs a directory' >&2; exit 2; }; OUTPUT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$DEVICE" ]] || { echo 'error: --device is required (e.g. --device cph2381)' >&2; exit 2; }
# The device must be a real row: a typo here would bake the wrong name into
# the banner and the boot dir. mt68xx serves the oppo-a77 tree today; a second
# mt68xx device tree adds its loader next to this one.
# shellcheck source=device/oppo-a77/devices.sh
. "$REPO_ROOT/device/oppo-a77/devices.sh"
a77_device_info "$DEVICE" >/dev/null || exit 1
OUTPUT="${OUTPUT:-$REPO_ROOT/build/mt68xx/$DEVICE}"
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd -P)"
# A moved source tree invalidates the CMake cache (the cache pins the source
# dir it was generated from): detect the mismatch and reconfigure from
# clean instead of failing on it.
FIRMWARE_DIR="$ROOT/firmware"
CACHE_HOME="$(grep -m1 '^CMAKE_HOME_DIRECTORY:' "$OUTPUT/obj/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
if [[ -n "$CACHE_HOME" && "$CACHE_HOME" != "$FIRMWARE_DIR" ]]; then
    echo "NOTE: source tree moved ($CACHE_HOME -> $FIRMWARE_DIR); reconfiguring from clean."
    rm -rf "$OUTPUT/obj"
fi
LLVM_ROOT="${MVII_LLVM_ROOT:-}"
if [[ -z "$LLVM_ROOT" ]]; then
    for candidate in /opt/homebrew/opt/llvm /usr/local/opt/llvm /usr/lib/llvm-{22,21,20,19,18}; do
        if [[ -x "$candidate/bin/clang" ]]; then LLVM_ROOT="$candidate"; break; fi
    done
fi
[[ -x "$LLVM_ROOT/bin/clang" ]] || { echo 'Set MVII_LLVM_ROOT to an LLVM installation containing bin/clang.' >&2; exit 1; }
COMMIT="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo nogit)"
cmake -S "$ROOT/firmware" -B "$OUTPUT/obj" \
    -DMVII_LLVM_ROOT="$LLVM_ROOT" -DMVII_PACKAGE_ROOT="$OUTPUT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMT68XX_DEVICE="$DEVICE" -DMVII_BUILD_COMMIT="$COMMIT"
cmake --build "$OUTPUT/obj" --parallel "$JOBS"
printf '\nmt68xx LK (%s): %s/boot\n' "$DEVICE" "$OUTPUT"
