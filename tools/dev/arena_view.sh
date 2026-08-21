#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Driver for `make arena-view` and `make arena-view-check`.
#
# Plays the pinned seed-7 3v3 demo (or views REPLAY=<file>) through
# tools/arena_view.c. Interactive mode always passes --show. CHECK=1 never
# opens a window: it refuses incomplete argv and requires --check-only to
# re-derive the pinned demo roots.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BIN="${ZCL_BIN_DIR:-$REPO_ROOT/build/bin}"
RUNNER="$BIN/arena_runner"
VIEW="$BIN/arena_view"
ZDOGVIEW="$BIN/zdogview"
PILOT_RED="$BIN/pilot_zdogace"
PILOT_BLUE="$BIN/pilot_zdogdrone"
REPLAY_OUT="$BIN/arena-view-demo.replay"

RED_LABEL="Red Ace — zdogace 0.1.1"
BLUE_LABEL="Blue Drone — zdogdrone 0.1.0"
REF_REPLAY_ROOT=05ed352dbb2213aad289cdf403d424d18d9ae075db57252a52c4e745a25e8396
REF_STATE_ROOT=e4b37a9b94547cead91a7d4ae2a63b0385b29a99bb603bd0ac3519cebd270ebd

av_die()
{
    printf 'arena-view: FAILED: %s\n' "$*" >&2
    exit 1
}

av_expect_exit()
{
    local want="$1"
    shift
    local rc=0
    "$@" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq "$want" ] || av_die "$* : exit $rc, expected $want"
}

av_play_demo()
{
    rm -f "$REPLAY_OUT"
    local rc=0
    "$RUNNER" --seed 7 --planes-per-team 3 \
        --pilot-red "$PILOT_RED" --pilot-blue "$PILOT_BLUE" \
        --replay-out "$REPLAY_OUT" || rc=$?
    if [ "$rc" -eq 3 ]; then
        printf 'arena-view: kernel cannot confine pilots; retrying unconfined\n'
        rc=0
        "$RUNNER" --no-sandbox --seed 7 --planes-per-team 3 \
            --pilot-red "$PILOT_RED" --pilot-blue "$PILOT_BLUE" \
            --replay-out "$REPLAY_OUT" || rc=$?
    fi
    if [ "$rc" -ne 0 ]; then
        av_die "arena_runner failed (exit $rc)"
    fi
    [ -f "$REPLAY_OUT" ] || av_die "no replay written"
}

[ -x "$VIEW" ] || av_die "missing $VIEW (build tools/arena-view first)"
[ -x "$RUNNER" ] || av_die "missing $RUNNER"

if [ "${CHECK:-}" = 1 ]; then
    av_expect_exit 0 "$VIEW" --help
    av_expect_exit 2 "$VIEW"
    av_expect_exit 2 "$VIEW" --frames
    av_expect_exit 2 "$VIEW" --seek
    av_expect_exit 2 "$VIEW" --cam
    av_play_demo
    if [ -x "$ZDOGVIEW" ]; then
        "$ZDOGVIEW" verify "$REPLAY_OUT" >/dev/null || av_die "zdogview verify failed"
        ppm="$BIN/arena-view-hosted.ppm"
        "$ZDOGVIEW" render "$REPLAY_OUT" --out "$ppm" || av_die "zdogview render failed"
        [ -s "$ppm" ] || av_die "hosted PPM was empty"
        [ "$(head -1 "$ppm")" = "P6" ] || av_die "hosted view is not a P6 PPM"
    fi
    report="$("$VIEW" --replay "$REPLAY_OUT" --check-only --red-label "$RED_LABEL" \
        --blue-label "$BLUE_LABEL")"
    printf '%s\n' "$report"
    case "$report" in
        *"replay_root=$REF_REPLAY_ROOT"*) ;;
        *) av_die "replay root mismatch: $report" ;;
    esac
    case "$report" in
        *"state_root=$REF_STATE_ROOT"*) ;;
        *) av_die "state root mismatch: $report" ;;
    esac
    printf 'arena-view-check: ok\n'
    exit 0
fi

if [ -n "${REPLAY:-}" ]; then
    exec "$VIEW" --show --replay "$REPLAY"
fi

av_play_demo
exec "$VIEW" --show --replay "$REPLAY_OUT" \
    --red-label "$RED_LABEL" --blue-label "$BLUE_LABEL"
