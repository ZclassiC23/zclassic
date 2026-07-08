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
# and hot-swap the binary. The default build is the cached non-LTO dev binary
# for fast agent iteration; set ZCL_DEV_DEPLOY_BUILD=strict to pay the
# production-style build/bin/zclassic23 link for a stricter dev-lane run.
#
# Usage: tools/dev/deploy-dev-lane.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

DEV_BIN="$HOME/.local/bin/zclassic23-dev"
DEV_DATADIR="$HOME/.zclassic-c23-dev"
LEGACY_SRC="$HOME/.zclassic"          # running zclassicd datadir (read-only import source)
UNIT="zcl23-dev.service"
NODE_LOG="$DEV_DATADIR/node.log"
AUTO_REINDEX_SENTINEL="$DEV_DATADIR/auto_reindex_request"
STALE_REINDEX_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/reindex.conf"
STALE_OOM_BUDGET_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/zz-oom-budget.conf"
BUILD_ID_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/90-build-identity.conf"
DEPLOY_STATE="$DEV_DATADIR/agent-deploy.json"
DEV_DEPLOY_BUILD="${ZCL_DEV_DEPLOY_BUILD:-fast}"
VERIFY_STATUS="started"
VERIFY_DETAIL=""

auto_reindex_status() {
    local anchor="" count=""
    [ -r "$AUTO_REINDEX_SENTINEL" ] || return 1
    read -r anchor count < "$AUTO_REINDEX_SENTINEL" || return 1
    [[ "$anchor" =~ ^-?[0-9]+$ ]] || return 1
    [[ "$count" =~ ^-?[0-9]+$ ]] || return 1
    printf '%s %s\n' "$anchor" "$count"
}

pre_rpc_boot_diagnostic() {
    [ -r "$NODE_LOG" ] || return 0
    tail -n 500 "$NODE_LOG" | awk '
        /crash-only recovery: consuming auto-reindex request/ {
            recovery=$0
        }
        /reindex-chainstate: rebuilding UTXO set/ {
            reindex=1
        }
        /height [0-9]+\/[0-9]+ .*ETA/ {
            progress=$0
        }
        END {
            if (progress != "") {
                print "pre-RPC recovery: reindex-chainstate " progress
            } else if (reindex) {
                print "pre-RPC recovery: reindex-chainstate active"
            } else if (recovery != "") {
                print "pre-RPC recovery: " recovery
            }
        }'
}

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

write_deploy_state() {
    local verify_status="$1" verify_detail="$2" tmp now status anchor count
    local auto_reindex_pending="false"
    local auto_reindex_anchor=""
    local auto_reindex_count=""

    mkdir -p "$DEV_DATADIR"
    now="$(date -u +%FT%TZ)"
    status="$(auto_reindex_status || true)"
    if [ -n "$status" ]; then
        anchor="${status%% *}"
        count="${status##* }"
        auto_reindex_anchor="$anchor"
        auto_reindex_count="$count"
        if [[ "$count" =~ ^[0-9]+$ ]] && [ "$count" -gt 0 ]; then
            auto_reindex_pending="true"
        fi
    fi

    tmp="$(mktemp "$DEV_DATADIR/agent-deploy.json.XXXXXX")" || return 0
    {
        printf '{\n'
        printf '  "schema": "zcl.agent_dev_deploy.v1",\n'
        printf '  "deployed_at_utc": "%s",\n' "$(json_escape "$now")"
        printf '  "build_commit": "%s",\n' "$(json_escape "$BUILD_COMMIT")"
        printf '  "build_type": "%s",\n' "$(json_escape "$DEV_DEPLOY_BUILD")"
        printf '  "build_artifact": "%s",\n' "$(json_escape "${BUILD_ARTIFACT:-}")"
        printf '  "installed_binary": "%s",\n' "$(json_escape "$DEV_BIN")"
        printf '  "service": "%s",\n' "$(json_escape "$UNIT")"
        printf '  "datadir": "%s",\n' "$(json_escape "$DEV_DATADIR")"
        printf '  "rpcport": 18252,\n'
        printf '  "verify_status": "%s",\n' "$(json_escape "$verify_status")"
        printf '  "verify_detail": "%s",\n' "$(json_escape "$verify_detail")"
        printf '  "auto_reindex_pending": %s,\n' "$auto_reindex_pending"
        printf '  "auto_reindex_anchor": "%s",\n' "$(json_escape "$auto_reindex_anchor")"
        printf '  "auto_reindex_count": "%s"\n' "$(json_escape "$auto_reindex_count")"
        printf '}\n'
    } > "$tmp"
    mv "$tmp" "$DEPLOY_STATE"
}

guard_pending_auto_reindex() {
    local status anchor count
    [ -e "$AUTO_REINDEX_SENTINEL" ] || return 0
    if [ ! -r "$AUTO_REINDEX_SENTINEL" ]; then
        echo "[dev-lane] WARN: unreadable auto-reindex marker: $AUTO_REINDEX_SENTINEL"
        return 0
    fi
    status="$(auto_reindex_status || true)"
    if [ -z "$status" ]; then
        echo "[dev-lane] WARN: ignoring malformed auto-reindex marker: $AUTO_REINDEX_SENTINEL"
        return 0
    fi
    anchor="${status%% *}"
    count="${status##* }"
    if [ "$count" = "-1" ]; then
        echo "[dev-lane] NOTE: terminal auto-reindex marker present anchor=$anchor; not a pending rebuild"
        return 0
    fi
    if ! [[ "$count" =~ ^[0-9]+$ ]] || [ "$count" -le 0 ]; then
        echo "[dev-lane] WARN: ignoring malformed auto-reindex marker: $AUTO_REINDEX_SENTINEL"
        return 0
    fi
    if [ "${ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY:-0}" != "1" ]; then
        echo "[dev-lane] BLOCKED: pending crash-only auto-reindex request anchor=$anchor count=$count"
        echo "[dev-lane] BLOCKED: refusing to start or hot-swap the dev lane because boot would consume the marker and rebuild chainstate before RPC is available"
        echo "[dev-lane] BLOCKED: let recovery finish, clear a stale marker only after proving it stale, or set ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1 to force this deploy"
        exit 1
    fi
    echo "[dev-lane] WARN: pending crash-only auto-reindex request anchor=$anchor count=$count; next boot will rebuild chainstate"
}

git update-index -q --refresh >/dev/null 2>&1 || true
BUILD_COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    BUILD_COMMIT="${BUILD_COMMIT}-dirty"
fi

guard_pending_auto_reindex

case "$DEV_DEPLOY_BUILD" in
    fast)
        echo "[dev-lane] building fast dev binary (stamp = $BUILD_COMMIT)..."
        make fast-rebuild >/dev/null
        BUILD_ARTIFACT="build/bin/zclassic23-dev"
        ;;
    strict)
        echo "[dev-lane] building strict node binary (stamp = $BUILD_COMMIT)..."
        make build/bin/zclassic23 -j"$(nproc)" >/dev/null
        BUILD_ARTIFACT="build/bin/zclassic23"
        ;;
    *)
        echo "[dev-lane] FATAL: unknown ZCL_DEV_DEPLOY_BUILD=$DEV_DEPLOY_BUILD (want fast or strict)" >&2
        exit 2
        ;;
esac

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
cp -f "$BUILD_ARTIFACT" "$DEV_BIN"

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
write_deploy_state "service_started" "systemd start issued"

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
    local validation_pack_ok blocker next readiness agent_work_ready
    local chain_serving_ready agent_ready

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
    chain_serving_ready="$(json_first_bool_field "$agent_json" "chain_serving_ready")"
    agent_work_ready="$(json_first_bool_field "$agent_json" "agent_work_ready")"

    if [ "$operator_needed" = "1" ] ||
       [ "$status" = "blocked" ] ||
       [ "$validation_pack_ok" = "0" ]; then
        echo "[dev-lane] BLOCKED: agent status=${status:-unknown} build=${build:-unknown} operator_needed=$operator_needed validation_pack_ok=$validation_pack_ok"
        echo "[dev-lane] BLOCKED: primary_blocker=${blocker:-unknown}"
        echo "[dev-lane] BLOCKED: next=${next:-zclassic23 healthcheck}"
        exit 1
    fi

    agent_ready=0
    if [ "$agent_work_ready" = "1" ]; then
        agent_ready=1
    elif [ "$agent_work_ready" = "null" ] &&
         [ "$healthy" = "1" ] && [ "$serving" = "1" ]; then
        agent_ready=1
    fi

    if [ "$agent_ready" = "1" ]; then
        echo "[dev-lane] AGENT READY: status=${status:-unknown} build=${build:-unknown} readiness=${readiness:-unknown} agent_work_ready=${agent_work_ready:-null}"
    else
        echo "[dev-lane] NOTE: agent status=${status:-unknown} healthy=$healthy serving=$serving chain_serving_ready=$chain_serving_ready agent_work_ready=$agent_work_ready; not declaring agent-ready yet"
        [ -n "$blocker" ] && echo "[dev-lane] NOTE: primary_blocker=$blocker"
        [ -n "$next" ] && echo "[dev-lane] NOTE: next=$next"
    fi
}

echo "[dev-lane] deployed $BUILD_COMMIT ($DEV_DEPLOY_BUILD); verifying sync health..."
h0=""; for i in $(seq 1 40); do
    h0="$("${CLI[@]}" getblockcount 2>/dev/null || true)"
    [ -n "$h0" ] && [[ "$h0" =~ ^[0-9]+$ ]] && break
    sleep 6
done
if [ -z "$h0" ] || ! [[ "$h0" =~ ^[0-9]+$ ]]; then
    echo "[dev-lane] WARN: RPC not up after ~4 min — still booting/reconciling. Check: tail $DEV_DATADIR/node.log"
    diag="$(pre_rpc_boot_diagnostic || true)"
    [ -n "$diag" ] && echo "[dev-lane] boot diagnostic: $diag"
    VERIFY_STATUS="pre_rpc"
    VERIFY_DETAIL="${diag:-rpc not up after readiness window}"
else
    sleep 20
    h1="$("${CLI[@]}" getblockcount 2>/dev/null || echo "$h0")"
    peer="$("${CLI[@]}" getpeerinfo 2>/dev/null | grep -oE '"(startingheight|synced_headers)": *[0-9]+' | grep -oE '[0-9]+' | sort -rn | head -1 || true)"
    if [ "${h1:-0}" -gt "${h0:-0}" ]; then
        echo "[dev-lane] SYNC OK: height advancing ${h0} -> ${h1} (peer tip ~${peer:-?})"
        VERIFY_STATUS="sync_ok"
        VERIFY_DETAIL="height advancing ${h0} -> ${h1} (peer tip ~${peer:-?})"
    elif [ -n "$peer" ] && [ "${h1:-0}" -ge $(( peer - 3 )) ]; then
        echo "[dev-lane] SYNC OK: at tip ${h1} (peer tip ~${peer})"
        VERIFY_STATUS="sync_ok"
        VERIFY_DETAIL="at tip ${h1} (peer tip ~${peer})"
    else
        echo "[dev-lane] NOTE: height ${h1} not advancing yet (peer tip ~${peer:-?}) — likely the cold-import reconcile; Restart=always will retry. Re-check: ${CLI[*]} getblockcount"
        VERIFY_STATUS="not_advancing_yet"
        VERIFY_DETAIL="height ${h1} not advancing yet (peer tip ~${peer:-?})"
    fi
    probe_agent_contract
fi
write_deploy_state "$VERIFY_STATUS" "$VERIFY_DETAIL"
echo "[dev-lane] deploy state: $DEPLOY_STATE"
echo "[dev-lane] query it:  build/bin/zclassic-cli -datadir=$DEV_DATADIR -rpcport=18252 getblockcount"
echo "[dev-lane] tail log:  tail -f $DEV_DATADIR/node.log"
