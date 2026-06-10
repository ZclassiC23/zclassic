#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# copyproof_p2_frontier.sh — INTEGRATION copy-proof for the P2 self-heal
# prerequisite: coins_applied_height (the contiguous applied-coins frontier)
# MUST equal stage_cursor('utxo_apply') after EVERY committed reducer txn,
# and that equality MUST survive a hard kill -9 + restart on a real datadir.
#
# The unit test (test_coins_applied_frontier) already pins the WRITE paths
# (forward/upstream_failed/reorg/poison_rewind/backfill) deterministically and
# proves — by revert-experiment — that deleting any co-write breaks them. This
# script adds the INTEGRATION layer those can't reach:
#   1. the NEW binary boots clean on the real (pre-P2, wedged) live datadir;
#   2. the boot backfill seeds frontier == utxo_apply cursor on real data
#      (it was ABSENT on disk before P2 shipped);
#   3. the equality holds on the raw kill -9 CRASH IMAGE (node dead, no clean
#      shutdown to mask a torn write) — the atomic-co-commit durability claim;
#   4. it still holds after a real restart re-opens the multi-GB progress.kv.
#
# Runs ENTIRELY on a throwaway COPY, isolated ports, setsid. NEVER writes the
# live datadir, NEVER touches the live ports (18232/8033), NEVER stops zclassicd.
#
# Usage: tools/copyproof_p2_frontier.sh [--src=DIR] [--rpcport=N] [--p2pport=N]
#                                       [--oracle-p2p=127.0.0.1:8033]
#                                       [--climb=90] [--settle=20]
set -u

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"
P2_CHECK_BIN="${ZCL_P2_CHECK_BIN:-$REPO_ROOT/build/bin/p2_invariant_check}"

SRC="$HOME/.zclassic-c23"
RPCPORT=18299
P2PPORT=18933
ORACLE_P2P="127.0.0.1:8033"
CLIMB=90       # seconds to let the reducer commit some txns before kill -9
SETTLE=20      # seconds after RPC-up before first invariant read (let boot backfill run)

while [ $# -gt 0 ]; do
  case "$1" in
    --src=*)          SRC="${1#--src=}" ;;
    --rpcport=*)      RPCPORT="${1#--rpcport=}" ;;
    --p2pport=*)      P2PPORT="${1#--p2pport=}" ;;
    --oracle-p2p=*)   ORACLE_P2P="${1#--oracle-p2p=}" ;;
    --climb=*)        CLIMB="${1#--climb=}" ;;
    --settle=*)       SETTLE="${1#--settle=}" ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

[ -d "$SRC" ] || { echo "src datadir not found: $SRC" >&2; exit 1; }
[ -x "$NODE_BIN" ] || { echo "$NODE_BIN not built" >&2; exit 1; }
[ -x "$P2_CHECK_BIN" ] || { echo "$P2_CHECK_BIN not built (make p2_invariant_check)" >&2; exit 1; }
[ -x "$RPC_BIN" ] || { echo "$RPC_BIN not built (make zcl-rpc)" >&2; exit 1; }
if [ "$RPCPORT" = "18232" ] || [ "$P2PPORT" = "8033" ]; then
  echo "refusing to use the live port (18232/8033)" >&2; exit 1
fi

TS="$(date +%Y%m%d-%H%M%S)"
DEST="$HOME/.zclassic-c23-COPY-$TS-p2"
[ ! -e "$DEST" ] || { echo "dest exists: $DEST" >&2; exit 1; }

say() { echo "[p2cp] $*"; }
rpc()  { ZCL_DATADIR="$DEST" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null; }
tip()  { rpc getblockcount | tr -dc '0-9-'; }
nodepid() { cat "$DEST/zclassic23.pid" 2>/dev/null | tr -dc '0-9'; }

# p2chk <label> -> echoes ONLY the exit code on stdout (so $(p2chk ..) captures
# it); the human-readable verdict goes to stderr (visible in the run log, not
# captured). Reads the COPY's progress.kv with mode=ro (honors a kill-9-left
# -wal), NOT --immutable.
p2chk() {
  local label="$1" out rc
  out="$("$P2_CHECK_BIN" "$DEST" 2>&1)"; rc=$?
  { say "P2-INVARIANT [$label] (rc=$rc):"; echo "$out" | sed 's/^/    /'; } >&2
  echo "$rc"
}

boot_node() {  # $1 = logfile -> echoes 1 if RPC came up, else 0
  local log="$1" up=0 i=0 t
  rm -f "$DEST/zclassic23.pid" "$DEST/.cookie" "$DEST/.lock" 2>/dev/null
  setsid "$NODE_BIN" -datadir="$DEST" -rpcport="$RPCPORT" -port="$P2PPORT" \
      -nobgvalidation -addnode="$ORACLE_P2P" > "$log" 2>&1 &
  while [ "$i" -lt 40 ]; do
    i=$((i+1)); t="$(tip)"
    [ -n "$t" ] && { up=1; break; }
    sleep 3
  done
  echo "$up"
}

# ---- snapshot --------------------------------------------------------------
say "snapshot $SRC -> $DEST (full, ~22G)"
mkdir -p "$DEST"
cp -a "$SRC"/. "$DEST"/ || { echo "copy failed" >&2; exit 1; }
rm -f "$DEST/zclassic23.pid" "$DEST/.cookie" "$DEST/.lock" 2>/dev/null
say "git_head: $(git rev-parse --short HEAD 2>/dev/null)"

# pre-boot on-disk state (expected ABSENT on a pre-P2 datadir copy)
P2_PRE="$(p2chk pre-boot)"

# ============================ P1: first boot ===============================
say "P1: booting NEW binary on the copy (rpc=$RPCPORT p2p=$P2PPORT)"
UP1="$(boot_node "$DEST/boot1.log")"
[ "$UP1" = "1" ] || { say "FAIL: no RPC on first boot"; tail -30 "$DEST/boot1.log"; echo "VERDICT: FAIL (no-rpc-boot1)"; exit 1; }
START_TIP="$(tip)"; START_TIP="${START_TIP:--1}"
say "P1: RPC up, start_tip=$START_TIP — settling ${SETTLE}s for boot backfill"
sleep "$SETTLE"

# boot backfill must have seeded frontier == cursor on real (formerly ABSENT) data
P2_BOOT="$(p2chk post-boot)"

# ============================ P2: brief climb ==============================
say "P2: letting the reducer run ${CLIMB}s (commit txns under the invariant)"
PID1="$(nodepid)"
deadline=$(( $(date +%s) + CLIMB )); maxtip="$START_TIP"
while [ "$(date +%s)" -lt "$deadline" ]; do
  kill -0 "$PID1" 2>/dev/null || { say "P2: node exited during climb"; break; }
  t="$(tip)"; t="${t:--1}"
  [ "$t" -gt "$maxtip" ] 2>/dev/null && { maxtip="$t"; say "P2: tip -> $t"; }
  sleep 6
done
P2_PREKILL="$(p2chk pre-kill)"

# ============================ P3: kill -9 + CRASH IMAGE ====================
PID1="$(nodepid)"
if [ -n "$PID1" ]; then
  say "P3: kill -9 $PID1 (hard crash — no clean shutdown)"
  kill -9 "$PID1" 2>/dev/null
  k=0; while kill -0 "$PID1" 2>/dev/null && [ "$k" -lt 20 ]; do k=$((k+1)); sleep 1; done
fi
sleep 3
# THE key assertion: the invariant on the raw crash image, node dead.
P2_CRASH="$(p2chk crash-image)"

# ============================ P4: restart ==================================
say "P4: restarting same binary on same copy"
UP2="$(boot_node "$DEST/boot2.log")"
[ "$UP2" = "1" ] || { say "FAIL: no RPC on restart"; tail -30 "$DEST/boot2.log"; echo "VERDICT: FAIL (no-rpc-restart)"; exit 1; }
RESTART_TIP="$(tip)"; RESTART_TIP="${RESTART_TIP:--1}"
say "P4: RPC up, restart_tip=$RESTART_TIP — settling ${SETTLE}s"
sleep "$SETTLE"
P2_RESTART="$(p2chk post-restart)"

PID2="$(nodepid)"; [ -n "$PID2" ] && kill -TERM "$PID2" 2>/dev/null
sleep 2
P2_FINAL="$(p2chk final-clean-stop)"

# ============================ verdict ======================================
# HOLDS=0. The durability claim requires the crash image AND every post-boot
# read to HOLD (==0). A MISMATCH (1) is a real bug. ABSENT (2) is acceptable
# ONLY pre-boot; post-boot ABSENT means the boot backfill failed to seed.
echo "========================================================================"
echo "  copyproof_p2_frontier  [copy: $DEST]"
echo "  git_head:     $(git rev-parse --short HEAD 2>/dev/null)"
echo "  tips:         start=$START_TIP climb_max=$maxtip restart=$RESTART_TIP"
echo "  p2chk rc:     pre-boot=$P2_PRE  post-boot=$P2_BOOT  pre-kill=$P2_PREKILL"
echo "                crash-image=$P2_CRASH  post-restart=$P2_RESTART  final=$P2_FINAL"
echo "  (0=HOLDS  1=MISMATCH  2=ABSENT  3=open-error)"
echo "------------------------------------------------------------------------"
RC=0
for v in "$P2_BOOT" "$P2_PREKILL" "$P2_CRASH" "$P2_RESTART" "$P2_FINAL"; do
  [ "$v" = "1" ] && RC=1   # any MISMATCH = hard fail
done
# post-boot/crash/restart must be HOLDS (0); ABSENT (2) there is a seeding miss
for v in "$P2_BOOT" "$P2_CRASH" "$P2_RESTART"; do
  [ "$v" = "0" ] || { [ "$RC" = "0" ] && RC=2; }
done
if [ "$RC" = "0" ]; then
  echo "  VERDICT: PASS — frontier==utxo_apply cursor held post-boot, on the kill-9"
  echo "           crash image, and post-restart (atomic co-commit is durable)."
elif [ "$RC" = "1" ]; then
  echo "  VERDICT: FAIL — frontier diverged from the cursor (MISMATCH); see rc line."
else
  echo "  VERDICT: INCONCLUSIVE — no MISMATCH, but a post-boot read was not HOLDS"
  echo "           (likely boot backfill did not seed; inspect $DEST/boot1.log)."
fi
echo "  boot1.log: $DEST/boot1.log   boot2.log: $DEST/boot2.log"
echo "  copy left on disk; remove with: rm -rf '$DEST'"
echo "========================================================================"
exit $RC
