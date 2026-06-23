#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# seed_anchor_snapshot.sh — Option 2 (DEPLOY-WRITE) bridge for snapshot
# reachability. Stage a verified anchor UTXO snapshot into the operator's
# datadir at <datadir>/utxo-anchor.snapshot — the exact path the torn-import
# self-heal (boot_anchor_seed_from_snapshot / boot_load_verify_snapshot_eligible)
# resolves when $ZCL_MINT_ANCHOR_OUT is unset. With it in place an owner-deployed
# install carries a reachable snapshot from boot one, including the cold-start
# case the Option-3 self-mint cannot reach (a node only ever torn BELOW the
# anchor never folds across the checkpoint, so it never self-mints).
#
# SAFETY MODEL: this only STAGES a candidate file. It does NOT bless it. The node
# re-verifies the body SHA3 against the compiled checkpoint via uss_open
# (verify_full_sha3=true, expected_sha3 = cp->sha3_hash) before a single coin is
# re-seeded; a stale/corrupt/wrong-height file is ignored, never trusted. So a
# bad copy here can never route a wrong UTXO set toward coins_kv.
#
# BEST-EFFORT: a missing source is NOT an error — deploy proceeds without the
# bridge (the offline -mint-anchor ceremony and the in-fold self-mint remain the
# fallbacks). Idempotent: a snapshot already present at the target with the same
# size is left untouched.
#
# Env:
#   ZCL_DATADIR              target datadir (default: ~/.zclassic-c23)
#   ZCL_ANCHOR_SNAPSHOT_SRC  source snapshot path
#                            (default: /tmp/anchor-ram.snapshot — the output the
#                            offline `-mint-anchor` ceremony writes via
#                            ZCL_MINT_ANCHOR_OUT. NOTE: the old default
#                            /tmp/utxo-anchor-3056758.snapshot is MISLABELED — its
#                            header decodes to height 3,151,901, count 1,344,817, so
#                            uss_open rejects it on the count check. Never use it.)
set -u

DATADIR="${ZCL_DATADIR:-$HOME/.zclassic-c23}"
SRC="${ZCL_ANCHOR_SNAPSHOT_SRC:-/tmp/anchor-ram.snapshot}"
DST="$DATADIR/utxo-anchor.snapshot"

if [ ! -d "$DATADIR" ]; then
    echo "[seed-anchor-snapshot] datadir $DATADIR absent — skip (fresh install mints in-fold)"
    exit 0
fi

if [ ! -f "$SRC" ]; then
    echo "[seed-anchor-snapshot] no source snapshot at $SRC — skip (self-mint will produce one on the anchor-crossing fold; set ZCL_ANCHOR_SNAPSHOT_SRC to override)"
    exit 0
fi

# Idempotent: same-size file already at the target -> leave it. (The node SHA3-
# verifies on boot regardless; size match is enough to avoid a needless 100 MB
# recopy. A wrong-but-same-size file is still rejected by the node's verify.)
if [ -f "$DST" ]; then
    src_sz=$(stat -c %s "$SRC" 2>/dev/null || echo 0)
    dst_sz=$(stat -c %s "$DST" 2>/dev/null || echo 0)
    if [ "$src_sz" = "$dst_sz" ] && [ "$src_sz" != "0" ]; then
        echo "[seed-anchor-snapshot] $DST already present (size $dst_sz) — not re-copying"
        exit 0
    fi
fi

# Atomic stage: copy to a temp sibling, then rename (rename is atomic within a
# filesystem). A crash mid-copy leaves the partial .tmp, never a torn target.
TMP="$DST.tmp.$$"
if ! cp -f "$SRC" "$TMP"; then
    echo "[seed-anchor-snapshot] copy $SRC -> $TMP failed — skip (non-fatal)"
    rm -f "$TMP"
    exit 0
fi
if ! mv -f "$TMP" "$DST"; then
    echo "[seed-anchor-snapshot] rename $TMP -> $DST failed — skip (non-fatal)"
    rm -f "$TMP"
    exit 0
fi
sz=$(stat -c %s "$DST" 2>/dev/null || echo "?")
echo "[seed-anchor-snapshot] staged $SRC -> $DST (size $sz). The node SHA3-verifies it vs the compiled checkpoint on boot before any trust."
exit 0
