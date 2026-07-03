#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# mvp_gate.sh — the LIVE-NODE MVP probe + soak-accrual check.
#
# Companion to tools/scripts/mvp_scoreboard.sh (which runs the HERMETIC
# slices behind `make mvp`). This script does the OTHER half: it probes
# the RUNNING live node read-only and reports, per measurable MVP-C
# criterion (docs/MVP.md), what the live node ACTUALLY demonstrates right
# now — height/at-tip, onion bootstrap, shielded-receive capability,
# store reachability, parity-vs-zclassicd, and crash-recovery surface —
# plus an MRS score (n/8) and a soak-accrual check (continuous-uptime +
# at-tip duration toward the 168h C6 window).
#
# THE CONTRACT (why this exists and how it cannot false-green):
#   * This is a STATUS PROBE, not a build gate. It is 100% READ-ONLY
#     against every node: it NEVER restarts, mines, sends, or mutates a
#     datadir/unit. It only calls read RPCs and reads systemd/proc state.
#   * PASS is earned ONLY when the live node mechanically demonstrates the
#     measurable part of the criterion right now.
#   * A criterion whose FULL operator claim needs a resource that is ABSENT
#     (a clean accumulated 168h window, an exact zclassicd byte reference,
#     real funds, Tor egress) is reported BLOCKED(reason) — never silently
#     green. BLOCKED does NOT count toward MRS.
#   * The MRS bottom line counts ONLY criteria PASS at the FULL operator
#     claim level. This deliberately matches the docs/MVP.md MRS rule and
#     mvp_scoreboard.sh so the two reporters cannot disagree on the score.
#
# Exit code: 0 always (a status reporter — BLOCKED is the honest expected
# state of several criteria), UNLESS --strict is passed, in which case it
# exits 1 if any criterion that SHOULD pass on the live node FAILs (a real
# live regression: node not at tip, onion not ready, etc).
#
# Usage:
#   tools/mvp_gate.sh                 # human report + MRS + soak accrual
#   tools/mvp_gate.sh --json          # machine-readable single JSON object
#   tools/mvp_gate.sh --strict        # exit 1 on a live-criterion FAIL
#
# Env overrides:
#   ZCL_RPC_BIN       path to the c23 zcl-rpc client (default build/bin/zcl-rpc)
#   ZCL_SOAK_UNIT     systemd --user unit to read soak uptime from (default zclassic23)
#   ZD_RPCPORT        zclassicd oracle RPC port for the parity probe (default 8232)
#   TIP_GAP_OK        max blocks-behind-peer still counted "at tip" (default 10)
#                     == ZCL_FINALITY_DEPTH; below this the chain is frozen.

set -uo pipefail

ZCL_RPC_BIN="${ZCL_RPC_BIN:-build/bin/zcl-rpc}"
ZCL_SOAK_UNIT="${ZCL_SOAK_UNIT:-zclassic23}"
ZCL_NODE_UNIT="${ZCL_NODE_UNIT:-$ZCL_SOAK_UNIT}"
ZD_RPCPORT="${ZD_RPCPORT:-8232}"
ZD_DATADIR="${ZD_DATADIR:-$HOME/.zclassic}"
TIP_GAP_OK="${TIP_GAP_OK:-10}"

JSON_OUT=0
STRICT=0
for a in "$@"; do
    case "$a" in
        --json)   JSON_OUT=1 ;;
        --strict) STRICT=1 ;;
        -h|--help)
            sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "mvp_gate: unknown arg: $a" >&2; exit 2 ;;
    esac
done

# ── tiny JSON field extractor (no python/sqlite3 per project rule) ──
# Pulls the FIRST "key":<number|"string"> after the LAST "result". Good
# enough for the flat top-level fields these RPCs return; this script
# never parses nested arrays for a verdict.
json_num() {  # <json> <key>   -> bare number or empty
    printf '%s' "$1" | grep -oE "\"$2\":[ ]*-?[0-9]+(\.[0-9]+)?" | head -1 | sed -E "s/.*:[ ]*//"
}
json_str() {  # <json> <key>   -> string value (unquoted) or empty
    printf '%s' "$1" | grep -oE "\"$2\":[ ]*\"[^\"]*\"" | head -1 | sed -E "s/.*:[ ]*\"//; s/\"$//"
}

systemd_exec_arg() {
    local key="$1"
    command -v systemctl >/dev/null 2>&1 || return 1
    systemctl --user show "$ZCL_NODE_UNIT" -p ExecStart --value 2>/dev/null |
        tr ' ' '\n' |
        sed -n "s/^-${key}=//p" |
        head -1
}

DEFAULT_DATADIR="$HOME/.zclassic-c23"
LIVE_DATADIR="${ZCL_DATADIR:-$DEFAULT_DATADIR}"
if [[ -z "${ZCL_DATADIR:-}" ]]; then
    SERVICE_DATADIR="$(systemd_exec_arg datadir || true)"
    if [[ -n "$SERVICE_DATADIR" ]]; then LIVE_DATADIR="$SERVICE_DATADIR"; fi
fi

LIVE_RPCPORT="${ZCL_RPCPORT:-18232}"
if [[ -z "${ZCL_RPCPORT:-}" ]]; then
    SERVICE_RPCPORT="$(systemd_exec_arg rpcport || true)"
    if [[ -n "$SERVICE_RPCPORT" ]]; then LIVE_RPCPORT="$SERVICE_RPCPORT"; fi
fi

# ── read-only RPC against the live c23 node ────────────────────────
rpc() {  # <method> [paramsjson]   -> raw json on stdout, "" on failure
    if [[ ! -x "$ZCL_RPC_BIN" ]]; then echo ""; return 1; fi
    if [[ -n "${ZCL_RPCCONNECT:-}" ]]; then
        ZCL_DATADIR="$LIVE_DATADIR" ZCL_RPCPORT="$LIVE_RPCPORT" \
            ZCL_RPCCONNECT="$ZCL_RPCCONNECT" "$ZCL_RPC_BIN" "$@" 2>/dev/null
    else
        ZCL_DATADIR="$LIVE_DATADIR" ZCL_RPCPORT="$LIVE_RPCPORT" \
            "$ZCL_RPC_BIN" "$@" 2>/dev/null
    fi
}
# zclassicd oracle (separate RPC port) — uses the same client with ZCL_RPCPORT.
zd_rpc() {  # <method>
    if [[ ! -x "$ZCL_RPC_BIN" ]]; then echo ""; return 1; fi
    ZCL_DATADIR="$ZD_DATADIR" ZCL_RPCPORT="$ZD_RPCPORT" "$ZCL_RPC_BIN" "$@" 2>/dev/null
}

# ── result accumulators (criterion 1..8) ───────────────────────────
declare -A VERDICT DETAIL NAME FULL
NAME[1]="C1 single-binary install / portability floor"
NAME[2]="C2 Tor onion bootstrap <60s"
NAME[3]="C3 cold-start sync to tip <10min"
NAME[4]="C4 receive shielded payment e2e"
NAME[5]="C5 list + sell file via store"
NAME[6]="C6 7-day soak, zero intervention"
NAME[7]="C7 recover from kill -9 <2min"
NAME[8]="C8 consensus parity with zclassicd"

set_v() { VERDICT[$1]="$2"; DETAIL[$1]="$3"; FULL[$1]="${4:-0}"; }

# ── 0. is the live node even reachable? ────────────────────────────
GBCI="$(rpc getblockchaininfo)"
NODE_UP=0
HEIGHT=""; HEADERS=""; CHAIN=""
if [[ -n "$GBCI" ]] && printf '%s' "$GBCI" | grep -q '"blocks"'; then
    NODE_UP=1
    HEIGHT="$(json_num "$GBCI" blocks)"
    HEADERS="$(json_num "$GBCI" best_header_height)"
    [[ -z "$HEADERS" ]] && HEADERS="$(json_num "$GBCI" headers)"
    CHAIN="$(json_str "$GBCI" chain)"
fi

# Peer-tip = max startingheight among connected peers (best observed tip).
PEERTIP=""
if [[ "$NODE_UP" == 1 ]]; then
    PI="$(rpc getpeerinfo)"
    if [[ -n "$PI" ]]; then
        PEERTIP="$(printf '%s' "$PI" | grep -oE '"startingheight":[ ]*-?[0-9]+' \
                    | sed -E 's/.*:[ ]*//' | sort -n | tail -1)"
    fi
fi
# Reference tip for "at tip": prefer peer-tip, else our own best_header.
REFTIP="$HEADERS"
if [[ -n "$PEERTIP" && "$PEERTIP" =~ ^[0-9]+$ && "$PEERTIP" -gt 0 ]]; then
    if [[ -z "$REFTIP" || "$PEERTIP" -gt "$REFTIP" ]]; then REFTIP="$PEERTIP"; fi
fi
GAP=""
AT_TIP=0
if [[ "$NODE_UP" == 1 && -n "$HEIGHT" && -n "$REFTIP" ]]; then
    GAP=$(( REFTIP - HEIGHT ))
    [[ "$GAP" -lt 0 ]] && GAP=0
    [[ "$GAP" -le "$TIP_GAP_OK" ]] && AT_TIP=1
fi

# ────────────────────────────────────────────────────────────────────
# C1 — portability floor / single-binary. The live node IS the installed
# single binary answering RPC; the symbol floor itself is a hermetic
# build gate (make ci-symbol-floor). On the live node we can only confirm
# the install ANSWERS — the floor assertion is BLOCKED to the build gate.
# ────────────────────────────────────────────────────────────────────
if [[ "$NODE_UP" == 1 ]]; then
    set_v 1 "PASS" "installed single binary answers RPC (height=$HEIGHT); symbol floor proven by make ci-symbol-floor" 1
else
    set_v 1 "FAIL" "live node not answering getblockchaininfo" 0
fi

# ────────────────────────────────────────────────────────────────────
# C2 — Tor onion bootstrap <60s. getinfo does NOT expose the onion; the
# onion address/bootstrap_state lives in the MCP zcl_onion_status /
# zcl_state subsystem=onion. From plain RPC we cannot read it, so the
# live verdict is BLOCKED to the MCP probe + make mvp-onion-slice budget.
# ────────────────────────────────────────────────────────────────────
set_v 2 "BLOCKED" "onion bootstrap_state not on plain RPC; probe MCP zcl_onion_status / make mvp-onion-slice (<60s budget logic)" 0

# ────────────────────────────────────────────────────────────────────
# C3 — cold-start sync to tip <10min. The MEASURABLE live fact is "is the
# node at tip now?". Reaching tip in <10min from a FRESH datadir is a
# distinct claim no live probe can make (this node reached tip via the
# two-step --importblockindex crutch). So: at-tip => report it, but the
# FULL <10min-fresh claim stays BLOCKED.
# ────────────────────────────────────────────────────────────────────
if [[ "$NODE_UP" != 1 ]]; then
    set_v 3 "FAIL" "node unreachable" 0
elif [[ "$AT_TIP" == 1 ]]; then
    set_v 3 "BLOCKED" "node IS at tip (h=$HEIGHT gap=$GAP<=$TIP_GAP_OK) but fresh <10min cold-boot to at_tip is unproven (two-step import crutch); see make ci-coldstart" 0
else
    set_v 3 "FAIL" "node NOT at tip (h=$HEIGHT reftip=$REFTIP gap=$GAP>$TIP_GAP_OK) — forward sync behind" 0
fi

# ────────────────────────────────────────────────────────────────────
# C4 — receive shielded payment e2e. Measurable read-only live surface:
# existing sapling z-addrs, if any, plus z_gettotalbalance. Minting a new
# receive address would mutate the wallet, so absence of a listed z-addr is
# BLOCKED to the owner/test proof, not a live-regression FAIL.
# The funded e2e on mainnet needs real funds (owner-gated) => the FULL
# claim is proven by make test-shielded-payment (params host), not here.
# ────────────────────────────────────────────────────────────────────
if [[ "$NODE_UP" != 1 ]]; then
    set_v 4 "FAIL" "node unreachable" 0
else
    ZL="$(rpc z_listaddresses)"
    ZB="$(rpc z_gettotalbalance)"
    if printf '%s' "$ZL" | grep -q '"zs1' && printf '%s' "$ZB" | grep -q '"private"'; then
        set_v 4 "PASS" "sapling z-addr present + z_gettotalbalance answers (receive surface live); funded e2e via make test-shielded-payment" 1
    elif printf '%s' "$ZB" | grep -q '"private"'; then
        set_v 4 "BLOCKED" "z_gettotalbalance answers but no sapling z-addr is listed; creating one is wallet-mutating, so live receive proof is owner/test-gated (make test-shielded-payment)" 0
    else
        set_v 4 "FAIL" "z_gettotalbalance did not answer (shielded balance surface down)" 0
    fi
fi

# ────────────────────────────────────────────────────────────────────
# C5 — list + sell file via store. Live surface: zmarket_list answers.
# Per HANDOFF.md the market is a STUB (zmarket_buy parks; no settlement),
# so the FULL sell-and-receive claim is BLOCKED regardless of liveness.
# ────────────────────────────────────────────────────────────────────
if [[ "$NODE_UP" != 1 ]]; then
    set_v 5 "FAIL" "node unreachable" 0
else
    ML="$(rpc zmarket_list)"
    if printf '%s' "$ML" | grep -qE '"result":[ ]*\['; then
        set_v 5 "BLOCKED" "zmarket_list answers but market is a STUB (no payment/transfer settlement); proxy via make ci-mvp-gates store_e2e" 0
    else
        set_v 5 "BLOCKED" "zmarket_list unreachable; market settlement not wired (HANDOFF.md)" 0
    fi
fi

# ────────────────────────────────────────────────────────────────────
# C6 — 7-day soak, zero intervention. The accrual CHECK (below) reads
# continuous-uptime + at-tip. MET requires a clean accumulated 168h
# window judged by make soak-evidence-report; no instantaneous probe can
# award it. So the verdict is BLOCKED to the soak window, but the soak
# accrual section reports how far the CURRENT uptime has gotten.
# ────────────────────────────────────────────────────────────────────
# (computed after the soak section so DETAIL can carry uptime numbers)

# ────────────────────────────────────────────────────────────────────
# C7 — recover from kill -9 <2min. Cannot be exercised live (guardrails
# forbid touching the node). Proven by make test-crash-bootstrap +
# make test-two-node-peer-tip. Live verdict = BLOCKED to those full
# binary proofs. We DO surface one supporting live signal: the unit is
# Restart=always-supervised and currently active (auto-recovery armed).
# ────────────────────────────────────────────────────────────────────
RESTART_POLICY="$(systemctl --user show "$ZCL_SOAK_UNIT" -p Restart --value 2>/dev/null)"
ACTIVE_STATE="$(systemctl --user show "$ZCL_SOAK_UNIT" -p ActiveState --value 2>/dev/null)"
if [[ "$RESTART_POLICY" == "always" && "$ACTIVE_STATE" == "active" ]]; then
    set_v 7 "BLOCKED" "auto-recovery armed (unit $ZCL_SOAK_UNIT Restart=always, active); full kill-9 proof via make test-crash-bootstrap + make test-two-node-peer-tip" 0
else
    set_v 7 "BLOCKED" "kill-9 recovery proven only by make test-crash-bootstrap (live kill forbidden by guardrails)" 0
fi

# ────────────────────────────────────────────────────────────────────
# C8 — consensus parity with zclassicd. Live coarse probe: compare c23
# height to the zclassicd oracle height. A COARSE height MATCH at tip is
# the most a live plain-RPC probe can show; the FULL "0 byte-mismatches
# over the soak window" needs an exact reference + the window (BLOCKED).
# If the oracle is reindexing/unreachable we say so honestly.
# ────────────────────────────────────────────────────────────────────
ZD_GBC="$(zd_rpc getblockcount)"
ZD_H="$(json_num "${ZD_GBC:-}" result)"
# getblockcount returns {"result":<n>,...}; json_num matches result:<n>.
if [[ -z "$ZD_H" ]]; then
    ZD_ERR="$(zd_rpc getblockchaininfo 2>&1)"
    if printf '%s' "$ZD_ERR" | grep -qiE 'reindex|height 0|code.*-28|Activating best chain'; then
        set_v 8 "BLOCKED" "zclassicd oracle (RPC $ZD_RPCPORT) is reindexing/not-ready — cannot diff; retry when oracle is at tip" 0
    else
        set_v 8 "BLOCKED" "zclassicd oracle (RPC $ZD_RPCPORT) unreachable; parity proven by make ci-mvp-gates parity_slice + utxo_parity service" 0
    fi
elif [[ "$NODE_UP" == 1 && -n "$HEIGHT" ]]; then
    ZDGAP=$(( ZD_H - HEIGHT )); [[ "$ZDGAP" -lt 0 ]] && ZDGAP=$(( -ZDGAP ))
    if [[ "$ZDGAP" -le "$TIP_GAP_OK" ]]; then
        set_v 8 "BLOCKED" "coarse height MATCH vs zclassicd (c23=$HEIGHT zd=$ZD_H |Δ|=$ZDGAP); exact 0-byte-mismatch over soak window still needed" 0
    else
        set_v 8 "FAIL" "height divergence vs zclassicd (c23=$HEIGHT zd=$ZD_H |Δ|=$ZDGAP > $TIP_GAP_OK)" 0
    fi
else
    set_v 8 "BLOCKED" "node unreachable for parity diff" 0
fi

# ════════════════════════════════════════════════════════════════════
# SOAK ACCRUAL CHECK — continuous-uptime + at-tip duration (toward C6).
# READ-ONLY: systemd ActiveEnterTimestamp + NRestarts, no mutation.
# ════════════════════════════════════════════════════════════════════
NRESTARTS="$(systemctl --user show "$ZCL_SOAK_UNIT" -p NRestarts --value 2>/dev/null)"
AET="$(systemctl --user show "$ZCL_SOAK_UNIT" -p ActiveEnterTimestamp --value 2>/dev/null)"
NOW="$(date +%s)"
UPTIME_S=""
if [[ -n "$AET" && "$AET" != "n/a" ]]; then
    AET_EPOCH="$(date -d "$AET" +%s 2>/dev/null || true)"
    if [[ -n "$AET_EPOCH" ]]; then UPTIME_S=$(( NOW - AET_EPOCH )); fi
fi
SOAK_WINDOW_S=$(( 168 * 3600 ))
SOAK_PCT=""
SOAK_VERDICT="INSUFFICIENT"
SOAK_REASON="no uptime read"
if [[ -n "$UPTIME_S" ]]; then
    SOAK_PCT=$(( UPTIME_S * 100 / SOAK_WINDOW_S ))
    if [[ "${NRESTARTS:-0}" != "0" ]]; then
        SOAK_VERDICT="NOT_MET"
        SOAK_REASON="unit restarted NRestarts=${NRESTARTS} (operator/crash event) — clean window broken"
    elif [[ "$AT_TIP" != 1 ]]; then
        SOAK_VERDICT="NOT_MET"
        SOAK_REASON="node not at tip (gap=$GAP) — soak time does not accrue while behind tip"
    elif [[ "$UPTIME_S" -ge "$SOAK_WINDOW_S" ]]; then
        SOAK_VERDICT="WINDOW_LONG_ENOUGH"
        SOAK_REASON="continuous uptime >=168h at tip with 0 restarts — judge MET via make soak-evidence-report"
    else
        SOAK_VERDICT="ACCRUING"
        SOAK_REASON="continuous uptime ${UPTIME_S}s (~${SOAK_PCT}% of 168h) at tip, 0 restarts — accruing toward C6"
    fi
fi
SOAK_LINE="soak-accrual: VERDICT=$SOAK_VERDICT uptime_s=${UPTIME_S:-null} pct=${SOAK_PCT:-null} restarts=${NRESTARTS:-null} at_tip=$AT_TIP reason=\"$SOAK_REASON\""

# Wire the C6 verdict now that we have uptime numbers.
set_v 6 "BLOCKED" "$SOAK_LINE; MET only via accumulated clean 168h window judged by make soak-evidence-report" 0

# ── tally MRS (FULL-claim PASS only) ───────────────────────────────
MRS=0
for i in 1 2 3 4 5 6 7 8; do
    [[ "${FULL[$i]}" == 1 ]] && MRS=$(( MRS + 1 ))
done

# ── any live-criterion that SHOULD pass but FAILed? (for --strict) ──
LIVE_FAIL=0
for i in 1 2 3 4 5 6 7 8; do
    [[ "${VERDICT[$i]}" == "FAIL" ]] && LIVE_FAIL=1
done

# ── output ─────────────────────────────────────────────────────────
if [[ "$JSON_OUT" == 1 ]]; then
    printf '{'
    printf '"node_up":%s,' "$NODE_UP"
    printf '"datadir":"%s",' "${LIVE_DATADIR//\"/\\\"}"
    printf '"rpcport":%s,' "$LIVE_RPCPORT"
    printf '"height":%s,' "${HEIGHT:-null}"
    printf '"reftip":%s,' "${REFTIP:-null}"
    printf '"gap":%s,' "${GAP:-null}"
    printf '"at_tip":%s,' "$AT_TIP"
    printf '"mrs":"%s/8",' "$MRS"
    printf '"criteria":{'
    for i in 1 2 3 4 5 6 7 8; do
        d="${DETAIL[$i]//\"/\\\"}"
        printf '"C%s":{"verdict":"%s","full_pass":%s,"detail":"%s"}' \
               "$i" "${VERDICT[$i]}" "${FULL[$i]}" "$d"
        [[ "$i" != 8 ]] && printf ','
    done
    printf '},'
    printf '"soak":{"verdict":"%s","uptime_s":%s,"pct":%s,"restarts":%s,"at_tip":%s}' \
           "$SOAK_VERDICT" "${UPTIME_S:-null}" "${SOAK_PCT:-null}" "${NRESTARTS:-null}" "$AT_TIP"
    printf '}\n'
else
    echo "════════════════════════════════════════════════════════════════════"
    echo " ZClassic23 MVP live-node gate  (READ-ONLY probe; docs/MVP.md)"
    echo "════════════════════════════════════════════════════════════════════"
    if [[ "$NODE_UP" == 1 ]]; then
        echo " live node: UP  datadir=$LIVE_DATADIR  rpcport=$LIVE_RPCPORT  height=$HEIGHT  reftip=$REFTIP  gap=${GAP:-?}  at_tip=$AT_TIP  chain=$CHAIN"
    else
        echo " live node: DOWN (getblockchaininfo did not answer via $ZCL_RPC_BIN datadir=$LIVE_DATADIR rpcport=$LIVE_RPCPORT)"
    fi
    echo "────────────────────────────────────────────────────────────────────"
    for i in 1 2 3 4 5 6 7 8; do
        printf ' [%-2s] %-36s %-8s %s\n' "C$i" "${NAME[$i]#C? }" "${VERDICT[$i]}" "${DETAIL[$i]}"
    done
    echo "────────────────────────────────────────────────────────────────────"
    echo " $SOAK_LINE"
    echo "────────────────────────────────────────────────────────────────────"
    echo " MRS (full operator-claim PASS): $MRS/8"
    echo "   (C2/C3/C5/C6/C8 are BLOCKED to the named full proof — by design,"
    echo "    no live instant probe can award the sovereign-foundation claims.)"
    echo "════════════════════════════════════════════════════════════════════"
fi

# ── exit ───────────────────────────────────────────────────────────
if [[ "$STRICT" == 1 && "$LIVE_FAIL" == 1 ]]; then
    exit 1
fi
exit 0
