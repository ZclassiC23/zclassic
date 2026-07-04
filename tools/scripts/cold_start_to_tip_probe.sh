#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# cold_start_to_tip_probe.sh — empirical C3 settle: does a FRESH datadir that
# imports the consensus snapshot then forward-syncs the header chain + body
# delta from a serving peer actually reach zcl_syncstate at_tip at the PEER
# tip within the budget? The existing cold_start gates prove only the UTXO
# IMPORT (>1M UTXOs <90s) or the FSM transitions — neither proves the full
# operator claim (fresh datadir -> phase=at_tip). This probe settles whether
# the snapshot->delta composite reaches at_tip with a wrapper, or surfaces the
# documented coins_kv-not-seeded / no-header-chain seam (needing a code fix).
#
# FULLY ISOLATED + NON-DESTRUCTIVE to the live node:
#   - /tmp-only datadir, isolated 39xxx ports (never the live 8023/18232 or the
#     soak 18242 or zclassicd 8034/8232),
#   - dials the zclassicd reference peer (P2P 8034) as a CLIENT only, so the
#     live soak node's network surface is untouched,
#   - -nolegacyimport (never reads ~/.zclassic), -nobgvalidation (lean),
#   - process-group SIGKILL teardown on every exit.
#
# Exit: 0 reached at_tip at peer-tip within budget (C3 wrapper viable)
#       3 imported snapshot but did NOT reach at_tip in budget (code seam — needs the fix)
#       2 SKIP (no snapshot fixture / no serving peer / binaries absent)
#       1 FAIL (harness/setup error)

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"
SNAPSHOT="${ZCL_C3_SNAPSHOT:-$HOME/.zclassic-c23/consensus_snapshot.db}"
PEER="${ZCL_C3_PEER:-127.0.0.1:8034}"        # zclassicd P2P — DIAL this for sync
# Read the target tip from a zclassic23 node that answers zcl-rpc cleanly (the
# live node), rather than zclassicd (whose RPC uses a different cookie). Same
# chain → same tip. This is one cheap getblockcount, no sync load on it.
TIP_DATADIR="${ZCL_C3_TIP_DATADIR:-$HOME/.zclassic-c23}"
PEER_RPC="${ZCL_C3_PEER_RPC:-18232}"         # live zclassic23 RPC (read tip only)
BUDGET="${ZCL_C3_BUDGET_SECS:-600}"          # 10-minute MVP target
P2P=39070; RPC=39071; FS=39072; HTTPS=39073

skip() { echo "c3-probe: SKIP ($*)"; exit 2; }
die()  { echo "c3-probe: FAIL: $*" >&2; exit 1; }

[ -x "$NODE_BIN" ] || skip "node binary absent: $NODE_BIN"
[ -x "$RPC_BIN" ]  || skip "zcl-rpc absent: $RPC_BIN"
[ -r "$SNAPSHOT" ] || skip "snapshot fixture absent: $SNAPSHOT"

# Reference peer tip (the target). zclassicd has no params on getblockcount.
PEER_TIP="$(ZCL_DATADIR="$TIP_DATADIR" ZCL_RPCPORT="$PEER_RPC" "$RPC_BIN" getblockcount 2>/dev/null \
            | sed -E 's/.*"result":(-?[0-9]+).*/\1/')"
printf '%s' "$PEER_TIP" | grep -qE '^[0-9]+$' || skip "tip source ($PEER_RPC) not answering getblockcount"
[ "$PEER_TIP" -gt 1000000 ] || skip "reference peer tip implausibly low ($PEER_TIP)"

DATADIR="$(mktemp -d /tmp/zcl-c3-probe.XXXXXX)" || die "mktemp datadir failed"
PID=""
cleanup() {
    [ -n "$PID" ] && kill -KILL -- "-$PID" 2>/dev/null || true
    case "$DATADIR" in /tmp/zcl-c3-probe.*) rm -rf "$DATADIR" 2>/dev/null || true ;; esac
}
trap cleanup EXIT INT TERM

# Seed ONLY the snapshot into the fresh datadir (the cold-start trigger).
echo "c3-probe: peer_tip=$PEER_TIP  budget=${BUDGET}s  datadir=$DATADIR"
echo "c3-probe: copying snapshot ($(du -h "$SNAPSHOT" | cut -f1)) ..."
cp --reflink=auto "$SNAPSHOT" "$DATADIR/consensus_snapshot.db" 2>/dev/null \
    || cp "$SNAPSHOT" "$DATADIR/consensus_snapshot.db" || die "snapshot copy failed"

echo "c3-probe: booting fresh node (snapshot import -> delta sync from $PEER) ..."
setsid "$NODE_BIN" -datadir="$DATADIR" -port=$P2P -rpcport=$RPC -fsport=$FS \
    -httpsport=$HTTPS -connect="$PEER" -nolegacyimport -nobgvalidation \
    -showmetrics=0 >"$DATADIR/probe.log" 2>&1 &
PID=$!

start=$(date +%s)
imported=0; reached=0; last_h=-1
while :; do
    now=$(date +%s); elapsed=$((now - start))
    [ "$elapsed" -ge "$BUDGET" ] && break
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "c3-probe: node EXITED early (t=${elapsed}s) — log tail:"
        tail -15 "$DATADIR/probe.log" | sed 's/^/  /'
        die "node process died before reaching at_tip"
    fi
    # getblockchaininfo is param-free and reports both blocks (connected tip)
    # and headers (header chain) — at_tip = both >= peer tip AND blocks==headers.
    bci="$(ZCL_DATADIR="$DATADIR" ZCL_RPCPORT="$RPC" "$RPC_BIN" getblockchaininfo 2>/dev/null)"
    h="$(printf '%s' "$bci"   | sed -E 's/.*"blocks":(-?[0-9]+).*/\1/')"
    hdr="$(printf '%s' "$bci" | sed -E 's/.*"headers":(-?[0-9]+).*/\1/')"
    if printf '%s' "$h" | grep -qE '^[0-9]+$'; then
        [ "$h" != "$last_h" ] && { echo "c3-probe: t=${elapsed}s blocks=$h headers=${hdr:-?}"; last_h="$h"; }
        [ "$h" -ge 1000000 ] && imported=1
        if [ "$h" -ge "$PEER_TIP" ] && printf '%s' "$hdr" | grep -qE '^[0-9]+$' \
           && [ "$hdr" -ge "$PEER_TIP" ] && [ "$h" -eq "$hdr" ]; then
            reached=1; echo "c3-probe: REACHED at_tip blocks=$h headers=$hdr in ${elapsed}s"; break
        fi
    fi
    sleep 5
done

if [ "$reached" = 1 ]; then
    echo "=== c3-probe: PASS — fresh datadir -> snapshot import -> delta sync -> at_tip@$PEER_TIP within budget ==="
    exit 0
fi
echo "c3-probe: did NOT reach at_tip in ${BUDGET}s (imported=$imported last_height=$last_h). Log tail:"
tail -20 "$DATADIR/probe.log" | sed 's/^/  /'
if [ "$imported" = 1 ]; then
    echo "=== c3-probe: SEAM — snapshot imported (>1M) but forward-sync to at_tip did NOT complete (needs the coins_kv-seed / header-wiring fix) ==="
    exit 3
fi
die "snapshot import itself did not complete (<1M utxos) in budget"
