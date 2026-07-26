#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# fold_profile.sh — sample the reducer's cumulative fold telemetry on a datadir
# COPY and emit a CSV plus a derived per-stage / per-transaction summary.
#
# This is a THIN WRAPPER around tools/repro_on_copy.sh. That script already
# owns the parts that are easy to get dangerously wrong — snapshotting the live
# datadir to a fresh labelled copy, refusing to run against a live datadir or a
# live port, isolating HOME/rpcport/p2p/fs/https, and enforcing a deadline —
# and this script adds exactly one thing: a periodic sampler. There is no
# second harness here on purpose.
#
# WHAT IT ANSWERS
#   The node's per-stage timing array (drain_last_stage_us) describes only the
#   MOST RECENT drain round and is overwritten every round, so a sampled
#   observer almost always lands on a converged all-idle round and reads zeros.
#   The cumulative counters this script samples are monotonic, so the
#   DIFFERENCE between two samples is an exact interval measurement:
#     * per-stage microsecond share of a fold round (drain_stage_totals)
#     * write transactions per folded block, and what fraction of them are
#       EMPTY — opened, nothing advanced, rolled back (batch_*_total)
#     * microseconds per commit (batch_commit_us_total / commits)
#     * durability barriers per folded block (fsync_flush_count)
#     * proof_validate sub-phase split (reducer_stage_profile proof_validate)
#
# USAGE
#   tools/scripts/fold_profile.sh --slug fold-profile \
#       [--src DIR] [--deadline 3600] [--sample-secs 30] [--port 18299] \
#       [--connect IP:PORT] [--full] [--out DIR] [-- <node args...>]
#
#   --slug NAME       label for the copy + this run's artifacts (required)
#   --src DIR         source datadir (default $HOME/.zclassic-c23)
#   --deadline SECS   how long the copy node runs (default 3600)
#   --sample-secs N   sampling cadence (default 30)
#   --port N          isolated rpcport for the copy (default 18299)
#   --connect ADDR    peer for the copy to dial; without one the copy is
#                     isolated against a dead sink and folds nothing, which is
#                     a valid but empty measurement
#   --full            pass --full to repro_on_copy.sh (copies blocks/ too;
#                     required for anything that reads historical bodies)
#   --out DIR         artifact dir (default
#                     ~/.local/state/zclassic23-fold-profile/<slug>-<ts>)
#   --                everything after is passed verbatim to the copy node
#
# HONESTY RULES
#   * Every number in the summary is a DIFFERENCE between two samples of a
#     monotonic counter taken from the running copy — never a single reading,
#     never a value transcribed from a doc.
#   * A run whose block delta is zero prints NO per-block figure. A fold that
#     did not fold cannot price a block, and printing a divide-by-zero dash is
#     the honest answer.
#   * The CSV is kept next to the summary so any figure can be recomputed.
#
# bash-free: sh + awk + sed only.
set -u

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"

SLUG=""
SRC="$HOME/.zclassic-c23"
DEADLINE=3600
SAMPLE_SECS=30
PORT=18299
CONNECT=""
FULL=""
OUTDIR=""
PASS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --slug=*)        SLUG="${1#--slug=}" ;;
        --slug)          shift; SLUG="${1:-}" ;;
        --src=*)         SRC="${1#--src=}" ;;
        --deadline=*)    DEADLINE="${1#--deadline=}" ;;
        --sample-secs=*) SAMPLE_SECS="${1#--sample-secs=}" ;;
        --port=*)        PORT="${1#--port=}" ;;
        --connect=*)     CONNECT="${1#--connect=}" ;;
        --full)          FULL="--full" ;;
        --out=*)         OUTDIR="${1#--out=}" ;;
        --)              shift; PASS="$*"; break ;;
        *) echo "fold_profile: unknown option '$1'" >&2; exit 2 ;;
    esac
    shift
done

[ -n "$SLUG" ] || { echo "fold_profile: --slug is required" >&2; exit 2; }
[ -x "$NODE_BIN" ] || {
    echo "fold_profile: $NODE_BIN not built — this harness measures a BINARY," >&2
    echo "              so there is nothing to sample without one." >&2
    exit 2
}

TS="$(date -u +%Y%m%dT%H%M%SZ)"
[ -n "$OUTDIR" ] || OUTDIR="$HOME/.local/state/zclassic23-fold-profile/$SLUG-$TS"
mkdir -p "$OUTDIR" || exit 2
CSV="$OUTDIR/samples.csv"
STATUS="$OUTDIR/copy_status.json"
REPRO_LOG="$OUTDIR/repro_on_copy.log"
SUMMARY="$OUTDIR/summary.txt"

# ── sampling primitives ─────────────────────────────────────────────────────
# Compact JSON, no jq (NO external dependencies is a project rule). Every
# helper takes the whole response text and pulls ONE number out of it.

# jnum TEXT KEY — first "KEY":<int> in TEXT, 0 when absent or null.
jnum() {
    v="$(printf '%s' "$1" |
         sed -n "s/.*\"$2\":\\(-\\{0,1\\}[0-9][0-9]*\\).*/\\1/p" | head -1)"
    [ -n "$v" ] || v=0
    printf '%s' "$v"
}

# jnum1 TEXT KEY — like jnum but takes the FIRST occurrence when the key
# appears more than once. The stage-profile dump emits `cumulative` before
# `last_batch` with identical field names, and the cumulative value is the one
# an interval difference needs.
jnum1() {
    v="$(printf '%s' "$1" | tr ',' '\n' |
         sed -n "s/^[^\"]*\"$2\":\\(-\\{0,1\\}[0-9][0-9]*\\).*/\\1/p" | head -1)"
    [ -n "$v" ] || v=0
    printf '%s' "$v"
}

# jstage TEXT STAGE FIELD — one field of one stage inside drain_stage_totals,
# whose shape is "<stage>":{"us":N,"calls":N,"adv":N}. The same stage names
# also appear in drain_last_stage_us as plain scalars, which this pattern
# cannot match, so the object form is unambiguous.
jstage() {
    v="$(printf '%s' "$1" |
         sed -n "s/.*\"$2\":{\"us\":\\([0-9]*\\),\"calls\":\\([0-9]*\\),\"adv\":\\([0-9]*\\)}.*/\\1 \\2 \\3/p" |
         head -1)"
    [ -n "$v" ] || v="0 0 0"
    case "$3" in
        us)    printf '%s' "$v" | cut -d' ' -f1 ;;
        calls) printf '%s' "$v" | cut -d' ' -f2 ;;
        adv)   printf '%s' "$v" | cut -d' ' -f3 ;;
    esac
}

STAGES="header_admit validate_headers body_fetch body_persist script_validate proof_validate utxo_apply tip_finalize"

ask() { "$NODE_BIN" -datadir="$COPY" -rpcport="$PORT" "$@" 2>/dev/null; }

write_header() {
    h="ts,h_star,rounds_total"
    for s in $STAGES; do h="$h,${s}_us,${s}_calls,${s}_adv"; done
    h="$h,batch_opened,batch_committed,batch_rolled_back,batch_empty"
    h="$h,batch_commit_us_total,fsync_flush_count,fsync_flush_us_total"
    h="$h,pv_blocks,pv_total_us,pv_body_acquire_us,pv_verify_us,pv_log_insert_us"
    h="$h,pv_spends,pv_outputs,pv_sprout_groth16,pv_sprout_phgr13"
    h="$h,pv_binding_sigs,pv_lookahead_hits,pv_lookahead_misses"
    h="$h,tf_blocks,tf_total_us,ua_blocks,ua_total_us"
    printf '%s\n' "$h" > "$CSV"
}

sample_once() {
    drive="$(ask ops state --subsystem=reducer_drive)"
    [ -n "$drive" ] || return 1
    front="$(ask ops state --subsystem=reducer_frontier)"
    pv="$(ask ops state --subsystem=reducer_stage_profile --key=proof_validate)"
    tf="$(ask ops state --subsystem=reducer_stage_profile --key=tip_finalize)"
    ua="$(ask ops state --subsystem=reducer_stage_profile --key=utxo_apply)"

    row="$(date -u +%s),$(jnum "$front" provable_tip),$(jnum "$drive" drain_rounds_total)"
    for s in $STAGES; do
        row="$row,$(jstage "$drive" "$s" us),$(jstage "$drive" "$s" calls),$(jstage "$drive" "$s" adv)"
    done
    row="$row,$(jnum "$drive" batch_opened_total),$(jnum "$drive" batch_committed_total)"
    row="$row,$(jnum "$drive" batch_rolled_back_total),$(jnum "$drive" batch_empty_total)"
    row="$row,$(jnum "$drive" batch_commit_us_total)"
    row="$row,$(jnum "$drive" fsync_flush_count),$(jnum "$drive" fsync_flush_us_total)"
    row="$row,$(jnum1 "$pv" blocks),$(jnum1 "$pv" total_us)"
    row="$row,$(jnum1 "$pv" pv_body_acquire_us),$(jnum1 "$pv" pv_verify_us)"
    row="$row,$(jnum1 "$pv" pv_log_insert_us)"
    row="$row,$(jnum1 "$pv" pv_sapling_spends),$(jnum1 "$pv" pv_sapling_outputs)"
    row="$row,$(jnum1 "$pv" pv_sprout_groth16_joinsplits),$(jnum1 "$pv" pv_sprout_phgr13_joinsplits)"
    row="$row,$(jnum1 "$pv" pv_binding_sigs)"
    row="$row,$(jnum1 "$pv" pv_lookahead_hits),$(jnum1 "$pv" pv_lookahead_misses)"
    row="$row,$(jnum1 "$tf" blocks),$(jnum1 "$tf" total_us)"
    row="$row,$(jnum1 "$ua" blocks),$(jnum1 "$ua" total_us)"
    printf '%s\n' "$row" >> "$CSV"
    return 0
}

# ── launch the copy through the existing harness ────────────────────────────
set -- "$SLUG" "--src=$SRC" "--port=$PORT" "--deadline=$DEADLINE" \
       "--status-file=$STATUS"
[ -n "$FULL" ] && set -- "$@" "$FULL"
[ -n "$CONNECT" ] && set -- "$@" "--connect=$CONNECT"
[ -n "$PASS" ] && set -- "$@" -- $PASS

echo "[fold_profile] artifacts: $OUTDIR"
echo "[fold_profile] launching tools/repro_on_copy.sh $*"
"$REPO_ROOT/tools/repro_on_copy.sh" "$@" > "$REPRO_LOG" 2>&1 &
REPRO_PID=$!

# The copy path is only known once repro_on_copy.sh has snapshotted and
# launched; it publishes it in the status file.
COPY=""
waited=0
while [ "$waited" -lt 1800 ]; do
    if [ -f "$STATUS" ]; then
        COPY="$(sed -n 's/.*"copy_path":"\([^"]*\)".*/\1/p' "$STATUS" | head -1)"
        [ -n "$COPY" ] && break
    fi
    kill -0 "$REPRO_PID" 2>/dev/null || break
    sleep 5
    waited=$((waited + 5))
done
[ -n "$COPY" ] || {
    echo "[fold_profile] FAIL: no copy_path published (see $REPRO_LOG)" >&2
    wait "$REPRO_PID" 2>/dev/null
    exit 1
}
echo "[fold_profile] copy: $COPY"

write_header
samples=0
while kill -0 "$REPRO_PID" 2>/dev/null; do
    if sample_once; then samples=$((samples + 1)); fi
    sleep "$SAMPLE_SECS"
done
wait "$REPRO_PID"
REPRO_RC=$?
sample_once && samples=$((samples + 1))

echo "[fold_profile] $samples samples -> $CSV (repro rc=$REPRO_RC)"

# ── derive the table from the FIRST and LAST usable samples ─────────────────
awk -F, -v OFS=' ' '
NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
{ if (!have_first) { for (i = 1; i <= NF; i++) f[i] = $i; have_first = 1 }
  for (i = 1; i <= NF; i++) l[i] = $i; n++ }
function d(name) { return l[col[name]] - f[col[name]] }
END {
    if (n < 2) { print "fold_profile: fewer than 2 samples — nothing to difference"; exit 0 }
    secs = d("ts")
    printf "interval: %d s over %d samples\n\n", secs, n
    split("header_admit validate_headers body_fetch body_persist script_validate proof_validate utxo_apply tip_finalize", S, " ")
    tot = 0
    for (i = 1; i <= 8; i++) tot += d(S[i] "_us")
    print "STAGE                 delta_us      share%   calls   advances   us/advance"
    for (i = 1; i <= 8; i++) {
        us = d(S[i] "_us"); ca = d(S[i] "_calls"); ad = d(S[i] "_adv")
        printf "%-18s %12d %9.2f %7d %10d %12s\n", S[i], us,
               (tot > 0 ? 100.0 * us / tot : 0), ca, ad,
               (ad > 0 ? sprintf("%.1f", us / ad) : "-")
    }
    printf "%-18s %12d %9.2f %7d %10d\n\n", "TOTAL", tot, 100.0,
           d("rounds_total"), 0
    blocks = d("utxo_apply_adv")
    op = d("batch_opened"); cm = d("batch_committed"); em = d("batch_empty")
    printf "TRANSACTIONS\n"
    printf "  batches opened        %d\n", op
    printf "  batches committed     %d\n", cm
    printf "  batches rolled back   %d\n", d("batch_rolled_back")
    printf "  batches EMPTY         %d  (%.1f%% of opened)\n", em,
           (op > 0 ? 100.0 * em / op : 0)
    printf "  us per commit         %s\n",
           (cm > 0 ? sprintf("%.1f", d("batch_commit_us_total") / cm) : "-")
    printf "  durability barriers   %d\n", d("fsync_flush_count")
    printf "  us per barrier        %s\n",
           (d("fsync_flush_count") > 0 ?
            sprintf("%.1f", d("fsync_flush_us_total") / d("fsync_flush_count")) : "-")
    if (blocks > 0) {
        printf "  blocks folded         %d\n", blocks
        printf "  transactions/block    %.2f\n", op / blocks
        printf "  empty txns/block      %.2f\n", em / blocks
        printf "  barriers/block        %.2f\n", d("fsync_flush_count") / blocks
    } else {
        printf "  blocks folded         0  (no per-block figure: nothing folded)\n"
    }
    pvb = d("pv_blocks")
    printf "\nPROOF_VALIDATE (blocks=%d)\n", pvb
    if (pvb > 0) {
        printf "  total us/block        %.1f\n", d("pv_total_us") / pvb
        printf "    body acquire        %.1f\n", d("pv_body_acquire_us") / pvb
        printf "    proof sweep         %.1f\n", d("pv_verify_us") / pvb
        printf "    log insert          %.1f\n", d("pv_log_insert_us") / pvb
        printf "  sapling spends        %d\n", d("pv_spends")
        printf "  sapling outputs       %d\n", d("pv_outputs")
        printf "  sprout groth16        %d\n", d("pv_sprout_groth16")
        printf "  sprout phgr13         %d\n", d("pv_sprout_phgr13")
        printf "  binding sigs          %d\n", d("pv_binding_sigs")
        printf "  lookahead hit/miss    %d/%d\n", d("pv_lookahead_hits"),
               d("pv_lookahead_misses")
    } else {
        printf "  no proof_validate advance in the interval\n"
    }
}' "$CSV" | tee "$SUMMARY"

echo "[fold_profile] summary: $SUMMARY"
exit "$REPRO_RC"
