#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# netdisrupt_stopwatch_run_and_record.sh — the COLLECT half of PROOF B
# (network-disruption recovery). Runs
# tools/scripts/network_disruption_recovery_stopwatch.sh exactly once and
# appends ONE JSON line to the durable ledger at
# ~/.local/state/zclassic23-netdisrupt-stopwatch/history.jsonl. Same
# collect/judge split as c3_stopwatch_run_and_record.sh and
# tools/scripts/soak_evidence.sh — this wrapper NEVER gates on the run's
# outcome; tools/scripts/stopwatch_evidence_judge.sh /
# `make netdisrupt-stopwatch-report` does.
#
# Ledger line: {ts, verdict, exit_code, wall_clock_seconds, budget_seconds,
#               cut_seconds, peer, node_bin, build_commit, artifact_dir}
#   "peer" here is the CLIENT's own loopback RPC endpoint
#   (127.0.0.1:<client-rpc>) — this harness has no P2P peer of its own to
#   dial (unlike the C3 stopwatch), so the field is repurposed to name the
#   node under test, keeping the ledger schema close enough to
#   c3_stopwatch_run_and_record.sh's for one shared judge script.
#   verdict is one of pass|fail|skip|seam|stalled-named|frontier-busy-timeout|
#   error, mapped from the underlying script's exit code (0/1/2/3/4/5/other).
#
# Env (forwarded straight through to network_disruption_recovery_stopwatch.sh):
#   ZCL_ND_NODE_BIN            client CLI binary (default $REPO_ROOT/build/bin/zclassic23)
#   ZCL_ND_UPSTREAM_PID_FILE   this wrapper's own source of --upstream-pid-file=
#   ZCL_ND_UPSTREAM_PID        bare upstream pid, used when no pid file is set
#   ZCL_ND_CLIENT_RPCPORT      client RPC port (required — no default; a
#                              missing target is an operator misconfig, not
#                              a silently-skippable default)
#   ZCL_ND_CLIENT_DATADIR      client datadir (required, same reasoning)
#   ZCL_ND_CUT_SECS            upstream outage duration (default 600)
#   ZCL_ND_BUDGET_SECS         recovery budget (default 600)
#   ZCL_ND_HISTORY_DIR         ledger dir override (default
#                              ~/.local/state/zclassic23-netdisrupt-stopwatch)
#
# Exit code: 0 once the ledger append succeeds, REGARDLESS of the
# underlying run's verdict — the only hard failure is an unlockable/
# unappendable ledger. Never fails the append silently.

set -uo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
STOPWATCH="$SCRIPT_DIR/network_disruption_recovery_stopwatch.sh"

NODE_BIN="${ZCL_ND_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
UPSTREAM_PID_FILE="${ZCL_ND_UPSTREAM_PID_FILE:-}"
CLIENT_RPCPORT="${ZCL_ND_CLIENT_RPCPORT:-}"
CLIENT_DATADIR="${ZCL_ND_CLIENT_DATADIR:-}"
CUT_SECS="${ZCL_ND_CUT_SECS:-600}"
BUDGET="${ZCL_ND_BUDGET_SECS:-600}"

HISTORY_DIR="${ZCL_ND_HISTORY_DIR:-${HOME:-/root}/.local/state/zclassic23-netdisrupt-stopwatch}"
HISTORY_FILE="$HISTORY_DIR/history.jsonl"
mkdir -p "$HISTORY_DIR"

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g; s/\r/\\r/g' | tr '\n' ' '; }
json_string() { printf '"%s"' "$(json_escape "$1")"; }
json_num_or_null() { case "${1:-}" in ''|*[!0-9-]*) printf 'null' ;; *) printf '%s' "$1" ;; esac; }

build_commit="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
[ -z "$build_commit" ] && build_commit="unknown"

flag_args=("--bin=$NODE_BIN" "--cut-secs=$CUT_SECS" "--budget=$BUDGET")
[ -n "$UPSTREAM_PID_FILE" ] && flag_args+=("--upstream-pid-file=$UPSTREAM_PID_FILE")
[ -n "$CLIENT_RPCPORT" ] && flag_args+=("--client-rpc=$CLIENT_RPCPORT")
[ -n "$CLIENT_DATADIR" ] && flag_args+=("--client-datadir=$CLIENT_DATADIR")

echo "netdisrupt-stopwatch-run: bin=$NODE_BIN client_rpc=${CLIENT_RPCPORT:-<unset>} client_datadir=${CLIENT_DATADIR:-<unset>} cut=${CUT_SECS}s budget=${BUDGET}s build_commit=$build_commit"

set +e
out="$(ZCL_ND_UPSTREAM_PID="${ZCL_ND_UPSTREAM_PID:-}" bash "$STOPWATCH" "${flag_args[@]}" 2>&1)"
rc=$?
set -e
printf '%s\n' "$out"

verdict="error"
case "$rc" in
    0) verdict="pass" ;;
    1) verdict="fail" ;;
    2) verdict="skip" ;;
    3) verdict="seam" ;;
    4) verdict="stalled-named" ;;
    5) verdict="frontier-busy-timeout" ;;
esac

wall_clock="$(printf '%s\n' "$out" | sed -n 's/^WALL_CLOCK_SECONDS=\([0-9][0-9]*\)$/\1/p' | tail -1)"
artifact_dir="$(printf '%s\n' "$out" | sed -n 's/^netdisrupt-stopwatch: artifact=\(.*\)$/\1/p' | tail -1)"
peer_desc="127.0.0.1:${CLIENT_RPCPORT:-0}"

ts="$(date +%s)"
line="$(printf '{"ts":%s,"verdict":%s,"exit_code":%s,"wall_clock_seconds":%s,"budget_seconds":%s,"cut_seconds":%s,"peer":%s,"node_bin":%s,"build_commit":%s,"artifact_dir":%s}' \
    "$ts" "$(json_string "$verdict")" "$rc" "$(json_num_or_null "$wall_clock")" \
    "$(json_num_or_null "$BUDGET")" "$(json_num_or_null "$CUT_SECS")" "$(json_string "$peer_desc")" \
    "$(json_string "$NODE_BIN")" "$(json_string "$build_commit")" "$(json_string "${artifact_dir:-}")")"

append_rc=0
(
    flock -x -w 30 9 || exit 9
    printf '%s\n' "$line" >&9
) 9>>"$HISTORY_FILE" || append_rc=$?
if [ "$append_rc" -ne 0 ]; then
    if [ "$append_rc" -eq 9 ]; then
        echo "netdisrupt-stopwatch-run: FAIL could not acquire append lock on $HISTORY_FILE within 30s" >&2
    else
        echo "netdisrupt-stopwatch-run: FAIL could not append to $HISTORY_FILE (rc=$append_rc)" >&2
    fi
    exit 1
fi

echo "netdisrupt-stopwatch-run: appended file=$HISTORY_FILE verdict=$verdict rc=$rc"
echo "$line"
exit 0
