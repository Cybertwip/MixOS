#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/j36-ultra/LICENSE for the texts and for what they do not cover.
# Build everything: the J36 Ultra handheld layer, every OPPO phone in
# device/oppo-mt6877/devices.sh, and every LG phone in
# device/lg-k20plus/devices.sh.
#
# This script builds nothing itself. It runs the three family wrappers in
# sequence -- ./build-j36-ultra.sh, ./build-oppo.sh, ./build-lg.sh -- which
# is what keeps it correct when devices are added: the families own their
# tables, this script only enumerates them. New OPPO or LG device rows are
# picked up with no edits here.
#
# The two bring-up scaffolds (device/oppo-a77, device/lg-k20) are wired
# but OPT-IN ONLY (--only oppo-a77 / --only lg-k20): they build images
# that will not boot yet, and their ACK gates would fail a default run.
# Promotion into the default plan is each tree's BRINGUP step 6.
#
# Sequential, not parallel: all families share the one Multipass VM,
# and the VM refuses concurrent builds on purpose.

if [ -z "${BASH_VERSION:-}" ] || [ -z "${MIXOS_SNAPSHOT:-}" ]; then
    MIXOS_ROOT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)" || exit 1
    MIXOS_SNAPSHOT="$(mktemp "${TMPDIR:-/tmp}/build-mixos.XXXXXX")" || exit 1
    cat -- "$0" > "$MIXOS_SNAPSHOT" || { rm -f -- "$MIXOS_SNAPSHOT"; exit 1; }
    export MIXOS_ROOT_DIR MIXOS_SNAPSHOT
    exec bash "$MIXOS_SNAPSHOT" "$@"
fi

set -Euo pipefail

trap 'case "${MIXOS_SNAPSHOT:-}" in */build-mixos.??????) rm -f -- "$MIXOS_SNAPSHOT" ;; esac' EXIT

ROOT="${MIXOS_ROOT_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)}"
DARKOS_LOG_TAG="build-mixos"
# shellcheck source=device/common/multipass.sh
. "$ROOT/device/common/multipass.sh"
# shellcheck source=device/oppo-mt6877/devices.sh
. "$ROOT/device/oppo-mt6877/devices.sh"
# shellcheck source=device/lg-k20plus/devices.sh
. "$ROOT/device/lg-k20plus/devices.sh"
# shellcheck source=device/oppo-a77/devices.sh
. "$ROOT/device/oppo-a77/devices.sh"
# shellcheck source=device/lg-k20/devices.sh
. "$ROOT/device/lg-k20/devices.sh"
# shellcheck source=device/oppo-a77-4g/devices.sh
. "$ROOT/device/oppo-a77-4g/devices.sh"

MIX_ONLY=0
COMPRESS=0
SKIP_J36=0
SKIP_OPPO=0
SKIP_LG=0
ONLY=""

usage() {
    cat <<USAGE
Usage: ./build-mixos.sh [--mix-only | --compress] [--skip-j36] [--skip-oppo] [--skip-lg]
                        [--only j36|oppo|lg|oppo-a77|lg-k20|oppo-a77-4g] [--list]

Builds all families in sequence: J36 Ultra, then every OPPO device, then
every LG device. Each family runs in the shared Multipass VM with its own
work directory, so a finished family is checkpointed and a re-run rebuilds
only what changed.

    ./build-mixos.sh --list        show the build plan without building
    ./build-mixos.sh --mix-only    board specifics only, per family
    ./build-mixos.sh --only oppo   just the OPPO family (all its devices)

Bring-up scaffolds are opt-in only (never in the default plan):
    ./build-mixos.sh --only oppo-a77     needs OPPO_A77_BRINGUP_ACK=1
    ./build-mixos.sh --only lg-k20       needs LG_K20_BRINGUP_ACK=1
    ./build-mixos.sh --only oppo-a77-4g  needs OPPO_A77_4G_BRINGUP_ACK=1

Anything the family wrappers honour (OPPO_DEVICE is NOT honoured here --
this script enumerates devices itself; OPPO_FIRMWARE_DIR, LG_FIRMWARE_DIR,
BUILD_JOBS, ...) passes through to them.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --mix-only) MIX_ONLY=1; shift ;;
        --compress) COMPRESS=1; shift ;;
        --skip-j36) SKIP_J36=1; shift ;;
        --skip-oppo) SKIP_OPPO=1; shift ;;
        --skip-lg) SKIP_LG=1; shift ;;
        --only) ONLY="$2"; shift 2 ;;
        --list) ONLY="list"; shift ;;
        *) usage >&2; exit 2 ;;
    esac
done

if [[ "$MIX_ONLY" == 1 && "$COMPRESS" == 1 ]]; then
    darkos_die "--compress cannot be combined with --mix-only."
fi
case "$ONLY" in
    ""|j36|oppo|lg|oppo-a77|lg-k20|oppo-a77-4g|list) ;;
    *) darkos_die "--only takes j36, oppo, lg, oppo-a77, lg-k20 or oppo-a77-4g." ;;
esac

# The plan: (label, command...). OPPO_DEVICE/LG_DEVICE are set per device so
# a value leaked from the environment cannot silently rebuild one device
# under every name.
PLAN_LABELS=()
PLAN_CMDS=()
EXTRA=()
[[ "$MIX_ONLY" == 1 ]] && EXTRA+=(--mix-only)
[[ "$COMPRESS" == 1 ]] && EXTRA+=(--compress)

if [[ "$ONLY" == "" || "$ONLY" == j36 ]] && [[ "$SKIP_J36" == 0 ]]; then
    PLAN_LABELS+=("j36-ultra")
    PLAN_CMDS+=("$ROOT/build-j36-ultra.sh ${EXTRA[*]}")
fi
if [[ "$ONLY" == "" || "$ONLY" == oppo ]] && [[ "$SKIP_OPPO" == 0 ]]; then
    while read -r dev; do
        [[ -n "$dev" ]] || continue
        PLAN_LABELS+=("oppo:$dev")
        PLAN_CMDS+=("OPPO_DEVICE=$dev $ROOT/build-oppo.sh ${EXTRA[*]}")
    done < <(oppo_devices)
fi
if [[ "$ONLY" == "" || "$ONLY" == lg ]] && [[ "$SKIP_LG" == 0 ]]; then
    while read -r dev; do
        [[ -n "$dev" ]] || continue
        PLAN_LABELS+=("lg:$dev")
        PLAN_CMDS+=("LG_DEVICE=$dev $ROOT/build-lg.sh ${EXTRA[*]}")
    done < <(lg_devices)
fi
# Bring-up scaffolds: explicit --only, never the default plan (their ACK
# gates would fail it, and their images do not boot yet).
if [[ "$ONLY" == oppo-a77 ]]; then
    while read -r dev; do
        [[ -n "$dev" ]] || continue
        PLAN_LABELS+=("oppo-a77:$dev")
        PLAN_CMDS+=("A77_DEVICE=$dev $ROOT/build-oppo-a77.sh ${EXTRA[*]}")
    done < <(a77_devices)
fi
if [[ "$ONLY" == lg-k20 ]]; then
    while read -r dev; do
        [[ -n "$dev" ]] || continue
        PLAN_LABELS+=("lg-k20:$dev")
        PLAN_CMDS+=("K20_DEVICE=$dev $ROOT/build-lg-k20.sh ${EXTRA[*]}")
    done < <(k20_devices)
fi
if [[ "$ONLY" == oppo-a77-4g ]]; then
    while read -r dev; do
        [[ -n "$dev" ]] || continue
        PLAN_LABELS+=("oppo-a77-4g:$dev")
        PLAN_CMDS+=("A77_4G_DEVICE=$dev $ROOT/build-oppo-a77-4g.sh ${EXTRA[*]}")
    done < <(a77_4g_devices)
fi

if [[ "$ONLY" == list ]]; then
    echo "j36-ultra"
    oppo_devices | sed 's/^/oppo:/'
    lg_devices | sed 's/^/lg:/'
    echo "# bring-up scaffolds (opt-in only, never in the default plan):"
    a77_devices | sed 's/^/oppo-a77:/'
    k20_devices | sed 's/^/lg-k20:/'
    a77_4g_devices | sed 's/^/oppo-a77-4g:/'
    exit 0
fi

if [[ ${#PLAN_LABELS[@]} -eq 0 ]]; then
    darkos_die "nothing to build (all families skipped?)"
fi

darkos_log "MixOS full build: ${#PLAN_LABELS[@]} target(s): ${PLAN_LABELS[*]}"
declare -a RESULTS=()
FAILED=0
START_ALL=$SECONDS
for i in "${!PLAN_LABELS[@]}"; do
    label="${PLAN_LABELS[$i]}"
    START=$SECONDS
    darkos_log "[$((i+1))/${#PLAN_LABELS[@]}] $label ..."
    # The command strings above are built from this file's own constants
    # plus device names out of devices.sh; no user input reaches them.
    if bash -c "${PLAN_CMDS[$i]}"; then
        RESULTS+=("$label OK $((SECONDS-START))s")
    else
        RESULTS+=("$label FAILED $((SECONDS-START))s")
        FAILED=1
    fi
done

echo
darkos_log "MixOS build finished in $((SECONDS-START_ALL))s"
for r in "${RESULTS[@]}"; do
    printf '  %s\n' "$r"
done
if [[ "$FAILED" != 0 ]]; then
    darkos_die "one or more families failed; the failures are above."
fi
