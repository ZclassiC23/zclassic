#!/bin/bash
# zclassic23-host-watchdog.sh — SYSTEM-level watchdog ABOVE user@<uid>.
#
# Every zclassic23 unit (node, timers, the slo-probe that watches THEM)
# runs under systemd --user for OPERATOR_USER, and that user manager has
# been OOM-killed before (see the fleet memory note: no-sudo revive via
# linger + a localhost ssh login). This runs one level up, as root, via
# deploy/system/zclassic23-host-watchdog.{service,timer} every 2 min:
#   1. probe user@<uid>.service; if dead, revive it (re-assert linger,
#      then an ssh-localhost no-op run AS OPERATOR_USER — that's whose
#      PAM session needs to open, not root's).
#   2. probe the canonical node's RPC port for a bare TCP/HTTP response
#      (no RPC auth needed — this only needs the port to answer).
#   3. append ONE line to LOG_FILE, but ONLY on a verdict change or while
#      unhealthy (quiet when OK, loud on transitions + every failing cycle).
#
# Idempotent. Read-only except the revive step and the log/state append.
# --dry-run: decide + print, no systemctl/ssh mutation (probes still run).
set -euo pipefail

OPERATOR_USER="${ZCL_HOST_WATCHDOG_USER:-rhett}"
RPC_HOST="${ZCL_HOST_WATCHDOG_RPC_HOST:-127.0.0.1}"
RPC_PORT="${ZCL_HOST_WATCHDOG_RPC_PORT:-18232}"
LOG_FILE="${ZCL_HOST_WATCHDOG_LOG:-/var/log/zclassic23-host-watchdog.log}"
STATE_FILE="${ZCL_HOST_WATCHDOG_STATE:-/var/lib/zclassic23-host-watchdog/state}"

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

uid="$(id -u "$OPERATOR_USER" 2>/dev/null || echo 0)"
[ "$uid" -gt 0 ] || { echo "ERROR: cannot resolve uid for '$OPERATOR_USER'" >&2; exit 1; }

# ── 1. user manager liveness ────────────────────────────────────────────
manager_alive() { systemctl is-active --quiet "user@${uid}.service"; }

verdict="OK"
detail="user-manager=up"

if ! manager_alive; then
    detail="user-manager=DEAD"
    if [ "$DRY_RUN" -eq 1 ]; then
        detail="$detail action=would-revive(enable-linger+ssh-localhost)"
    else
        loginctl enable-linger "$OPERATOR_USER" >/dev/null 2>&1 || true
        runuser -u "$OPERATOR_USER" -- ssh -o BatchMode=yes \
            -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 \
            localhost true >/dev/null 2>&1 || true
        sleep 3
        if manager_alive; then detail="$detail action=revived"
        else detail="$detail action=revive-attempted-still-dead"; fi
    fi
    verdict="REVIVED-USER-MANAGER"
fi

# ── 2. canonical node RPC port reachability (unauthenticated probe) ────
node_reachable() {
    if command -v curl >/dev/null 2>&1; then
        code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
                "http://${RPC_HOST}:${RPC_PORT}/" 2>/dev/null || true)"
        [ -n "$code" ] && [ "$code" != "000" ]
    else
        timeout 5 bash -c ": >/dev/tcp/${RPC_HOST}/${RPC_PORT}" 2>/dev/null
    fi
}

if node_reachable; then
    detail="$detail node=up"
else
    detail="$detail node=DOWN"
    [ "$verdict" = "OK" ] && verdict="NODE-DOWN"
fi

# ── 3. log only on state change or while unhealthy ──────────────────────
if [ "$DRY_RUN" -eq 1 ]; then
    prev="$(cat "$STATE_FILE" 2>/dev/null || echo "(none)")"
    echo "HOST-WATCHDOG: $verdict ($detail) [dry-run, prev=$prev, no side effects taken]"
    exit 0
fi

install -d -m755 "$(dirname "$LOG_FILE")" "$(dirname "$STATE_FILE")" 2>/dev/null || true
# A missing state file (first-ever run) is treated as a prior OK so a
# healthy first cycle stays quiet too — only a genuinely new failure or
# an actual transition writes a line.
prev="$(cat "$STATE_FILE" 2>/dev/null || echo "OK")"
if [ "$verdict" != "OK" ] || [ "$verdict" != "$prev" ]; then
    printf '%s HOST-WATCHDOG: %s (%s)\n' "$(date -u +%FT%TZ)" "$verdict" "$detail" >> "$LOG_FILE"
fi
echo "$verdict" > "$STATE_FILE"
