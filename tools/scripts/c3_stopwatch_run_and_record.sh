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
# Ledger line: {ts, verdict, exit_code, wall_clock_seconds, boots,
#               budget_seconds, peer, file_peer, node_bin, build_commit,
#               artifact_dir, final_network_tip, final_hstar, peer_datadir,
#               peer_block_index_rows, peer_tip_finalize_rows, peer_utxo_rows}
#   boots is the total node launches the run spanned (1 = no respawn; >1 = the
#   harness followed that many supervised self-respawns across the one wiped
#   datadir — how an install-on-next-boot run survives the respawn seam).
#   verdict is one of pass|fail|skip|seam|stalled-named|frontier-busy-timeout|
#   error, mapped from the underlying stopwatch's exit code (0/1/2/3/4/5/other).
#   final_network_tip/final_hstar are lifted straight out of the run's
#   proof.json (the best peer-advertised height and the reducer's authoritative
#   tip at end-of-run) so the judge can refuse a "pass" against a below-
#   checkpoint / stale fixture peer WITHOUT re-reading the artifact directory —
#   the anti-thin-fixture gate keys on these two fields.
#   peer_datadir + peer_*_rows record the SHAPE of the serving fixture peer's
#   hot tables (block-index / tip_finalize_log / utxo row volumes) at collect
#   time, so a future judge can ratchet on fixture shape — a thin fixture that
#   never carried live row volumes once hid an O(delta^2) tip_finalize collapse
#   behind a green stopwatch. Best-effort (SELECT-only, read-only, bounded):
#   any count that is not cheaply available is recorded null, never fabricated
#   and never allowed to fail the collect. These are ADDITIVE fields — every
#   pre-existing field keeps its exact name/position/value, so older ledger
#   consumers (the shared judge, `make c3-stopwatch-report`) are unaffected.
#
# Env:
#   ZCL_BIN               node binary to time (default $REPO_ROOT/build/bin/zclassic23)
#   ZCL_PEER              peer H:P to dial (default 127.0.0.1:39070 — the
#                          dedicated zcl-stopwatch-peer.service fixture)
#   ZCL_CS_FILE_PEER      ROM file-service H:P (defaults to the dedicated
#                          fixture's 127.0.0.1:39072 only for the autonomous
#                          fetch path; copied-header/staged-bundle runs leave
#                          it empty unless explicitly supplied)
#   ZCL_CS_HEADER_SOURCE  optional copied legacy datadir for the board's
#                          headers-first proof move
#   ZCL_CS_BUNDLE_PATH    optional immutable checkpoint bundle to stage into
#                          the wiped proof datadir
#   ZCL_CS_BUDGET_SECS    stopwatch budget seconds, forwarded straight
#                          through to cold_start_to_tip_stopwatch.sh
#                          (its own default is 600 if unset here)
#   ZCL_C3_HISTORY_DIR    ledger dir override (default
#                          ~/.local/state/zclassic23-c3-stopwatch)
#   ZCL_CS_PEER_DATADIR   serving fixture peer's datadir, counted (read-only)
#                          for the fixture-shape fields (default
#                          ~/.local/state/zclassic23-stopwatch-peer, the
#                          dedicated zcl-stopwatch-peer.service datadir)
#   ZCL_CS_PEER_RPCPORT   that peer's RPC port used for the read-only row
#                          counts (default 39071, the fixture peer's rpcport).
#                          Counting goes through the RUNNING peer's own RPC via
#                          the SELECT-only `core storage query` primitive, so it
#                          never opens the live datadir out from under the node.
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
if [ -z "${ZCL_CS_FILE_PEER+x}" ]; then
    if [ -n "${ZCL_CS_HEADER_SOURCE:-}" ] || [ -n "${ZCL_CS_BUNDLE_PATH:-}" ]; then
        export ZCL_CS_FILE_PEER=""
    else
        export ZCL_CS_FILE_PEER="127.0.0.1:39072"
    fi
fi
export ZCL_CS_BUDGET_SECS="${ZCL_CS_BUDGET_SECS:-600}"

HISTORY_DIR="${ZCL_C3_HISTORY_DIR:-${HOME:-/root}/.local/state/zclassic23-c3-stopwatch}"
HISTORY_FILE="$HISTORY_DIR/history.jsonl"
mkdir -p "$HISTORY_DIR"

# Serving fixture peer whose hot-table row volumes we record (read-only) for
# the fixture-shape fields. Defaults match the dedicated zcl-stopwatch-peer
# .service (datadir ~/.local/state/zclassic23-stopwatch-peer, rpcport 39071).
PEER_DATADIR="${ZCL_CS_PEER_DATADIR:-${HOME:-/root}/.local/state/zclassic23-stopwatch-peer}"
PEER_RPCPORT="${ZCL_CS_PEER_RPCPORT:-39071}"

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g; s/\r/\\r/g' | tr '\n' ' '; }
json_string() { printf '"%s"' "$(json_escape "$1")"; }
json_num_or_null() { case "${1:-}" in ''|*[!0-9-]*) printf 'null' ;; *) printf '%s' "$1" ;; esac; }

# proof_num <proof.json> <key> — first integer value of "key" in the run's
# proof.json (schema zcl.c3_stopwatch_artifact.v1). Echoes nothing if the file
# is absent or the field is null/non-numeric (json_num_or_null then records
# null). The proof.json writer emits `"key": <value>,` with a space after the
# colon, so the pattern tolerates optional whitespace.
proof_num() {
    [ -f "$1" ] || return 0
    grep -oE "\"$2\"[[:space:]]*:[[:space:]]*-?[0-9]+" "$1" 2>/dev/null |
        head -n1 | grep -oE -- '-?[0-9]+$'
}

# count_peer_rows <table> — best-effort read-only COUNT(*) of one hot table in
# the serving fixture peer's datadir, via the RUNNING peer's own SELECT-only
# `core storage query` primitive (semicolon-rejected, auto-LIMIT, its own 2s
# budget + 100-row cap). Echoes the integer, or nothing on ANY hiccup (peer
# down, datadir gone, table absent, query interrupted by the node's budget) so
# a fat fixture that can't be cheaply counted records null rather than blocking
# the collect. A thin toy fixture — the exact cheat this guards against —
# counts near-instantly, which is precisely when we want the number recorded.
# NEVER allowed to fail the collect: wrapped so a non-zero rc is swallowed.
count_peer_rows() {
    local table="$1" out cnt
    [ -x "$ZCL_BIN" ] || return 0
    [ -d "$PEER_DATADIR" ] || return 0
    out="$(timeout 12 "$ZCL_BIN" -rpcport="$PEER_RPCPORT" -datadir="$PEER_DATADIR" \
            core storage query --sql="SELECT COUNT(*) FROM $table" --format=json \
            2>/dev/null)" || out=""
    # dbquery renders result rows as a positional array: {"rows":[[<count>]],...}
    cnt="$(printf '%s' "$out" | tr -d ' \n' |
           grep -oE '"rows":\[\[-?[0-9]+' | grep -oE -- '-?[0-9]+$' | head -n1)"
    printf '%s' "$cnt"
}

build_commit="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
[ -z "$build_commit" ] && build_commit="unknown"

echo "c3-stopwatch-run: bin=$ZCL_BIN peer=$ZCL_PEER file_peer=$ZCL_CS_FILE_PEER budget=${ZCL_CS_BUDGET_SECS}s build_commit=$build_commit"

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
    5) verdict="frontier-busy-timeout" ;;
esac

wall_clock="$(printf '%s\n' "$out" | sed -n 's/^WALL_CLOCK_SECONDS=\([0-9][0-9]*\)$/\1/p' | tail -1)"
artifact_dir="$(printf '%s\n' "$out" | sed -n 's/^cold-start-wipe-stopwatch: artifact=\(.*\)$/\1/p' | tail -1)"
# boots = total node launches this run spanned (1 = no respawn; >1 = the
# harness followed N-1 supervised self-respawns across the one wiped datadir).
# Distinguishes an install-respawn run that SURVIVED the seam from a plain
# single-boot run in the durable ledger.
boots="$(printf '%s\n' "$out" | sed -n 's/^BOOTS=\([0-9][0-9]*\)$/\1/p' | tail -1)"

# All of the following extractions are BEST-EFFORT enrichment of the ledger
# line — none may ever fail the collect. Run them with set +e (same idiom as
# the stopwatch invocation above) so a grep-found-nothing / peer-unreachable
# non-zero can never abort the append; a missing value is simply recorded null.
set +e
# final_network_tip / final_hstar: lifted from the run's proof.json so the
# judge's anti-thin-fixture gate has them in the ledger line itself (it reads
# ONLY the last line, never the artifact dir). Absent/null on a run that never
# read a frontier — recorded null, gate then tolerates the line.
proof_json=""
[ -n "${artifact_dir:-}" ] && proof_json="$artifact_dir/proof.json"
final_network_tip="$(proof_num "$proof_json" final_network_tip)"
final_hstar="$(proof_num "$proof_json" final_hstar)"

# Fixture shape: row volumes of the serving peer's hot tables at collect time.
# Best-effort, read-only, null when not cheaply countable (see count_peer_rows).
peer_block_index_rows="$(count_peer_rows blocks)"
peer_tip_finalize_rows="$(count_peer_rows tip_finalize_log)"
peer_utxo_rows="$(count_peer_rows utxos)"
set -e

ts="$(date +%s)"
line="$(printf '{"ts":%s,"verdict":%s,"exit_code":%s,"wall_clock_seconds":%s,"boots":%s,"budget_seconds":%s,"peer":%s,"file_peer":%s,"node_bin":%s,"build_commit":%s,"artifact_dir":%s,"final_network_tip":%s,"final_hstar":%s,"peer_datadir":%s,"peer_block_index_rows":%s,"peer_tip_finalize_rows":%s,"peer_utxo_rows":%s}' \
    "$ts" "$(json_string "$verdict")" "$rc" "$(json_num_or_null "$wall_clock")" \
    "$(json_num_or_null "$boots")" \
    "$(json_num_or_null "$ZCL_CS_BUDGET_SECS")" "$(json_string "$ZCL_PEER")" \
    "$(json_string "$ZCL_CS_FILE_PEER")" "$(json_string "$ZCL_BIN")" \
    "$(json_string "$build_commit")" "$(json_string "${artifact_dir:-}")" \
    "$(json_num_or_null "$final_network_tip")" "$(json_num_or_null "$final_hstar")" \
    "$(json_string "$PEER_DATADIR")" \
    "$(json_num_or_null "$peer_block_index_rows")" \
    "$(json_num_or_null "$peer_tip_finalize_rows")" \
    "$(json_num_or_null "$peer_utxo_rows")")"

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
