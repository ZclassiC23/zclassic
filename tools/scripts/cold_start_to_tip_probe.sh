#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# cold_start_to_tip_probe.sh — empirical C3 settle: does a FRESH datadir boot
# from the current operator bundle, then forward-sync the remaining header/body
# delta from a serving zclassic23 peer, and reach at_tip at the peer tip within
# the budget? The fast cold_start gate proves only the seed authority (>1M
# UTXOs <90s) or the FSM transitions — neither proves the full operator claim
# (fresh datadir -> phase=at_tip). This probe is the long wall-clock proof for
# that full C3 claim.
#
# FULLY ISOLATED + NON-DESTRUCTIVE to the live node:
#   - /tmp-only datadir, isolated 39xxx ports (never the live 8033/18232 or the
#     soak 18242 or zclassicd 8034/8232),
#   - copies only public local fixtures (block_index.bin + utxo-seed snapshot),
#   - dials the serving zclassic23 peer (default P2P 8033) as a CLIENT only,
#   - -listen=0, -nolegacyimport (never reads ~/.zclassic), -nobgvalidation,
#   - process-group SIGKILL teardown on every exit.
#
# Exit: 0 reached at_tip at peer-tip within budget (C3 wrapper viable)
#       3 seeded authority but did NOT reach at_tip in budget (code seam)
#       2 SKIP (no bundle fixture / no serving peer / binaries absent)
#       1 FAIL (harness/setup error)

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"
LEGACY_SNAPSHOT="${ZCL_C3_SNAPSHOT:-$HOME/.zclassic-c23/consensus_snapshot.db}"
BUNDLE_SNAP_CANDIDATES=(
    "$HOME"/.zclassic-c23-test/utxo-seed-*.snapshot
    "$HOME"/.zclassic-c23/utxo-seed-*.snapshot
)
PEER="${ZCL_C3_PEER:-127.0.0.1:8033}"        # zclassic23 P2P — DIAL this for sync
# Read the target tip from a zclassic23 node that answers zcl-rpc cleanly (the
# live node), rather than zclassicd (whose RPC uses a different cookie). Same
# chain → same tip. This is one cheap getblockcount, no sync load on it.
TIP_DATADIR="${ZCL_C3_TIP_DATADIR:-$HOME/.zclassic-c23}"
PEER_RPC="${ZCL_C3_PEER_RPC:-18232}"         # live zclassic23 RPC (read tip only)
BUDGET="${ZCL_C3_BUDGET_SECS:-600}"          # 10-minute MVP target
P2P=39070; RPC=39071; FS=39072; HTTPS=39073
BUNDLE_SUCCESS_PATTERN='-load-snapshot-at-own-height: coin set RE-SEEDED'
ARTIFACT_ROOT="${ZCL_C3_ARTIFACT_ROOT:-$REPO_ROOT/build/c3-probe}"
ARTIFACT_DIR=""
DATADIR=""
PID=""
start=0
seeded=0
last_h=-1
last_hdr=-1

write_artifact() {
    verdict="$1"
    rc="$2"
    [ -n "${ARTIFACT_DIR:-}" ] || return 0
    mkdir -p "$ARTIFACT_DIR" "$ARTIFACT_ROOT" 2>/dev/null || return 0
    {
        echo "schema=zcl.c3_probe_artifact.v1"
        echo "verdict=$verdict"
        echo "exit_code=$rc"
        echo "captured_at_unix=$(date +%s)"
        echo "peer=$PEER"
        echo "peer_tip=${PEER_TIP:-}"
        echo "budget_seconds=$BUDGET"
        echo "mode=${MODE:-unknown}"
        echo "seeded=$seeded"
        echo "last_height=$last_h"
        echo "last_headers=$last_hdr"
        echo "bundle_snapshot=${BUNDLE_SNAP:-}"
        echo "bundle_index=${BUNDLE_INDEX:-}"
        echo "scratch_datadir=${DATADIR:-}"
        echo "scratch_datadir_removed=true"
    } >"$ARTIFACT_DIR/summary.txt"
    if [ -n "${DATADIR:-}" ] && [ -f "$DATADIR/probe.log" ]; then
        cp "$DATADIR/probe.log" "$ARTIFACT_DIR/probe.log" 2>/dev/null || true
        tail -80 "$DATADIR/probe.log" >"$ARTIFACT_DIR/probe.tail.log" 2>/dev/null || true
    fi
    printf '%s\n' "$ARTIFACT_DIR" >"$ARTIFACT_ROOT/latest.txt" 2>/dev/null || true
    echo "c3-probe: artifact=$ARTIFACT_DIR"
}

skip() { echo "c3-probe: SKIP ($*)"; exit 2; }
die()  { echo "c3-probe: FAIL: $*" >&2; write_artifact "fail" 1; exit 1; }

[ -x "$NODE_BIN" ] || skip "node binary absent: $NODE_BIN"
[ -x "$RPC_BIN" ]  || skip "zcl-rpc absent: $RPC_BIN"

copy_fixture() {
    src="$1"
    dst="$2"
    cp --reflink=auto "$src" "$dst" 2>/dev/null || cp "$src" "$dst"
}

select_newest_bundle_snapshot() {
    newest_mtime=0
    newest_path=""
    for cand in "${BUNDLE_SNAP_CANDIDATES[@]}"; do
        [ -f "$cand" ] || continue
        size=$(stat -c %s "$cand" 2>/dev/null || echo 0)
        [ "$size" -gt $((10*1024*1024)) ] || continue
        mt=$(stat -c %Y "$cand" 2>/dev/null || echo 0)
        if [ "$mt" -ge "$newest_mtime" ]; then
            newest_mtime="$mt"
            newest_path="$cand"
        fi
    done
    printf '%s' "$newest_path"
}

peer_host="${PEER%:*}"
peer_port="${PEER##*:}"
[ -n "$peer_host" ] && [ -n "$peer_port" ] && [ "$peer_host" != "$peer_port" ] \
    || skip "invalid peer address: $PEER"
if ! timeout 3 bash -c "exec 3<>/dev/tcp/$peer_host/$peer_port" 2>/dev/null; then
    skip "serving peer not reachable: $PEER"
fi

# Reference peer tip (the target). zclassicd has no params on getblockcount.
PEER_TIP="$(ZCL_DATADIR="$TIP_DATADIR" ZCL_RPCPORT="$PEER_RPC" "$RPC_BIN" getblockcount 2>/dev/null \
            | sed -E 's/.*"result":(-?[0-9]+).*/\1/')"
printf '%s' "$PEER_TIP" | grep -qE '^[0-9]+$' || skip "tip source ($PEER_RPC) not answering getblockcount"
[ "$PEER_TIP" -gt 1000000 ] || skip "reference peer tip implausibly low ($PEER_TIP)"

DATADIR="$(mktemp -d /tmp/zcl-c3-probe.XXXXXX)" || die "mktemp datadir failed"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="$ARTIFACT_ROOT/$RUN_ID"
mkdir -p "$ARTIFACT_DIR" || die "artifact dir create failed: $ARTIFACT_DIR"
cleanup() {
    [ -n "$PID" ] && kill -KILL -- "-$PID" 2>/dev/null || true
    case "$DATADIR" in /tmp/zcl-c3-probe.*) rm -rf "$DATADIR" 2>/dev/null || true ;; esac
}
trap cleanup EXIT INT TERM

BUNDLE_SNAP="${ZCL_C3_BUNDLE_SNAPSHOT:-}"
BUNDLE_INDEX="${ZCL_C3_BLOCK_INDEX:-}"
if [ -z "$BUNDLE_SNAP" ]; then
    BUNDLE_SNAP="$(select_newest_bundle_snapshot)"
fi
if [ -n "$BUNDLE_SNAP" ] && [ -z "$BUNDLE_INDEX" ]; then
    BUNDLE_INDEX="$(dirname "$BUNDLE_SNAP")/block_index.bin"
fi

MODE="operator-bundle"
declare -a LOAD_ARGS=()
if [ -n "$BUNDLE_SNAP" ] &&
   [ -f "$BUNDLE_SNAP" ] &&
   [ -f "$BUNDLE_INDEX" ] &&
   [ "$(stat -c %s "$BUNDLE_INDEX" 2>/dev/null || echo 0)" -gt $((10*1024*1024)) ]; then
    bundle_snap="$DATADIR/$(basename "$BUNDLE_SNAP")"
    echo "c3-probe: peer=$PEER peer_tip=$PEER_TIP budget=${BUDGET}s datadir=$DATADIR"
    echo "c3-probe: using operator bundle snapshot: $BUNDLE_SNAP"
    echo "c3-probe: using operator bundle block index: $BUNDLE_INDEX"
    echo "c3-probe: snapshot $(du -h "$BUNDLE_SNAP" | cut -f1), block_index $(du -h "$BUNDLE_INDEX" | cut -f1)"
    copy_fixture "$BUNDLE_SNAP" "$bundle_snap" || die "bundle snapshot copy failed"
    copy_fixture "$BUNDLE_INDEX" "$DATADIR/block_index.bin" || die "block_index.bin copy failed"
    LOAD_ARGS=("-load-snapshot-at-own-height=$bundle_snap")
elif [ -r "$LEGACY_SNAPSHOT" ]; then
    MODE="legacy-consensus-snapshot"
    echo "c3-probe: peer=$PEER peer_tip=$PEER_TIP budget=${BUDGET}s datadir=$DATADIR"
    echo "c3-probe: no complete operator bundle found; using legacy consensus snapshot: $LEGACY_SNAPSHOT"
    echo "c3-probe: legacy snapshot $(du -h "$LEGACY_SNAPSHOT" | cut -f1)"
    copy_fixture "$LEGACY_SNAPSHOT" "$DATADIR/consensus_snapshot.db" || die "legacy snapshot copy failed"
else
    skip "no operator bundle (utxo-seed snapshot + block_index.bin) or legacy consensus_snapshot.db found"
fi

echo "c3-probe: booting fresh node mode=$MODE (seed authority -> delta sync from $PEER) ..."
setsid "$NODE_BIN" \
    -datadir="$DATADIR" \
    -port=$P2P \
    -rpcport=$RPC \
    -fsport=$FS \
    -httpsport=$HTTPS \
    -listen=0 \
    -connect="$PEER" \
    -nolegacyimport \
    -nobgvalidation \
    -showmetrics=0 \
    "${LOAD_ARGS[@]}" \
    >"$DATADIR/probe.log" 2>&1 &
PID=$!

start=$(date +%s)
reached=0
while :; do
    now=$(date +%s); elapsed=$((now - start))
    [ "$elapsed" -ge "$BUDGET" ] && break
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "c3-probe: node EXITED early (t=${elapsed}s) — log tail:"
        tail -15 "$DATADIR/probe.log" | sed 's/^/  /'
        die "node process died before reaching at_tip"
    fi
    # getblockchaininfo is param-free and reports both blocks (connected tip)
    # and headers (header chain) — at_tip = both >= peer tip AND blocks==headers.
    if [ "$MODE" = "operator-bundle" ] && [ "$seeded" = 0 ]; then
        seed_hit=$(grep -m1 -F -- "$BUNDLE_SUCCESS_PATTERN" "$DATADIR/probe.log" 2>/dev/null || true)
        if [ -n "$seed_hit" ]; then
            seeded=1
            echo "c3-probe: seed authority ready — $seed_hit"
        fi
    fi
    bci="$(ZCL_DATADIR="$DATADIR" ZCL_RPCPORT="$RPC" "$RPC_BIN" getblockchaininfo 2>/dev/null)"
    h="$(printf '%s' "$bci"   | sed -E 's/.*"blocks":(-?[0-9]+).*/\1/')"
    hdr="$(printf '%s' "$bci" | sed -E 's/.*"headers":(-?[0-9]+).*/\1/')"
    if printf '%s' "$h" | grep -qE '^[0-9]+$'; then
        last_hdr="${hdr:-?}"
        [ "$h" != "$last_h" ] && { echo "c3-probe: t=${elapsed}s blocks=$h headers=${hdr:-?}"; last_h="$h"; }
        [ "$h" -ge 1000000 ] && seeded=1
        if [ "$h" -ge "$PEER_TIP" ] && printf '%s' "$hdr" | grep -qE '^[0-9]+$' \
           && [ "$hdr" -ge "$PEER_TIP" ] && [ "$h" -eq "$hdr" ]; then
            reached=1; echo "c3-probe: REACHED at_tip blocks=$h headers=$hdr in ${elapsed}s"; break
        fi
    fi
    sleep 5
done

if [ "$reached" = 1 ]; then
    echo "=== c3-probe: PASS — fresh datadir -> snapshot import -> delta sync -> at_tip@$PEER_TIP within budget ==="
    write_artifact "pass" 0
    exit 0
fi
echo "c3-probe: did NOT reach at_tip in ${BUDGET}s (seeded=$seeded last_height=$last_h). Log tail:"
tail -20 "$DATADIR/probe.log" | sed 's/^/  /'
if [ "$seeded" = 1 ]; then
    echo "=== c3-probe: SEAM — seed authority loaded but forward-sync to at_tip did NOT complete within budget ==="
    write_artifact "seam" 3
    exit 3
fi
die "seed authority itself did not complete (<1M height/UTXOs) in budget"
