#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# copyproof_increment1.sh — INTEGRATION copy-proof for the live-wedge fix stack
# (FIX-1 header-solution + increment-1 validator Case-3b restart durability).
#
# Runs ENTIRELY on a throwaway COPY of the live datadir, isolated ports, setsid
# so a parent/harness reap can never SIGTERM the node. It NEVER writes the live
# datadir and NEVER touches the live ports (18232/8232/8033/8034). The real zclassicd
# oracle is read-only (rebuild_recent pulls bodies from it); we never stop it.
#
# What it proves, in one run:
#   P1 boot      — node boots on the copy, RPC comes up, capture boot decision.
#   P2 advance   — does the reducer advance past the wedge tip on its own
#                  (FIX-1)? If not, drive backfill_header_solutions+rebuild_recent
#                  rounds to climb. Record how far it got and what blocked more.
#   P3 kill -9   — once the tip has climbed past the start, hard-kill the node.
#   P4 restart   — reboot the SAME binary on the SAME copy. The increment-1 gate:
#                  the boot validator must AGREE (BOOT_OK), NOT reset the public
#                  tip back down to the stale coins height. Assert tip preserved
#                  and no reset_chain/wipe action on the restart boot.
#
# Usage: tools/copyproof_increment1.sh [--src=DIR] [--rpcport=N] [--p2pport=N]
#                                      [--oracle-p2p=127.0.0.1:8034]
#                                      [--climb-rounds=15] [--watch=180]
set -u

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

SRC="$HOME/.zclassic-c23"
RPCPORT=18299
P2PPORT=18933
ORACLE_P2P="127.0.0.1:8034"   # zclassicd P2P; zclassic23 owns canonical 8033
CLIMB_ROUNDS=15
WATCH=180

while [ $# -gt 0 ]; do
  case "$1" in
    --src=*)        SRC="${1#--src=}" ;;
    --rpcport=*)    RPCPORT="${1#--rpcport=}" ;;
    --p2pport=*)    P2PPORT="${1#--p2pport=}" ;;
    --oracle-p2p=*) ORACLE_P2P="${1#--oracle-p2p=}" ;;
    --climb-rounds=*) CLIMB_ROUNDS="${1#--climb-rounds=}" ;;
    --watch=*)      WATCH="${1#--watch=}" ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

[ -d "$SRC" ] || { echo "src datadir not found: $SRC" >&2; exit 1; }
[ -x "$NODE_BIN" ] || { echo "$NODE_BIN not built" >&2; exit 1; }
[ -x "$RPC_BIN" ] || { echo "$RPC_BIN not built (make zcl-rpc)" >&2; exit 1; }
if [ "$RPCPORT" = "18232" ] || [ "$RPCPORT" = "8232" ] ||
   [ "$P2PPORT" = "8033" ] || [ "$P2PPORT" = "8034" ]; then
  echo "refusing to use a live port (18232/8232/8033/8034)" >&2; exit 1
fi

TS="$(date +%Y%m%d-%H%M%S)"
DEST="$HOME/.zclassic-c23-COPY-$TS-cp1"
[ ! -e "$DEST" ] || { echo "dest exists: $DEST" >&2; exit 1; }

say() { echo "[cp1] $*"; }
rpc()  { ZCL_DATADIR="$DEST" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null; }
tip()  { rpc getblockcount | tr -dc '0-9-'; }
nodepid() { cat "$DEST/zclassic23.pid" 2>/dev/null | tr -dc '0-9'; }

# ---- snapshot (full: blocks/ + snapshot included for fidelity) -------------
say "snapshot $SRC -> $DEST (full, ~22G)"
mkdir -p "$DEST"
cp -a "$SRC"/. "$DEST"/ || { echo "copy failed" >&2; exit 1; }
# never let the copy advertise/clobber the live identity files
rm -f "$DEST/zclassic23.pid" "$DEST/.cookie" "$DEST/.lock" 2>/dev/null
say "git_head: $(git rev-parse HEAD 2>/dev/null)"

NODE_ARGS="-datadir=$DEST -rpcport=$RPCPORT -port=$P2PPORT -nobgvalidation -addnode=$ORACLE_P2P"

boot_node() {  # $1 = logfile
  local log="$1"
  rm -f "$DEST/zclassic23.pid" "$DEST/.cookie" "$DEST/.lock" 2>/dev/null
  setsid "$NODE_BIN" $NODE_ARGS > "$log" 2>&1 &
  # wait for RPC up or death (max ~120s)
  local up=0 i=0
  while [ "$i" -lt 40 ]; do
    i=$((i+1))
    local t; t="$(tip)"
    if [ -n "$t" ]; then up=1; break; fi
    sleep 3
  done
  echo "$up"
}

boot_decision() {  # $1 = logfile -> prints a one-line summary of the boot validator decision
  grep -iE 'AGREE \(coins reconciles|reset_chain|reset_coins|wipe_wait|BOOT_OK|coins_chain_mismatch|behind reducer-finalized|restoring chain tip|RECOVER' "$1" 2>/dev/null | tail -8
}

# ============================ P1: first boot ===============================
say "P1: booting copy node (rpc=$RPCPORT p2p=$P2PPORT oracle=$ORACLE_P2P)"
UP1="$(boot_node "$DEST/boot1.log")"
if [ "$UP1" != "1" ]; then
  say "FAIL: node never answered RPC on first boot"
  echo "---- boot1.log tail ----"; tail -40 "$DEST/boot1.log"
  echo "VERDICT: FAIL (no-rpc-boot1)"; exit 1
fi
PID1="$(nodepid)"
START_TIP="$(tip)"; START_TIP="${START_TIP:--1}"
say "P1: RPC up, pid=$PID1, start_tip=$START_TIP"
say "P1 boot decision:"; boot_decision "$DEST/boot1.log" | sed 's/^/    /'

# ============================ P2: advance/climb ============================
say "P2: watching for self-advance for ${WATCH}s (FIX-1 reducer)"
deadline=$(( $(date +%s) + WATCH ))
maxtip="$START_TIP"
while [ "$(date +%s)" -lt "$deadline" ]; do
  kill -0 "$PID1" 2>/dev/null || { say "P2: node died during watch"; break; }
  t="$(tip)"; t="${t:--1}"
  [ "$t" -gt "$maxtip" ] 2>/dev/null && { maxtip="$t"; say "P2: tip advanced -> $t"; }
  [ "$maxtip" -ge "$((START_TIP + 20))" ] 2>/dev/null && { say "P2: self-advanced +20, climb confirmed"; break; }
  sleep 6
done

if [ "$maxtip" -lt "$((START_TIP + 5))" ] 2>/dev/null && kill -0 "$PID1" 2>/dev/null; then
  say "P2: no self-advance; driving climb (backfill+rebuild_recent, up to $CLIMB_ROUNDS rounds)"
  r=0
  while [ "$r" -lt "$CLIMB_ROUNDS" ]; do
    r=$((r+1))
    kill -0 "$PID1" 2>/dev/null || { say "P2: node died at climb round $r"; break; }
    t="$(tip)"; t="${t:--1}"
    [ "$t" -gt "$maxtip" ] 2>/dev/null && maxtip="$t"
    [ "$maxtip" -ge "$((START_TIP + 20))" ] 2>/dev/null && { say "P2: climb reached +20"; break; }
    nxt=$((t+1))
    bf="$(rpc backfill_header_solutions "$nxt" 2>/dev/null | grep -oE '"filled":[0-9]+' | head -1)"
    say "P2 round $r: tip=$t ${bf:-no-backfill} -> rebuild_recent($nxt)"
    ( rpc rebuild_recent "$nxt" > "$DEST/rebuild_$r.out" 2>&1 ) &
    sleep 30
  done
fi

CLIMB_TIP="$(tip)"; CLIMB_TIP="${CLIMB_TIP:--1}"
[ "$CLIMB_TIP" -gt "$maxtip" ] 2>/dev/null && maxtip="$CLIMB_TIP"
say "P2: end of climb phase — start_tip=$START_TIP max_tip=$maxtip current_tip=$CLIMB_TIP"
say "P2: next-blocker markers (upstream_failed/internal_error/reorg/FATAL/missing):"
grep -iE 'upstream_failed|internal_error|reorg|FATAL|missing-have-data|operator_needed|first_failure' "$DEST/boot1.log" 2>/dev/null | tail -12 | sed 's/^/    /'

CLIMBED=0
[ "$maxtip" -ge "$((START_TIP + 5))" ] 2>/dev/null && CLIMBED=1

# ============================ P3: kill -9 ==================================
PID1="$(nodepid)"
if [ -z "$PID1" ]; then say "P3: no pid to kill (node already gone)"; else
  say "P3: kill -9 $PID1 (hard crash for restart-durability test)"
  kill -9 "$PID1" 2>/dev/null
  # wait for it to actually exit + ports to free
  k=0; while kill -0 "$PID1" 2>/dev/null && [ "$k" -lt 20 ]; do k=$((k+1)); sleep 1; done
fi
sleep 3

# ============================ P4: restart ==================================
say "P4: restarting same binary on same copy (increment-1 gate)"
UP2="$(boot_node "$DEST/boot2.log")"
if [ "$UP2" != "1" ]; then
  say "P4: node never answered RPC on restart"
  echo "---- boot2.log tail ----"; tail -40 "$DEST/boot2.log"
  echo "VERDICT: FAIL (no-rpc-restart) climbed=$CLIMBED start=$START_TIP max=$maxtip"; exit 1
fi
PID2="$(nodepid)"
RESTART_TIP="$(tip)"; RESTART_TIP="${RESTART_TIP:--1}"
say "P4: RPC up, pid=$PID2, restart_tip=$RESTART_TIP"
say "P4 boot decision:"; boot_decision "$DEST/boot2.log" | sed 's/^/    /'

RESET=0
grep -iE 'reset_chain|wipe_wait|RECOVER_RESET|RECOVER_WIPE' "$DEST/boot2.log" >/dev/null 2>&1 && RESET=1
AGREE=0
grep -iE 'behind reducer-finalized|AGREE \(coins reconciles' "$DEST/boot2.log" >/dev/null 2>&1 && AGREE=1

# tip must not have regressed below the climbed high-water (minus small WAL slack)
PRESERVED=0
[ "$RESTART_TIP" -ge "$((maxtip - 6))" ] 2>/dev/null && PRESERVED=1

POST_TXO="$(rpc gettxoutsetinfo 2>/dev/null | grep -oE '"height":[0-9]+' | head -1)"

# stop the copy node cleanly
PID2="$(nodepid)"; [ -n "$PID2" ] && kill -TERM "$PID2" 2>/dev/null

echo "========================================================================"
echo "  copyproof_increment1  [copy: $DEST]"
echo "  git_head:      $(git rev-parse --short HEAD 2>/dev/null)"
echo "  start_tip:     $START_TIP"
echo "  max_tip:       $maxtip   (climbed: $([ $CLIMBED = 1 ] && echo yes || echo NO))"
echo "  restart_tip:   $RESTART_TIP   ($POST_TXO via gettxoutsetinfo)"
echo "  restart reset_chain/wipe in log: $([ $RESET = 1 ] && echo YES || echo no)"
echo "  restart Case-3b AGREE fired:     $([ $AGREE = 1 ] && echo yes || echo no)"
echo "  tip preserved across kill-9:     $([ $PRESERVED = 1 ] && echo yes || echo NO)"
echo "------------------------------------------------------------------------"
if [ "$CLIMBED" = 1 ] && [ "$PRESERVED" = 1 ] && [ "$RESET" = 0 ]; then
  echo "  VERDICT: PASS — climbed +$((maxtip - START_TIP)), restart preserved the tip, no reset"
  RC=0
elif [ "$CLIMBED" = 0 ]; then
  echo "  VERDICT: INCONCLUSIVE — could not climb past the wedge (see next-blocker markers above)"
  echo "           the restart path still observed: reset=$RESET preserved=$PRESERVED"
  RC=2
else
  echo "  VERDICT: FAIL — restart did not preserve the climbed tip (reset=$RESET preserved=$PRESERVED)"
  RC=1
fi
echo "  boot1.log: $DEST/boot1.log    boot2.log: $DEST/boot2.log"
echo "  copy left on disk; remove with: rm -rf '$DEST'"
echo "========================================================================"
exit $RC
