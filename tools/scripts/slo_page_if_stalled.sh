#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# slo_page_if_stalled.sh — the EXTERNAL SLO PAGER (the pager half of the
# uptime-SLO loop). node_slo_probe.sh is the scoreboard (it dials each
# instance's RPC port and appends what it saw); slo_ledger_summary.sh is the
# read surface (it prints a windowed per-instance summary). Neither of those
# WAKES ANYONE UP. This script is the missing half: it reads the same ledger
# and, when a page-worthy condition is active, it (a) appends a durable page
# record, (b) best-effort broadcasts via wall(1), and (c) exits non-zero so a
# systemd unit wrapping it STAYS FAILED — the failed unit IS the page surface.
#
# WHY this exists at all: the live canonical node once made ZERO height
# progress for its ENTIRE 13-day uptime and nobody was paged — it was
# "reachable" the whole time, just not advancing. Worse, the external prober
# ledger itself went stale on 2026-07-20 and NOTHING noticed the prober had
# died. A pager that only fired on "unreachable" would have slept through
# both. So this pager treats three distinct failure shapes as page-worthy:
#
#   1. stale_ledger        — the prober itself stopped writing (self-watchdog:
#                            if the scoreboard dies, the pager must page; a
#                            silent prober is the worst failure of all).
#   2. canonical_unreachable — the canonical node stopped answering RPC.
#   3. canonical_no_advance  — the canonical node answers but its served
#                            height is not climbing (the 13-day incident).
#
# Reads the prober ledger $ZCL_SLO_LEDGER_DIR/uptime-ledger.jsonl (schema in
# node_slo_probe.sh's header: one JSON object per probe per instance with
# fields ts, instance, reachable, served_height, oracle_height, gap_vs_oracle,
# error_detail, ...). Never TRUSTS a node self-report — everything here is a
# derivation over what an outside client actually observed and recorded.
#
# Page action per ACTIVE condition:
#   - append ONE JSON line to $ZCL_SLO_LEDGER_DIR/pages.jsonl:
#       {"ts":<now>,"condition":"<id>","detail":"<text>",
#        "served_height":<n|null>,"oracle_height":<n|null>,"gap_vs_oracle":<n|null>}
#   - DEDUP: a per-condition state file $ZCL_SLO_LEDGER_DIR/page-state/<id>.last
#     holds the epoch of the last announcement; a fresh page line + wall
#     broadcast is only emitted when the previous announcement is older than
#     ZCL_SLO_PAGE_RENOTIFY_SEC (a still-failing condition must not spam the
#     ledger every probe cycle, but must re-announce periodically so an
#     unacknowledged page stays visible).
#   - Whether or not deduped, an active condition means EXIT 1. The systemd
#     unit staying failed is the page — dedup only throttles the ledger/wall
#     noise, it never suppresses the failed-unit signal.
# No active condition => one OK summary line to stdout, exit 0.
#
# Blocker enrichment (best-effort, NEVER fatal): when a page is announced and
# ZCL_SLO_NODE_BIN is executable, the current typed blocker is folded into the
# detail so the pager record says WHY, not just THAT. Any failure/timeout of
# enrichment is swallowed — it can never change the verdict or the exit code.
#
# Usage:
#   slo_page_if_stalled.sh [evaluate]   # default: evaluate ledger, page/exit
#   slo_page_if_stalled.sh --selftest   # hermetic; tmp ledgers + fixed clock
#
# Env (test/operator injection seams):
#   ZCL_SLO_LEDGER_DIR        ledger dir (default ~/.local/state/zclassic23-slo)
#   ZCL_SLO_NOW               epoch override for "now" (staleness/window calc)
#   ZCL_SLO_PAGE_STALE_SEC    max age of the newest sample of ANY instance
#                             before stale_ledger fires (default 300)
#   ZCL_SLO_PAGE_UNREACH_SEC  canonical_unreachable window (default 600)
#   ZCL_SLO_PAGE_ADVANCE_SEC  canonical_no_advance window (default 7200)
#   ZCL_SLO_PAGE_RENOTIFY_SEC re-announce interval per condition (default 21600)
#   ZCL_SLO_PAGE_WALL         set 0 to skip the wall(1) broadcast (default 1)
#   ZCL_SLO_NODE_BIN          node binary for blocker enrichment
#                             (default <script-dir>/../../build/bin/zclassic23,
#                              skipped when not executable)
#
# No python (banned), no jq — bash + awk + flock only, same rule as
# node_slo_probe.sh / slo_ledger_summary.sh.

set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"

LEDGER_DIR="${ZCL_SLO_LEDGER_DIR:-${HOME:-/root}/.local/state/zclassic23-slo}"
LEDGER_FILE="$LEDGER_DIR/uptime-ledger.jsonl"
PAGES_FILE="$LEDGER_DIR/pages.jsonl"
PAGE_STATE_DIR="$LEDGER_DIR/page-state"

STALE_SEC="${ZCL_SLO_PAGE_STALE_SEC:-300}"
UNREACH_SEC="${ZCL_SLO_PAGE_UNREACH_SEC:-600}"
ADVANCE_SEC="${ZCL_SLO_PAGE_ADVANCE_SEC:-7200}"
RENOTIFY_SEC="${ZCL_SLO_PAGE_RENOTIFY_SEC:-21600}"
WALL="${ZCL_SLO_PAGE_WALL:-1}"

# Node binary for best-effort blocker enrichment. An explicit override always
# wins (even to empty, which disables enrichment); otherwise default to this
# checkout's build output. Never required — enrichment is decoration.
resolve_node_bin() {
    if [ -n "${ZCL_SLO_NODE_BIN+x}" ]; then printf '%s' "${ZCL_SLO_NODE_BIN}"; return 0; fi
    printf '%s' "$SCRIPT_DIR/../../build/bin/zclassic23"
}
NODE_BIN="$(resolve_node_bin)"

# ── helpers ────────────────────────────────────────────────────────────

# json_escape / jstr: emit a JSON string literal (escaped) — same pattern as
# node_slo_probe.sh so page detail text with quotes/backslashes stays valid.
json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
jstr() { printf '"%s"' "$(json_escape "${1:-}")"; }

# jtok <value>: pass a bare integer through as a JSON number, everything else
# (empty, "null", non-numeric) becomes JSON null — a page record never carries
# a fabricated 0 or an unescaped token.
jtok() {
    local v="${1:-}"
    case "$v" in
        ''|null|-) printf 'null' ;;
        -*) case "${v#-}" in ''|*[!0-9]*) printf 'null' ;; *) printf '%s' "$v" ;; esac ;;
        *[!0-9]*) printf 'null' ;;
        *) printf '%s' "$v" ;;
    esac
}

# append_line <file> <json-line>: flock-serialized append (bounded -w 30,
# explicit failure) — copied from node_slo_probe.sh so a timer run and an
# ad-hoc operator run can never interleave a torn line into pages.jsonl.
append_line() {
    local file="$1" line="$2" append_rc=0
    (
        flock -x -w 30 9 || exit 9
        printf '%s\n' "$line" >&9
    ) 9>>"$file" || append_rc=$?
    if [ "$append_rc" -ne 0 ]; then
        if [ "$append_rc" -eq 9 ]; then
            echo "slo-page: FAIL could not acquire append lock on $file within 30s" >&2
        else
            echo "slo-page: FAIL could not append to $file (rc=$append_rc)" >&2
        fi
        return 1
    fi
    return 0
}

# enrich_blocker: best-effort typed blocker snapshot, single-line, <=400 bytes,
# NEVER fatal. Prints "" on any absence/timeout/failure. The 5 s timeout and
# the executable guard keep a wedged or missing node from ever stalling or
# aborting the pager.
enrich_blocker() {
    local bin="$NODE_BIN" out=""
    [ -n "$bin" ] || { printf ''; return 0; }
    [ -x "$bin" ] || { printf ''; return 0; }
    out="$(timeout 5 "$bin" dumpstate blocker 2>/dev/null | head -c 400 || true)"
    out="$(printf '%s' "$out" | tr '\n\r\t' '   ')"
    printf '%s' "$out"
    return 0
}

# evaluate_ledger <now>: emit zero-or-more tab-separated PAGE rows plus one
# SUMMARY row. PAGE row is: PAGE<TAB>condition<TAB>detail<TAB>served<TAB>oracle
# <TAB>gap  (served/oracle/gap already normalized to an integer or the literal
# "null"). Only called when the ledger file is non-empty; the missing/empty
# case is handled by the caller as an unconditional stale_ledger.
evaluate_ledger() {
    local now="$1"
    awk -v now="$now" -v stale_sec="$STALE_SEC" -v unreach_sec="$UNREACH_SEC" \
        -v advance_sec="$ADVANCE_SEC" '
        function fld(line, key,   re, s) {
            re = "\"" key "\":(-?[0-9]+|null|true|false)"
            if (match(line, re) == 0) return ""
            s = substr(line, RSTART, RLENGTH)
            sub(/^"[^"]*":/, "", s)
            return s
        }
        function isnum(s) { return (s != "" && s != "null" && s != "true" && s != "false") }
        function ntok(s)  { return (isnum(s) ? s : "null") }
        function instof(line,   re, s) {
            re = "\"instance\":\"[a-zA-Z0-9_]+\""
            if (match(line, re) == 0) return ""
            s = substr(line, RSTART, RLENGTH)
            sub(/^"instance":"/, "", s)
            sub(/"$/, "", s)
            return s
        }
        {
            t = fld($0, "ts")
            if (t == "") next
            t = t + 0
            inst = instof($0)
            if (inst == "") next
            if (!(inst in imax) || t > imax[inst]) imax[inst] = t
            if (inst == "canonical") {
                nc++
                cts[nc]    = t
                creach[nc] = (index($0, "\"reachable\":true") > 0) ? 1 : 0
                cserved[nc] = fld($0, "served_height")
                coracle[nc] = fld($0, "oracle_height")
                cgap[nc]    = fld($0, "gap_vs_oracle")
            }
        }
        END {
            # chronological order = append order, but sort defensively by ts
            # (insertion sort is fine at this n).
            for (i = 2; i <= nc; i++) {
                tv=cts[i]; rv=creach[i]; sv=cserved[i]; ov=coracle[i]; gv=cgap[i]
                j = i - 1
                while (j >= 1 && cts[j] > tv) {
                    cts[j+1]=cts[j]; creach[j+1]=creach[j]; cserved[j+1]=cserved[j]
                    coracle[j+1]=coracle[j]; cgap[j+1]=cgap[j]
                    j--
                }
                cts[j+1]=tv; creach[j+1]=rv; cserved[j+1]=sv; coracle[j+1]=ov; cgap[j+1]=gv
            }

            last_served="null"; last_oracle="null"; last_gap="null"
            newest_c_reach=0; newest_c_ts=0
            if (nc > 0) {
                newest_c_ts    = cts[nc]
                newest_c_reach = creach[nc]
                last_served = ntok(cserved[nc])
                last_oracle = ntok(coracle[nc])
                last_gap    = ntok(cgap[nc])
            }

            # ── stale_ledger: the OLDEST newest-per-instance sample is too
            # old. If the prober died, every instance goes stale together;
            # deterministic tie-break on instance name keeps the detail stable.
            stale=0; stale_detail=""
            worst_inst=""; worst_age=-1
            for (inst in imax) {
                age = now - imax[inst]
                if (age > worst_age || (age == worst_age && (worst_inst == "" || inst < worst_inst))) {
                    worst_age = age; worst_inst = inst
                }
            }
            if (worst_inst != "" && worst_age > stale_sec) {
                stale = 1
                stale_detail = "instance=" worst_inst " newest_sample_age=" worst_age \
                    "s exceeds stale threshold " stale_sec "s (prober may be dead)"
            }

            # ── canonical_unreachable: >=3 canonical samples in the window,
            # ALL reachable:false, and the newest canonical sample unreachable.
            unreach=0; unreach_detail=""
            cutoff_u = now - unreach_sec
            win_n=0; win_reach_true=0
            for (i = 1; i <= nc; i++) if (cts[i] >= cutoff_u) {
                win_n++
                if (creach[i]) win_reach_true++
            }
            if (win_n >= 3 && win_reach_true == 0 && nc > 0 && newest_c_reach == 0) {
                unreach = 1
                unreach_detail = "canonical unreachable: " win_n " samples in last " \
                    unreach_sec "s all reachable=false (newest sample age=" (now - newest_c_ts) "s)"
            }

            # ── canonical_no_advance: >=2 reachable canonical samples in the
            # window, spanning >= 3/4 of it, with last served <= first served.
            noadv=0; noadv_detail=""; noadv_served="null"; noadv_oracle="null"; noadv_gap="null"
            cutoff_a = now - advance_sec
            first_i=0; last_i=0; rcount=0
            for (i = 1; i <= nc; i++) if (cts[i] >= cutoff_a && creach[i]) {
                rcount++
                if (first_i == 0) first_i = i
                last_i = i
            }
            if (rcount >= 2) {
                span   = cts[last_i] - cts[first_i]
                needed = int(advance_sec * 3 / 4)
                fs = cserved[first_i]; ls = cserved[last_i]
                if (span >= needed && isnum(fs) && isnum(ls) && (ls + 0) <= (fs + 0)) {
                    noadv = 1
                    noadv_detail = "canonical no net advance: served_height " fs "->" ls \
                        " over " span "s (>= " needed "s of " advance_sec "s window), " \
                        rcount " reachable samples, zero climb"
                    noadv_served = ntok(cserved[last_i])
                    noadv_oracle = ntok(coracle[last_i])
                    noadv_gap    = ntok(cgap[last_i])
                }
            }

            # emit active conditions in a fixed order (self-watchdog first)
            if (stale)   printf "PAGE\tstale_ledger\t%s\t%s\t%s\t%s\n", stale_detail, last_served, last_oracle, last_gap
            if (unreach) printf "PAGE\tcanonical_unreachable\t%s\t%s\t%s\t%s\n", unreach_detail, last_served, last_oracle, last_gap
            if (noadv)   printf "PAGE\tcanonical_no_advance\t%s\t%s\t%s\t%s\n", noadv_detail, noadv_served, noadv_oracle, noadv_gap

            printf "SUMMARY\tcanonical_samples=%d newest_canonical_age=%s stale=%d unreachable=%d no_advance=%d\n", \
                nc, (nc > 0 ? (now - newest_c_ts) "s" : "na"), stale, unreach, noadv
        }
    ' "$LEDGER_FILE"
}

# announce_or_dedup <now> <cond> <detail> <served> <oracle> <gap>: throttled
# announcement. Appends a page line + wall broadcast + WARN only when the last
# announcement of this condition is older than RENOTIFY_SEC (or never made);
# otherwise it is a silent no-op (the caller's exit 1 is the standing page).
announce_or_dedup() {
    local now="$1" cond="$2" detail="$3" served="$4" oracle="$5" gap="$6"
    local statefile="$PAGE_STATE_DIR/$cond.last"
    local last_epoch=0 since
    if [ -f "$statefile" ]; then
        last_epoch="$(<"$statefile")"
        case "$last_epoch" in ''|*[!0-9]*) last_epoch=0 ;; esac
    fi
    since=$(( now - last_epoch ))
    if [ -f "$statefile" ] && [ "$since" -lt "$RENOTIFY_SEC" ]; then
        echo "slo-page: WARN condition=$cond ACTIVE but deduped (last announced ${since}s ago < renotify ${RENOTIFY_SEC}s)" >&2
        return 0
    fi

    local detail_full="$detail" blk
    blk="$(enrich_blocker)"
    [ -n "$blk" ] && detail_full="$detail | blocker=$blk"

    local line
    line="$(printf '{"ts":%s,"condition":%s,"detail":%s,"served_height":%s,"oracle_height":%s,"gap_vs_oracle":%s}' \
        "$now" "$(jstr "$cond")" "$(jstr "$detail_full")" \
        "$(jtok "$served")" "$(jtok "$oracle")" "$(jtok "$gap")")"
    append_line "$PAGES_FILE" "$line" || true

    mkdir -p "$PAGE_STATE_DIR"
    printf '%s\n' "$now" > "$statefile"

    if [ "$WALL" != "0" ]; then
        printf 'zclassic23 SLO PAGE [%s]: %s\n' "$cond" "$detail_full" | wall 2>/dev/null || true
    fi
    echo "slo-page: WARN PAGE condition=$cond detail=$detail_full" >&2
    return 0
}

# ── evaluate ────────────────────────────────────────────────────────────

cmd_evaluate() {
    mkdir -p "$LEDGER_DIR"
    local now="${ZCL_SLO_NOW:-$(date +%s)}"

    local CONDS=()
    local -A DETAIL=() SERVED=() ORACLE=() GAP=()
    local SUMMARY_TEXT=""

    if [ ! -s "$LEDGER_FILE" ]; then
        # Self-watchdog top priority: no ledger at all means the prober never
        # wrote (dead or never started). That is itself the loudest page.
        CONDS+=("stale_ledger")
        DETAIL["stale_ledger"]="ledger file missing or empty at $LEDGER_FILE (prober has written no sample; it may be dead or never started)"
        SERVED["stale_ledger"]="null"; ORACLE["stale_ledger"]="null"; GAP["stale_ledger"]="null"
        SUMMARY_TEXT="canonical_samples=0 newest_canonical_age=na stale=1 unreachable=0 no_advance=0 reason=no_ledger_file"
    else
        local marker c1 c2 c3 c4 c5
        while IFS=$'\t' read -r marker c1 c2 c3 c4 c5; do
            case "$marker" in
                PAGE)
                    CONDS+=("$c1")
                    DETAIL["$c1"]="$c2"; SERVED["$c1"]="$c3"; ORACLE["$c1"]="$c4"; GAP["$c1"]="$c5"
                    ;;
                SUMMARY) SUMMARY_TEXT="$c1" ;;
            esac
        done < <(evaluate_ledger "$now")
    fi

    if [ "${#CONDS[@]}" -gt 0 ]; then
        local cond
        for cond in "${CONDS[@]}"; do
            announce_or_dedup "$now" "$cond" "${DETAIL[$cond]}" "${SERVED[$cond]}" "${ORACLE[$cond]}" "${GAP[$cond]}"
        done
        echo "slo-page: PAGE active=${CONDS[*]} | $SUMMARY_TEXT" >&2
        exit 1
    fi

    echo "slo-page: OK no active page conditions | $SUMMARY_TEXT"
    exit 0
}

# ── selftest (hermetic; tmp ledger dirs + fixed clock, no live nodes) ──

st_fail() { echo "selftest: FAIL $*" >&2; exit 1; }

# st_line <file> <instance> <ts> <reachable 0|1> <served|null> <oracle|null> <gap|null>
st_line() {
    local f="$1" inst="$2" ts="$3" reach="$4" served="$5" oracle="$6" gap="$7"
    printf '{"ts":%d,"instance":"%s","rpcport":1,"datadir":"/x","reachable":%s,"served_height":%s,"header_height":%s,"latency_ms":1,"oracle_height":%s,"max_height":%s,"gap_vs_max":0,"gap_vs_oracle":%s,"error_detail":""}\n' \
        "$ts" "$inst" "$( [ "$reach" = 1 ] && echo true || echo false )" "$served" "$served" "$oracle" "$oracle" "$gap" >> "$f"
}

cmd_selftest() {
    local base f p k ts served now now2 rc before
    # ST_TMP stays global (not local): the EXIT trap runs after this function
    # returns, where a local would be out of scope and trip set -u.
    ST_TMP="$(mktemp -d /tmp/zcl-slo-page-selftest.XXXXXX)"
    trap 'rm -rf "$ST_TMP"' EXIT
    base=1700000000

    # Shared throttle knobs for the no-advance cluster (b/c/d): a big stale
    # threshold and a big advance window so pushing "now" forward past the
    # renotify interval re-announces the SAME condition rather than tripping
    # a different one.
    local ENV_NA="ZCL_SLO_PAGE_WALL=0 ZCL_SLO_NODE_BIN=/nonexistent/zclassic23 ZCL_SLO_PAGE_STALE_SEC=10000 ZCL_SLO_PAGE_UNREACH_SEC=600 ZCL_SLO_PAGE_ADVANCE_SEC=4000 ZCL_SLO_PAGE_RENOTIFY_SEC=100"

    # (a) advancing canonical => rc 0, no pages.jsonl.
    mkdir -p "$ST_TMP/a"; f="$ST_TMP/a/uptime-ledger.jsonl"
    for k in 0 1 2 3 4 5 6 7; do
        ts=$((base + k * 500)); served=$((10 + k))
        st_line "$f" canonical "$ts" 1 "$served" 100 $((100 - served))
    done
    now=$((base + 7 * 500))
    rc=0
    env ZCL_SLO_LEDGER_DIR="$ST_TMP/a" ZCL_SLO_NOW="$now" $ENV_NA \
        bash "$SELF" evaluate >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 0 ] || st_fail "case=advancing expected rc 0 got $rc"
    [ ! -f "$ST_TMP/a/pages.jsonl" ] || st_fail "case=advancing expected NO pages.jsonl"
    echo "selftest: ok case=advancing"

    # (b) no-advance => rc 1 + exactly one page line containing canonical_no_advance.
    mkdir -p "$ST_TMP/b"; f="$ST_TMP/b/uptime-ledger.jsonl"
    for k in 0 1 2 3 4 5 6 7; do
        ts=$((base + k * 500))
        st_line "$f" canonical "$ts" 1 10 100 90
    done
    now=$((base + 7 * 500))
    p="$ST_TMP/b/pages.jsonl"
    rc=0
    env ZCL_SLO_LEDGER_DIR="$ST_TMP/b" ZCL_SLO_NOW="$now" $ENV_NA \
        bash "$SELF" evaluate >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || st_fail "case=no-advance expected rc 1 got $rc"
    [ -f "$p" ] || st_fail "case=no-advance expected pages.jsonl"
    [ "$(wc -l < "$p")" -eq 1 ] || { cat "$p" >&2; st_fail "case=no-advance expected exactly 1 page line"; }
    grep -q 'canonical_no_advance' "$p" || { cat "$p" >&2; st_fail "case=no-advance missing condition id"; }
    echo "selftest: ok case=no-advance"

    # (c) immediate rerun => rc 1 and pages.jsonl UNCHANGED (dedup).
    before="$(<"$p")"
    rc=0
    env ZCL_SLO_LEDGER_DIR="$ST_TMP/b" ZCL_SLO_NOW="$now" $ENV_NA \
        bash "$SELF" evaluate >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || st_fail "case=dedup expected rc 1 got $rc"
    [ "$(wc -l < "$p")" -eq 1 ] || { cat "$p" >&2; st_fail "case=dedup pages.jsonl line count changed"; }
    [ "$(<"$p")" = "$before" ] || { cat "$p" >&2; st_fail "case=dedup pages.jsonl content changed"; }
    echo "selftest: ok case=dedup"

    # (d) rerun with now pushed past RENOTIFY => second page line appended.
    now2=$((now + 200))    # renotify=100
    rc=0
    env ZCL_SLO_LEDGER_DIR="$ST_TMP/b" ZCL_SLO_NOW="$now2" $ENV_NA \
        bash "$SELF" evaluate >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || st_fail "case=renotify expected rc 1 got $rc"
    [ "$(wc -l < "$p")" -eq 2 ] || { cat "$p" >&2; st_fail "case=renotify expected 2 page lines"; }
    echo "selftest: ok case=renotify"

    # (e) stale ledger (present but newest sample far in the past) => rc 1 + stale_ledger.
    mkdir -p "$ST_TMP/e"; f="$ST_TMP/e/uptime-ledger.jsonl"
    st_line "$f" canonical "$base" 1 5 100 95
    now=$((base + 100000))
    p="$ST_TMP/e/pages.jsonl"
    rc=0
    env ZCL_SLO_LEDGER_DIR="$ST_TMP/e" ZCL_SLO_NOW="$now" ZCL_SLO_PAGE_WALL=0 \
        ZCL_SLO_NODE_BIN=/nonexistent/zclassic23 ZCL_SLO_PAGE_STALE_SEC=300 \
        bash "$SELF" evaluate >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || st_fail "case=stale expected rc 1 got $rc"
    grep -q 'stale_ledger' "$p" || { cat "$p" >&2; st_fail "case=stale missing stale_ledger"; }
    echo "selftest: ok case=stale-ledger"

    # (f) all-unreachable streak => rc 1 + canonical_unreachable.
    mkdir -p "$ST_TMP/f"; f="$ST_TMP/f/uptime-ledger.jsonl"
    for k in 0 1 2 3; do
        ts=$((base + k * 60))
        st_line "$f" canonical "$ts" 0 null null null
    done
    now=$((base + 3 * 60))
    p="$ST_TMP/f/pages.jsonl"
    rc=0
    env ZCL_SLO_LEDGER_DIR="$ST_TMP/f" ZCL_SLO_NOW="$now" ZCL_SLO_PAGE_WALL=0 \
        ZCL_SLO_NODE_BIN=/nonexistent/zclassic23 ZCL_SLO_PAGE_STALE_SEC=300 \
        ZCL_SLO_PAGE_UNREACH_SEC=600 ZCL_SLO_PAGE_ADVANCE_SEC=7200 \
        bash "$SELF" evaluate >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || st_fail "case=unreachable expected rc 1 got $rc"
    grep -q 'canonical_unreachable' "$p" || { cat "$p" >&2; st_fail "case=unreachable missing condition id"; }
    echo "selftest: ok case=unreachable"

    # (g) missing ledger file entirely => rc 1 + stale_ledger.
    p="$ST_TMP/g/pages.jsonl"
    rc=0
    env ZCL_SLO_LEDGER_DIR="$ST_TMP/g" ZCL_SLO_NOW="$base" ZCL_SLO_PAGE_WALL=0 \
        ZCL_SLO_NODE_BIN=/nonexistent/zclassic23 \
        bash "$SELF" evaluate >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || st_fail "case=missing-ledger expected rc 1 got $rc"
    grep -q 'stale_ledger' "$p" || { cat "$p" >&2; st_fail "case=missing-ledger missing stale_ledger"; }
    echo "selftest: ok case=missing-ledger"

    echo "selftest: PASS"
}

# ── dispatch ─────────────────────────────────────────────────────────

case "${1:-evaluate}" in
    evaluate)   shift || true; cmd_evaluate "$@" ;;
    --selftest) shift; cmd_selftest "$@" ;;
    *)
        echo "usage: slo_page_if_stalled.sh [evaluate] | --selftest" >&2
        exit 2
        ;;
esac
