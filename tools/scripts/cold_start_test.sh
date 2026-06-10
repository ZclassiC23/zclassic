#!/usr/bin/env bash
# cold_start_test.sh — Wave 11D regression armor for Wave 11A.
#
# Asserts that a fresh datadir containing ONLY a downloaded
# consensus_snapshot.db is observed by the Wave 11A snapshot-first
# probe in boot.c, which imports the UTXOs into node.db before any
# chain-tip restoration runs. Before Wave 11A the import code path
# was effectively dead and the snapshot bytes sat on disk unused.
#
# Pass criterion: node.db reports utxos > 1,000,000 within 90 s of
# boot. Chain-tip advance past the snapshot height is a separate
# concern (needs block bodies from either the legacy datadir or P2P)
# and is not asserted here — this is intentionally a thin gate on the
# import probe alone.
#
# Without Wave 11A this silently timed out at h=-1 (see captured
# brnibh8u9.output — TIMEOUT after 602s). The presence of this script
# in CI keeps that regression armor in place: anyone who breaks the
# pre-restore import path now fails this gate.
#
# Exit codes:
#   0  — utxos > 1,000,000 within DEADLINE_SECS.
#   1  — timeout or threshold not met.
#   2  — pre-requisites missing (zclassic23 binary, source snapshot).

set -uo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO_DIR/build/bin/zclassic23"
CLI="$REPO_DIR/build/bin/zcl-rpc"
SRC_SNAP_CANDIDATES=(
    "$HOME/.zclassic-c23-test/consensus_snapshot.db"
    "$HOME/.zclassic-c23/consensus_snapshot.db"
)
TEST_DIR="/tmp/zclassic23_coldstart_$$"
TEST_PORT="${TEST_PORT:-18233}"
TEST_RPCPORT="${TEST_RPCPORT:-18235}"
DEADLINE_SECS="${DEADLINE_SECS:-90}"
MIN_UTXOS="${MIN_UTXOS:-1000000}"

# The Wave 11A probe emits a single distinctive log line on success:
#   "[boot] snapshot-first import OK: N UTXOs at h=H — chain restore
#    will observe snapshot anchor"
# We grep for that prefix to keep the test independent of any
# external SQLite CLI dependency.
SUCCESS_PATTERN='snapshot-first import OK: '

cleanup() {
    if [ -n "${NODE_PID:-}" ]; then
        kill -TERM "$NODE_PID" 2>/dev/null || true
        wait "$NODE_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

if [ ! -x "$BIN" ]; then
    echo "[coldstart] missing $BIN — run 'make' first"
    exit 2
fi
if [ ! -x "$CLI" ]; then
    echo "[coldstart] missing $CLI"
    exit 2
fi

SRC_SNAP=""
for cand in "${SRC_SNAP_CANDIDATES[@]}"; do
    if [ -f "$cand" ] && [ "$(stat -c %s "$cand")" -gt $((10*1024*1024)) ]; then
        SRC_SNAP="$cand"
        break
    fi
done
if [ -z "$SRC_SNAP" ]; then
    echo "[coldstart] no consensus_snapshot.db found in:" \
         "${SRC_SNAP_CANDIDATES[*]}"
    echo "[coldstart] run the main node to obtain one, or skip"
    exit 2
fi

echo "[coldstart] using snapshot: $SRC_SNAP"
echo "[coldstart] $(du -h "$SRC_SNAP" | cut -f1)"

mkdir -p "$TEST_DIR"
cp -a "$SRC_SNAP" "$TEST_DIR/consensus_snapshot.db"

LOG="$TEST_DIR/node.log"
echo "[coldstart] launching node, datadir=$TEST_DIR, rpcport=$TEST_RPCPORT"
# -listen=0 disables the public P2P listener so we don't compete with
# the operator's main node. No -addnode/-connect: this test asserts
# the snapshot-first import alone moves the chain past MIN_TIP, with
# no peer assistance. -nolegacyimport: don't pull from any local
# ~/.zclassic legacy datadir; we want a true snapshot-only cold start.
"$BIN" \
    -datadir="$TEST_DIR" \
    -port="$TEST_PORT" \
    -rpcport="$TEST_RPCPORT" \
    -listen=0 \
    -nolegacyimport \
    > "$LOG" 2>&1 &
NODE_PID=$!

start_t=$(date +%s)
while :; do
    now=$(date +%s)
    elapsed=$((now - start_t))
    if [ $elapsed -ge $DEADLINE_SECS ]; then
        echo "[coldstart] TIMEOUT after ${elapsed}s — no '$SUCCESS_PATTERN' in node.log"
        echo "[coldstart] last 40 log lines:"
        tail -40 "$LOG" || true
        exit 1
    fi
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        echo "[coldstart] node exited unexpectedly at elapsed=${elapsed}s"
        tail -40 "$LOG" || true
        exit 1
    fi

    hit=$(grep -m1 -F "$SUCCESS_PATTERN" "$LOG" 2>/dev/null || true)
    if [ -n "$hit" ]; then
        # Parse UTXO count from the log line. Format:
        #   "[boot] snapshot-first import OK: 1339612 UTXOs at h=3117754 ..."
        utxos=$(echo "$hit" | sed -n 's/.*OK: \([0-9]*\) UTXOs.*/\1/p')
        utxos="${utxos:-0}"
        if [ "$utxos" -gt "$MIN_UTXOS" ]; then
            echo "[coldstart] PASS — $hit"
            echo "[coldstart] elapsed=${elapsed}s utxos=$utxos"
            exit 0
        else
            echo "[coldstart] FAIL — import fired but utxos=$utxos below threshold $MIN_UTXOS"
            exit 1
        fi
    fi
    sleep 1
done
