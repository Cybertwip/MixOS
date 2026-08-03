#!/usr/bin/env bash
# Generate and compile the J36 Ultra MT6592 bring-up DTB directly from the
# PowerEngine MVII J36Ultra Drivers tree. Existing dArkOS/PowerEngine sources
# are read-only inputs; generated files stay under device/j36-ultra/generated.

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
POWERENGINE_ROOT="${POWERENGINE_ROOT:-$(dirname "$ROOT")/PowerEngineV3/PowerEngine}"
DRIVERS="${J36_DRIVERS_DIR:-$POWERENGINE_ROOT/OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers}"
OUT_DIR="${J36_DTB_OUT_DIR:-$ROOT/device/j36-ultra/generated}"
DTS="$OUT_DIR/mt6592-j36-ultra.dts"
DTB="$OUT_DIR/mt6592-j36-ultra.dtb"
ROUNDTRIP="$OUT_DIR/mt6592-j36-ultra.roundtrip.dts"
GENERATOR="$ROOT/device/j36-ultra/generate_dts.py"

for tool in python3 dtc fdtget; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: required tool not found: $tool" >&2
        exit 1
    }
done

[[ -d "$DRIVERS" ]] || {
    echo "error: J36 Ultra Drivers directory not found: $DRIVERS" >&2
    echo "set POWERENGINE_ROOT or J36_DRIVERS_DIR to override it" >&2
    exit 1
}

mkdir -p "$OUT_DIR"
python3 "$GENERATOR" --drivers "$DRIVERS" --output "$DTS"
dtc -Wno-unit_address_vs_reg -I dts -O dtb -o "$DTB" "$DTS"
dtc -I dtb -O dts -o "$ROUNDTRIP" "$DTB"

model="$(fdtget -t s "$DTB" / model)"
width="$(fdtget -t u "$DTB" /soc/dsi@1400c000/panel@0 hactive)"
height="$(fdtget -t u "$DTB" /soc/dsi@1400c000/panel@0 vactive)"
records="$(fdtget -t u "$DTB" /soc/dsi@1400c000/panel@0 mediatek,panel-init-record-count)"
bytes="$(fdtget -t bx "$DTB" /soc/dsi@1400c000/panel@0 panel-init-sequence | wc -w | tr -d ' ')"

[[ "$model" == "J36 Ultra (MediaTek MT6592)" ]] || { echo "error: model validation failed" >&2; exit 1; }
[[ "$width" == "640" && "$height" == "480" ]] || { echo "error: panel geometry validation failed" >&2; exit 1; }
[[ "$records" == "155" && "$bytes" == "620" ]] || { echo "error: JD9365 table validation failed" >&2; exit 1; }

printf '%s\n' \
    "J36 Ultra bring-up DTB built successfully." \
    "  DTS: $DTS" \
    "  DTB: $DTB" \
    "  Model: $model" \
    "  Panel: ${width}x${height}, ${records} records / ${bytes} bytes" \
    "  Source: $DRIVERS"
