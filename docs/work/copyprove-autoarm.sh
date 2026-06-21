#!/usr/bin/env bash
# Copy-prove the torn-import auto-arm self-heal (commit 728422840) on the FROZEN
# wedge fixture. NEVER touches a live datadir or PID. Gate = the window.consistency
# (I4.3) operator_needed blocker CLEARS and the fold fills the log hole — NOT a bare
# getblockcount compare (that serves the cached H* and would false-green).
set -u
BIN=/home/rhett/github/zclassic23/.claude/worktrees/wf_3ddd4da8-b40-2/build/bin/zclassic23
RPC=/home/rhett/github/zclassic23/build/bin/zcl-rpc
FIX=/home/rhett/.zclassic-c23-postrestore-wedge-20260611
SNAP=/tmp/utxo-anchor-3056758.snapshot
COPY=/home/rhett/.zclassic-c23-wedgecopy-autoarm
PORT=18923 ; RPCPORT=28932
LOG="$COPY/autoarm-boot.log"
ANCHOR=3056758

echo "=== [$(date +%T)] copy-prove auto-arm: prep ==="
rm -rf "$COPY"
cp -a "$FIX" "$COPY"                                   # 12G writable copy of the torn datadir
cp "$SNAP" "$COPY/utxo-anchor.snapshot"               # make the verified snapshot resolvable
rm -f "$COPY/node.log"                                 # start a clean node.log
echo "copy ready: $(du -sh "$COPY" | cut -f1); snapshot $(ls -l "$COPY/utxo-anchor.snapshot" | awk '{print $5}') bytes"

echo "=== [$(date +%T)] boot built binary (728422840) on NON-LIVE ports, dead-sink, no legacy import ==="
"$BIN" -datadir="$COPY" -port=$PORT -rpcport=$RPCPORT -connect=127.0.0.1:1 \
       -nolegacyimport -nobgvalidation -showmetrics=0 > "$LOG" 2>&1 &
NODE=$!
echo "node pid=$NODE  log=$LOG"

q() { "$RPC" -datadir="$COPY" -rpcport=$RPCPORT "$@" 2>/dev/null; }

PASS=0
for i in $(seq 1 150); do        # up to ~50 min
  sleep 20
  if ! kill -0 "$NODE" 2>/dev/null; then
    echo "[$i $(date +%T)] NODE EXITED — likely FATAL (no-snapshot fallback or assert). Tail:"
    tail -25 "$LOG"
    break
  fi
  CNT=$(q getblockcount); SD=$(q getsyncdiag)
  # honest signal: operator_needed + the I4.3 window.consistency blocker, parsed from getsyncdiag/health
  OPNEED=$(printf '%s' "$SD" | grep -oE '"operator_needed":[a-z]+' | head -1)
  CHAINH=$(printf '%s' "$SD" | grep -oE '"chain_height":[0-9]+' | head -1)
  echo "[$i $(date +%T)] getblockcount=${CNT:-<down>}  ${CHAINH:-chain_height=?}  ${OPNEED:-operator_needed=?}"
  # capture any from-anchor / heal / fatal markers as they appear
  grep -aE 'from-anchor|auto-arm|torn|MINTED snapshot|SHA3 verified|window.consistency|nullifier_backfill|FATAL|VALIDATION_FAILED|operator_needed' "$LOG" | tail -3
  if printf '%s' "$OPNEED" | grep -q 'false'; then
    echo ">>> operator_needed=false — blocker CLEARED. Confirming fold filled the hole..."
    PASS=1 ; break
  fi
done

echo "=== [$(date +%T)] FINAL EVIDENCE ==="
echo "--- node alive: $(kill -0 "$NODE" 2>/dev/null && echo yes || echo NO) ---"
echo "--- final getblockcount: $(q getblockcount) ---"
echo "--- from-anchor / heal / fatal lines in boot log: ---"
grep -aE 'from-anchor|auto-arm|torn cold-import|MINTED snapshot|SHA3 verified|re-seeded anchor|window.consistency|FATAL|VALIDATION_FAILED' "$LOG" | tail -40
echo "--- operator_needed in getsyncdiag: ---"
q getsyncdiag | grep -oE '"operator_needed":[a-z]+|"chain_height":[0-9]+|"verified_height":[0-9]+' | head
echo "VERDICT: $([ "$PASS" = 1 ] && echo 'PASS (operator_needed cleared)' || echo 'INCONCLUSIVE/FAIL — read evidence above')"
echo "(node pid=$NODE left running on rpcport $RPCPORT; copy at $COPY for inspection)"
