#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$ROOT/../../.." && pwd -P)"
WITHOUT_BATTERY=0
DEVICE="j36-ultra"
OUTPUT=""
JOBS="${BUILD_JOBS:-4}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --without-battery) WITHOUT_BATTERY=1; shift ;;
        --device) [[ $# -ge 2 ]] || { echo '--device needs a codename' >&2; exit 2; }; DEVICE="$2"; shift 2 ;;
        --output) [[ $# -ge 2 ]] || { echo '--output needs a directory' >&2; exit 2; }; OUTPUT="$2"; shift 2 ;;
        -h|--help)
            echo 'Usage: tools/mediatek/mt65xx/build.sh [--device j36-ultra] [--without-battery] [--output DIR]'
            echo 'Builds lk.bin, lk-release.bin, MVIIFlash.bin and assets.bin.'
            echo 'The flash CLI is build/flash (built by build-flashtools.sh, above the device split).'
            echo 'Requires LLVM (clang, ld.lld, llvm-objcopy), CMake and Python 3.'
            echo 'Set MVII_LLVM_ROOT to override LLVM discovery.'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ "$DEVICE" == j36-ultra ]] || { echo "unknown mt65xx device '$DEVICE' (only j36-ultra)" >&2; exit 2; }
MODE=battery
[[ "$WITHOUT_BATTERY" == 0 ]] || MODE=without-battery
OUTPUT="${OUTPUT:-$REPO_ROOT/build/mt65xx/j36-ultra/$MODE}"
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd -P)"
# The CLI moved to build/flash: remove the per-mode copy this script used to
# build here. Stale copies predate the phone-root guard, so leaving them
# would leave the j36-DA-against-phones footgun behind.
rm -f "$OUTPUT/boot/flash"
LLVM_ROOT="${MVII_LLVM_ROOT:-}"
if [[ -z "$LLVM_ROOT" ]]; then
    for candidate in /opt/homebrew/opt/llvm /usr/local/opt/llvm /usr/lib/llvm-{22,21,20,19,18}; do
        if [[ -x "$candidate/bin/clang" ]]; then LLVM_ROOT="$candidate"; break; fi
    done
fi
[[ -x "$LLVM_ROOT/bin/clang" ]] || { echo 'Set MVII_LLVM_ROOT to an LLVM installation containing bin/clang.' >&2; exit 1; }
cmake -S "$ROOT/firmware" -B "$OUTPUT/obj" \
    -DMVII_LLVM_ROOT="$LLVM_ROOT" -DMVII_PACKAGE_ROOT="$OUTPUT" \
    -DCMAKE_BUILD_TYPE=Release -DJ36_WITHOUT_BATTERY="$WITHOUT_BATTERY"
cmake --build "$OUTPUT/obj" --parallel "$JOBS"
printf '\nJ36 firmware (%s): %s/boot\n' "$MODE" "$OUTPUT"
