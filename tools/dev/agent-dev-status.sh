#!/usr/bin/env bash
# Print the no-build development-lane status for agents and operators.
#
# This is intentionally read-only. It never restarts the dev lane and never
# builds. Use it before deciding whether to run agent-stage-dev or deploy-dev.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

DEV_BIN="${ZCL_AGENT_DEV_BIN:-$HOME/.local/bin/zclassic23-dev}"
SRC_BIN="${ZCL_AGENT_SRC_BIN:-build/bin/zclassic23-dev}"
DEV_DATADIR="${ZCL_AGENT_DEV_DATADIR:-$HOME/.zclassic-c23-dev}"
DEV_RPCPORT="${ZCL_AGENT_DEV_RPCPORT:-18252}"
UNIT="${ZCL_AGENT_DEV_UNIT:-zcl23-dev.service}"
NODE_LOG="$DEV_DATADIR/node.log"
DEPLOY_STATE="$DEV_DATADIR/agent-deploy.json"
AUTO_REINDEX_SENTINEL="$DEV_DATADIR/auto_reindex_request"
SCHEMA="zcl.agent_dev_status.v1"
MODE="${1:-text}"

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

file_state_json() {
    local path="$1" executable="false" size="" mtime="" build_commit=""
    if [ -x "$path" ]; then
        executable="true"
        size="$(stat -c '%s' "$path" 2>/dev/null || true)"
        mtime="$(stat -c '%y' "$path" 2>/dev/null || true)"
        build_commit="$("$path" agentbuild 2>/dev/null |
            sed -n 's/.*"build_commit"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
            head -1 || true)"
    fi
    printf '{"path":"%s","executable":%s,"size":"%s","mtime":"%s","build_commit":"%s"}' \
        "$(json_escape "$path")" "$executable" \
        "$(json_escape "$size")" "$(json_escape "$mtime")" \
        "$(json_escape "$build_commit")"
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
        /\[coins\] flush ok: max_height=/ {
            progress=$0
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

auto_reindex_json() {
    local anchor="" count="" pending="false" malformed="false"
    if [ -r "$AUTO_REINDEX_SENTINEL" ]; then
        read -r anchor count < "$AUTO_REINDEX_SENTINEL" || malformed="true"
        if ! [[ "$anchor" =~ ^-?[0-9]+$ ]] ||
           ! [[ "$count" =~ ^-?[0-9]+$ ]]; then
            malformed="true"
        elif [[ "$count" =~ ^[0-9]+$ ]] && [ "$count" -gt 0 ]; then
            pending="true"
        fi
    fi
    printf '{"path":"%s","present":%s,"pending":%s,"malformed":%s,"anchor":"%s","count":"%s"}' \
        "$(json_escape "$AUTO_REINDEX_SENTINEL")" \
        "$([ -e "$AUTO_REINDEX_SENTINEL" ] && printf true || printf false)" \
        "$pending" "$malformed" "$(json_escape "$anchor")" \
        "$(json_escape "$count")"
}

json_bool_field() {
    local body="$1" key="$2" token
    token="$(printf '%s\n' "$body" |
        grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\\(true\\|false\\)" 2>/dev/null |
        head -1 || true)"
    case "$token" in
        *true) printf true ;;
        *false) printf false ;;
        *) printf unknown ;;
    esac
}

json_string_field() {
    local body="$1" key="$2" token
    token="$(printf '%s\n' "$body" |
        grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" 2>/dev/null |
        head -1 || true)"
    [ -n "$token" ] || return 0
    printf '%s\n' "$token" |
        sed -n "s/^\"${key}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"$/\1/p"
}

agent_marker_clear_probe() {
    local body operator_needed validation_pack_ok agent_work_ready blocker
    local timeout_s="${ZCL_AGENT_DEV_STATUS_AGENT_TIMEOUT:-3}"
    if ! body="$(timeout "$timeout_s" build/bin/zclassic-cli \
        -datadir="$DEV_DATADIR" -rpcport="$DEV_RPCPORT" agent 2>/dev/null)"; then
        printf 'unknown|agent_contract_unavailable'
        return 0
    fi
    operator_needed="$(json_bool_field "$body" "operator_needed")"
    validation_pack_ok="$(json_bool_field "$body" "validation_pack_ok")"
    agent_work_ready="$(json_bool_field "$body" "agent_work_ready")"
    blocker="$(json_string_field "$body" "primary_blocker")"
    if [ "$operator_needed" = "true" ]; then
        printf 'false|%s' "${blocker:-operator_needed}"
    elif [ "$validation_pack_ok" = "false" ]; then
        printf 'false|validation_pack_not_ok'
    elif [ "$agent_work_ready" = "false" ]; then
        printf 'false|agent_work_not_ready'
    else
        printf 'true|ready'
    fi
}

service_field() {
    local field="$1"
    systemctl --user show "$UNIT" -p "$field" --value 2>/dev/null || true
}

is_uint() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

memory_pressure_state() {
    local current="$1" high="$2" threshold
    if is_uint "$current" && is_uint "$high" && [ "$high" -gt 0 ]; then
        threshold=$((high * 85 / 100))
        if [ "$current" -ge "$high" ]; then
            printf 'warn'
        elif [ "$current" -ge "$threshold" ]; then
            printf 'watch'
        else
            printf 'ok'
        fi
    else
        printf 'unknown'
    fi
}

service_json() {
    local active pid started linger mem_current mem_high mem_max pressure wchan=""
    active="$(service_field ActiveState)"
    pid="$(service_field MainPID)"
    started="$(service_field ExecMainStartTimestamp)"
    linger="$(loginctl show-user "$USER" -p Linger --value 2>/dev/null || true)"
    mem_current="$(service_field MemoryCurrent)"
    mem_high="$(service_field MemoryHigh)"
    mem_max="$(service_field MemoryMax)"
    pressure="$(memory_pressure_state "$mem_current" "$mem_high")"
    if is_uint "$pid" && [ "$pid" -gt 0 ] && [ -r "/proc/$pid/wchan" ]; then
        wchan="$(cat "/proc/$pid/wchan" 2>/dev/null || true)"
    fi
    printf '{"unit":"%s","active_state":"%s","main_pid":"%s","started":"%s","linger":"%s","memory_current_bytes":"%s","memory_high_bytes":"%s","memory_max_bytes":"%s","memory_pressure":"%s","wait_channel":"%s"}' \
        "$(json_escape "$UNIT")" "$(json_escape "$active")" \
        "$(json_escape "$pid")" "$(json_escape "$started")" \
        "$(json_escape "$linger")" "$(json_escape "$mem_current")" \
        "$(json_escape "$mem_high")" "$(json_escape "$mem_max")" \
        "$(json_escape "$pressure")" "$(json_escape "$wchan")"
}

rpc_json() {
    local height="" status="unreachable" detail="" diag=""
    height="$(build/bin/zclassic-cli -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" getblockcount 2>/dev/null || true)"
    if [[ "$height" =~ ^[0-9]+$ ]]; then
        status="ok"
        detail="height=$height"
    else
        diag="$(pre_rpc_boot_diagnostic || true)"
        detail="${diag:-rpc unavailable}"
    fi
    printf '{"status":"%s","height":"%s","detail":"%s","rpcport":"%s"}' \
        "$(json_escape "$status")" "$(json_escape "$height")" \
        "$(json_escape "$detail")" "$(json_escape "$DEV_RPCPORT")"
}

deploy_state_summary_json() {
    local verify_status="" verify_detail="" build_commit="" deployed_at=""
    if [ -r "$DEPLOY_STATE" ] && command -v jq >/dev/null 2>&1; then
        verify_status="$(jq -r '.verify_status // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        verify_detail="$(jq -r '.verify_detail // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        build_commit="$(jq -r '.build_commit // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        deployed_at="$(jq -r '.deployed_at_utc // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
    fi
    printf '{"path":"%s","present":%s,"build_commit":"%s","deployed_at_utc":"%s","verify_status":"%s","verify_detail":"%s"}' \
        "$(json_escape "$DEPLOY_STATE")" \
        "$([ -r "$DEPLOY_STATE" ] && printf true || printf false)" \
        "$(json_escape "$build_commit")" "$(json_escape "$deployed_at")" \
        "$(json_escape "$verify_status")" "$(json_escape "$verify_detail")"
}

staged_matches() {
    [ -x "$SRC_BIN" ] && [ -x "$DEV_BIN" ] && cmp -s "$SRC_BIN" "$DEV_BIN"
}

next_action() {
    local rpc_status="$1" active_state="$2" auto_pending="$3"
    local stale_candidate="$4"
    if [ "$auto_pending" = "true" ]; then
        if [ "$stale_candidate" = "true" ]; then
            printf 'make agent-clear-stale-dev-reindex'
        else
            printf 'inspect dev agent blocker before clearing auto_reindex_request; use ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1 only for deliberate recovery boot'
        fi
    elif [ "$rpc_status" = "ok" ]; then
        printf 'make agent-mcp-call-dev TOOL=zcl_status'
    elif [ "$active_state" = "active" ]; then
        printf 'wait; tail -f %s' "$NODE_LOG"
    else
        printf 'make agent-deploy-fast'
    fi
}

emit_json() {
    local src installed service rpc deploy auto active_state rpc_status auto_pending
    local rpc_height auto_anchor auto_count agent_probe agent_ready agent_reason
    local deploy_blocker="false"
    local deploy_blocker_reason="" stale_candidate="false" staged="false" action
    src="$(file_state_json "$SRC_BIN")"
    installed="$(file_state_json "$DEV_BIN")"
    service="$(service_json)"
    rpc="$(rpc_json)"
    deploy="$(deploy_state_summary_json)"
    auto="$(auto_reindex_json)"
    staged_matches && staged="true"
    active_state="$(printf '%s\n' "$service" |
        sed -n 's/.*"active_state":"\([^"]*\)".*/\1/p')"
    rpc_status="$(printf '%s\n' "$rpc" |
        sed -n 's/.*"status":"\([^"]*\)".*/\1/p')"
    rpc_height="$(printf '%s\n' "$rpc" |
        sed -n 's/.*"height":"\([^"]*\)".*/\1/p')"
    auto_pending="$(printf '%s\n' "$auto" |
        sed -n 's/.*"pending":\([^,}]*\).*/\1/p')"
    auto_anchor="$(printf '%s\n' "$auto" |
        sed -n 's/.*"anchor":"\([^"]*\)".*/\1/p')"
    auto_count="$(printf '%s\n' "$auto" |
        sed -n 's/.*"count":"\([^"]*\)".*/\1/p')"
    if [ "$auto_pending" = "true" ]; then
        deploy_blocker="true"
        deploy_blocker_reason="pending_auto_reindex_requires_explicit_recovery_boot"
        if [ "$rpc_status" = "ok" ] && is_uint "$rpc_height" &&
           is_uint "$auto_anchor" && [ "$rpc_height" -ge "$auto_anchor" ]; then
            agent_probe="$(agent_marker_clear_probe)"
            agent_ready="${agent_probe%%|*}"
            agent_reason="${agent_probe#*|}"
            if [ "$agent_ready" = "true" ]; then
                stale_candidate="true"
            fi
        fi
    fi
    agent_ready="${agent_ready:-not_checked}"
    agent_reason="${agent_reason:-not_checked}"
    action="$(next_action "$rpc_status" "$active_state" "$auto_pending" \
        "$stale_candidate")"
    printf '{\n'
    printf '  "schema": "%s",\n' "$SCHEMA"
    printf '  "source_binary": %s,\n' "$src"
    printf '  "installed_binary": %s,\n' "$installed"
    printf '  "installed_matches_source": %s,\n' "$staged"
    printf '  "service": %s,\n' "$service"
    printf '  "rpc": %s,\n' "$rpc"
    printf '  "deploy_state": %s,\n' "$deploy"
    printf '  "auto_reindex": %s,\n' "$auto"
    printf '  "deploy_blocker": %s,\n' "$deploy_blocker"
    printf '  "deploy_blocker_reason": "%s",\n' \
        "$(json_escape "$deploy_blocker_reason")"
    printf '  "explicit_recovery_env": "ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY",\n'
    printf '  "agent_contract_ready_for_marker_clear": "%s",\n' \
        "$(json_escape "$agent_ready")"
    printf '  "agent_contract_marker_clear_reason": "%s",\n' \
        "$(json_escape "$agent_reason")"
    printf '  "auto_reindex_stale_candidate": %s,\n' "$stale_candidate"
    printf '  "next_action": "%s"\n' "$(json_escape "$action")"
    printf '}\n'
}

emit_text() {
    local json
    json="$(emit_json)"
    if command -v jq >/dev/null 2>&1; then
        printf '%s\n' "$json" | jq -r '
            "[agent-dev-status] service=" + .service.active_state +
            " pid=" + .service.main_pid +
            " rpc=" + .rpc.status +
            " detail=" + .rpc.detail,
            "[agent-dev-status] source_bin=" + .source_binary.path +
            " installed_bin=" + .installed_binary.path +
            " source_commit=" + .source_binary.build_commit +
            " installed_commit=" + .installed_binary.build_commit +
            " staged_matches_source=" + (.installed_matches_source|tostring),
            "[agent-dev-status] memory current=" + .service.memory_current_bytes +
            " high=" + .service.memory_high_bytes +
            " max=" + .service.memory_max_bytes +
            " pressure=" + .service.memory_pressure +
            " wait_channel=" + .service.wait_channel,
            "[agent-dev-status] auto_reindex pending=" + (.auto_reindex.pending|tostring) +
            " anchor=" + .auto_reindex.anchor +
            " count=" + .auto_reindex.count,
            "[agent-dev-status] deploy_blocker=" + (.deploy_blocker|tostring) +
            " reason=" + .deploy_blocker_reason +
            " stale_candidate=" + (.auto_reindex_stale_candidate|tostring) +
            " agent_marker_clear=" + .agent_contract_ready_for_marker_clear +
            " marker_clear_reason=" + .agent_contract_marker_clear_reason,
            "[agent-dev-status] deploy_state=" + .deploy_state.path +
            " present=" + (.deploy_state.present|tostring) +
            " verify=" + .deploy_state.verify_status,
            "[agent-dev-status] next=" + .next_action'
    else
        printf '%s\n' "$json"
    fi
}

case "$MODE" in
    --json|json)
        emit_json
        ;;
    ""|text)
        emit_text
        ;;
    *)
        echo "usage: tools/dev/agent-dev-status.sh [--json]" >&2
        exit 2
        ;;
esac
