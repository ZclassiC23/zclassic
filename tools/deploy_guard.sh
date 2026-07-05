#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Guard the long-running canonical node from accidental deploy restarts.
# Prefer the native zcl.operator_deployment_safety.v1 contract exposed by the
# running daemon; fall back to the systemd ExecStart lane flag for older
# binaries that do not expose the nested safety contract yet.

set -euo pipefail

ACTION="${1:-canonical-deploy}"
UNIT="${ZCL_DEPLOY_GUARD_UNIT:-zclassic23}"
RPC_TOOL="${ZCL_DEPLOY_GUARD_RPC_TOOL:-./build/bin/zclassic-cli}"
RPC_TIMEOUT="${ZCL_DEPLOY_GUARD_RPC_TIMEOUT:-5}"
DEFAULT_DATADIR="${ZCL_DATADIR:-$HOME/.zclassic-c23}"
DEFAULT_RPCPORT="${ZCL_RPCPORT:-18232}"

log() {
    printf 'deploy_guard: %s\n' "$*"
}

fail() {
    printf 'deploy_guard: REFUSE: %s\n' "$*" >&2
    printf 'deploy_guard: set ZCL_DEPLOY_ALLOW_CANONICAL=1 only for an intentional canonical restart window\n' >&2
    exit 1
}

is_true() {
    case "${1:-}" in
        1|true|TRUE|yes|YES|on|ON) return 0 ;;
        *) return 1 ;;
    esac
}

systemd_show() {
    local prop="$1"
    if [ -n "${ZCL_DEPLOY_GUARD_SYSTEMD_ACTIVE:-}" ] &&
       [ "$prop" = "ActiveState" ]; then
        printf '%s\n' "$ZCL_DEPLOY_GUARD_SYSTEMD_ACTIVE"
        return 0
    fi
    if [ -n "${ZCL_DEPLOY_GUARD_SYSTEMD_EXECSTART:-}" ] &&
       [ "$prop" = "ExecStart" ]; then
        printf '%s\n' "$ZCL_DEPLOY_GUARD_SYSTEMD_EXECSTART"
        return 0
    fi
    command -v systemctl >/dev/null 2>&1 || return 1
    systemctl --user show "$UNIT" -p "$prop" --value 2>/dev/null || true
}

exec_arg_value() {
    local cmdline="$1" key="$2"
    printf '%s\n' "$cmdline" |
        tr ' ' '\n' |
        sed -n "s/^${key}=//p" |
        head -1
}

lane_is_canonical() {
    case "${1:-}" in
        canonical|live|main) return 0 ;;
        *) return 1 ;;
    esac
}

json_rpc_result() {
    command -v python3 >/dev/null 2>&1 || {
        printf '%s\n' "$1"
        return 0
    }
    printf '%s\n' "$1" | python3 -c '
import json
import sys
raw = sys.stdin.read()
try:
    d = json.loads(raw)
except Exception:
    sys.stdout.write(raw)
    sys.exit(0)
if isinstance(d, dict) and "result" in d and d.get("error") in (None, {}) and d.get("result") is not None:
    result = d.get("result")
    if isinstance(result, str):
        sys.stdout.write(result)
    else:
        sys.stdout.write(json.dumps(result, separators=(",", ":")))
else:
    sys.stdout.write(raw)
' 2>/dev/null
}

json_get() {
    local body="$1" path="$2"
    command -v python3 >/dev/null 2>&1 || return 1
    printf '%s\n' "$body" | python3 -c '
import json
import sys
try:
    cur = json.load(sys.stdin)
except Exception:
    sys.exit(1)
for part in sys.argv[1].split("."):
    if isinstance(cur, dict) and part in cur:
        cur = cur[part]
    else:
        sys.exit(1)
if isinstance(cur, bool):
    print("true" if cur else "false")
elif cur is None:
    sys.exit(1)
else:
    print(cur)
' "$path" 2>/dev/null
}

service_exec_start() {
    systemd_show ExecStart || true
}

service_active_state() {
    systemd_show ActiveState || true
}

service_exec_datadir() {
    local cmdline="$1" datadir
    datadir="$(exec_arg_value "$cmdline" "-datadir")"
    printf '%s\n' "${datadir:-$DEFAULT_DATADIR}"
}

service_exec_rpcport() {
    local cmdline="$1" rpcport
    rpcport="$(exec_arg_value "$cmdline" "-rpcport")"
    printf '%s\n' "${rpcport:-$DEFAULT_RPCPORT}"
}

read_agent_json() {
    if [ -n "${ZCL_DEPLOY_GUARD_AGENT_JSON:-}" ]; then
        printf '%s\n' "$ZCL_DEPLOY_GUARD_AGENT_JSON"
        return 0
    fi

    [ -x "$RPC_TOOL" ] || return 1
    local execstart datadir rpcport raw
    execstart="$(service_exec_start)"
    datadir="$(service_exec_datadir "$execstart")"
    rpcport="$(service_exec_rpcport "$execstart")"
    if command -v timeout >/dev/null 2>&1; then
        raw="$(timeout "${RPC_TIMEOUT}s" "$RPC_TOOL" \
            "-datadir=$datadir" "-rpcport=$rpcport" agent 2>/dev/null || true)"
    else
        raw="$("$RPC_TOOL" "-datadir=$datadir" "-rpcport=$rpcport" agent 2>/dev/null || true)"
    fi
    [ -n "$raw" ] || return 1
    json_rpc_result "$raw"
}

allow() {
    log "allow: $*"
    exit 0
}

guard_from_agent_json() {
    local body="$1"
    local lane schema deploy_ok restart_ok requires canonical safe_action guard_env

    lane="$(json_get "$body" "operator_lane.lane" || true)"
    schema="$(json_get "$body" "operator_lane.deployment_safety.schema" || true)"
    deploy_ok="$(json_get "$body" "operator_lane.deployment_safety.automation_deploy_ok" || true)"
    restart_ok="$(json_get "$body" "operator_lane.deployment_safety.automation_restart_ok" || true)"
    requires="$(json_get "$body" "operator_lane.deployment_safety.requires_operator_confirmation" || true)"
    canonical="$(json_get "$body" "operator_lane.canonical" || true)"
    safe_action="$(json_get "$body" "operator_lane.deployment_safety.safe_default_action" || true)"
    guard_env="$(json_get "$body" "operator_lane.deployment_safety.guard_env" || true)"

    [ -n "$lane$schema$deploy_ok$restart_ok$requires$canonical" ] || return 2

    if [ "$schema" = "zcl.operator_deployment_safety.v1" ]; then
        if [ "$deploy_ok" = "true" ] &&
           [ "$restart_ok" = "true" ] &&
           [ "$requires" != "true" ]; then
            allow "native deployment_safety permits action=$ACTION lane=${lane:-unknown}"
        fi
        fail "native deployment_safety blocks action=$ACTION lane=${lane:-unknown} deploy_ok=${deploy_ok:-unknown} restart_ok=${restart_ok:-unknown} requires_operator_confirmation=${requires:-unknown} guard_env=${guard_env:-unknown} safe_default_action=${safe_action:-unknown}"
    fi

    if [ "$canonical" = "true" ] || lane_is_canonical "$lane"; then
        fail "legacy operator_lane reports canonical lane without deployment_safety"
    fi

    local development ephemeral
    development="$(json_get "$body" "operator_lane.development" || true)"
    ephemeral="$(json_get "$body" "operator_lane.ephemeral" || true)"
    if [ "$development" = "true" ] || [ "$ephemeral" = "true" ]; then
        allow "legacy operator_lane permits noncanonical lane=${lane:-unknown}"
    fi

    return 2
}

guard_from_systemd() {
    local active execstart lane
    active="$(service_active_state)"
    execstart="$(service_exec_start)"
    lane="$(exec_arg_value "$execstart" "-operator-lane")"

    if [ "$active" = "active" ]; then
        if lane_is_canonical "$lane"; then
            fail "active systemd unit $UNIT is operator-lane=${lane}"
        fi
        if [ -z "$lane" ]; then
            fail "active systemd unit $UNIT has no declared operator lane"
        fi
        allow "active systemd unit $UNIT is noncanonical lane=$lane"
    fi

    allow "no active canonical systemd lane detected for $UNIT"
}

selftest_case_pass() {
    local name="$1"
    shift
    if ! env -u ZCL_DEPLOY_GUARD_SELFTEST "$@" "$0" canonical-deploy >/dev/null 2>&1; then
        printf 'deploy_guard selftest FAIL pass-case: %s\n' "$name" >&2
        exit 1
    fi
}

selftest_case_fail() {
    local name="$1"
    shift
    if env -u ZCL_DEPLOY_GUARD_SELFTEST "$@" "$0" canonical-deploy >/dev/null 2>&1; then
        printf 'deploy_guard selftest FAIL fail-case: %s\n' "$name" >&2
        exit 1
    fi
}

selftest() {
    local canonical_json dev_json
    canonical_json='{"schema":"zcl.public_status.v1","operator_lane":{"schema":"zcl.operator_lane.v1","lane":"canonical","canonical":true,"deployment_safety":{"schema":"zcl.operator_deployment_safety.v1","automation_restart_ok":false,"automation_deploy_ok":false,"requires_operator_confirmation":true,"guard_env":"ZCL_DEPLOY_ALLOW_CANONICAL","safe_default_action":"observe_only_or_use_dev_lane"}}}'
    dev_json='{"schema":"zcl.public_status.v1","operator_lane":{"schema":"zcl.operator_lane.v1","lane":"dev","development":true,"deployment_safety":{"schema":"zcl.operator_deployment_safety.v1","automation_restart_ok":true,"automation_deploy_ok":true,"requires_operator_confirmation":false,"safe_default_action":"deploy_dev_lane"}}}'

    selftest_case_fail "native canonical blocks" \
        ZCL_DEPLOY_GUARD_AGENT_JSON="$canonical_json"
    selftest_case_pass "native dev allows" \
        ZCL_DEPLOY_GUARD_AGENT_JSON="$dev_json"
    selftest_case_pass "explicit canonical override allows" \
        ZCL_DEPLOY_GUARD_AGENT_JSON="$canonical_json" \
        ZCL_DEPLOY_ALLOW_CANONICAL=1
    selftest_case_fail "active canonical systemd fallback blocks" \
        ZCL_DEPLOY_GUARD_AGENT_JSON= \
        ZCL_DEPLOY_GUARD_SYSTEMD_ACTIVE=active \
        ZCL_DEPLOY_GUARD_SYSTEMD_EXECSTART='path=/x/zclassic23 ; argv[]=/x/zclassic23 -datadir=/tmp/z -operator-lane=canonical -rpcport=18232'
    selftest_case_pass "inactive canonical systemd fallback allows" \
        ZCL_DEPLOY_GUARD_AGENT_JSON= \
        ZCL_DEPLOY_GUARD_SYSTEMD_ACTIVE=inactive \
        ZCL_DEPLOY_GUARD_SYSTEMD_EXECSTART='path=/x/zclassic23 ; argv[]=/x/zclassic23 -datadir=/tmp/z -operator-lane=canonical -rpcport=18232'

    log "selftest OK"
}

main() {
    case "$ACTION" in
        canonical-deploy|canonical-restart) ;;
        -h|--help)
            cat <<'USAGE'
usage: tools/deploy_guard.sh [canonical-deploy]

Blocks accidental restarts of an active canonical zclassic23 lane unless
ZCL_DEPLOY_ALLOW_CANONICAL=1 is set for an intentional restart window.
USAGE
            exit 0
            ;;
        *)
            fail "unknown action: $ACTION"
            ;;
    esac

    if is_true "${ZCL_DEPLOY_GUARD_SELFTEST:-0}"; then
        selftest
        exit 0
    fi

    if is_true "${ZCL_DEPLOY_ALLOW_CANONICAL:-0}"; then
        allow "ZCL_DEPLOY_ALLOW_CANONICAL=1 explicitly permits action=$ACTION"
    fi

    local agent_json
    agent_json="$(read_agent_json || true)"
    if [ -n "$agent_json" ]; then
        guard_from_agent_json "$agent_json" || true
    fi

    guard_from_systemd
}

main "$@"
