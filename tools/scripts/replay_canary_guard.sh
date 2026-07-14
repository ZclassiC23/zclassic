#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# replay_canary_guard.sh — mint-collision guard for the replay canary
# timers (lane S2d, wf/s2d-replay-canary-crashloop).
#
# The replay canary (tools/scripts/replay_canary.sh) is a CPU/IO-heavy
# oneshot (Nice=10, IOSchedulingClass=idle already — see the installed
# .service — but a from-genesis run still occupies a full core + disk
# bandwidth for ~6 h). The sovereign-state cure mint producers
# (zclassic23-anchor-mint, zclassic23-mint-fast-v2, zclassic23-mint-receipt
# — all installed as TRANSIENT `systemctl --user` units, see
# UnitFileState=transient) are the #1 priority track and must never be
# starved by an evidence job. This wrapper is the systemd ExecStart= for
# both the nightly (--from=anchor) and weekly (--from=genesis) canary
# services: it checks for any active `*mint*` unit FIRST and, if one is
# folding, SKIPS the run with a logged reason and a clean exit(0) — never
# a FAIL sentinel, never an OnFailure page, just "not now". A skip is not
# a verdict: replay_canary.sh's own "never exit-0-as-proof" sentinel
# contract is untouched (no sentinel is reset or written on a skip), so a
# downstream sentinel-freshness reader still sees "no fresh PASS" rather
# than a fabricated one.
#
# Pattern: the task-of-record names the literal `zclassic23-mint-*` glob;
# this guard also matches `zclassic23-anchor-mint` (a live producer today
# that does not fit that glob) via a broader `*mint*` match, so a future
# mint producer with any other name still trips the guard. Deliberately
# conservative: any active unit with "mint" in its name skips the run.
#
# Usage: replay_canary_guard.sh --from=anchor|genesis [any replay_canary.sh arg...]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CANARY="$SCRIPT_DIR/replay_canary.sh"
ORIG_ARGS="$*"

skip() {
    local reason="$1"
    echo "replay-canary-guard: SKIP reason=$reason args=[$ORIG_ARGS]" >&2
    logger -t replay-canary "SKIP reason=$reason — evidence job yields to an active mint fold" 2>/dev/null || true
    exit 0
}

if command -v systemctl >/dev/null 2>&1; then
    active_mint="$(systemctl --user list-units --type=service --state=active --no-legend '*mint*' 2>/dev/null || true)"
    if [ -n "$active_mint" ]; then
        skip "mint_unit_active: $(printf '%s' "$active_mint" | awk '{print $1}' | paste -sd, -)"
    fi
else
    # No systemctl on this box at all — cannot check, cannot safely run
    # a CPU/IO-heavy replay unattended either. Skip loud rather than guess.
    skip "no_systemctl"
fi

exec "$CANARY" "$@"
