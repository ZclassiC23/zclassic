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
#   stopwatch-judge: VERDICT=PASS|FAIL|STALE|THIN_FIXTURE|LAGGING_FIXTURE reason=... artifact=<dir>
# and exits 0 (PASS) / 1 (FAIL/THIN_FIXTURE/LAGGING_FIXTURE) / 2 (STALE).
#
#   PASS  — the last recorded run's verdict field is "pass", it is fresh
#           (age <= --max-age-secs), AND it survives the fixture-integrity
#           gates below (a PASS against a thin/stale fixture peer is NOT a
#           real cold-start-to-tip proof and is refused, see THIN_FIXTURE /
#           LAGGING_FIXTURE).
#   FAIL  — the last recorded run's verdict is anything else
#           (fail/skip/seam/stalled-named/frontier-busy-timeout/readback-failed/
#           error) but still fresh. A SKIP still does not get to look green here
#           — the whole point of a periodic gate is that "no evidence of
#           success" reads as failure, never as a silent pass. In particular an
#           instrument failure (readback-failed) is a NAMED FAIL, never a PASS
#           and never mistaken for a silent stall.
#   THIN_FIXTURE — the run's verdict was "pass" but its final_network_tip is
#           BELOW the compiled sovereign checkpoint height (3,056,758): a
#           below-checkpoint toy chain can never mint a real cold-start-to-tip
#           PASS, so a hermetic-green run against one is refused, never
#           reported PASS. (Exit 1, same non-green class as FAIL.) The whole
#           reform: a thin fixture that never carried live row volumes once
#           hid an O(delta^2) tip_finalize collapse behind a green stopwatch —
#           this gate makes that impossible to fake.
#   LAGGING_FIXTURE — the run's verdict was "pass" but a FRESH external
#           oracle_height sample (from the SLO uptime ledger, age < 1h) shows
#           final_network_tip lagging the true network tip by more than the
#           lag budget (default 2000 blocks): the fixture peer was not
#           presenting a genuinely current tip. (Exit 1.)
#   STALE — the ledger is missing/empty/malformed, OR the last sample is
#           older than --max-age-secs (default 86400 = 24h). This is the
#           "the timer died" case: a green last run from a week ago must
#           NOT keep reporting PASS forever once the timer/collector stops
#           running (same staleness-guard discipline as
#           soak_evidence.sh's "stale_evidence_age" rung — a hole in
#           evidence is evidence, never a silent PASS-by-omission).
#
# Backward compatibility: BOTH fixture-integrity gates key on the
# final_network_tip field the c3 collector now records. A ledger line WITHOUT
# that field (a pre-hardening c3 line, OR any netdisrupt line — which has no
# cold-start-to-tip / checkpoint semantics at all) is judged only on the
# fields it carries: neither gate can fire, so those lines are graded exactly
# as before. Old ledgers stay judgeable.
#
# Usage: stopwatch_evidence_judge.sh <history.jsonl> [--max-age-secs N]
#        stopwatch_evidence_judge.sh --selftest
#
# Env: ZCL_STOPWATCH_JUDGE_NOW  epoch override for "now" (hermetic test
#        seam — same pattern as soak_evidence.sh's ZCL_SOAK_NOW).
#      ZCL_STOPWATCH_CHECKPOINT_HEIGHT   compiled sovereign checkpoint height
#        the below-checkpoint gate uses (default 3056758; overridable so the
#        selftest can stay hermetic and so a future re-baked checkpoint is a
#        one-line config change, never a source edit here).
#      ZCL_STOPWATCH_ORACLE_LAG_BUDGET   max blocks final_network_tip may lag
#        a fresh oracle sample before LAGGING_FIXTURE fires (default 2000).
#      ZCL_STOPWATCH_ORACLE_MAX_AGE_SECS freshness window for an oracle_height
#        sample to count (default 3600 = 1h).
#      ZCL_STOPWATCH_SLO_LEDGER          path to the SLO uptime ledger read
#        for the oracle sample (default
#        ~/.local/state/zclassic23-slo/uptime-ledger.jsonl). Its ABSENCE is
#        never a failure — a hermetic CI box has no SLO ledger; rule 1 still
#        applies, rule 2 records ledger_available=false and is skipped.
#
# No python (banned), no jq — bash + sed + awk + tail + tac + flock only, same
# rule as soak_evidence.sh / replay_canary.sh. This script only READS its
# ledgers (no flock needed here — a torn trailing line from a mid-write reader
# race is theoretically possible but a subsequent judge run heals it; the
# WRITER side is what must be flock-serialized, and it is).

set -uo pipefail
export LC_ALL=C

# fld_num/fld_str <json_line> <key> — first matching "key":value extraction.
# Deliberately simple (single-line JSON, no nesting) — matches the
# soak_evidence.sh awk fld() convention, just in sed for a one-line read.
fld_num() {
    printf '%s' "$1" | grep -oE "\"$2\":-?[0-9]+" | head -n1 | sed -E "s/\"$2\"://"
}
fld_str() {
    printf '%s' "$1" | grep -oE "\"$2\":\"[^\"]*\"" | head -n1 | sed -E "s/\"$2\":\"([^\"]*)\"/\1/"
}

# ── fixture-integrity config (env-overridable; validated positive ints) ──
CHECKPOINT_HEIGHT="${ZCL_STOPWATCH_CHECKPOINT_HEIGHT:-3056758}"
ORACLE_LAG_BUDGET="${ZCL_STOPWATCH_ORACLE_LAG_BUDGET:-2000}"
ORACLE_MAX_AGE_SECS="${ZCL_STOPWATCH_ORACLE_MAX_AGE_SECS:-3600}"
SLO_LEDGER="${ZCL_STOPWATCH_SLO_LEDGER:-${HOME:-/root}/.local/state/zclassic23-slo/uptime-ledger.jsonl}"

for _cfg_name in CHECKPOINT_HEIGHT ORACLE_LAG_BUDGET ORACLE_MAX_AGE_SECS; do
    eval "_cfg_val=\"\$$_cfg_name\""
    case "$_cfg_val" in
        ''|*[!0-9]*)
            echo "stopwatch-judge: $_cfg_name must be a non-negative integer (got '$_cfg_val')" >&2
            exit 2 ;;
    esac
done

# slo_freshest_oracle <ledger> <now> <max_age> — echoes the oracle_height of
# the MOST-RECENT SLO ledger line carrying a numeric (non-null) oracle_height
# IFF that line's ts is within <max_age> of <now>; echoes nothing otherwise
# (missing/empty ledger, no numeric oracle sample in the recent tail, or the
# freshest such sample is stale). Scans only the recent tail (bounded) newest-
# first so a large rotated ledger stays cheap. The FIRST numeric-oracle line
# found scanning backwards IS the freshest sample and decides the outcome:
# fresh -> echo it, stale -> echo nothing (an older-but-fresher-looking sample
# further back is not "the current oracle").
slo_freshest_oracle() {
    local f="$1" nowv="$2" maxage="$3" line oh ts age
    [ -s "$f" ] || return 1
    while IFS= read -r line; do
        oh="$(fld_num "$line" oracle_height)"
        [ -n "$oh" ] || continue
        ts="$(fld_num "$line" ts)"
        [ -n "$ts" ] || continue
        age=$((nowv - ts))
        [ "$age" -lt 0 ] && age=$((-age))
        if [ "$age" -lt "$maxage" ] 2>/dev/null; then
            printf '%s\n' "$oh"
        fi
        return 0
    done < <(tail -n 500 "$f" | tac)
    return 1
}

# ── --selftest: hermetic gate checks (canned tmp ledgers, no live infra) ──
# Same idiom as node_slo_probe.sh --selftest / cold_start_to_tip_stopwatch.sh
# --selftest: build tiny fixtures under a mktemp dir, re-invoke THIS script
# against each, assert the VERDICT token + exit code, and end with a single
# "selftest: PASS" / non-zero on any mismatch.
if [ "${1:-}" = "--selftest" ]; then
    SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    st_fail=0
    st_tmp="$(mktemp -d "${TMPDIR:-/tmp}/stopwatch-judge-selftest.XXXXXX")" || {
        echo "selftest: FAIL could not mktemp" >&2; exit 1; }
    trap 'rm -rf "$st_tmp" 2>/dev/null || true' EXIT

    NOW=2000000000                 # fixed epoch so ts freshness is deterministic
    CK=3056758                     # the real compiled checkpoint (default)
    GOOD_TIP=3100000               # comfortably above checkpoint
    BELOW_TIP=200                  # a below-checkpoint toy chain

    # run_case <name> <ledger> <expect_token> <expect_rc> [extra judge/env...]
    # extra args of the form K=V set env for the sub-invocation; anything else
    # is passed through as a judge flag.
    run_case() {
        local name="$1" ledger="$2" want_tok="$3" want_rc="$4"; shift 4
        local -a envv=() flags=()
        local a
        for a in "$@"; do
            case "$a" in *=*) envv+=("$a") ;; *) flags+=("$a") ;; esac
        done
        local out rc tok
        out="$(env ZCL_STOPWATCH_JUDGE_NOW="$NOW" "${envv[@]}" \
               bash "$SELF" "$ledger" --max-age-secs=1000000 "${flags[@]}" 2>&1)"
        rc=$?
        tok="$(printf '%s' "$out" | sed -n 's/.*VERDICT=\([A-Z_]*\).*/\1/p' | head -n1)"
        if [ "$tok" = "$want_tok" ] && [ "$rc" = "$want_rc" ]; then
            echo "  ok: $name -> $tok (rc=$rc)"
        else
            echo "  FAIL: $name -> got tok='$tok' rc=$rc, wanted tok='$want_tok' rc=$want_rc"
            echo "        out: $out"
            st_fail=1
        fi
    }

    echo "stopwatch-judge: --selftest running hermetic gate checks"

    # 1a. Modern c3 PASS with a healthy above-checkpoint tip, no SLO ledger.
    modern_pass="$st_tmp/modern_pass.jsonl"
    printf '{"ts":%s,"verdict":"pass","exit_code":0,"wall_clock_seconds":42,"final_network_tip":%s,"final_hstar":%s,"artifact_dir":"/a/1"}\n' \
        "$NOW" "$GOOD_TIP" "$GOOD_TIP" >"$modern_pass"
    run_case "modern PASS, good tip, no SLO" "$modern_pass" PASS 0 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    # 1b. LEGACY PASS line with NO final_network_tip field still passes
    #     (backward compat — old c3 lines + every netdisrupt line).
    legacy_pass="$st_tmp/legacy_pass.jsonl"
    printf '{"ts":%s,"verdict":"pass","exit_code":0,"wall_clock_seconds":30,"budget_seconds":600,"peer":"127.0.0.1:39070","artifact_dir":"/a/legacy"}\n' \
        "$NOW" >"$legacy_pass"
    run_case "legacy PASS (no final_network_tip) tolerated" "$legacy_pass" PASS 0 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    # 2. Below-checkpoint tip -> THIN_FIXTURE, exit 1.
    thin_pass="$st_tmp/thin_pass.jsonl"
    printf '{"ts":%s,"verdict":"pass","exit_code":0,"wall_clock_seconds":3,"final_network_tip":%s,"final_hstar":%s,"artifact_dir":"/a/thin"}\n' \
        "$NOW" "$BELOW_TIP" "$BELOW_TIP" >"$thin_pass"
    run_case "below-checkpoint tip rejected" "$thin_pass" THIN_FIXTURE 1 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    # 3a. Fresh SLO oracle leads final_network_tip by > budget -> LAGGING_FIXTURE.
    slo_fresh_lead="$st_tmp/slo_fresh_lead.jsonl"
    printf '{"ts":%s,"instance":"canonical","rpcport":18232,"datadir":"/x","reachable":true,"served_height":%s,"header_height":%s,"latency_ms":5,"oracle_height":%s,"max_height":%s,"gap_vs_max":100000,"gap_vs_oracle":100000,"error_detail":""}\n' \
        "$NOW" "$GOOD_TIP" "$GOOD_TIP" 3200000 3200000 >"$slo_fresh_lead"
    run_case "lagging vs fresh oracle rejected" "$modern_pass" LAGGING_FIXTURE 1 \
        "ZCL_STOPWATCH_SLO_LEDGER=$slo_fresh_lead"

    # 3b. Fresh SLO oracle within budget -> PASS (fixture is genuinely current).
    slo_fresh_close="$st_tmp/slo_fresh_close.jsonl"
    printf '{"ts":%s,"instance":"canonical","rpcport":18232,"datadir":"/x","reachable":true,"served_height":%s,"header_height":%s,"latency_ms":5,"oracle_height":%s,"max_height":%s,"gap_vs_max":500,"gap_vs_oracle":500,"error_detail":""}\n' \
        "$NOW" "$GOOD_TIP" "$GOOD_TIP" 3100500 3100500 >"$slo_fresh_close"
    run_case "within-budget vs fresh oracle passes" "$modern_pass" PASS 0 \
        "ZCL_STOPWATCH_SLO_LEDGER=$slo_fresh_close"

    # 3c. STALE oracle sample (age > 1h) -> no fresh sample -> rule 2 skipped,
    #     PASS (a stale oracle can't be used to reject a fixture).
    slo_stale="$st_tmp/slo_stale.jsonl"
    printf '{"ts":%s,"instance":"canonical","rpcport":18232,"datadir":"/x","reachable":true,"served_height":%s,"header_height":%s,"latency_ms":5,"oracle_height":%s,"max_height":%s,"gap_vs_max":100000,"gap_vs_oracle":100000,"error_detail":""}\n' \
        "$((NOW - 7200))" "$GOOD_TIP" "$GOOD_TIP" 3200000 3200000 >"$slo_stale"
    run_case "stale oracle sample tolerated (rule 2 skipped)" "$modern_pass" PASS 0 \
        "ZCL_STOPWATCH_SLO_LEDGER=$slo_stale"

    # 4. Missing SLO ledger entirely -> PASS, rule 1 only (hermetic CI box).
    run_case "missing SLO ledger tolerated" "$modern_pass" PASS 0 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/nope/absent.jsonl"

    # 5. A non-pass verdict (e.g. skip) still reads FAIL, unchanged.
    skip_line="$st_tmp/skip.jsonl"
    printf '{"ts":%s,"verdict":"skip","exit_code":2,"final_network_tip":%s,"artifact_dir":"/a/skip"}\n' \
        "$NOW" "$GOOD_TIP" >"$skip_line"
    run_case "skip verdict still FAILs" "$skip_line" FAIL 1 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    # 6. A below-checkpoint tip but a NON-pass verdict is graded on the verdict
    #    (FAIL), not upgraded to THIN_FIXTURE — the gates only DOWNGRADE a pass.
    thin_seam="$st_tmp/thin_seam.jsonl"
    printf '{"ts":%s,"verdict":"seam","exit_code":3,"final_network_tip":%s,"artifact_dir":"/a/seam"}\n' \
        "$NOW" "$BELOW_TIP" >"$thin_seam"
    run_case "below-checkpoint non-pass stays FAIL" "$thin_seam" FAIL 1 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    # 7. Missing evidence file -> STALE (unchanged).
    run_case "missing ledger file -> STALE" "$st_tmp/no-such-ledger.jsonl" STALE 2 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    # 8. The new readback-failed verdict (stopwatch exit 6): a run whose final
    #    frontier readback failed is an INSTRUMENT failure, judged FAIL with the
    #    verdict name in the reason — never PASS, never silently swallowed. Its
    #    below-checkpoint / null tip must NOT be upgraded to THIN_FIXTURE (the
    #    gates only downgrade a pass).
    readback_line="$st_tmp/readback_failed.jsonl"
    printf '{"ts":%s,"verdict":"readback-failed","exit_code":6,"final_network_tip":-1,"final_hstar":-1,"final_provable_sample":3107923,"artifact_dir":"/a/readback"}\n' \
        "$NOW" >"$readback_line"
    run_case "readback-failed verdict is judged FAIL (named cause), never PASS" "$readback_line" FAIL 1 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    # 9. A seam verdict (real climb, budget seam) still reads FAIL, unchanged —
    #    it is honest forward progress but not a cold-start-to-tip PASS.
    seam_line="$st_tmp/seam.jsonl"
    printf '{"ts":%s,"verdict":"seam","exit_code":3,"final_network_tip":%s,"final_hstar":3105872,"artifact_dir":"/a/seam2"}\n' \
        "$NOW" "$GOOD_TIP" >"$seam_line"
    run_case "seam verdict reads FAIL (not PASS, not upgraded)" "$seam_line" FAIL 1 \
        "ZCL_STOPWATCH_SLO_LEDGER=$st_tmp/does-not-exist.jsonl"

    if [ "$st_fail" = 0 ]; then
        echo "selftest: PASS"
        exit 0
    fi
    echo "selftest: FAIL" >&2
    exit 1
fi

HISTORY_FILE="${1:-}"
if [ -z "$HISTORY_FILE" ]; then
    echo "usage: stopwatch_evidence_judge.sh <history.jsonl> [--max-age-secs N]" >&2
    echo "       stopwatch_evidence_judge.sh --selftest" >&2
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
    # ── fixture-integrity gates ─────────────────────────────────────────
    # A recorded "pass" is only a REAL cold-start-to-tip proof if it caught
    # a genuine, current, full-history fixture peer. Both gates key on
    # final_network_tip (the best height a handshake-complete peer advertised,
    # recorded by the c3 collector). A line WITHOUT that field (legacy c3 line
    # or any netdisrupt line) is graded exactly as before — neither gate fires.
    final_nt="$(fld_num "$last_line" final_network_tip)"

    # Rule 1 — a below-checkpoint toy chain can never mint a PASS. The compiled
    # sovereign checkpoint height is 3,056,758; a fixture peer that never
    # reached it is definitionally thin.
    if [ -n "$final_nt" ] && [ "$final_nt" -lt "$CHECKPOINT_HEIGHT" ] 2>/dev/null; then
        echo "stopwatch-judge: VERDICT=THIN_FIXTURE reason=final_network_tip_${final_nt}_below_checkpoint_${CHECKPOINT_HEIGHT} artifact=$artifact"
        exit 1
    fi

    # Rule 2 — the fixture peer must present a genuinely CURRENT tip. When a
    # fresh oracle_height sample exists in the external SLO ledger, a PASS
    # whose final_network_tip lags it by more than the budget is a stale
    # fixture. Absence of the SLO ledger / no fresh oracle is NOT a failure
    # (hermetic CI boxes): ledger_available=false, rule 1 stands alone.
    ledger_available=false
    oracle_h="$(slo_freshest_oracle "$SLO_LEDGER" "$now" "$ORACLE_MAX_AGE_SECS")"
    if [ -n "$oracle_h" ]; then
        ledger_available=true
        if [ -n "$final_nt" ]; then
            lag=$((oracle_h - final_nt))
            if [ "$lag" -gt "$ORACLE_LAG_BUDGET" ] 2>/dev/null; then
                echo "stopwatch-judge: VERDICT=LAGGING_FIXTURE reason=final_network_tip_${final_nt}_lags_oracle_${oracle_h}_by_${lag}_gt_budget_${ORACLE_LAG_BUDGET} ledger_available=true artifact=$artifact"
                exit 1
            fi
        fi
    fi

    echo "stopwatch-judge: VERDICT=PASS reason=last_run_verdict_pass_age_${age}s ledger_available=${ledger_available} artifact=$artifact"
    exit 0
fi

echo "stopwatch-judge: VERDICT=FAIL reason=last_run_verdict_${verdict}_age_${age}s artifact=$artifact"
exit 1
