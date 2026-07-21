#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# c3_stopwatch_run_and_record.sh — the COLLECT half of the C3 wall-clock
# stopwatch evidence ledger. Runs
# tools/scripts/cold_start_to_tip_stopwatch.sh exactly once and appends ONE
# JSON line to the durable ledger at
# ~/.local/state/zclassic23-c3-stopwatch/history.jsonl. The JUDGE half is
# tools/scripts/stopwatch_evidence_judge.sh / `make c3-stopwatch-report`
# (same collect/judge split as tools/scripts/soak_evidence.sh — collect
# NEVER gates on the run's outcome, judge does).
#
# Ledger line: {ts, verdict, exit_code, wall_clock_seconds, budget_seconds,
#               peer, node_bin, build_commit, artifact_dir}
#   verdict is one of pass|fail|skip|seam|stalled-named|error, mapped from
#   the underlying stopwatch's exit code (0/1/2/3/4/other).
#
# Env:
#   ZCL_BIN               node binary to time (default $REPO_ROOT/build/bin/zclassic23)
#   ZCL_PEER              peer H:P to dial (default 127.0.0.1:39070 — the
#                          dedicated zcl-stopwatch-peer.service fixture)
#   ZCL_CS_BUDGET_SECS    stopwatch budget seconds, forwarded straight
#                          through to cold_start_to_tip_stopwatch.sh
#                          (its own default is 600 if unset here)
#   ZCL_C3_HISTORY_DIR    ledger dir override (default
#                          ~/.local/state/zclassic23-c3-stopwatch)
#
# Exit code: 0 once the ledger append succeeds, REGARDLESS of the
# underlying run's verdict (pass/fail/skip/seam/stalled-named are all
# recorded, not gated here — that is the judge's job). The ONLY thing that
# makes this wrapper itself fail is being unable to lock or append the
# ledger line — never fails the append silently, same discipline as
# soak_evidence.sh's collect command.

set -uo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
STOPWATCH="$SCRIPT_DIR/cold_start_to_tip_stopwatch.sh"

ZCL_BIN="${ZCL_BIN:-$REPO_ROOT/build/bin/zclassic23}"
ZCL_PEER="${ZCL_PEER:-127.0.0.1:39070}"
export ZCL_CS_BUDGET_SECS="${ZCL_CS_BUDGET_SECS:-600}"

HISTORY_DIR="${ZCL_C3_HISTORY_DIR:-${HOME:-/root}/.local/state/zclassic23-c3-stopwatch}"
HISTORY_FILE="$HISTORY_DIR/history.jsonl"
mkdir -p "$HISTORY_DIR"

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g; s/\r/\\r/g' | tr '\n' ' '; }
json_string() { printf '"%s"' "$(json_escape "$1")"; }
json_num_or_null() { case "${1:-}" in ''|*[!0-9-]*) printf 'null' ;; *) printf '%s' "$1" ;; esac; }

build_commit="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
[ -z "$build_commit" ] && build_commit="unknown"

echo "c3-stopwatch-run: bin=$ZCL_BIN peer=$ZCL_PEER budget=${ZCL_CS_BUDGET_SECS}s build_commit=$build_commit"

set +e
out="$(bash "$STOPWATCH" --bin="$ZCL_BIN" --peer="$ZCL_PEER" 2>&1)"
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
esac

wall_clock="$(printf '%s\n' "$out" | sed -n 's/^WALL_CLOCK_SECONDS=\([0-9][0-9]*\)$/\1/p' | tail -1)"
artifact_dir="$(printf '%s\n' "$out" | sed -n 's/^cold-start-wipe-stopwatch: artifact=\(.*\)$/\1/p' | tail -1)"

ts="$(date +%s)"
line="$(printf '{"ts":%s,"verdict":%s,"exit_code":%s,"wall_clock_seconds":%s,"budget_seconds":%s,"peer":%s,"node_bin":%s,"build_commit":%s,"artifact_dir":%s}' \
    "$ts" "$(json_string "$verdict")" "$rc" "$(json_num_or_null "$wall_clock")" \
    "$(json_num_or_null "$ZCL_CS_BUDGET_SECS")" "$(json_string "$ZCL_PEER")" \
    "$(json_string "$ZCL_BIN")" "$(json_string "$build_commit")" "$(json_string "${artifact_dir:-}")")"

# flock-serialized append (same pattern as soak_evidence.sh cmd_collect):
# a bounded lock acquire (-w 30) whose failure is EXPLICIT, so a missing
# or stuck flock can never silently degrade to an unlocked/torn append,
# and can never hang past the unit's TimeoutStartSec.
append_rc=0
(
    flock -x -w 30 9 || exit 9
    printf '%s\n' "$line" >&9
) 9>>"$HISTORY_FILE" || append_rc=$?
if [ "$append_rc" -ne 0 ]; then
    if [ "$append_rc" -eq 9 ]; then
        echo "c3-stopwatch-run: FAIL could not acquire append lock on $HISTORY_FILE within 30s" >&2
    else
        echo "c3-stopwatch-run: FAIL could not append to $HISTORY_FILE (rc=$append_rc)" >&2
    fi
    exit 1
fi

echo "c3-stopwatch-run: appended file=$HISTORY_FILE verdict=$verdict rc=$rc"
echo "$line"
exit 0
