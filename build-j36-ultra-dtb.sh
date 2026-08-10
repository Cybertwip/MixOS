#!/usr/bin/env bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
# Generate and compile the J36 Ultra MT6592 bring-up DTB from the MVII board
# sources vendored under device/j36-ultra/mvii-board. Those are read-only inputs;
# generated files stay under device/j36-ultra/generated.
#
# This needs no PowerEngine checkout. The five files the generator parses are
# committed here, refreshed by device/j36-ultra/sync-mvii-board.sh when the MVII
# drivers move; point J36_DRIVERS_DIR at a live Drivers tree to build against it
# directly without vendoring first.

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
DRIVERS="${J36_DRIVERS_DIR:-$ROOT/device/j36-ultra/mvii-board}"
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
    echo "error: J36 Ultra board sources not found: $DRIVERS" >&2
    echo "these are committed; run device/j36-ultra/sync-mvii-board.sh to restore" >&2
    echo "them from a PowerEngine checkout, or set J36_DRIVERS_DIR" >&2
    exit 1
}

mkdir -p "$OUT_DIR"

# ── Stage once, restage on change ─────────────────────────────────────────────
#
# This ran on every invocation, including every J36 build that changed nothing but
# a kernel config symbol.  The generator is a pure function of the five board
# files plus its own source plus this script, so hash exactly those and skip when
# the hash and all three outputs are already in place.
#
# Hashed, not compared by mtime: an rsync into the VM or a git checkout rewrites
# mtimes wholesale, and mtime ordering would then rebuild on every sync while a
# hand-edited generator restored from a backup with an older mtime would silently
# not rebuild at all.  J36_FORCE_DTB=1 regenerates regardless.
STAMP="$OUT_DIR/.inputs.sha256"
if command -v sha256sum >/dev/null 2>&1; then
    hash_files() { sha256sum "$@"; }
else
    hash_files() { shasum -a 256 "$@"; }
fi
want_stamp="$(hash_files "$GENERATOR" "${BASH_SOURCE[0]}" "$DRIVERS"/* | awk '{print $1}' | hash_files - | awk '{print $1}')"

if [[ "${J36_FORCE_DTB:-0}" != 1 && -f "$STAMP" && -s "$DTS" && -s "$DTB" && -s "$ROUNDTRIP" ]] &&
   [[ "$(cat "$STAMP")" == "$want_stamp" ]]; then
    printf '%s\n' \
        "J36 Ultra bring-up DTB is up to date; nothing to regenerate." \
        "  DTB: $DTB" \
        "  Source: $DRIVERS" \
        "  Force a rebuild with J36_FORCE_DTB=1"
    exit 0
fi

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

# The keypad pad mux, checked against the values MVII's kpd_pads_apply() writes.
# Without these the block scans two rows against three columns and seven keys
# (VOL-, VOL+, SELECT, START, MENU, R2, A) are dead however right the keymap is,
# so a silent regression here looks like a keymap bug and gets chased in the
# wrong file. Pad 93 is the D-pad UP EINT and must stay mode 0.
kpd="$(fdtget -t u "$DTB" /soc/keypad@10011000 j36,kpd-strobe-pads)"
[[ "$kpd" == "74 1 92 1 11 3" ]] || { echo "error: KPD strobe pads are '$kpd'" >&2; exit 1; }
kpd="$(fdtget -t u "$DTB" /soc/keypad@10011000 j36,kpd-sense-pads)"
[[ "$kpd" == "75 1 167 1 168 1 12 3 2 6" ]] || { echo "error: KPD sense pads are '$kpd'" >&2; exit 1; }
kpd="$(fdtget -t u "$DTB" /soc/keypad@10011000 j36,kpd-reserved-pads)"
[[ "$kpd" == "93 0" ]] || { echo "error: KPD reserved pads are '$kpd'" >&2; exit 1; }

# Last, and only now: a stamp written before the assertions above would let a
# failed validation be skipped on the next run instead of failing again.
printf '%s' "$want_stamp" > "$STAMP"

printf '%s\n' \
    "J36 Ultra bring-up DTB built successfully." \
    "  DTS: $DTS" \
    "  DTB: $DTB" \
    "  Model: $model" \
    "  Panel: ${width}x${height}, ${records} records / ${bytes} bytes" \
    "  Keypad: 3 strobe + 5 sense pads muxed, pad 93 reserved for D-pad UP" \
    "  Source: $DRIVERS"
