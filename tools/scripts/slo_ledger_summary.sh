#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# slo_ledger_summary.sh — reads the EXTERNAL uptime prober's ledger
# (tools/scripts/node_slo_probe.sh, ~/.local/state/zclassic23-slo/
# uptime-ledger.jsonl) and prints a last-N-hours summary PER INSTANCE:
#   probe_count               samples in the window
#   reachable_pct             % of samples with reachable:true
#   max_gap_vs_oracle         max(gap_vs_oracle) over reachable samples in
#                             the window, or null if never reachable /
#                             oracle never answered alongside it
#   longest_unreachable_run_probes / _sec
#                             longest CONSECUTIVE run of reachable:false
#                             samples (chronological), in probe count and
#                             in elapsed seconds (last_ts - first_ts of
#                             that run)
#   last_sample_age_sec       staleness of the newest sample for that
#                             instance (a prober that stopped running is
#                             itself a hole this must not hide)
#
# This is the read surface the future B7 drive loop and the 7-day proof
# (D5/M3) consume — one bash invocation, no ledger parsing duplicated
# elsewhere. Parsed-data only: an empty/missing ledger prints an explicit
# "no data" summary per instance, never a silent pass.
#
# Usage:
#   slo_ledger_summary.sh [--window-hours N] [--instance NAME]
#   slo_ledger_summary.sh --selftest        # hermetic fixture, no live nodes
#
# Env:
#   ZCL_SLO_LEDGER_DIR   ledger dir (default ~/.local/state/zclassic23-slo)
#   ZCL_SLO_NOW          epoch override for "now" (staleness calc; test seam)
#
# No python (banned), no jq — bash + awk only, same rule as
# soak_evidence.sh / node_slo_probe.sh.

set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"

LEDGER_DIR="${ZCL_SLO_LEDGER_DIR:-${HOME:-/root}/.local/state/zclassic23-slo}"
LEDGER_FILE="$LEDGER_DIR/uptime-ledger.jsonl"

INSTANCES="canonical soak dev"

summarize() {
    local window_hours="$1" only_instance="$2" ledger_file="$3" now_ts="$4"

    if [ ! -s "$ledger_file" ]; then
        echo "slo-summary: window_hours=$window_hours file=$ledger_file file_samples=0"
        echo "slo-summary: VERDICT=NO_DATA reason=no_ledger_file"
        return 2
    fi

    local inst
    local overall_rc=0
    for inst in $INSTANCES; do
        if [ -n "$only_instance" ] && [ "$inst" != "$only_instance" ]; then
            continue
        fi
        local out rc
        set +e
        out="$(awk -v inst="$inst" -v wh="$window_hours" -v now="$now_ts" '
            function fld(line, key,    re, s) {
                re = "\"" key "\":(-?[0-9]+|null|true|false)"
                if (match(line, re) == 0) return ""
                s = substr(line, RSTART, RLENGTH)
                sub(/^"[^"]*":/, "", s)
                return s
            }
            function isnum(s) { return (s != "" && s != "null" && s != "true" && s != "false") }
            index($0, "\"instance\":\"" inst "\"") == 0 { next }
            {
                t = fld($0, "ts")
                if (t == "") next
                n++
                ts[n] = t + 0
                reach[n] = (index($0, "\"reachable\":true") > 0) ? 1 : 0
                gapo[n] = fld($0, "gap_vs_oracle")
            }
            END {
                if (n == 0) {
                    printf "slo-summary: instance=%s window_hours=%d probe_count=0\n", inst, wh
                    printf "slo-summary: instance=%s VERDICT=NO_DATA reason=no_samples_for_instance\n", inst
                    exit 2
                }
                # chronological order is append order in the ledger; sort
                # defensively by ts anyway (insertion sort is fine at this n).
                for (i = 2; i <= n; i++) {
                    tv = ts[i]; rv = reach[i]; gv = gapo[i]
                    j = i - 1
                    while (j >= 1 && ts[j] > tv) {
                        ts[j+1] = ts[j]; reach[j+1] = reach[j]; gapo[j+1] = gapo[j]
                        j--
                    }
                    ts[j+1] = tv; reach[j+1] = rv; gapo[j+1] = gv
                }

                last = ts[n]
                cutoff = last - wh * 3600
                i0 = 1
                for (i = 1; i <= n; i++) { if (ts[i] < cutoff) i0 = i + 1; else break }
                if (i0 > n) i0 = n
                cnt = n - i0 + 1

                reach_cnt = 0; max_gap = ""
                run_len = 0; run_start_ts = ""; best_run_len = 0; best_run_sec = 0
                for (i = i0; i <= n; i++) {
                    if (reach[i]) {
                        reach_cnt++
                        if (isnum(gapo[i])) {
                            g = gapo[i] + 0
                            if (max_gap == "" || g > max_gap + 0) max_gap = g
                        }
                        if (run_len > 0) {
                            run_sec = ts[i-1] - run_start_ts
                            if (run_len > best_run_len || (run_len == best_run_len && run_sec > best_run_sec)) {
                                best_run_len = run_len; best_run_sec = run_sec
                            }
                        }
                        run_len = 0; run_start_ts = ""
                    } else {
                        if (run_len == 0) run_start_ts = ts[i]
                        run_len++
                    }
                }
                if (run_len > 0) {
                    run_sec = ts[n] - run_start_ts
                    if (run_len > best_run_len || (run_len == best_run_len && run_sec > best_run_sec)) {
                        best_run_len = run_len; best_run_sec = run_sec
                    }
                }

                reach_pct = 100.0 * reach_cnt / cnt
                age = now - last

                printf "slo-summary: instance=%s window_hours=%d probe_count=%d reachable_pct=%.2f max_gap_vs_oracle=%s longest_unreachable_run_probes=%d longest_unreachable_run_sec=%d last_sample_age_sec=%d\n", \
                    inst, wh, cnt, reach_pct, (max_gap == "" ? "null" : max_gap ""), best_run_len, best_run_sec, age

                v = "OK"; reason = "nominal"
                if (reach_pct < 99.0) { v = "DEGRADED"; reason = sprintf("reachable_pct_%.2f_lt_99", reach_pct) }
                if (age > 180) { v = "STALE"; reason = sprintf("last_sample_age_%ds_gt_180s", age) }
                printf "slo-summary: instance=%s VERDICT=%s reason=%s\n", inst, v, reason
            }
        ' "$ledger_file")"
        rc=$?
        set -e
        printf '%s\n' "$out"
        [ "$rc" -ne 0 ] && overall_rc=1
    done
    return "$overall_rc"
}

# ── selftest ─────────────────────────────────────────────────────────

st_fail() { echo "selftest: FAIL $*" >&2; exit 1; }

st_line() {
    # st_line <file> <instance> <ts> <reachable 0|1> <gap_vs_oracle|null>
    local f="$1" inst="$2" ts="$3" reach="$4" gap="$5"
    printf '{"ts":%d,"instance":"%s","rpcport":1,"datadir":"/x","reachable":%s,"served_height":1,"header_height":1,"latency_ms":1,"oracle_height":1,"max_height":1,"gap_vs_max":0,"gap_vs_oracle":%s,"error_detail":""}\n' \
        "$ts" "$inst" "$( [ "$reach" = 1 ] && echo true || echo false )" "$gap" >> "$f"
}

cmd_selftest() {
    ST_TMP="$(mktemp -d /tmp/zcl-slo-summary-selftest.XXXXXX)"
    trap 'rm -rf "$ST_TMP"' EXIT

    # A) missing ledger -> NO_DATA, rc=2.
    local f="$ST_TMP/missing/uptime-ledger.jsonl"
    set +e
    out="$(summarize 24 "" "$f" 1700100000)"
    rc=$?
    set -e
    printf '%s\n' "$out" | grep -q 'VERDICT=NO_DATA reason=no_ledger_file' \
        || st_fail "case=missing-ledger wrong output: $out"
    [ "$rc" -eq 2 ] || st_fail "case=missing-ledger expected rc=2 got $rc"
    echo "selftest: ok case=missing-ledger"

    # B) all-reachable, gap steady at 0 except one sample gap=5 (max).
    f="$ST_TMP/ok/uptime-ledger.jsonl"; mkdir -p "$ST_TMP/ok"
    local base=1700000000 i
    for ((i = 0; i < 60; i++)); do
        local gap=0
        [ "$i" -eq 30 ] && gap=5
        st_line "$f" canonical $((base + i * 60)) 1 "$gap"
    done
    local now=$((base + 59 * 60 + 30))
    set +e
    out="$(summarize 24 canonical "$f" "$now")"
    rc=$?
    set -e
    printf '%s\n' "$out" | grep -q 'probe_count=60 reachable_pct=100.00 max_gap_vs_oracle=5' \
        || st_fail "case=all-reachable wrong summary: $out"
    printf '%s\n' "$out" | grep -q 'longest_unreachable_run_probes=0 longest_unreachable_run_sec=0' \
        || st_fail "case=all-reachable expected zero unreachable run: $out"
    printf '%s\n' "$out" | grep -q 'VERDICT=OK' || st_fail "case=all-reachable expected VERDICT=OK: $out"
    [ "$rc" -eq 0 ] || st_fail "case=all-reachable expected rc=0 got $rc"
    echo "selftest: ok case=all-reachable"

    # C) an unreachable streak of 5 consecutive probes (5 min at 60s cadence)
    # in the middle of an otherwise-green run.
    f="$ST_TMP/streak/uptime-ledger.jsonl"; mkdir -p "$ST_TMP/streak"
    for ((i = 0; i < 60; i++)); do
        if [ "$i" -ge 20 ] && [ "$i" -le 24 ]; then
            st_line "$f" soak $((base + i * 60)) 0 null
        else
            st_line "$f" soak $((base + i * 60)) 1 0
        fi
    done
    now=$((base + 59 * 60))
    set +e
    out="$(summarize 24 soak "$f" "$now")"
    rc=$?
    set -e
    printf '%s\n' "$out" | grep -q 'longest_unreachable_run_probes=5 longest_unreachable_run_sec=240' \
        || st_fail "case=unreachable-streak wrong run math: $out"
    printf '%s\n' "$out" | grep -q 'reachable_pct=91.67' \
        || st_fail "case=unreachable-streak wrong reachable_pct: $out"
    printf '%s\n' "$out" | grep -q 'VERDICT=DEGRADED' \
        || st_fail "case=unreachable-streak expected VERDICT=DEGRADED: $out"
    echo "selftest: ok case=unreachable-streak"

    # D) stale: last sample far in the past relative to "now".
    f="$ST_TMP/stale/uptime-ledger.jsonl"; mkdir -p "$ST_TMP/stale"
    for ((i = 0; i < 10; i++)); do
        st_line "$f" dev $((base + i * 60)) 1 0
    done
    now=$((base + 10 * 60 + 3600))
    set +e
    out="$(summarize 24 dev "$f" "$now")"
    rc=$?
    set -e
    printf '%s\n' "$out" | grep -q 'VERDICT=STALE' || st_fail "case=stale expected VERDICT=STALE: $out"
    echo "selftest: ok case=stale"

    # E) window trims older samples: 48h of hourly canonical samples,
    # windowed to the trailing 24h only.
    f="$ST_TMP/window/uptime-ledger.jsonl"; mkdir -p "$ST_TMP/window"
    for ((i = 0; i <= 48; i++)); do
        st_line "$f" canonical $((base + i * 3600)) 1 0
    done
    now=$((base + 48 * 3600))
    set +e
    out="$(summarize 24 canonical "$f" "$now")"
    rc=$?
    set -e
    printf '%s\n' "$out" | grep -q 'probe_count=25' \
        || st_fail "case=window-trim expected probe_count=25 (24h + boundary sample): $out"
    echo "selftest: ok case=window-trim"

    echo "selftest: PASS"
}

# ── dispatch ─────────────────────────────────────────────────────────

WINDOW_HOURS=24
ONLY_INSTANCE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --window-hours)   shift; WINDOW_HOURS="${1:?--window-hours needs a value}" ;;
        --window-hours=*) WINDOW_HOURS="${1#*=}" ;;
        --instance)       shift; ONLY_INSTANCE="${1:?--instance needs a value}" ;;
        --instance=*)     ONLY_INSTANCE="${1#*=}" ;;
        --selftest)       cmd_selftest; exit 0 ;;
        *) echo "usage: slo_ledger_summary.sh [--window-hours N] [--instance NAME] | --selftest" >&2; exit 2 ;;
    esac
    shift
done
case "$WINDOW_HOURS" in
    ''|*[!0-9]*) echo "slo-summary: --window-hours must be a positive integer" >&2; exit 2 ;;
esac
if [ -n "$ONLY_INSTANCE" ]; then
    case " $INSTANCES " in
        *" $ONLY_INSTANCE "*) ;;
        *) echo "slo-summary: unknown --instance '$ONLY_INSTANCE' (known: $INSTANCES)" >&2; exit 2 ;;
    esac
fi

summarize "$WINDOW_HOURS" "$ONLY_INSTANCE" "$LEDGER_FILE" "${ZCL_SLO_NOW:-$(date +%s)}"
