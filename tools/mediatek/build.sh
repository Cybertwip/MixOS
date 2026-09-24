#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
WITHOUT_BATTERY=0
OUTPUT=""
JOBS="${BUILD_JOBS:-4}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --without-battery) WITHOUT_BATTERY=1; shift ;;
        --output) [[ $# -ge 2 ]] || { echo '--output needs a directory' >&2; exit 2; }; OUTPUT="$2"; shift 2 ;;
        -h|--help)
            echo 'Usage: tools/mediatek/build.sh [--without-battery] [--output DIR]'
            echo 'Builds lk.bin, lk-release.bin, MVIIFlash.bin, assets.bin and the flash CLI.'
            echo 'Requires LLVM (clang, ld.lld, llvm-objcopy), CMake, Python 3 and Go.'
            echo 'Set MVII_LLVM_ROOT to override LLVM discovery.'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
MODE=battery
[[ "$WITHOUT_BATTERY" == 0 ]] || MODE=without-battery
OUTPUT="${OUTPUT:-$ROOT/../../build/mediatek/j36-ultra/$MODE}"
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd -P)"
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
(
    cd "$ROOT"
    GOCACHE="${GOCACHE:-$OUTPUT/go-cache}" go build -o "$OUTPUT/boot/flash" ./mvii-flash
)
printf '\nJ36 firmware (%s): %s/boot\n' "$MODE" "$OUTPUT"
