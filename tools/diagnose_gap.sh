#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# diagnose_gap.sh — one-shot THREE-ORTHOGONAL-VIEWS live-truth dump + decision
# tree for a stalled / lagging chain. This is the "live truth BEFORE design"
# default first move that kills the bodies-vs-coins class of misdiagnosis
# (a stall was once diagnosed as "missing block bodies -> body-fetch" when the
# bodies were present and the real cause was coins-application lag; that wrong
# turn cost a full design cycle — see the speed-up-our-process forensics).
#
# The three orthogonal views it composes (numbers that must agree at tip):
#   (A) active public tip      getblockcount / getsyncdiag.chain_height
#   (H) best-known header tip   getsyncdiag.best_header_height
#   (C) applied coins tip       node_state['cec.coins_best_block_height']
#   (D) HAVE_DATA at A+1         blocks.status & 8  (is the next body on disk?)
# plus the operational mode (dumpstate service_state) and active Conditions.
#
# It reads ONLY via the node's RPC surface (zcl-rpc): getsyncdiag, getblockcount,
# dumpstate, and dbquery (SELECT-only over node.db). It never writes. Point it
# at a running node (live) or at a repro copy by setting the port:
#
#   make diagnose-gap SLUG=mystall                 # live node, :18232
#   ZCL_RPCPORT=18299 ZCL_DATADIR=$COPY tools/diagnose_gap.sh slug   # a copy
#
# Output: a human banner with a verdict + the most likely root-cause class, and
# the full JSON written to $ZCL_DATADIR/diagnoses/<timestamp>-<slug>.json.
set -eu

SLUG="${1:-adhoc}"
RPCPORT="${ZCL_RPCPORT:-18232}"
DATADIR="${ZCL_DATADIR:-$HOME/.zclassic-c23}"
RPC="${ZCL_RPC_TOOL:-build/bin/zcl-rpc}"
[ -x "$RPC" ] || RPC="./build/bin/zcl-rpc"

export ZCL_RPCPORT="$RPCPORT" ZCL_DATADIR="$DATADIR"

if [ ! -f "$DATADIR/.cookie" ]; then
    echo "diagnose_gap: node not running (no cookie at $DATADIR/.cookie)." >&2
    echo "  Start the node, or run against a repro copy:" >&2
    echo "    ZCL_RPCPORT=<copy-port> ZCL_DATADIR=<copy-dir> tools/diagnose_gap.sh $SLUG" >&2
    exit 1
fi

rpc() { "$RPC" "$@" 2>/dev/null || true; }

SYNCDIAG="$(rpc getsyncdiag)"
SVCSTATE="$(rpc dumpstate service_state)"
ACTIVE="$(rpc getblockcount | tr -dc '0-9-' )"
[ -n "$ACTIVE" ] || ACTIVE=-1
COINS="$(rpc dbquery "SELECT value FROM node_state WHERE key='cec.coins_best_block_height'")"
NEXT=$((ACTIVE + 1))
HAVENEXT="$(rpc dbquery "SELECT status FROM blocks WHERE height=$NEXT")"

OUTDIR="$DATADIR/diagnoses"
mkdir -p "$OUTDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$OUTDIR/$STAMP-$SLUG.json"

json_num() {
    printf '%s' "$1" | grep -oE "\"$2\"[[:space:]]*:[[:space:]]*-?[0-9]+" |
        head -1 | sed -E 's/.*:[ ]*//' || true
}
json_str() {
    printf '%s' "$1" | grep -oE "\"$2\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" |
        head -1 | sed -E 's/.*:[ ]*"//; s/"$//' || true
}
first_int() {
    printf '%s' "$1" | grep -oE -- '-?[0-9]+' | head -1 || true
}

header=$(json_num "$SYNCDIAG" best_header_height)
chain_h=$(json_num "$SYNCDIAG" chain_height)
A="$ACTIVE"
if [ -z "$A" ] || [ "$A" -lt 0 ]; then
    A="${chain_h:-}"
fi
C=$(first_int "$COINS")
status_next=$(first_int "$HAVENEXT")
nexth=$(first_int "$NEXT")
sync_state=$(json_str "$SYNCDIAG" sync_state)
svc_mode=$(json_str "$SVCSTATE" state)
svc_reason=$(json_str "$SVCSTATE" reason)
active_conditions=$(json_num "$SYNCDIAG" active_conditions)
unresolved_conditions=$(json_num "$SYNCDIAG" unresolved_conditions)
[ -n "$active_conditions" ] || active_conditions=0
[ -n "$unresolved_conditions" ] || unresolved_conditions=0

have_data_next=false
if [ -n "$status_next" ] && [ $((status_next & 8)) -ne 0 ]; then
    have_data_next=true
fi

H="$header"
gap_hdr=""
if [ -n "$H" ] && [ -n "$A" ]; then
    gap_hdr=$((H - A))
fi

verdict="UNKNOWN"
detail=""
if [ -z "$A" ] || [ "$A" -lt 0 ]; then
    verdict="NODE-UNREACHABLE"
    detail="getblockcount/getsyncdiag returned no tip; is RPC up on this port?"
elif [ -n "$C" ] && [ -n "$A" ] && [ "$C" -lt $((A - 1)) ]; then
    verdict="COINS-APPLICATION-LAG"
    detail="public tip A=${A} is AHEAD of applied coins C=${C} by $((A - C)). The tip is published past the coins it has actually applied -> reducer cursor/coins desync (reconcile), NOT a body gap. This is the I2/import-reset class: clamp/seed tip_finalize at coins_best, never delete tip_finalize_log rows."
elif [ -n "$gap_hdr" ] && [ "$gap_hdr" -gt 1 ] && [ "$have_data_next" = true ]; then
    verdict="BODIES-PRESENT-NOT-CONNECTED"
    detail="behind header tip (A=${A} < H=${H}) but the next body at ${nexth} IS on disk (HAVE_DATA). This is NOT body-fetch — the reducer/activation is not connecting present bodies. Check cursors/activation, not downloads. (This is the exact bodies-vs-coins misdiagnosis guard.)"
elif [ -n "$gap_hdr" ] && [ "$gap_hdr" -gt 1 ]; then
    verdict="GENUINE-BODY-GAP"
    detail="behind header tip (A=${A} < H=${H}) and the next body at ${nexth} is NOT on disk -> genuine body download needed (body-fetch / peers)."
elif [ "$active_conditions" -gt 0 ]; then
    verdict="REPAIRING"
    detail="${active_conditions} active condition(s); a named repair is in progress."
elif [ -n "$gap_hdr" ] && [ "$gap_hdr" -le 1 ] && { [ -z "$C" ] || [ "$C" -ge $((A - 1)) ]; }; then
    verdict="AT-TIP / HEALTHY"
    detail="A=${A} within 1 of header H=${H}, coins C=${C} caught up."
else
    verdict="SYNCING"
    detail="closing the gap (A=${A} H=${H} C=${C} sync_state=${sync_state})."
fi

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

js_str_or_null() {
    if [ -n "$1" ]; then
        printf '"%s"' "$(json_escape "$1")"
    else
        printf 'null'
    fi
}
js_num_or_null() {
    if [ -n "$1" ]; then
        printf '%s' "$1"
    else
        printf 'null'
    fi
}

{
    printf '{\n'
    printf '  "slug": "%s",\n' "$(json_escape "$SLUG")"
    printf '  "views": {\n'
    printf '    "active_public_tip_A": %s,\n' "$(js_num_or_null "$A")"
    printf '    "best_header_tip_H": %s,\n' "$(js_num_or_null "$H")"
    printf '    "applied_coins_tip_C": %s,\n' "$(js_num_or_null "$C")"
    printf '    "header_gap_H_minus_A": %s,\n' "$(js_num_or_null "$gap_hdr")"
    printf '    "next_height": %s,\n' "$(js_num_or_null "$nexth")"
    printf '    "next_status_raw": %s,\n' "$(js_num_or_null "$status_next")"
    printf '    "have_data_at_next_D": %s,\n' "$have_data_next"
    printf '    "sync_state": %s,\n' "$(js_str_or_null "$sync_state")"
    printf '    "service_state": %s,\n' "$(js_str_or_null "$svc_mode")"
    printf '    "service_state_reason": %s,\n' "$(js_str_or_null "$svc_reason")"
    printf '    "active_conditions": %s,\n' "$active_conditions"
    printf '    "unresolved_conditions": %s\n' "$unresolved_conditions"
    printf '  },\n'
    printf '  "verdict": "%s",\n' "$(json_escape "$verdict")"
    printf '  "detail": "%s"\n' "$(json_escape "$detail")"
    printf '}\n'
} >"$OUT"

bar="========================================================================"
printf '%s\n' "$bar"
printf '  diagnose-gap [%s]  VERDICT: %s\n' "$SLUG" "$verdict"
printf '%s\n' "$bar"
printf '  A active public tip   : %s\n' "$A"
printf '  H best header tip     : %s   (gap H-A = %s)\n' "$H" "$gap_hdr"
printf '  C applied coins tip   : %s\n' "$C"
printf '  D have_data at A+1=%s : %s (status=%s)\n' "$nexth" "$have_data_next" "$status_next"
printf '  sync_state            : %s\n' "$sync_state"
printf '  service_state         : %s (%s)\n' "$svc_mode" "$svc_reason"
printf '  conditions active/unres: %s/%s\n' "$active_conditions" "$unresolved_conditions"
printf '%s\n' "$bar"
printf '  %s\n' "$detail"
printf '%s\n' "$bar"
printf '  written: %s\n' "$OUT"
