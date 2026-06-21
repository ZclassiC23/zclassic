#!/usr/bin/env bash
# Verify the node's OWN requested self-heal completes: boot the corrupted fixture
# copy with -reindex-chainstate (what crash-only recovery asked for) and watch
# whether it rebuilds the chainstate from blocks/ to a healthy tip, or loops/fails.
# NEVER touches a live datadir or PID. Signal = node.log, not RPC.
set -u
BIN=/home/rhett/github/zclassic23/.claude/worktrees/wf_3ddd4da8-b40-2/build/bin/zclassic23
COPY=/home/rhett/.zclassic-c23-wedgecopy-autoarm
PORT=18923 ; RPCPORT=28932
LOG="$COPY/reindex-boot.log" ; NLOG="$COPY/node.log"

echo "=== [$(date +%T)] reindex-chainstate recovery test ==="
rm -f "$LOG" "$NLOG"
"$BIN" -datadir="$COPY" -port=$PORT -rpcport=$RPCPORT -connect=127.0.0.1:1 \
       -reindex-chainstate -nolegacyimport -nobgvalidation -showmetrics=0 > "$LOG" 2>&1 &
NODE=$!
echo "node pid=$NODE  stdout=$LOG  node.log=$NLOG"

DONE=0
for i in $(seq 1 180); do        # up to ~90 min
  sleep 30
  ALIVE=$(kill -0 "$NODE" 2>/dev/null && echo yes || echo NO)
  # progress markers from BOTH logs
  PROG=$(grep -aoE "Reindexing|reindex|UTXO .*h=[0-9]+|coins at h=[0-9]+|tip .*h=[0-9]+|post-restore check (FAILED|clean|passed)|integrity .* (clean|FAILED)|Initialization failed|attempt [0-9]/3|activation: .*ready|boot_done|chain_restore_finalize (failed|END)" "$LOG" "$NLOG" 2>/dev/null | tail -4)
  echo "[$i $(date +%T)] alive=$ALIVE"
  printf '%s\n' "$PROG" | sed 's/^/    /'
  if [ "$ALIVE" = NO ]; then
    echo ">>> node exited. Last 30 lines of stdout:"
    tail -30 "$LOG"
    break
  fi
  # success: integrity clean / healthy boot reached
  if grep -aqE "post-restore .* (clean|passed)|chain-integrity .* clean|reindex complete|Done loading" "$LOG" "$NLOG" 2>/dev/null; then
    echo ">>> integrity-clean / load-done marker seen — likely recovered."
    DONE=1 ; sleep 20 ; break
  fi
done

echo "=== [$(date +%T)] FINAL ==="
echo "--- alive: $(kill -0 "$NODE" 2>/dev/null && echo yes || echo NO) ---"
echo "--- recovery-relevant lines (stdout): ---"
grep -aE "Reindex|post-restore|integrity|Initialization failed|attempt [0-9]/3|activation|boot_done|tip .*h=[0-9]+|coins at h=[0-9]+|operator_needed" "$LOG" 2>/dev/null | tail -30
echo "VERDICT: $([ "$DONE" = 1 ] && echo 'RECOVERED (integrity clean)' || echo 'NOT CONFIRMED — read evidence')"
echo "(node pid=$NODE; copy at $COPY)"
