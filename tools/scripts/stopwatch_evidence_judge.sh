#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# stopwatch_evidence_judge.sh — the generic LAST-LINE judge for the
# wall-clock stopwatch ledgers written by c3_stopwatch_run_and_record.sh
# and netdisrupt_stopwatch_run_and_record.sh (one flock-appended JSON line
# per timer run). Reads ONLY the last line — unlike soak_evidence.sh's
# windowed 168h judge, a stopwatch run is a point-in-time proof, not an
# accrual claim, so there is no window to cover, only freshness to check.
#
# Prints exactly one line:
#   stopwatch-judge: VERDICT=PASS|FAIL|STALE reason=... artifact=<dir>
# and exits 0 (PASS) / 1 (FAIL) / 2 (STALE).
#
#   PASS  — the last recorded run's verdict field is "pass" AND it is
#           fresh (age <= --max-age-secs).
#   FAIL  — the last recorded run's verdict is anything else
#           (fail/skip/seam/stalled-named/error) but still fresh. A SKIP
#           still does not get to look green here — the whole point of a
#           periodic gate is that "no evidence of success" reads as
#           failure, never as a silent pass.
#   STALE — the ledger is missing/empty/malformed, OR the last sample is
#           older than --max-age-secs (default 86400 = 24h). This is the
#           "the timer died" case: a green last run from a week ago must
#           NOT keep reporting PASS forever once the timer/collector stops
#           running (same staleness-guard discipline as
#           soak_evidence.sh's "stale_evidence_age" rung — a hole in
#           evidence is evidence, never a silent PASS-by-omission).
#
# Usage: stopwatch_evidence_judge.sh <history.jsonl> [--max-age-secs N]
#
# Env: ZCL_STOPWATCH_JUDGE_NOW  epoch override for "now" (hermetic test
#      seam — same pattern as soak_evidence.sh's ZCL_SOAK_NOW).
#
# No python (banned), no jq — bash + sed + awk + flock only, same rule as
# soak_evidence.sh / replay_canary.sh. This script only READS the ledger
# (no flock needed here — a torn trailing line from a mid-write reader race
# is theoretically possible but a subsequent judge run heals it; the
# WRITER side is what must be flock-serialized, and it is).

set -uo pipefail
export LC_ALL=C

HISTORY_FILE="${1:-}"
if [ -z "$HISTORY_FILE" ]; then
    echo "usage: stopwatch_evidence_judge.sh <history.jsonl> [--max-age-secs N]" >&2
    exit 2
fi
shift || true

MAX_AGE_SECS=86400
while [ $# -gt 0 ]; do
    case "$1" in
        --max-age-secs)   shift; MAX_AGE_SECS="${1:?--max-age-secs needs a value}" ;;
        --max-age-secs=*) MAX_AGE_SECS="${1#*=}" ;;
        *) echo "stopwatch-judge: unknown arg '$1'" >&2; exit 2 ;;
    esac
    shift
done
case "$MAX_AGE_SECS" in
    ''|*[!0-9]*) echo "stopwatch-judge: --max-age-secs must be a positive integer" >&2; exit 2 ;;
esac

now="${ZCL_STOPWATCH_JUDGE_NOW:-$(date +%s)}"
case "$now" in
    ''|*[!0-9]*) echo "stopwatch-judge: ZCL_STOPWATCH_JUDGE_NOW must be a positive integer epoch" >&2; exit 2 ;;
esac

if [ ! -s "$HISTORY_FILE" ]; then
    echo "stopwatch-judge: VERDICT=STALE reason=no_evidence_file artifact=-"
    exit 2
fi

last_line="$(tail -n 1 "$HISTORY_FILE")"
if [ -z "$last_line" ]; then
    echo "stopwatch-judge: VERDICT=STALE reason=empty_last_line artifact=-"
    exit 2
fi

# fld_num/fld_str <json_line> <key> — first matching "key":value extraction.
# Deliberately simple (single-line JSON, no nesting) — matches the
# soak_evidence.sh awk fld() convention, just in sed for a one-line read.
fld_num() {
    printf '%s' "$1" | grep -oE "\"$2\":-?[0-9]+" | head -n1 | sed -E "s/\"$2\"://"
}
fld_str() {
    printf '%s' "$1" | grep -oE "\"$2\":\"[^\"]*\"" | head -n1 | sed -E "s/\"$2\":\"([^\"]*)\"/\1/"
}

ts="$(fld_num "$last_line" ts)"
verdict="$(fld_str "$last_line" verdict)"
artifact="$(fld_str "$last_line" artifact_dir)"
[ -z "$artifact" ] && artifact="-"

if [ -z "$ts" ]; then
    echo "stopwatch-judge: VERDICT=STALE reason=malformed_last_line_no_ts artifact=$artifact"
    exit 2
fi

age=$((now - ts))
if [ "$age" -gt "$MAX_AGE_SECS" ] 2>/dev/null; then
    echo "stopwatch-judge: VERDICT=STALE reason=last_sample_age_${age}s_gt_${MAX_AGE_SECS}s artifact=$artifact"
    exit 2
fi

if [ -z "$verdict" ]; then
    echo "stopwatch-judge: VERDICT=STALE reason=missing_verdict_field artifact=$artifact"
    exit 2
fi

if [ "$verdict" = "pass" ]; then
    echo "stopwatch-judge: VERDICT=PASS reason=last_run_verdict_pass_age_${age}s artifact=$artifact"
    exit 0
fi

echo "stopwatch-judge: VERDICT=FAIL reason=last_run_verdict_${verdict}_age_${age}s artifact=$artifact"
exit 1
