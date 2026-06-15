#!/usr/bin/env bash
# Deploy the freshly-built zclassic23 binary to the DEV linger lane.
#
# The dev lane is where code-in-progress actually RUNS, so it is exercised live
# instead of rotting unrun in git. It is fully isolated from the operator-gated
# live node and the pinned soak lane:
#   live node : ~/.zclassic-c23      ports 8023 / 18232   (make deploy; owner-gated)
#   soak lane : ~/.zclassic-c23-soak ports 8043 / 18242   (pinned binary)
#   DEV lane  : ~/.zclassic-c23-dev  ports 8053 / 18252   (THIS — fresh build)
#
# This script NEVER touches the live node, its datadir, or its service.
# First run bootstraps via the proven two-step cold import (header import, then
# the service boot auto-imports UTXOs + syncs forward); later runs just rebuild
# and hot-swap the binary.
#
# Usage: tools/dev/deploy-dev-lane.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

DEV_BIN="$HOME/.local/bin/zclassic23-dev"
DEV_DATADIR="$HOME/.zclassic-c23-dev"
LEGACY_SRC="$HOME/.zclassic"          # running zclassicd datadir (read-only import source)
UNIT="zcl23-dev.service"

echo "[dev-lane] building fresh binary (stamp = $(git rev-parse --short HEAD))..."
make build/bin/zclassic23 -j"$(nproc)" >/dev/null

mkdir -p "$(dirname "$DEV_BIN")" "$DEV_DATADIR"
install -m 644 "$REPO/deploy/$UNIT" "$HOME/.config/systemd/user/$UNIT"
systemctl --user daemon-reload

# Stop before swapping the binary file (avoids ETXTBSY on a running text file).
systemctl --user stop "$UNIT" 2>/dev/null || true
cp -f build/bin/zclassic23 "$DEV_BIN"

if [ ! -f "$DEV_DATADIR/node.db" ]; then
    echo "[dev-lane] fresh datadir — two-step cold-import bootstrap"
    echo "[dev-lane]   step 1/2: header import from $LEGACY_SRC (read-only, LOCK-safe; never stops zclassicd)"
    "$DEV_BIN" -datadir="$DEV_DATADIR" --importblockindex "$LEGACY_SRC"
    echo "[dev-lane]   step 2/2: enabling+starting the lane (boot auto-imports UTXOs, then syncs)"
    systemctl --user enable --now "$UNIT"
else
    echo "[dev-lane] redeploy: starting lane on the new binary"
    systemctl --user start "$UNIT"
fi

# Verify the lane reaches a healthy, advancing tip — "is-active" alone is true
# even mid-boot. The first cold-import boot has a reconcile phase (coins seeded
# at the import tip while the connected chain catches up via P2P); it can take a
# few minutes and a stuck first boot self-recovers on the unit's Restart=always.
# Poll RPC, then confirm the height ADVANCES across two samples (or is already
# at a peer's tip). A lane that never advances is a genuinely stuck bootstrap.
CLI=(build/bin/zclassic-cli -datadir="$DEV_DATADIR" -rpcport=18252)
echo "[dev-lane] deployed $(git rev-parse --short HEAD); verifying sync health..."
h0=""; for i in $(seq 1 40); do
    h0="$("${CLI[@]}" getblockcount 2>/dev/null || true)"
    [ -n "$h0" ] && [[ "$h0" =~ ^[0-9]+$ ]] && break
    sleep 6
done
if [ -z "$h0" ] || ! [[ "$h0" =~ ^[0-9]+$ ]]; then
    echo "[dev-lane] WARN: RPC not up after ~4 min — still booting/reconciling. Check: tail $DEV_DATADIR/node.log"
else
    sleep 20
    h1="$("${CLI[@]}" getblockcount 2>/dev/null || echo "$h0")"
    peer="$("${CLI[@]}" getpeerinfo 2>/dev/null | grep -oE '"(startingheight|synced_headers)": *[0-9]+' | grep -oE '[0-9]+' | sort -rn | head -1 || true)"
    if [ "${h1:-0}" -gt "${h0:-0}" ]; then
        echo "[dev-lane] HEALTHY: height advancing ${h0} -> ${h1} (peer tip ~${peer:-?})"
    elif [ -n "$peer" ] && [ "${h1:-0}" -ge $(( peer - 3 )) ]; then
        echo "[dev-lane] HEALTHY: at tip ${h1} (peer tip ~${peer})"
    else
        echo "[dev-lane] NOTE: height ${h1} not advancing yet (peer tip ~${peer:-?}) — likely the cold-import reconcile; Restart=always will retry. Re-check: ${CLI[*]} getblockcount"
    fi
fi
echo "[dev-lane] query it:  build/bin/zclassic-cli -datadir=$DEV_DATADIR -rpcport=18252 getblockcount"
echo "[dev-lane] tail log:  tail -f $DEV_DATADIR/node.log"
