#!/usr/bin/env bash
# Deploy the freshly-built zclassic23 binary to the DEV linger lane.
#
# The dev lane is where code-in-progress actually RUNS, so it is exercised live
# instead of rotting unrun in git. It is fully isolated from the operator-gated
# live node and the pinned soak lane:
#   live node : ~/.zclassic-c23      ports 8033 / 18232   (make deploy; owner-gated)
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
STALE_REINDEX_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/reindex.conf"
STALE_OOM_BUDGET_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/zz-oom-budget.conf"
BUILD_ID_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/90-build-identity.conf"

git update-index -q --refresh >/dev/null 2>&1 || true
BUILD_COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    BUILD_COMMIT="${BUILD_COMMIT}-dirty"
fi

echo "[dev-lane] building fresh binary (stamp = $BUILD_COMMIT)..."
make build/bin/zclassic23 -j"$(nproc)" >/dev/null

mkdir -p "$(dirname "$DEV_BIN")" "$DEV_DATADIR"
install -m 644 "$REPO/deploy/$UNIT" "$HOME/.config/systemd/user/$UNIT"
mkdir -p "$(dirname "$BUILD_ID_DROPIN")"
{
    printf '[Service]\n'
    printf 'Environment="ZCL_AGENT_EXPECT_BUILD_COMMIT=%s"\n' "$BUILD_COMMIT"
    printf 'Environment="ZCL_AGENT_EXPECT_BUILD_SOURCE=deploy-dev"\n'
} > "$BUILD_ID_DROPIN"
if [ -f "$STALE_REINDEX_DROPIN" ]; then
    if [ "${ZCL_DEV_ALLOW_REINDEX_DROPIN:-0}" = "1" ]; then
        echo "[dev-lane] preserving explicit reindex drop-in: $STALE_REINDEX_DROPIN"
    else
        echo "[dev-lane] removing stale reindex drop-in: $STALE_REINDEX_DROPIN"
        echo "[dev-lane]   set ZCL_DEV_ALLOW_REINDEX_DROPIN=1 to keep a deliberate reindex override"
        rm -f "$STALE_REINDEX_DROPIN"
    fi
fi
if [ -f "$STALE_OOM_BUDGET_DROPIN" ]; then
    if [ "${ZCL_DEV_ALLOW_OOM_BUDGET_DROPIN:-0}" = "1" ]; then
        echo "[dev-lane] preserving explicit memory-budget drop-in: $STALE_OOM_BUDGET_DROPIN"
    else
        echo "[dev-lane] removing stale memory-budget drop-in: $STALE_OOM_BUDGET_DROPIN"
        echo "[dev-lane]   deploy/zcl23-dev.service owns the dev lane memory budget"
        echo "[dev-lane]   set ZCL_DEV_ALLOW_OOM_BUDGET_DROPIN=1 to keep a deliberate override"
        rm -f "$STALE_OOM_BUDGET_DROPIN"
    fi
fi
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

json_first_string_field() {
    local body="$1" key="$2" token
    token="$(printf '%s\n' "$body" \
        | grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" 2>/dev/null \
        | head -1 || true)"
    [ -n "$token" ] || return 0
    printf '%s\n' "$token" \
        | sed -n "s/^\"${key}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"$/\1/p"
}

json_first_bool_field() {
    local body="$1" key="$2" token
    token="$(printf '%s\n' "$body" \
        | grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\\(true\\|false\\)" 2>/dev/null \
        | head -1 || true)"
    case "$token" in
        *true) printf 1 ;;
        *false) printf 0 ;;
        *) printf null ;;
    esac
}

probe_agent_contract() {
    local timeout_s="${ZCL_DEV_AGENT_TIMEOUT:-10}"
    local agent_json rc build status healthy serving operator_needed
    local validation_pack_ok blocker next readiness

    agent_json=""
    if agent_json="$(timeout "$timeout_s" "${CLI[@]}" agent 2>/dev/null)"; then
        if [ -z "$agent_json" ]; then
            echo "[dev-lane] WARN: agent contract returned empty JSON; not declaring agent-ready"
            return 0
        fi
    else
        rc=$?
        if [ "$rc" = "124" ]; then
            echo "[dev-lane] WARN: agent contract timed out after ${timeout_s}s; not declaring agent-ready"
        else
            echo "[dev-lane] WARN: agent contract unavailable (rc=$rc); not declaring agent-ready"
        fi
        return 0
    fi

    build="$(json_first_string_field "$agent_json" "build_commit")"
    status="$(json_first_string_field "$agent_json" "status")"
    healthy="$(json_first_bool_field "$agent_json" "healthy")"
    serving="$(json_first_bool_field "$agent_json" "serving")"
    operator_needed="$(json_first_bool_field "$agent_json" "operator_needed")"
    validation_pack_ok="$(json_first_bool_field "$agent_json" "validation_pack_ok")"
    blocker="$(json_first_string_field "$agent_json" "primary_blocker")"
    next="$(json_first_string_field "$agent_json" "next")"
    readiness="$(json_first_string_field "$agent_json" "readiness_status")"

    if [ "$operator_needed" = "1" ] ||
       [ "$status" = "blocked" ] ||
       [ "$validation_pack_ok" = "0" ]; then
        echo "[dev-lane] BLOCKED: agent status=${status:-unknown} build=${build:-unknown} operator_needed=$operator_needed validation_pack_ok=$validation_pack_ok"
        echo "[dev-lane] BLOCKED: primary_blocker=${blocker:-unknown}"
        echo "[dev-lane] BLOCKED: next=${next:-zclassic23 healthcheck}"
        exit 1
    fi

    if [ "$healthy" = "1" ] && [ "$serving" = "1" ]; then
        echo "[dev-lane] AGENT READY: status=${status:-unknown} build=${build:-unknown} readiness=${readiness:-unknown}"
    else
        echo "[dev-lane] NOTE: agent status=${status:-unknown} healthy=$healthy serving=$serving; not declaring agent-ready yet"
        [ -n "$blocker" ] && echo "[dev-lane] NOTE: primary_blocker=$blocker"
        [ -n "$next" ] && echo "[dev-lane] NOTE: next=$next"
    fi
}

echo "[dev-lane] deployed $BUILD_COMMIT; verifying sync health..."
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
        echo "[dev-lane] SYNC OK: height advancing ${h0} -> ${h1} (peer tip ~${peer:-?})"
    elif [ -n "$peer" ] && [ "${h1:-0}" -ge $(( peer - 3 )) ]; then
        echo "[dev-lane] SYNC OK: at tip ${h1} (peer tip ~${peer})"
    else
        echo "[dev-lane] NOTE: height ${h1} not advancing yet (peer tip ~${peer:-?}) — likely the cold-import reconcile; Restart=always will retry. Re-check: ${CLI[*]} getblockcount"
    fi
    probe_agent_contract
fi
echo "[dev-lane] query it:  build/bin/zclassic-cli -datadir=$DEV_DATADIR -rpcport=18252 getblockcount"
echo "[dev-lane] tail log:  tail -f $DEV_DATADIR/node.log"
