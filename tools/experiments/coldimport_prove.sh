#!/usr/bin/env bash
# coldimport_prove.sh — cold-import tear reproduction probe (2026-06-13).
#
# QUESTION (gates the Phase-1 import-correctness-gate design):
#   Does a FRESH two-step cold import on the CURRENT HEAD binary reproduce the
#   orphan-seed coin tear that wedged the live node — i.e. does a freshly
#   imported node re-wedge at the canonical spend block h=3,145,595 with
#   block-not-finalized-by-reducer / prevout_unresolved (a DETERMINISTIC import
#   bug), or does it cleanly reach tip (the live tear was a one-off transient
#   reorg captured during the original sync, so the gate is purely preventive)?
#
# METHOD: the proven two-step recipe (docs/CLAUDE.md) into a THROWAWAY datadir
#   on UNUSED ports, then let it run and watch node.log around the spend height.
#   The boot-path legacy UTXO import (utxo_recovery_restore.c, the SHA3
#   "verify-later" branch) is the path under suspicion — NOT --importchainstate,
#   which enforces the strict checkpoint gate and would not reproduce the tear.
#
# SAFETY: only ever writes the throwaway datadir below; reads ~/.zclassic
#   (zclassicd, the live reference) read-only and LOCK-safe (importblockindex
#   copies blocks/index if LOCKed, never touches the source LOCK). Refuses to
#   run if the target resolves to any real datadir. Distinct ports (8053/18252)
#   so it cannot collide with the main (8023/18232), soak (8043/18242), or
#   zclassicd (8033/8232) nodes.
set -u

REPO="$HOME/github/zclassic23"
BIN="$REPO/build/bin/zclassic23"
SRC="$HOME/.zclassic"                  # zclassicd datadir — read-only source
DD="/tmp/zcl-coldimport-prove"         # throwaway target — NEVER a real datadir
RES="$DD/PROBE_RESULTS.txt"
P2P=8053
RPC=18252
SPEND_H=3145595                        # canonical block spending the missing coin
ORPHAN_H=3145486                       # where the orphan seed sat on the live node
CANON_HASH=00000adaf0cb7668be0a6b707ed59d152d2c7e4e3104410c6c77d42a3d83885f

# --- safety: never operate on a protected datadir ---
case "$DD" in
  "$HOME/.zclassic"|"$HOME/.zclassic-c23"|"$HOME/.zclassic-c23-soak"|"$HOME"|"/"|"")
    echo "REFUSE: target '$DD' is a protected datadir" >&2; exit 2;;
esac
[ -x "$BIN" ] || { echo "REFUSE: binary $BIN not found/executable" >&2; exit 2; }
[ -d "$SRC/blocks/index" ] || { echo "REFUSE: source $SRC/blocks/index missing" >&2; exit 2; }

mkdir -p "$DD"
log() { printf '[%s] %s\n' "$(date -Is)" "$*" | tee -a "$RES"; }

log "=== cold-import tear reproduction probe START (HEAD=$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null)) ==="
log "throwaway datadir=$DD  source=$SRC  ports P2P=$P2P RPC=$RPC"
log "canonical h=$ORPHAN_H expected hash $CANON_HASH ; watching for re-wedge at h=$SPEND_H"

# --- STEP A: header (block-index) import — idempotent across service restarts ---
if [ ! -f "$DD/.bidx_done" ]; then
  log "STEP A: --importblockindex (header chain) from $SRC -> $DD/node.db"
  if "$BIN" --importblockindex "$SRC" "$DD/node.db" >>"$RES" 2>&1; then
    touch "$DD/.bidx_done"
    log "STEP A: complete"
  else
    log "STEP A: FAILED (see above) — aborting"
    exit 1
  fi
else
  log "STEP A: already done (.bidx_done present) — skipping"
fi

# --- STEP B: normal boot triggers the boot-path legacy UTXO import from ~/.zclassic.
# exec so this service's main process IS the node (systemd-managed, survives
# session restarts). Watch $DD/node.log + the RPC for whether it passes h=$SPEND_H. ---
log "STEP B: booting throwaway node; legacy UTXO import auto-reads ~/.zclassic."
log "OBSERVE: tail -f $DD/node.log ; verdict = does served tip advance past $SPEND_H?"
exec "$BIN" -datadir="$DD" -port=$P2P -rpcport=$RPC -nobgvalidation -showmetrics=0
