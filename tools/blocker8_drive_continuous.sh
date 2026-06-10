#!/bin/sh
# blocker8_drive_continuous.sh — diagnostic ONLY (throwaway copy). Repeatedly
# feeds the reducer pipeline (backfill_header_solutions + rebuild_recent windows)
# so the tip keeps climbing past the header-solution frontier toward the prior
# ~900-block destabilization point, letting the separate monitor capture state.
# Stops when tip >= TARGET, the node dies, or ROUNDS exhausted.
set -u
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
DEST="$1"; RPCPORT="$2"
TARGET="${3:-3136600}"
ROUNDS="${4:-40}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"
[ -x "$RPC_BIN" ] || { echo "$RPC_BIN not built (make zcl-rpc)" >&2; exit 1; }
rpc() { ZCL_DATADIR="$DEST" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null; }
tip() { rpc getblockcount | tr -dc '0-9-'; }

i=0
while [ "$i" -lt "$ROUNDS" ]; do
  i=$((i+1))
  t="$(tip)"; t="${t:--1}"
  if [ "$t" -lt 0 ] 2>/dev/null; then echo "[cont] round $i: node not answering (likely DOWN) — stopping"; break; fi
  if [ "$t" -ge "$TARGET" ] 2>/dev/null; then echo "[cont] round $i: tip=$t >= target $TARGET — done"; break; fi
  nxt=$((t+1))
  bf="$(rpc backfill_header_solutions "$nxt" | grep -oE '"filled":[0-9]+')"
  echo "[cont] round $i tip=$t backfill($nxt) $bf -> rebuild_recent($nxt)"
  # rebuild_recent often exceeds the client timeout; the server keeps going.
  ( rpc rebuild_recent "$nxt" > "$DEST/rebuild_recent_cont_$i.out" 2>&1 ) &
  sleep 40
done
echo "[cont] finished after $i rounds, final tip=$(tip)"
