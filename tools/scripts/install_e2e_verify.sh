#!/usr/bin/env bash
# install_e2e_verify.sh — post-install proof for the INSTALL lane fixture.
# 1. Boots the fixture datadir NORMALLY (no install verb) with offline flags.
# 2. Waits for RPC, then captures: status, dumpstate reducer_frontier,
#    core consensus utxo commitment.
# 3. Independently recomputes the SHA3-256 UTXO commitment from the installed
#    progress.kv coins table (Python, no repo code) and compares it to the
#    compiled checkpoint root.
#
# usage: install_e2e_verify.sh <fixture-datadir> <binary> <zcl-rpc-path>
set -u
DATADIR="${1:?fixture datadir}"
BINARY="${2:?candidate binary}"
ZCLRPC="${3:?zcl-rpc path}"
RPCPORT=19602
LOG="$DATADIR/verify-boot.log"
CP_ROOT="5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85"

rm -f "$DATADIR/zclassic23.pid"
echo "== booting fixture (log: $LOG) =="
setsid nohup "$BINARY" \
  -datadir="$DATADIR" \
  -allow-plaintext-wallet \
  -nobgvalidation -connect=127.0.0.1:39999 \
  -rpcport=$RPCPORT -port=19603 -fsport=19604 -httpsport=19605 \
  > "$LOG" 2>&1 < /dev/null &
BOOT_PID=$!
echo "boot pid: $BOOT_PID"

echo "== waiting for RPC (up to 300s) =="
ok=0
for i in $(seq 1 60); do
  if "$BINARY" -datadir="$DATADIR" -rpcport=$RPCPORT status >/tmp/install-e2e-status.json 2>/dev/null; then
    ok=1; break
  fi
  sleep 5
done
if [ "$ok" != 1 ]; then
  echo "FAIL: RPC never came up; last boot log lines:"; tail -20 "$LOG"; exit 1
fi

echo "== status =="
cat /tmp/install-e2e-status.json
echo
echo "== dumpstate reducer_frontier =="
"$BINARY" -datadir="$DATADIR" -rpcport=$RPCPORT dumpstate reducer_frontier
echo
echo "== core consensus utxo commitment =="
"$BINARY" -datadir="$DATADIR" -rpcport=$RPCPORT core consensus utxo commitment || true
echo

echo "== independent python recompute from installed progress.kv =="
python3 - "$DATADIR" "$CP_ROOT" <<'PYEOF'
import hashlib, sqlite3, sys
datadir, want = sys.argv[1], sys.argv[2]
db = sqlite3.connect(f"file:{datadir}/progress.kv?mode=ro&immutable=1", uri=True)
cur = db.cursor()
h = hashlib.sha3_256()
n = 0
supply = 0
for txid, vout, value, height, cb, script in cur.execute(
        "SELECT txid,vout,value,height,is_coinbase,script FROM coins "
        "ORDER BY txid,vout"):
    rec = bytearray()
    rec += txid
    rec += vout.to_bytes(4, "little")
    rec += (value % (1 << 64)).to_bytes(8, "little")
    rec += len(script).to_bytes(4, "little")
    rec += script
    rec += height.to_bytes(4, "little")
    rec += b"\x01" if cb else b"\x00"
    h.update(rec)
    n += 1
    supply += value
root = h.hexdigest()
print(f"installed coins: {n}, supply: {supply}")
print(f"recomputed utxo_root: {root}")
print(f"checkpoint root:      {want}")
print("MATCH" if root == want and n == 1354769 else "MISMATCH")
cur2 = db.cursor()
print("progress_meta:", cur2.execute(
    "SELECT key, hex(value) FROM progress_meta WHERE key IN "
    "('coins_applied_height','coins_kv_migration_complete')").fetchall())
print("cursors:", cur2.execute(
    "SELECT name,cursor FROM stage_cursor ORDER BY name").fetchall())
print("tip_finalize anchor:", cur2.execute(
    "SELECT height,status,ok,hex(tip_hash) FROM tip_finalize_log "
    "WHERE status='anchor'").fetchall())
db.close()
PYEOF

echo
echo "== installed files =="
ls -la "$DATADIR" | grep -E "preinstall|decision" || true
echo
echo "== stopping fixture node =="
"$BINARY" -datadir="$DATADIR" -rpcport=$RPCPORT stop >/dev/null 2>&1 || true
sleep 5
pkill -f "datadir=$DATADIR" 2>/dev/null || true
echo DONE
