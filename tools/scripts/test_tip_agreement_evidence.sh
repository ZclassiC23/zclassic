#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# test_tip_agreement_evidence.sh — the hermetic proof for the off-host
# tip-hash agreement ledger (tools/scripts/tip_agreement_probe.sh) and its
# judge (tools/scripts/tip_agreement_judge.sh).
#
# Hermetic: every node reader is stubbed with a fixture command, so this
# never dials a node, never reads a datadir, and never touches the live
# instance. It runs the REAL prober and the REAL judge — the stubs replace
# only what the node would have answered.
#
# The three cases the whole design turns on are asserted SEPARATELY and by
# name, because conflating them is the defect this ledger exists to prevent:
#
#   agreement       recorded as outcome "agrees" with the witness count
#   disagreement    recorded as outcome "disagrees" with both hashes
#   could-not-ask   an unreachable or unusable source records
#                   "could-not-ask" — never a zero, never a blank, never a
#                   pass — AND the judge does not pass on a window of them
#
# The could-not-ask family is tested in five distinct shapes, four of which
# were observed against the live node while this was being written:
# a source that refuses outright, a query INTERRUPTED by the command budget
# (which returns "rows":[] — an empty result that means "we did not ask",
# not "nobody answered"), an envelope whose pager DROPPED the rows key, a
# genuinely empty result, and a cluster below the distinct-peer control.
#
# Usage:
#   test_tip_agreement_evidence.sh                 # everything
#   test_tip_agreement_evidence.sh --only recorder
#   test_tip_agreement_evidence.sh --only judge
#
# Ends with "selftest: PASS" on success (the token every sibling harness in
# tools/scripts uses and the Makefile gates on) and exits non-zero on any
# failure.

set -uo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROBE="$SCRIPT_DIR/tip_agreement_probe.sh"
JUDGE="$SCRIPT_DIR/tip_agreement_judge.sh"

ONLY="all"
case "${1:-}" in
    --only) ONLY="${2:?--only needs recorder|judge}" ;;
    '') ;;
    *) echo "usage: test_tip_agreement_evidence.sh [--only recorder|judge]" >&2; exit 2 ;;
esac

FAILED=0
ok()   { echo "  ok: $*"; }
bad()  { echo "  FAIL: $*"; FAILED=1; }

for f in "$PROBE" "$JUDGE"; do
    if [ ! -r "$f" ]; then
        echo "selftest: FAIL missing $f" >&2
        echo "selftest: this harness proves the off-host tip-hash agreement ledger;" >&2
        echo "          on a tree without the recorder and judge it must fail, and does." >&2
        exit 1
    fi
done

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zcl-tip-agreement-selftest.XXXXXX")" || {
    echo "selftest: FAIL could not mktemp" >&2; exit 1; }
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT

NOW=2000000000
HASH_OURS="00000413d24a748e127ed332680da715e676b9a3d60c2f9d4b0dd592113a8a05"
HASH_THEM="000006b9193728f7b4cc399428202a38bde9c41220dd28a3164be7bee87a16c5"
HEIGHT=3197949

# ── fixture builders ───────────────────────────────────────────────────

# envelope <columns-json> <rows-json> <row_count> <interrupted>
envelope() {
    printf '{"schema":"zcl.result.v1","command":"core.storage.query","ok":true,"status":"passed","data_schema":"zcl.storage_query.v1","data":{"columns":%s,"rows":%s,"row_count":%s,"truncated":false,"interrupted":%s,"elapsed_ms":1,"_page":{"view":"normal","total_fields":7,"included":7,"truncated":false}}}' \
        "$1" "$2" "$3" "$4"
}

# sql_stub <file> <cluster-rows-json> <cluster-count> <interrupted>
# Dispatches on the statement the prober exported as ZCL_PARITY_SQL:
#   GROUP BY        -> the cluster aggregate (the case under test)
#   SELECT DISTINCT -> the winning cluster's hosts (address-group column)
#   otherwise       -> the peers_usable count
sql_stub() {
    local f="$1" rows="$2" n="$3" interrupted="$4"
    cat >"$f" <<STUB
#!/usr/bin/env bash
s="\${ZCL_PARITY_SQL:-}"
case "\$s" in
  *"GROUP BY"*)
    printf '%s' '$(envelope '["h","t","n"]' "$rows" "$n" "$interrupted")' ;;
  *"SELECT DISTINCT"*)
    printf '%s' '$(envelope '["host"]' '[["203.0.113.7"],["198.51.100.9"]]' 2 false)' ;;
  *)
    printf '%s' '$(envelope '["n"]' '[[9]]' 1 false)' ;;
esac
STUB
    chmod +x "$f"
}

state_stub() {
    printf '{"subsystem":"network_monitor","state":{"ready":true,"num_peers":21,"peers_with_height":20,"our_height":%s,"tip_clusters":[]}}' "$1"
}

# run_probe <ledger-dir> <sql-cmd> <hash-cmd> [ts] [extra env assignments...]
run_probe() {
    local dir="$1" sqlcmd="$2" hashcmd="$3" ts="${4:-$NOW}"
    if [ "$#" -ge 4 ]; then shift 4; else shift 3; fi
    env "ZCL_PARITY_LEDGER_DIR=$dir" \
        "ZCL_PARITY_NOW=$ts" \
        "ZCL_PARITY_SQL_CMD=$sqlcmd" \
        "ZCL_PARITY_HASH_CMD=$hashcmd" \
        "ZCL_PARITY_STATE_CMD=printf '%s' '$(state_stub "$HEIGHT")'" \
        "ZCL_PARITY_NODE_BIN=" "ZCL_PARITY_RPC_BIN=" \
        "$@" \
        bash "$PROBE" collect >/dev/null 2>&1
}

ledger_of() { printf '%s/agreement-ledger.jsonl' "$1"; }

# ── recorder cases ─────────────────────────────────────────────────────

recorder_cases() {
    echo "tip-agreement: recorder cases"

    # R1 — AGREEMENT. Two distinct remote hosts report HASH_OURS at HEIGHT
    # and our node holds HASH_OURS there.
    local d="$TMP/r1"; sql_stub "$TMP/r1.sh" \
        "[[$HEIGHT,\"$HASH_OURS\",2],[$((HEIGHT - 1)),\"$HASH_THEM\",1]]" 2 false
    run_probe "$d" "$TMP/r1.sh" "printf '{\"result\":\"$HASH_OURS\",\"error\":null,\"id\":1}'"
    local L; L="$(ledger_of "$d")"
    if grep -q '"outcome":"agrees"' "$L" &&
       grep -q "\"height\":$HEIGHT," "$L" &&
       grep -q '"modal_remote_peers":2,' "$L" &&
       grep -q "\"our_tip_hash\":\"$HASH_OURS\"" "$L" &&
       grep -q '"clusters_seen":2,' "$L" &&
       grep -q '"min_distinct_peers":2,' "$L"; then
        ok "agreement recorded as outcome=agrees with 2 distinct remote peers"
    else
        cat "$L" >&2; bad "R1 agreement not recorded correctly"
    fi
    # The address-group column must be derived, not invented: two hosts in
    # different /16s must read as 2 groups.
    grep -q '"modal_remote_groups":2,' "$L" \
        || { cat "$L" >&2; bad "R1 modal_remote_groups not derived from distinct hosts"; }
    # The R1 fixture also carries a ONE-witness cluster holding a different
    # hash at HEIGHT-1. It must be recorded honestly (disagreeing_peers 1),
    # and it must NOT contest the sample: the distinct-peer control is
    # symmetric, so one remote host can neither mint agreement nor hold the
    # verdict red by shouting a hash nobody else reports.
    if grep -q '"disagreeing_peers":1,"contested_peers":0,' "$L" &&
       grep -q '"rival_heights_unresolved":0,"heights_above_tip":0,' "$L"; then
        ok "a one-witness rival is recorded but does not contest (control is symmetric)"
    else
        cat "$L" >&2; bad "R1 one-witness rival accounting wrong"
    fi

    # R2 — DISAGREEMENT. The same two remote hosts agree with each other on
    # HASH_THEM at HEIGHT; we hold HASH_OURS.
    d="$TMP/r2"; sql_stub "$TMP/r2.sh" "[[$HEIGHT,\"$HASH_THEM\",2]]" 1 false
    run_probe "$d" "$TMP/r2.sh" "printf '{\"result\":\"$HASH_OURS\",\"error\":null,\"id\":1}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"disagrees"' "$L" &&
       grep -q "\"our_tip_hash\":\"$HASH_OURS\",\"modal_remote_hash\":\"$HASH_THEM\"" "$L" &&
       grep -q '"disagreeing_peers":2,"contested_peers":2,' "$L" &&
       grep -q "\"disagreeing_hashes\":\[{\"height\":$HEIGHT,\"hash\":\"$HASH_THEM\",\"peers\":2}\]" "$L"; then
        ok "disagreement recorded as outcome=disagrees with both hashes and the rival count"
    else
        cat "$L" >&2; bad "R2 disagreement not recorded correctly"
    fi

    # R3 — UNREACHABLE SOURCE. The peer-observation read refuses outright.
    # This must be could-not-ask, and must NOT be a zero that reads like
    # "nobody disagreed".
    d="$TMP/r3"
    run_probe "$d" "exit 1" "printf '{\"result\":\"$HASH_OURS\"}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"could-not-ask"' "$L" &&
       grep -q '"reason":"remote_observations_unavailable"' "$L" &&
       grep -q '"modal_remote_peers":null' "$L" &&
       grep -q '"height":null' "$L" &&
       ! grep -q '"outcome":"agrees"' "$L"; then
        ok "unreachable source recorded as could-not-ask with null (not zero) witness fields"
    else
        cat "$L" >&2; bad "R3 unreachable source not recorded as could-not-ask"
    fi

    # R4 — INTERRUPTED QUERY. Observed live: the command budget kills the
    # scan and the reply carries "rows":[] with interrupted:true. An empty
    # row set here means WE DID NOT ASK, not "no peer answered".
    d="$TMP/r4"; sql_stub "$TMP/r4.sh" '[]' 0 true
    run_probe "$d" "$TMP/r4.sh" "printf '{\"result\":\"$HASH_OURS\"}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"could-not-ask"' "$L" &&
       grep -q '"error_detail":"query_interrupted_budget_exceeded"' "$L"; then
        ok "an INTERRUPTED query recorded as could-not-ask, never as silence"
    else
        cat "$L" >&2; bad "R4 interrupted query not distinguished from an empty result"
    fi

    # R5 — PAGED ENVELOPE. Observed live: a large result comes back with
    # the rows key dropped entirely and only _page.truncated saying so.
    d="$TMP/r5"
    cat >"$TMP/r5.sh" <<'STUB'
#!/usr/bin/env bash
printf '%s' '{"schema":"zcl.result.v1","ok":true,"data":{"columns":["h","t","n"],"_page":{"view":"normal","total_fields":7,"included":1,"truncated":true,"next_cursor":1}}}'
STUB
    chmod +x "$TMP/r5.sh"
    run_probe "$d" "$TMP/r5.sh" "printf '{\"result\":\"$HASH_OURS\"}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"could-not-ask"' "$L" &&
       grep -q '"error_detail":"envelope_paged_rows_key_absent"' "$L"; then
        ok "an envelope whose pager dropped the rows key recorded as could-not-ask"
    else
        cat "$L" >&2; bad "R5 paged envelope not recorded as could-not-ask"
    fi

    # R6 — GENUINELY EMPTY. The query ran and no peer reported any hash.
    # Still not agreement: zero peers is not consensus.
    d="$TMP/r6"; sql_stub "$TMP/r6.sh" '[]' 0 false
    run_probe "$d" "$TMP/r6.sh" "printf '{\"result\":\"$HASH_OURS\"}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"could-not-ask"' "$L" &&
       grep -q '"reason":"no_hash_with_min_distinct_peers_2"' "$L" &&
       grep -q '"clusters_seen":0,' "$L"; then
        ok "an empty-but-honest result recorded as could-not-ask, never as agreement"
    else
        cat "$L" >&2; bad "R6 empty result not recorded as could-not-ask"
    fi

    # R7 — THE CONTROL. ONE remote host reports exactly the hash we hold.
    # If a single peer could mint agreement this line would read "agrees";
    # it must not.
    d="$TMP/r7"; sql_stub "$TMP/r7.sh" "[[$HEIGHT,\"$HASH_OURS\",1]]" 1 false
    run_probe "$d" "$TMP/r7.sh" "printf '{\"result\":\"$HASH_OURS\",\"error\":null,\"id\":1}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"could-not-ask"' "$L" &&
       grep -q '"reason":"no_hash_with_min_distinct_peers_2"' "$L" &&
       ! grep -q '"outcome":"agrees"' "$L"; then
        ok "a SINGLE peer reporting our own hash cannot manufacture agreement"
    else
        cat "$L" >&2; bad "R7 the distinct-peer control did not hold"
    fi

    # R8 — OUR SIDE UNAVAILABLE. Remote evidence qualifies but our node
    # cannot answer for that height (we are behind, or the RPC is down).
    # Being behind is neither agreement nor disagreement.
    d="$TMP/r8"; sql_stub "$TMP/r8.sh" "[[$HEIGHT,\"$HASH_OURS\",3]]" 1 false
    run_probe "$d" "$TMP/r8.sh" "echo '{\"result\":null,\"error\":{\"code\":-8,\"message\":\"Block height out of range\"}}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"could-not-ask"' "$L" &&
       grep -q "\"reason\":\"our_hash_unavailable_at_height_$HEIGHT\"" "$L" &&
       grep -q '"our_tip_hash":""' "$L"; then
        ok "our own hash being unavailable recorded as could-not-ask, not as a match"
    else
        cat "$L" >&2; bad "R8 missing local hash not recorded as could-not-ask"
    fi

    # R10 — the operator-owned-peer filter. Measured live: the only remote
    # host surfacing tip hashes was the operator's own second server, on
    # three connections. Excluding it must (a) reach the SQL as a NOT IN
    # clause, (b) be recorded in the sample, and (c) be able to turn an
    # apparent agreement into could-not-ask — the filter narrows evidence,
    # it never manufactures it.
    d="$TMP/r10"
    cat >"$TMP/r10.sh" <<STUB
#!/usr/bin/env bash
s="\${ZCL_PARITY_SQL:-}"
case "\$s" in
  *"NOT IN ('198.51.100.9')"*) ;;
  *) echo "stub: exclusion clause missing from: \$s" >&2; exit 1 ;;
esac
case "\$s" in
  *"GROUP BY"*) printf '%s' '$(envelope '["h","t","n"]' '[]' 0 false)' ;;
  *)            printf '%s' '$(envelope '["n"]' '[[0]]' 1 false)' ;;
esac
STUB
    chmod +x "$TMP/r10.sh"
    run_probe "$d" "$TMP/r10.sh" "printf '{\"result\":\"$HASH_OURS\"}'" "$NOW" \
        "ZCL_PARITY_EXCLUDE_HOSTS=198.51.100.9"
    L="$(ledger_of "$d")"
    if grep -q '"excluded_hosts":1,' "$L" &&
       grep -q '"outcome":"could-not-ask"' "$L" &&
       grep -q '"reason":"no_hash_with_min_distinct_peers_2"' "$L"; then
        ok "an excluded operator-owned host is filtered in SQL and recorded"
    else
        cat "$L" >&2; bad "R10 excluded-host filter not applied/recorded"
    fi

    # R11 — the sanitiser: a host list carrying SQL punctuation must reach
    # the statement stripped, never as syntax.
    d="$TMP/r11"
    cat >"$TMP/r11.sh" <<STUB
#!/usr/bin/env bash
s="\${ZCL_PARITY_SQL:-}"
case "\$s" in
  *"');"*|*"DROP TABLE"*) echo "stub: unsanitised list reached SQL: \$s" >&2; exit 1 ;;
esac
case "\$s" in
  *"NOT IN ('1.2.3.4DROPTABLEblocks--')"*) ;;
  *) echo "stub: sanitised exclusion clause absent from: \$s" >&2; exit 1 ;;
esac
case "\$s" in
  *"GROUP BY"*) printf '%s' '$(envelope '["h","t","n"]' '[]' 0 false)' ;;
  *)            printf '%s' '$(envelope '["n"]' '[[0]]' 1 false)' ;;
esac
STUB
    chmod +x "$TMP/r11.sh"
    run_probe "$d" "$TMP/r11.sh" "printf '{\"result\":\"$HASH_OURS\"}'" "$NOW" \
        "ZCL_PARITY_EXCLUDE_HOSTS=1.2.3.4'); DROP TABLE blocks --"
    L="$(ledger_of "$d")"
    if [ -s "$L" ] && grep -q '"outcome":"could-not-ask"' "$L"; then
        ok "an excluded-host list is sanitised before it reaches SQL"
    else
        cat "$L" 2>/dev/null >&2; bad "R11 exclusion list sanitiser did not hold"
    fi

    # R12 — THE MASKED TIP FORK. This is the defect that shipped in the
    # first cut of the recorder and it is the reason contested_peers exists.
    # A 3-witness cluster sits at an already-settled height (we match it, of
    # course) while a 2-witness cluster holds a DIFFERENT block at our own
    # tip. The old rival loop skipped every cluster whose height was not the
    # winner's, so the line read
    #     "outcome":"agrees","disagreeing_peers":0,"disagreeing_hashes":[]
    # and a window of them graded PASS while we were on another chain.
    d="$TMP/r12"; sql_stub "$TMP/r12.sh" \
        "[[$((HEIGHT - 9)),\"$HASH_OURS\",3],[$HEIGHT,\"$HASH_THEM\",2]]" 2 false
    run_probe "$d" "$TMP/r12.sh" "printf '{\"result\":\"$HASH_OURS\"}'"
    L="$(ledger_of "$d")"
    if grep -q "\"height\":$((HEIGHT - 9))," "$L" &&
       grep -q '"disagreeing_peers":2,"contested_peers":2,' "$L" &&
       grep -q "\"disagreeing_hashes\":\[{\"height\":$HEIGHT,\"hash\":\"$HASH_THEM\",\"peers\":2}\]" "$L"; then
        ok "a rival at a DIFFERENT height than the winner is counted, not skipped"
    else
        cat "$L" >&2; bad "R12 rival at another height was not recorded (masked tip fork)"
    fi

    # R13 — OUR HASH IN UPPER CASE. The extractor accepts [0-9a-fA-F] on
    # purpose, but the peer table is compared as lower(tip_hash) and the
    # comparison is a byte compare, so an upper-case RPC reply used to record
    # a FALSE "disagrees" — a fabricated fork alarm on a healthy node.
    d="$TMP/r13"; sql_stub "$TMP/r13.sh" "[[$HEIGHT,\"$HASH_OURS\",2]]" 1 false
    run_probe "$d" "$TMP/r13.sh" \
        "printf '{\"result\":\"$(printf '%s' "$HASH_OURS" | tr 'a-f' 'A-F')\"}'"
    L="$(ledger_of "$d")"
    if grep -q '"outcome":"agrees"' "$L" && ! grep -q '"outcome":"disagrees"' "$L"; then
        ok "an upper-case hash from our own RPC does not fabricate a disagreement"
    else
        cat "$L" >&2; bad "R13 hash case sensitivity produced a false disagreement"
    fi

    # R14 — A HEIGHT ABOVE OUR TIP. Peers ahead of us is the normal state of
    # a syncing node. It is neither a rival nor an unresolved check: it is
    # recorded as heights_above_tip so the sample stays clean.
    # (the at-tip cluster carries more witnesses so IT is the winner; a tie
    # would break to the higher height and make the above-tip cluster the
    # thing being compared, which is a different case.)
    d="$TMP/r14"; sql_stub "$TMP/r14.sh" \
        "[[$HEIGHT,\"$HASH_OURS\",3],[$((HEIGHT + 3)),\"$HASH_THEM\",2]]" 2 false
    run_probe "$d" "$TMP/r14.sh" "printf '{\"result\":\"$HASH_OURS\"}'"
    L="$(ledger_of "$d")"
    if grep -q '"contested_peers":0,"rival_heights_unresolved":0,"heights_above_tip":1,' "$L" &&
       grep -q '"outcome":"agrees"' "$L"; then
        ok "a cluster above our own tip is recorded as behind-ness, not as a rival"
    else
        cat "$L" >&2; bad "R14 above-tip cluster mis-classified"
    fi

    # R15 — A HEIGHT WE SHOULD HAVE AND CANNOT ANSWER. Below our tip, the
    # node will not produce a hash. That is UNKNOWN and must be recorded as
    # unresolved — never folded into "no rival at that height".
    d="$TMP/r15"; sql_stub "$TMP/r15.sh" \
        "[[$HEIGHT,\"$HASH_OURS\",2],[$((HEIGHT - 4)),\"$HASH_THEM\",2]]" 2 false
    run_probe "$d" "$TMP/r15.sh" \
        "if [ \"\$ZCL_PARITY_HEIGHT\" = \"$HEIGHT\" ]; then printf '{\"result\":\"$HASH_OURS\"}'; else printf '{\"result\":null,\"error\":{\"code\":-8}}'; fi"
    L="$(ledger_of "$d")"
    if grep -q '"disagreeing_peers":0,"contested_peers":0,"rival_heights_unresolved":1,' "$L"; then
        ok "an unanswerable height at or below our tip records as unresolved, not as zero rivals"
    else
        cat "$L" >&2; bad "R15 unresolved height was folded into 'no rivals'"
    fi

    # R16 — THE HOST KEY REACHES SQL IN THE GUARDED FORM. The distinctness
    # control is only as good as the expression that derives a host from an
    # addr, and the naive rtrim(rtrim(addr,'0-9'),':') splits ONE machine
    # into TWO witnesses twice over: it eats the last octet of a portless
    # "1.2.3.4", and it keys the bracketed IPv6 that -addnode produces
    # (net_service_to_string) differently from the unbracketed IPv6 that
    # addrman and every inbound peer produce (p2p_node_create's fallback).
    # Both were confirmed against the node's own SQLite. The port may only
    # be stripped when those trailing digits are actually preceded by a
    # colon, brackets must be removed, and the key must be lowercased.
    d="$TMP/r16"
    cat >"$TMP/r16.sh" <<'STUB'
#!/usr/bin/env bash
s="${ZCL_PARITY_SQL:-}"
case "$s" in
  *"rtrim(rtrim(addr,'0123456789'),':')"*)
      echo "stub: naive host key reached SQL (splits one host into two): $s" >&2; exit 1 ;;
esac
case "$s" in
  *"substr(rtrim(addr,'0123456789'),-1)=':'"*) ;;
  *) echo "stub: host key is not guarded on a preceding colon: $s" >&2; exit 1 ;;
esac
case "$s" in
  *"trim("*"'[]')"*) ;;
  *) echo "stub: host key does not unbracket: $s" >&2; exit 1 ;;
esac
case "$s" in
  *"lower("*) ;;
  *) echo "stub: host key is not lowercased: $s" >&2; exit 1 ;;
esac
printf '%s' '{"schema":"zcl.result.v1","ok":true,"data":{"columns":["h","t","n"],"rows":[],"row_count":0,"truncated":false,"interrupted":false}}'
STUB
    chmod +x "$TMP/r16.sh"
    run_probe "$d" "$TMP/r16.sh" "printf '{\"result\":\"$HASH_OURS\"}'"
    L="$(ledger_of "$d")"
    if [ -s "$L" ] && grep -q '"reason":"no_hash_with_min_distinct_peers_2"' "$L"; then
        ok "the distinct-host key reaches SQL guarded, unbracketed and lowercased"
    else
        cat "$L" 2>/dev/null >&2; bad "R16 host key shape wrong (one machine could count as two witnesses)"
    fi

    # R9 — the ledger is append-only across runs and every line is one
    # complete JSON object (the judge reads it line by line).
    d="$TMP/r9"; sql_stub "$TMP/r9.sh" "[[$HEIGHT,\"$HASH_OURS\",2]]" 1 false
    local i
    for i in 1 2 3; do
        run_probe "$d" "$TMP/r9.sh" \
            "printf '{\"result\":\"$HASH_OURS\"}'" "$((NOW - i * 600))"
    done
    L="$(ledger_of "$d")"
    if [ "$(wc -l < "$L")" -eq 3 ] &&
       [ "$(grep -c '^{.*}$' "$L")" -eq 3 ] &&
       [ "$(grep -c '"outcome":"agrees"' "$L")" -eq 3 ]; then
        ok "three runs append three complete, independently parseable lines"
    else
        cat "$L" >&2; bad "R9 ledger append shape wrong"
    fi
}

# ── judge cases ────────────────────────────────────────────────────────

# jrow <ts> <outcome> [peers] [control] [contested] [unresolved]
jrow() {
    local ts="$1" outcome="$2" peers="${3:-2}" ctl="${4:-2}"
    local contested="${5:-0}" unres="${6:-0}"
    local pnum="$peers"
    case "$outcome" in could-not-ask) pnum="null" ;; esac
    printf '{"ts":%s,"instance":"canonical","rpcport":18232,"datadir":"/x","window_secs":900,"min_distinct_peers":%s,"our_height":%s,"height":%s,"our_tip_hash":"%s","modal_remote_hash":"%s","modal_remote_peers":%s,"modal_remote_groups":2,"disagreeing_peers":%s,"contested_peers":%s,"rival_heights_unresolved":%s,"heights_above_tip":0,"disagreeing_hashes":[],"peers_total":21,"peers_with_height":20,"peers_usable":9,"clusters_seen":2,"outcome":"%s","reason":"fixture","error_detail":""}\n' \
        "$ts" "$ctl" "$HEIGHT" "$HEIGHT" "$HASH_OURS" "$HASH_OURS" "$pnum" \
        "$contested" "$contested" "$unres" "$outcome"
}

# jrow_legacy <ts> — a row in the pre-rival-scan shape: it says "agrees" and
# carries no contested_peers / rival_heights_unresolved keys at all. A
# recorder that never looked for rivals cannot testify that there were none,
# so the judge must refuse to count it.
jrow_legacy() {
    printf '{"ts":%s,"min_distinct_peers":2,"modal_remote_peers":2,"disagreeing_peers":0,"outcome":"agrees","reason":"legacy"}\n' "$1"
}

# jcase <name> <ledger> <want-token> <want-rc> [judge flags...]
jcase() {
    local name="$1" ledger="$2" want="$3" wantrc="$4"; shift 4
    local out rc tok
    out="$(env "ZCL_PARITY_JUDGE_NOW=$NOW" bash "$JUDGE" "$ledger" "$@" 2>&1)"
    rc=$?
    tok="$(printf '%s' "$out" | sed -n 's/.*VERDICT=\([A-Z_]*\).*/\1/p' | head -n1)"
    if [ "$tok" = "$want" ] && [ "$rc" = "$wantrc" ]; then
        ok "$name -> $tok (rc=$rc)"
    else
        bad "$name -> got tok='$tok' rc=$rc, wanted '$want' rc=$wantrc"
        echo "        out: $out"
    fi
}

judge_cases() {
    echo "tip-agreement: judge cases"

    # J1 — a healthy day: 8 agreeing samples across the window.
    local f="$TMP/j_pass.jsonl"; : >"$f"
    local i
    for i in 1 2 3 4 5 6 7 8; do jrow "$((NOW - i * 600))" agrees >>"$f"; done
    jcase "8 agreeing samples in a fresh window" "$f" PASS 0

    # J2 — the case this judge exists for: the recorder ran all day and
    # never got a comparable pair. NOT a pass.
    f="$TMP/j_cna.jsonl"; : >"$f"
    for i in $(seq 1 20); do jrow "$((NOW - i * 600))" could-not-ask >>"$f"; done
    jcase "an all-could-not-ask window is NO_EVIDENCE, never PASS" "$f" NO_EVIDENCE 1

    # J3 — the timer died two days ago holding a green ledger.
    f="$TMP/j_stale.jsonl"; : >"$f"
    for i in $(seq 1 8); do jrow "$((NOW - 172800 - i * 600))" agrees >>"$f"; done
    jcase "a ledger whose newest sample is 2 days old is STALE" "$f" STALE 2

    # J4 — no ledger at all.
    jcase "a missing ledger is STALE" "$TMP/does-not-exist.jsonl" STALE 2

    # J4b — a ledger with lines but none carrying a ts.
    f="$TMP/j_garbage.jsonl"; printf 'not json\n{"nope":1}\n' >"$f"
    jcase "a ledger with no parseable ts is STALE" "$f" STALE 2

    # J4c — a far-future timestamp must not make a dead ledger look fresh.
    # The stale rows are two days old; the bogus row is a year ahead.
    f="$TMP/j_future.jsonl"; : >"$f"
    for i in $(seq 1 8); do jrow "$((NOW - 172800 - i * 600))" agrees >>"$f"; done
    jrow "$((NOW + 31536000))" agrees >>"$f"
    jcase "a far-future stamp cannot refresh a stale ledger" "$f" STALE 2

    # J5 — one disagreement in an otherwise perfect window still fails, and
    # fails as DISAGREE (the named cause), not as a volume problem.
    f="$TMP/j_dis.jsonl"; : >"$f"
    for i in $(seq 1 12); do jrow "$((NOW - i * 600))" agrees >>"$f"; done
    jrow "$((NOW - 300))" disagrees >>"$f"
    jcase "a single disagreement fails as DISAGREE" "$f" DISAGREE 1

    # J6 — three agreeing samples is not a day of agreement.
    f="$TMP/j_thin.jsonl"; : >"$f"
    for i in 1 2 3; do jrow "$((NOW - i * 600))" agrees >>"$f"; done
    jcase "3 agreeing samples is THIN_EVIDENCE" "$f" THIN_EVIDENCE 1

    # J7 — the anti-weakening rule: a ledger written by a prober whose own
    # control was 1 cannot mint a PASS however many rows it has.
    f="$TMP/j_weak.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - i * 600))" agrees 2 1 >>"$f"; done
    jcase "a ledger written with a weakened control is THIN_EVIDENCE" "$f" THIN_EVIDENCE 1

    # J8 — an agreeing sample backed by one witness poisons the window.
    f="$TMP/j_onepeer.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - i * 600))" agrees >>"$f"; done
    jrow "$((NOW - 60))" agrees 1 >>"$f"
    jcase "one single-witness agreeing sample is THIN_EVIDENCE" "$f" THIN_EVIDENCE 1

    # J9 — THE WINDOW IS THE CLAIM. Yesterday's agreement plus a fresh
    # stretch of could-not-ask must not read as agreement today.
    f="$TMP/j_window.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - 90000 - i * 600))" agrees >>"$f"; done
    for i in $(seq 1 10); do jrow "$((NOW - i * 120))" could-not-ask >>"$f"; done
    jcase "old agreement + fresh silence is NO_EVIDENCE (the window is graded)" \
        "$f" NO_EVIDENCE 1

    # J10 — and the mirror image, which is what proves this is not a
    # tail -n 1 judge: the LAST line is could-not-ask and the window still
    # passes on the agreeing samples inside it.
    f="$TMP/j_lastline.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - i * 600))" agrees >>"$f"; done
    jrow "$((NOW - 60))" could-not-ask >>"$f"
    jcase "a could-not-ask LAST line does not sink a good window" "$f" PASS 0

    # J11 — an outcome this judge does not know is never quietly bucketed.
    f="$TMP/j_mal.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - i * 600))" agrees >>"$f"; done
    jrow "$((NOW - 60))" probably-fine >>"$f"
    jcase "an unrecognised outcome is MALFORMED" "$f" MALFORMED 1

    # J12 — the control cannot be argued away on the command line.
    f="$TMP/j_pass.jsonl"
    jcase "--min-distinct-peers=1 is refused outright" "$f" "" 2 --min-distinct-peers=1

    # J13 — end to end: a ledger the REAL prober wrote, graded by the REAL
    # judge. Nothing hand-written in the middle.
    local d="$TMP/j_e2e"; sql_stub "$TMP/j_e2e.sh" "[[$HEIGHT,\"$HASH_OURS\",2]]" 1 false
    for i in $(seq 1 8); do
        run_probe "$d" "$TMP/j_e2e.sh" \
            "printf '{\"result\":\"$HASH_OURS\"}'" "$((NOW - i * 600))"
    done
    jcase "prober-written ledger judged end to end" "$(ledger_of "$d")" PASS 0

    # J15 — a window of samples that each said "agrees" while remote hosts
    # meeting the control held a different block at some height. Before
    # contested_peers existed this window was a clean PASS.
    f="$TMP/j_contested.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - i * 600))" agrees 3 2 2 >>"$f"; done
    jcase "agreement carrying contesting remote hosts is DISAGREE" "$f" DISAGREE 1

    # J16 — ROW COUNT IS NOT DURATION. Eight agreeing samples one second
    # apart satisfied --min-agree 6 before --min-span-secs existed, so one
    # moment sampled eight times graded as a day of repeated agreement.
    f="$TMP/j_burst.jsonl"; : >"$f"
    for i in $(seq 1 8); do jrow "$((NOW - i))" agrees >>"$f"; done
    jcase "8 agreeing samples one second apart is THIN_EVIDENCE, not a day" \
        "$f" THIN_EVIDENCE 1

    # J17 — the knobs may only TIGHTEN. Each of these was accepted before,
    # and --window-hours/--max-age-secs together turned an almost year-old
    # ledger into PASS.
    f="$TMP/j_pass.jsonl"
    jcase "--window-hours above the shipped 24 is refused"  "$f" "" 2 --window-hours=87600
    jcase "--max-age-secs above the shipped 1800 is refused" "$f" "" 2 --max-age-secs=999999999
    jcase "--max-disagree above the shipped 0 is refused"    "$f" "" 2 --max-disagree=1000
    jcase "--min-agree below the shipped 6 is refused"       "$f" "" 2 --min-agree=1
    jcase "--min-span-secs below the shipped 2700 is refused" "$f" "" 2 --min-span-secs=0
    # and tightening is still allowed
    jcase "tightening the window is accepted"                "$f" PASS 0 --window-hours=24 --min-agree=8

    # J18 — a ledger written by a recorder that never looked for rivals.
    # Its "agrees" rows carry no contested_peers key; absence is UNKNOWN, so
    # they cannot prove there was no disagreement and cannot mint a PASS.
    f="$TMP/j_legacy.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow_legacy "$((NOW - i * 600))" >>"$f"; done
    jcase "a pre-rival-scan ledger cannot pass (missing key is unknown, not zero)" \
        "$f" NO_EVIDENCE 1

    # J19 — a SPLICED line: a torn append that a later append landed on top
    # of, so one physical line carries two outcome keys. Reading the first
    # and grading the rest is how a Frankenstein row becomes evidence.
    f="$TMP/j_splice.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - i * 600))" agrees >>"$f"; done
    printf '{"ts":%s,"instance":"cano' "$((NOW - 300))" >>"$f"
    jrow "$((NOW - 60))" agrees >>"$f"
    jcase "a spliced (torn + overwritten) line is MALFORMED" "$f" MALFORMED 1

    # J20 — samples the recorder could not fully check do not count toward
    # the accrual claim, and a window made only of them is not agreement.
    f="$TMP/j_unres.jsonl"; : >"$f"
    for i in $(seq 1 10); do jrow "$((NOW - i * 600))" agrees 2 2 0 1 >>"$f"; done
    jcase "a window of unverifiable agreements is NO_EVIDENCE" "$f" NO_EVIDENCE 1

    # J14 — the same end-to-end path with an unusable source: the prober
    # writes could-not-ask, the judge refuses. This is the pair the whole
    # design rests on and it is asserted as a pair.
    d="$TMP/j_e2e_cna"
    for i in $(seq 1 8); do
        run_probe "$d" "exit 1" "printf '{\"result\":\"$HASH_OURS\"}'" "$((NOW - i * 600))"
    done
    jcase "prober-written could-not-ask ledger is refused end to end" \
        "$(ledger_of "$d")" NO_EVIDENCE 1
}

case "$ONLY" in
    recorder) recorder_cases ;;
    judge)    judge_cases ;;
    all)      recorder_cases; judge_cases ;;
    *) echo "selftest: FAIL unknown --only '$ONLY'" >&2; exit 2 ;;
esac

if [ "$FAILED" -ne 0 ]; then
    echo "selftest: FAIL" >&2
    exit 1
fi
echo "selftest: PASS"
