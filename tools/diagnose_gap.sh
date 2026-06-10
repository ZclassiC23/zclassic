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

SYNCDIAG="$SYNCDIAG" SVCSTATE="$SVCSTATE" ACTIVE="$ACTIVE" COINS="$COINS" \
HAVENEXT="$HAVENEXT" NEXT="$NEXT" SLUG="$SLUG" OUT="$OUT" python3 - <<'PY'
import json, os, re, sys

def jload(s):
    try: return json.loads(s)
    except Exception: return {}

def first_int(s):
    if s is None: return None
    if isinstance(s, (int,)): return s
    m = re.search(r'-?\d+', str(s))
    return int(m.group()) if m else None

def dig(d, *keys):
    """find the first present key anywhere in a nested dict/list."""
    want = set(keys)
    stack = [d]
    while stack:
        cur = stack.pop()
        if isinstance(cur, dict):
            for k, v in cur.items():
                if k in want and isinstance(v, (int, float, str)):
                    iv = first_int(v)
                    if iv is not None: return iv
                stack.append(v)
        elif isinstance(cur, list):
            stack.extend(cur)
    return None

syncdiag = jload(os.environ.get('SYNCDIAG', '{}'))
svcstate = jload(os.environ.get('SVCSTATE', '{}'))
active   = first_int(os.environ.get('ACTIVE'))
coins    = first_int(jload(os.environ.get('COINS','')) if os.environ.get('COINS','').strip().startswith(('[','{')) else os.environ.get('COINS'))
havenext_raw = os.environ.get('HAVENEXT','')
status_next  = first_int(jload(havenext_raw) if havenext_raw.strip().startswith(('[','{')) else havenext_raw)
nexth    = first_int(os.environ.get('NEXT'))

header = dig(syncdiag, 'best_header_height')
chain_h = dig(syncdiag, 'chain_height')
if active is None or active < 0:
    active = chain_h
sync_state = None
svc_mode = None
for d in (syncdiag, svcstate):
    if sync_state is None: sync_state = _ = None
# pull strings explicitly
def find_str(d, key):
    stack=[d]
    while stack:
        c=stack.pop()
        if isinstance(c,dict):
            for k,v in c.items():
                if k==key and isinstance(v,str): return v
                stack.append(v)
        elif isinstance(c,list): stack.extend(c)
    return None
sync_state = find_str(syncdiag,'sync_state')
svc_mode   = find_str(svcstate,'state')
svc_reason = find_str(svcstate,'reason')
active_conditions   = dig(syncdiag, 'active_conditions') or 0
unresolved_conditions = dig(syncdiag, 'unresolved_conditions') or 0

have_data_next = (status_next is not None and (status_next & 8) != 0)

# ── decision tree ────────────────────────────────────────────────────────
verdict = "UNKNOWN"
detail  = ""
A, H, C = active, header, coins
gap_hdr = (H - A) if (H is not None and A is not None) else None

if A is None or A < 0:
    verdict = "NODE-UNREACHABLE"
    detail  = "getblockcount/getsyncdiag returned no tip; is RPC up on this port?"
elif C is not None and A is not None and C < A - 1:
    verdict = "COINS-APPLICATION-LAG"
    detail  = (f"public tip A={A} is AHEAD of applied coins C={C} by {A-C}. The tip "
               "is published past the coins it has actually applied -> reducer "
               "cursor/coins desync (reconcile), NOT a body gap. This is the I2/"
               "import-reset class: clamp/seed tip_finalize at coins_best, never "
               "delete tip_finalize_log rows.")
elif gap_hdr is not None and gap_hdr > 1 and have_data_next:
    verdict = "BODIES-PRESENT-NOT-CONNECTED"
    detail  = (f"behind header tip (A={A} < H={H}) but the next body at {nexth} IS "
               "on disk (HAVE_DATA). This is NOT body-fetch — the reducer/activation "
               "is not connecting present bodies. Check cursors/activation, not "
               "downloads. (This is the exact bodies-vs-coins misdiagnosis guard.)")
elif gap_hdr is not None and gap_hdr > 1 and not have_data_next:
    verdict = "GENUINE-BODY-GAP"
    detail  = (f"behind header tip (A={A} < H={H}) and the next body at {nexth} is "
               "NOT on disk -> genuine body download needed (body-fetch / peers).")
elif active_conditions and active_conditions > 0:
    verdict = "REPAIRING"
    detail  = f"{active_conditions} active condition(s); a named repair is in progress."
elif gap_hdr is not None and gap_hdr <= 1 and (C is None or C >= A - 1):
    verdict = "AT-TIP / HEALTHY"
    detail  = f"A={A} within 1 of header H={H}, coins C={C} caught up."
else:
    verdict = "SYNCING"
    detail  = f"closing the gap (A={A} H={H} C={C} sync_state={sync_state})."

result = {
    "slug": os.environ.get('SLUG'),
    "views": {
        "active_public_tip_A": A,
        "best_header_tip_H": H,
        "applied_coins_tip_C": C,
        "header_gap_H_minus_A": gap_hdr,
        "next_height": nexth,
        "next_status_raw": status_next,
        "have_data_at_next_D": have_data_next,
        "sync_state": sync_state,
        "service_state": svc_mode,
        "service_state_reason": svc_reason,
        "active_conditions": active_conditions,
        "unresolved_conditions": unresolved_conditions,
    },
    "verdict": verdict,
    "detail": detail,
}

with open(os.environ['OUT'], 'w') as f:
    json.dump(result, f, indent=2)

bar = "=" * 72
print(bar)
print(f"  diagnose-gap [{result['slug']}]  VERDICT: {verdict}")
print(bar)
v = result["views"]
print(f"  A active public tip   : {v['active_public_tip_A']}")
print(f"  H best header tip     : {v['best_header_tip_H']}   (gap H-A = {v['header_gap_H_minus_A']})")
print(f"  C applied coins tip   : {v['applied_coins_tip_C']}")
print(f"  D have_data at A+1={v['next_height']} : {v['have_data_at_next_D']} (status={v['next_status_raw']})")
print(f"  sync_state            : {v['sync_state']}")
print(f"  service_state         : {v['service_state']} ({v['service_state_reason']})")
print(f"  conditions active/unres: {v['active_conditions']}/{v['unresolved_conditions']}")
print(bar)
print("  " + detail.replace("\n", "\n  "))
print(bar)
print(f"  written: {os.environ['OUT']}")
PY
