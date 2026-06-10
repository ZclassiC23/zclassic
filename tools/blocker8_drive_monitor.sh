#!/bin/sh
# blocker8_drive_monitor.sh — diagnostic ONLY (throwaway copy). Drives the
# wedge-crossing (backfill_header_solutions -> rebuild_recent) on a COPY node and
# captures rich state every tick until the node dies or the tip climbs past the
# ~900-block destabilization point. NEVER calls dumpstate (that RPC segfaults).
#
# Usage: tools/blocker8_drive_monitor.sh <DEST_DATADIR> <RPCPORT> <P2PPORT>
set -u
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
DEST="$1"; RPCPORT="$2"; P2PPORT="$3"
BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"
CAP="$DEST/blocker8_capture.jsonl"
LOG="$DEST/repro_node.log"
rpc() { ZCL_DATADIR="$DEST" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null; }
tip() { rpc getblockcount | tr -dc '0-9-'; }

[ -x "$BIN" ] || { echo "[drive] node binary not found: $BIN" >&2; exit 2; }
[ -x "$RPC_BIN" ] || { echo "[drive] rpc binary not found: $RPC_BIN" >&2; exit 2; }

echo "[drive] launching copy node on $RPCPORT/$P2PPORT"
rm -f "$DEST/zclassic23.pid" "$DEST/.cookie" "$DEST/.lock" 2>/dev/null
"$BIN" -datadir="$DEST" -rpcport="$RPCPORT" -port="$P2PPORT" -nobgvalidation > "$LOG" 2>&1 &
NODE_PID=$!
echo "[drive] node pid=$NODE_PID"

# wait for RPC
up=0
for i in $(seq 1 40); do
  t="$(tip)"; if [ -n "$t" ]; then echo "[drive] RPC up ~$((i*3))s tip=$t"; up=1; break; fi
  kill -0 "$NODE_PID" 2>/dev/null || { echo "[drive] node died during boot"; break; }
  sleep 3
done
[ "$up" = 1 ] || { echo "[drive] RPC never came up"; exit 2; }

echo "[drive] backfill_header_solutions ..."
rpc backfill_header_solutions 3134304 | head -c 300; echo
echo "[drive] rebuild_recent 3134304 (backgrounded; client may time out, server continues) ..."
( rpc rebuild_recent 3134304 > "$DEST/rebuild_recent.out" 2>&1 ) &

echo "[drive] monitoring (tick=12s, up to ~30min, stop if tip>=3136500 or node dies)"
START=$(date +%s)
maxtip=-1
while :; do
  now=$(date +%s); el=$((now-START))
  if ! kill -0 "$NODE_PID" 2>/dev/null; then
    echo "[drive] *** NODE DIED at elapsed=${el}s maxtip=$maxtip ***"
    break
  fi
  t="$(tip)"; t="${t:--1}"
  [ "$t" -gt "$maxtip" ] 2>/dev/null && maxtip="$t"
  sd="$(rpc getsyncdetail)"
  sdg="$(rpc getsyncdiag)"
  # one capture line: elapsed, tip, plus the rich JSON blobs
  printf '{"el":%s,"tip":%s,"maxtip":%s,"getsyncdetail":%s,"getsyncdiag":%s}\n' \
    "$el" "$t" "$maxtip" "${sd:-null}" "${sdg:-null}" >> "$CAP"
  # console heartbeat
  ch="$(printf '%s' "$sd" | grep -oE '"height":[0-9]+' | head -1)"
  cond="$(printf '%s' "$sdg" | grep -oE '"active_conditions":[0-9]+')"
  echo "[t=${el}s] tip=$t maxtip=$maxtip $ch $cond"
  [ "$t" -ge 3136500 ] 2>/dev/null && { echo "[drive] reached target tip=$t — climb sustained past death point"; break; }
  [ "$el" -ge 1800 ] && { echo "[drive] 30min cap reached maxtip=$maxtip"; break; }
  sleep 12
done

echo "==================== POST-MORTEM ===================="
echo "[drive] final maxtip=$maxtip  node alive: $(kill -0 "$NODE_PID" 2>/dev/null && echo yes || echo no)"
echo "[drive] crash_log.txt:"; cat "$DEST/crash_log.txt" 2>/dev/null | head -40 || echo "(none)"
echo "[drive] last 40 log lines:"; tail -40 "$LOG"
echo "[drive] condition/integrity/coins/finalize/FATAL/reset/wipe markers:"
grep -iE 'condition|integrity|coins_best|finaliz|operator_needed|FATAL|reset_chain|wipe|signal 11|reorg' "$LOG" | tail -40
echo "[drive] capture file: $CAP ($(wc -l < "$CAP" 2>/dev/null) ticks)"
