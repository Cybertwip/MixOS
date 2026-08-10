#!/usr/bin/env bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
# Test a split .7z image archive, at most once per version of that archive.
#
# Usage: verify_archive.sh IMAGE_PATH STATE_DIR
#
# `7z t' on a 2.4 GiB LZMA2 archive decompresses all 7.7 GiB of it to check the
# CRCs. It takes minutes, it is single-threaded through the solid block, and it was
# being run TWICE per build from two different places: once by
# device/r36-ultra/build-in-vm.sh when it finds a completed archive and stops, and
# again by the artifact-copy step in build-r36-ultra.sh. A J36 build resumes the
# checkpointed R36 base, so both ran again on every J36 iteration, re-testing bytes
# that had not changed since the last time -- twenty-odd minutes of watching a
# progress counter crawl to 49% before anything J36-specific started.
#
# The check itself is worth keeping: this archive is the released artifact, and a
# truncated volume or a bad block should be caught here rather than by whoever
# unpacks it. So cache the result instead of dropping it. The stamp records name,
# size and mtime of every volume, which is what changes when create_image.sh
# rewrites them; DARKOS_FORCE_ARCHIVE_VERIFY=1 re-tests regardless.

set -Eeuo pipefail

IMAGE="${1:?usage: verify_archive.sh IMAGE_PATH STATE_DIR}"
STATE_DIR="${2:?usage: verify_archive.sh IMAGE_PATH STATE_DIR}"
STAMP="$STATE_DIR/archive-verified"

[[ -f "${IMAGE}.7z.001" ]] || { echo "missing archive: ${IMAGE}.7z.001" >&2; exit 1; }

# Sorted so volume enumeration order cannot make an unchanged archive look
# different, and mtime in epoch seconds so no locale or timezone gets involved.
identity="$(stat -c '%n %s %Y' "${IMAGE}.7z."* | sort)"

if [[ "${DARKOS_FORCE_ARCHIVE_VERIFY:-0}" != 1 && -f "$STAMP" ]] &&
   [[ "$(cat "$STAMP")" == "$identity" ]]; then
    printf '%s\n' \
        "Archive already verified; skipping the 7z test." \
        "  $(basename "${IMAGE}").7z.* ($(printf '%s\n' "$identity" | wc -l | tr -d ' ') volume(s))" \
        "  Re-test with DARKOS_FORCE_ARCHIVE_VERIFY=1"
    exit 0
fi

7z t "${IMAGE}.7z.001"

mkdir -p "$STATE_DIR"
printf '%s' "$identity" > "$STAMP"
