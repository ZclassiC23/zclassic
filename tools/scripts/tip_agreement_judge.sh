#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# tip_agreement_judge.sh — the WINDOWED judge over the off-host tip-hash
# agreement ledger written by tools/scripts/tip_agreement_probe.sh.
#
# Prints exactly one line:
#   tip-agreement-judge: VERDICT=<token> reason=<why> window_hours=<h> \
#     samples=<n> agrees=<n> disagrees=<n> could_not_ask=<n> \
#     min_peers_seen=<n> newest_age_s=<n> ledger=<path>
#
# WHAT WINDOW IS GRADED, EXACTLY
# ------------------------------
# Every ledger line whose `ts` falls in [now - window_hours*3600, now] is
# graded. All of them. Not the last line.
#
# That sentence is here because a sibling judge in this repo
# (stopwatch_evidence_judge.sh) reads `tail -n 1` — correctly, because a
# stopwatch run is a point-in-time proof — and the shape was then reused
# where it did not belong, so a single green row was read as a week of
# endurance. Agreement is an ACCRUAL claim: "our tip hash matched genuinely
# remote peers, repeatedly, across a day". One row cannot say that, so this
# judge does not let one row try. If you change the aggregation here, change
# this paragraph in the same commit.
#
# VERDICTS — all of them FAIL CLOSED
# ----------------------------------
#   PASS            fresh, >= --min-agree agreeing samples inside the
#                   window, zero (or <= --max-disagree) disagreeing samples,
#                   and EVERY agreeing sample carried at least
#                   --min-distinct-peers distinct remote witnesses AND was
#                   recorded by a prober whose own control was at least that
#                   strong.  exit 0
#   DISAGREE        more than --max-disagree samples recorded `disagrees`.
#                   Checked BEFORE the evidence-volume rules so a real
#                   divergence can never be masked by a thin window. exit 1
#   NO_EVIDENCE     the window contains zero `agrees` samples. An all
#                   could-not-ask window is the case this verdict exists
#                   for: the recorder ran, nothing was comparable, and that
#                   is NOT agreement. exit 1
#   THIN_EVIDENCE   fewer than --min-agree agreeing samples, or an agreeing
#                   sample backed by fewer distinct peers than the control
#                   requires, or a ledger written with a WEAKENED control
#                   (its own min_distinct_peers below ours). exit 1
#   STALE           no ledger, empty ledger, no line carrying a parseable
#                   ts, or the newest sample is older than --max-age-secs.
#                   The timer dying must not leave yesterday's PASS standing
#                   forever. exit 2
#   MALFORMED       a graded line carried an `outcome` this judge does not
#                   recognise. An unknown state is never silently dropped
#                   into the could-not-ask bucket, because dropping it is
#                   how a future recorder bug becomes an invisible one.
#                   exit 1
#
# THE NUMBERS, AND WHY THEY ARE THOSE NUMBERS
# -------------------------------------------
#   --window-hours 24   The accrual claim this rung supports is "a day of
#                       repeated agreement". 24h also matches the staleness
#                       convention already used by
#                       stopwatch_evidence_judge.sh and soak_evidence.sh, so
#                       an operator reading three judges reads one clock.
#   --max-age-secs 1800 The recorder's shipped cadence is 10 minutes
#                       (deploy/zclassic23-tip-agreement.timer), so 1800s is
#                       three missed runs. One missed run is a busy box; a
#                       dead timer crosses this within half an hour and the
#                       verdict becomes STALE rather than a stale PASS.
#   --min-agree 6       At a 10-minute cadence a 24h window holds ~144
#                       samples. Six agreeing samples means agreement was
#                       re-established at six independent moments; a single
#                       lucky sample cannot mint a PASS. It is deliberately
#                       far below 144: on the live host the peer table only
#                       intermittently carries a hash reported by two
#                       distinct remote hosts, so demanding near-total
#                       coverage would make the gate unpassable for reasons
#                       that are not about correctness. Six is the floor at
#                       which "repeatedly" is defensible; raise it as peer
#                       hash learnability improves — this file's numbers may
#                       be TIGHTENED at any time and must never be loosened
#                       to make a red gate green.
#   --min-distinct-peers 2
#                       The control. See tip_agreement_probe.sh: one peer is
#                       an anecdote in NM_FORK_MIN_CLUSTER,
#                       NM_NETSPLIT_MIN_RIVAL_PEERS and
#                       header_corroboration, and it is an anecdote here.
#                       The judge enforces it TWICE — against each agreeing
#                       sample's observed witness count, and against the
#                       control the prober recorded as being in force — so
#                       running the prober with a weakened floor produces a
#                       ledger this judge refuses instead of a PASS.
#   --max-disagree 0    A disagreement means we hold a different block hash
#                       at a height where two or more remote hosts agree
#                       with each other. A short reorg CAN produce one
#                       honestly, and the right response to that is to look
#                       at the ledger line — which names the height and both
#                       hashes — not to raise this number.
#
# Usage:
#   tip_agreement_judge.sh <agreement-ledger.jsonl> [flags]
#   tip_agreement_judge.sh --selftest
#
# Env: ZCL_PARITY_JUDGE_NOW   epoch override for "now" (hermetic test seam,
#      same pattern as ZCL_STOPWATCH_JUDGE_NOW / ZCL_SOAK_NOW).
#
# No python (banned), no jq — bash + awk only. Read-only: this judge never
# writes to the ledger it grades.

set -uo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "${1:-}" = "--selftest" ]; then
    exec bash "$SCRIPT_DIR/test_tip_agreement_evidence.sh" --only judge
fi

LEDGER="${1:-}"
if [ -z "$LEDGER" ]; then
    echo "usage: tip_agreement_judge.sh <agreement-ledger.jsonl> [--window-hours N] [--max-age-secs N] [--min-agree N] [--min-distinct-peers N] [--max-disagree N]" >&2
    echo "       tip_agreement_judge.sh --selftest" >&2
    exit 2
fi
shift || true

WINDOW_HOURS=24
MAX_AGE_SECS=1800
MIN_AGREE=6
MIN_DISTINCT_PEERS=2
MAX_DISAGREE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --window-hours)        shift; WINDOW_HOURS="${1:?--window-hours needs a value}" ;;
        --window-hours=*)      WINDOW_HOURS="${1#*=}" ;;
        --max-age-secs)        shift; MAX_AGE_SECS="${1:?--max-age-secs needs a value}" ;;
        --max-age-secs=*)      MAX_AGE_SECS="${1#*=}" ;;
        --min-agree)           shift; MIN_AGREE="${1:?--min-agree needs a value}" ;;
        --min-agree=*)         MIN_AGREE="${1#*=}" ;;
        --min-distinct-peers)  shift; MIN_DISTINCT_PEERS="${1:?--min-distinct-peers needs a value}" ;;
        --min-distinct-peers=*) MIN_DISTINCT_PEERS="${1#*=}" ;;
        --max-disagree)        shift; MAX_DISAGREE="${1:?--max-disagree needs a value}" ;;
        --max-disagree=*)      MAX_DISAGREE="${1#*=}" ;;
        *) echo "tip-agreement-judge: unknown arg '$1'" >&2; exit 2 ;;
    esac
    shift
done

for _cfg in WINDOW_HOURS MAX_AGE_SECS MIN_AGREE MIN_DISTINCT_PEERS MAX_DISAGREE; do
    eval "_v=\"\$$_cfg\""
    case "$_v" in
        '' | *[!0-9]*)
            echo "tip-agreement-judge: $_cfg must be a non-negative integer (got '$_v')" >&2
            exit 2 ;;
    esac
done
if [ "$MIN_DISTINCT_PEERS" -lt 2 ]; then
    # Refusing this at the front door rather than in the grading loop: a
    # judge invoked with --min-distinct-peers=1 is not a lenient judge, it
    # is a judge that has been told one peer may manufacture agreement.
    echo "tip-agreement-judge: --min-distinct-peers must be >= 2 (one peer must never be able to manufacture agreement)" >&2
    exit 2
fi
if [ "$MIN_AGREE" -lt 1 ]; then
    echo "tip-agreement-judge: --min-agree must be >= 1 (a window with no agreeing sample is NO_EVIDENCE, never a pass)" >&2
    exit 2
fi

NOW="${ZCL_PARITY_JUDGE_NOW:-$(date +%s)}"
case "$NOW" in
    '' | *[!0-9]*)
        echo "tip-agreement-judge: ZCL_PARITY_JUDGE_NOW must be a positive integer epoch" >&2
        exit 2 ;;
esac

verdict() {
    # min_peers_seen prints "-" when no agreeing sample existed to measure
    # one. The awk sentinel is -1 and a printed -1 would read like a real
    # count of minus one; the whole file is about not printing a number
    # where there is no measurement.
    local mp="${7:--}"
    case "$mp" in -1) mp="-" ;; esac
    echo "tip-agreement-judge: VERDICT=$1 reason=$2 window_hours=$WINDOW_HOURS samples=${3:-0} agrees=${4:-0} disagrees=${5:-0} could_not_ask=${6:-0} min_peers_seen=$mp newest_age_s=${8:--} ledger=$LEDGER"
}

if [ ! -s "$LEDGER" ]; then
    verdict STALE no_ledger_file 0 0 0 0 - -
    exit 2
fi

# One awk pass over the whole file. Emits a single space-separated summary:
#   total_in_window agrees disagrees cna malformed min_peers min_control newest_ts parseable
# Fields are pulled with match()/substr rather than a JSON parser — every
# value read here is a bare integer or a short identifier the prober wrote,
# and the ledger is one flat object per line by construction.
SUMMARY="$(awk -v now="$NOW" -v winsecs="$((WINDOW_HOURS * 3600))" '
function num(line, key,   m, s) {
    if (!match(line, "\"" key "\":-?[0-9]+")) return "";
    s = substr(line, RSTART, RLENGTH);
    sub("\"" key "\":", "", s);
    return s;
}
function str(line, key,   s, p) {
    if (!match(line, "\"" key "\":\"[^\"]*\"")) return "";
    s = substr(line, RSTART, RLENGTH);
    sub("\"" key "\":\"", "", s);
    sub("\"$", "", s);
    return s;
}
BEGIN { total=0; ag=0; dis=0; cna=0; mal=0; minp=-1; minctl=-1; newest=-1; parseable=0 }
{
    ts = num($0, "ts");
    if (ts == "") next;
    if (ts+0 > now + 3600) next;   # a far-future stamp is not evidence, and
                                   # must not be allowed to set `newest` —
                                   # that would make a dead ledger look fresh
    parseable++;
    if (ts+0 > newest) newest = ts+0;
    if (ts+0 < now - winsecs) next;                        # outside the graded window
    total++;
    o = str($0, "outcome");
    if (o == "agrees") {
        ag++;
        p = num($0, "modal_remote_peers");
        if (p == "") p = 0;                                 # unmeasured witnesses
        if (minp < 0 || p+0 < minp) minp = p+0;             # never counts as strong
        c = num($0, "min_distinct_peers");
        if (c == "") c = 0;
        if (minctl < 0 || c+0 < minctl) minctl = c+0;
    } else if (o == "disagrees") {
        dis++;
    } else if (o == "could-not-ask") {
        cna++;
    } else {
        mal++;
    }
}
END { printf "%d %d %d %d %d %d %d %d %d\n", total, ag, dis, cna, mal, minp, minctl, newest, parseable }
' "$LEDGER")"

read -r TOTAL AGREES DISAGREES CNA MALFORMED MINPEERS MINCTL NEWEST PARSEABLE <<<"$SUMMARY"

if [ "${PARSEABLE:-0}" -eq 0 ]; then
    verdict STALE no_line_with_parseable_ts 0 0 0 0 - -
    exit 2
fi

AGE=$((NOW - NEWEST))
[ "$AGE" -lt 0 ] && AGE=0
if [ "$AGE" -gt "$MAX_AGE_SECS" ]; then
    verdict STALE "newest_sample_age_${AGE}s_gt_${MAX_AGE_SECS}s" \
        "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
    exit 2
fi

if [ "$TOTAL" -eq 0 ]; then
    # Fresh newest sample but nothing inside the window is only reachable
    # with --window-hours smaller than --max-age-secs; name it rather than
    # dividing by an empty window.
    verdict STALE no_samples_inside_window 0 0 0 0 - "$AGE"
    exit 2
fi

if [ "$MALFORMED" -gt 0 ]; then
    verdict MALFORMED "${MALFORMED}_samples_with_unrecognised_outcome" \
        "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
    exit 1
fi

if [ "$DISAGREES" -gt "$MAX_DISAGREE" ]; then
    verdict DISAGREE "${DISAGREES}_disagreeing_samples_gt_max_${MAX_DISAGREE}" \
        "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
    exit 1
fi

if [ "$AGREES" -eq 0 ]; then
    verdict NO_EVIDENCE "zero_agreeing_samples_in_window_${CNA}_could_not_ask" \
        "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
    exit 1
fi

if [ "$AGREES" -lt "$MIN_AGREE" ]; then
    verdict THIN_EVIDENCE "only_${AGREES}_agreeing_samples_lt_min_${MIN_AGREE}" \
        "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
    exit 1
fi

if [ "$MINPEERS" -lt "$MIN_DISTINCT_PEERS" ]; then
    verdict THIN_EVIDENCE \
        "an_agreeing_sample_had_only_${MINPEERS}_distinct_remote_peers_lt_${MIN_DISTINCT_PEERS}" \
        "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
    exit 1
fi

if [ "$MINCTL" -lt "$MIN_DISTINCT_PEERS" ]; then
    verdict THIN_EVIDENCE \
        "ledger_written_with_weakened_control_min_distinct_peers_${MINCTL}_lt_${MIN_DISTINCT_PEERS}" \
        "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
    exit 1
fi

verdict PASS \
    "${AGREES}_agreeing_samples_ge_${MIN_AGREE}_each_backed_by_ge_${MIN_DISTINCT_PEERS}_distinct_remote_peers" \
    "$TOTAL" "$AGREES" "$DISAGREES" "$CNA" "$MINPEERS" "$AGE"
exit 0
