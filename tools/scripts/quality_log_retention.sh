#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Bounded retention for unattended background-quality logs.  Only the four
# dated lane-log families are in scope; status JSON, fuzz artifacts, isolated
# checkouts, and node/mint data are never touched.

set -euo pipefail

LANE="${1:-}"
STATE_ROOT="${ZCL_QUALITY_STATE_DIR:-${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/zclassic23-quality}"
LOG_DIR="$STATE_ROOT/logs"
KEEP="${ZCL_QUALITY_LOG_KEEP:-8}"

usage() {
    echo "usage: quality_log_retention.sh <fuzz|tests|coverage|simnet-nightly|all>" >&2
}

if ! [[ "$KEEP" =~ ^[0-9]+$ ]] || [ "$KEEP" -lt 1 ] || [ "$KEEP" -gt 64 ]; then
    echo "quality-log-retention: invalid ZCL_QUALITY_LOG_KEEP=$KEEP (expected 1..64)" >&2
    exit 64
fi

if [ ! -d "$LOG_DIR" ]; then
    exit 0
fi

prune_lane() {
    local lane="$1"
    local -a matches=() files=()
    local oldest_idx i file deleted=0

    shopt -s nullglob
    case "$lane" in
        fuzz) matches=("$LOG_DIR"/fuzz-*.log) ;;
        tests) matches=("$LOG_DIR"/tests-*.log) ;;
        coverage) matches=("$LOG_DIR"/coverage-*.log) ;;
        simnet-nightly) matches=("$LOG_DIR"/simnet-nightly-*.log) ;;
        *)
            echo "quality-log-retention: unsupported lane=$lane" >&2
            return 64
            ;;
    esac
    shopt -u nullglob

    for file in "${matches[@]}"; do
        [ -f "$file" ] && [ ! -L "$file" ] && files+=("$file")
    done

    # Repeatedly remove the oldest regular file.  The service manager keeps a
    # lane singleton, so the newest file is the current/latest verdict log.
    # Symlinks and non-regular entries were filtered above and are untouched.
    while [ "${#files[@]}" -gt "$KEEP" ]; do
        oldest_idx=-1
        for i in "${!files[@]}"; do
            if [ "$oldest_idx" -lt 0 ] \
                || [ "${files[$i]}" -ot "${files[$oldest_idx]}" ] \
                || { [ ! "${files[$oldest_idx]}" -ot "${files[$i]}" ] \
                    && [[ "${files[$i]}" < "${files[$oldest_idx]}" ]]; }; then
                oldest_idx="$i"
            fi
        done

        if [ "$oldest_idx" -lt 0 ]; then
            echo "quality-log-retention: internal oldest-log selection failed lane=$lane" >&2
            return 1
        fi

        rm -f -- "${files[$oldest_idx]}"
        unset 'files[oldest_idx]'
        files=("${files[@]}")
        deleted=$((deleted + 1))
    done

    if [ "$deleted" -gt 0 ]; then
        echo "quality-log-retention: lane=$lane deleted=$deleted keep=$KEEP"
    fi
}

case "$LANE" in
    fuzz|tests|coverage|simnet-nightly)
        prune_lane "$LANE"
        ;;
    all)
        prune_lane fuzz
        prune_lane tests
        prune_lane coverage
        prune_lane simnet-nightly
        ;;
    *)
        usage
        exit 64
        ;;
esac
