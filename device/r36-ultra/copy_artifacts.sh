#!/usr/bin/env bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
# Copy finished artifacts into the artifact directory, skipping any that are
# already there unchanged.
#
# Usage: copy_artifacts.sh DEST_DIR SRC [SRC...]
#
# The artifact directory is a Multipass mount, so every byte written here
# crosses the host/VM boundary.  The R36 archive is 2.4 GiB across two volumes
# and the raw image is 8.3 GB, and they were being re-copied on every run --
# including every J36 run, which resumes the checkpointed R36 base and so
# reaches this step with an archive that has not changed since the last time.
# That is minutes of copying identical bytes standing between a build and a
# card, right where the flash-and-boot loop wants to be tight.
#
# Skipping is decided per file on size and mtime of the source, recorded in
# DEST_DIR/.copy-stamps when the copy succeeds, and only trusted when the
# destination still exists at the recorded size.  A stamp written for a copy
# that was interrupted therefore cannot make a truncated file look current.
# DARKOS_FORCE_ARTIFACT_COPY=1 copies regardless.

set -Eeuo pipefail

DEST="${1:?usage: copy_artifacts.sh DEST_DIR SRC [SRC...]}"
shift
[[ $# -gt 0 ]] || { echo "usage: copy_artifacts.sh DEST_DIR SRC [SRC...]" >&2; exit 2; }
[[ -d "$DEST" ]] || { echo "missing artifact directory: $DEST" >&2; exit 1; }

STAMP_DIR="$DEST/.copy-stamps"
mkdir -p "$STAMP_DIR"

human() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || printf '%s bytes' "$1"; }

copied=0 skipped=0 copied_bytes=0 skipped_bytes=0

for src in "$@"; do
    [[ -f "$src" ]] || { echo "missing artifact: $src" >&2; exit 1; }
    base="$(basename -- "$src")"
    size="$(stat -c '%s' -- "$src")"
    identity="$size $(stat -c '%Y' -- "$src")"
    stamp="$STAMP_DIR/$base"

    if [[ "${DARKOS_FORCE_ARTIFACT_COPY:-0}" != 1 && -f "$stamp" && -f "$DEST/$base" ]] &&
       [[ "$(cat -- "$stamp")" == "$identity" ]] &&
       [[ "$(stat -c '%s' -- "$DEST/$base")" == "$size" ]]; then
        printf '  unchanged  %s (%s)\n' "$base" "$(human "$size")"
        skipped=$((skipped + 1)); skipped_bytes=$((skipped_bytes + size))
        continue
    fi

    printf '  copying    %s (%s)\n' "$base" "$(human "$size")"
    cp -f -- "$src" "$DEST/$base"
    printf '%s' "$identity" > "$stamp"
    copied=$((copied + 1)); copied_bytes=$((copied_bytes + size))
done

printf '%d copied (%s), %d unchanged (%s)\n' \
    "$copied" "$(human "$copied_bytes")" "$skipped" "$(human "$skipped_bytes")"
if [[ "$skipped" -gt 0 ]]; then
    printf 'Force a re-copy with DARKOS_FORCE_ARTIFACT_COPY=1\n'
fi
