#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# zclassic23-launch.sh — binary A/B launcher with auto-fallback.
#
# Used as the systemd unit ExecStart:
#     ExecStart=.../zclassic23-launch.sh /path/to/zclassic23 <node args...>
#
# It makes the node self-defending against its own bad releases. Restart=always
# would otherwise crash-loop forever on a deploy that cannot boot. Instead:
#
#   * Before every exec we INCREMENT a boot-failure streak counter file. A boot
#     that never reaches activation-ready leaves the increment in place; the
#     node resets the counter to 0 on ready (app/services/src/binary_ab_fallback.c),
#     so "success" is defined by the node reaching ready, not by living N seconds.
#   * When the streak reaches the threshold (default 3) and a last-good slot
#     exists, we exec the LAST-KNOWN-GOOD binary instead and set
#     ZCL_BINARY_FALLBACK_ACTIVE=1 — the node then raises the
#     binary.fallback_active blocker so `zclassic23 status` shows the
#     degraded-but-alive state.
#
# The last-good slot is an independent COPY of a binary that once reached ready
# (the node promotes it), never a symlink to the live build path a bad deploy
# just overwrote. `exec` replaces this shell so MAINPID stays the node's PID —
# Type=notify / WatchdogSec / ExecStop=$MAINPID all keep working unchanged.
set -eu

if [ "$#" -lt 1 ]; then
    printf '[launch] usage: %s <binary> [node args...]\n' "$0" >&2
    exit 64
fi

BIN="$1"; shift

SLOTS="${ZCL_BINARY_SLOTS_DIR:-$HOME/.local/lib/zclassic23-slots}"
THRESH="${ZCL_BINARY_FALLBACK_THRESHOLD:-3}"
STREAK_FILE="$SLOTS/boot-fail-streak"
LASTGOOD="$SLOTS/last-good"

log() { printf '[launch] %s\n' "$*" >&2; }

mkdir -p "$SLOTS"

streak=$(cat "$STREAK_FILE" 2>/dev/null || echo 0)
case "$streak" in ''|*[!0-9]*) streak=0 ;; esac

# Increment BEFORE exec (see header). The node resets to 0 on ready.
printf '%s\n' "$((streak + 1))" > "$STREAK_FILE"

export ZCL_BINARY_SLOTS_DIR="$SLOTS"

if [ "$streak" -ge "$THRESH" ] && [ -x "$LASTGOOD" ]; then
    export ZCL_BINARY_FALLBACK_ACTIVE=1
    unset ZCL_BINARY_CURRENT 2>/dev/null || true
    log "FALLBACK: boot-failure streak=$streak >= threshold=$THRESH — running last-good slot ($LASTGOOD) instead of $BIN"
    target="$LASTGOOD"
else
    unset ZCL_BINARY_FALLBACK_ACTIVE 2>/dev/null || true
    export ZCL_BINARY_CURRENT="$BIN"
    log "running current binary $BIN (boot-failure streak=$streak/$THRESH)"
    target="$BIN"
fi

# Test seam: print the decision instead of exec'ing a real node.
if [ "${ZCL_LAUNCH_TEST_ECHO:-}" = "1" ]; then
    printf 'EXEC %s\n' "$target"
    printf 'FALLBACK_ACTIVE=%s\n' "${ZCL_BINARY_FALLBACK_ACTIVE:-}"
    printf 'CURRENT=%s\n' "${ZCL_BINARY_CURRENT:-}"
    printf 'STREAK_WRITTEN=%s\n' "$(cat "$STREAK_FILE")"
    exit 0
fi

exec "$target" "$@"
