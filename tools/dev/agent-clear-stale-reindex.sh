#!/usr/bin/env bash
# Clear a stale dev-lane auto_reindex_request only after proving the lane serves
# at or above the marker anchor. This touches the dev lane only.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

DEV_DATADIR="${ZCL_AGENT_DEV_DATADIR:-$HOME/.zclassic-c23-dev}"
DEV_RPCPORT="${ZCL_AGENT_DEV_RPCPORT:-18252}"
UNIT="${ZCL_AGENT_DEV_UNIT:-zcl23-dev.service}"
MARKER="$DEV_DATADIR/auto_reindex_request"
MODE="${1:-text}"
DRY_RUN="${ZCL_AGENT_CLEAR_STALE_REINDEX_DRY_RUN:-0}"
SCHEMA="zcl.agent_dev_reindex_clear.v1"

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

is_int() {
    [[ "${1:-}" =~ ^-?[0-9]+$ ]]
}

is_uint() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

service_active() {
    local active
    active="$(systemctl --user show "$UNIT" -p ActiveState --value 2>/dev/null || true)"
    [ "$active" = "active" ]
}

rpc_height() {
    build/bin/zclassic-cli -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" getblockcount 2>/dev/null || true
}

agent_bool_field() {
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

agent_status_ok() {
    local body operator_needed validation_pack_ok agent_work_ready
    local timeout_s="${ZCL_AGENT_CLEAR_STALE_REINDEX_AGENT_TIMEOUT:-10}"
    if ! body="$(timeout "$timeout_s" build/bin/zclassic-cli \
        -datadir="$DEV_DATADIR" -rpcport="$DEV_RPCPORT" agent 2>/dev/null)"; then
        return 1
    fi
    [ -n "$body" ] || return 1
    operator_needed="$(agent_bool_field "$body" "operator_needed")"
    validation_pack_ok="$(agent_bool_field "$body" "validation_pack_ok")"
    agent_work_ready="$(agent_bool_field "$body" "agent_work_ready")"
    [ "$operator_needed" != "true" ] &&
        [ "$validation_pack_ok" != "false" ] &&
        [ "$agent_work_ready" != "false" ]
}

emit_json() {
    local status="$1" reason="$2" anchor="$3" count="$4" height="$5" moved="$6"
    printf '{\n'
    printf '  "schema": "%s",\n' "$SCHEMA"
    printf '  "status": "%s",\n' "$(json_escape "$status")"
    printf '  "reason": "%s",\n' "$(json_escape "$reason")"
    printf '  "datadir": "%s",\n' "$(json_escape "$DEV_DATADIR")"
    printf '  "marker": "%s",\n' "$(json_escape "$MARKER")"
    printf '  "archive": "%s",\n' "$(json_escape "$moved")"
    printf '  "rpcport": "%s",\n' "$(json_escape "$DEV_RPCPORT")"
    printf '  "height": "%s",\n' "$(json_escape "$height")"
    printf '  "anchor": "%s",\n' "$(json_escape "$anchor")"
    printf '  "count": "%s",\n' "$(json_escape "$count")"
    printf '  "dry_run": %s\n' "$([ "$DRY_RUN" = "1" ] && printf true || printf false)"
    printf '}\n'
}

finish() {
    local status="$1" reason="$2" anchor="${3:-}" count="${4:-}" height="${5:-}" moved="${6:-}"
    case "$MODE" in
        --json|json)
            emit_json "$status" "$reason" "$anchor" "$count" "$height" "$moved"
            ;;
        ""|text)
            printf '[agent-clear-stale-reindex] status=%s reason=%s height=%s anchor=%s count=%s\n' \
                "$status" "$reason" "$height" "$anchor" "$count"
            [ -n "$moved" ] &&
                printf '[agent-clear-stale-reindex] archived=%s\n' "$moved"
            ;;
        *)
            echo "usage: tools/dev/agent-clear-stale-reindex.sh [--json]" >&2
            exit 2
            ;;
    esac
}

if [ ! -e "$MARKER" ]; then
    finish noop marker_absent "" "" "" ""
    exit 0
fi
if [ ! -r "$MARKER" ]; then
    finish refused marker_unreadable "" "" "" ""
    exit 1
fi

read -r anchor count < "$MARKER" || {
    finish refused marker_read_failed "" "" "" ""
    exit 1
}
if ! is_int "$anchor" || ! is_int "$count"; then
    finish refused marker_malformed "$anchor" "$count" "" ""
    exit 1
fi
if [ "$count" -le 0 ]; then
    finish noop marker_not_pending "$anchor" "$count" "" ""
    exit 0
fi
if ! service_active; then
    finish refused service_not_active "$anchor" "$count" "" ""
    exit 1
fi

height="$(rpc_height)"
if ! is_uint "$height"; then
    finish refused rpc_height_unavailable "$anchor" "$count" "$height" ""
    exit 1
fi
if [ "$height" -lt "$anchor" ]; then
    finish refused marker_anchor_above_served_height "$anchor" "$count" "$height" ""
    exit 1
fi
if ! agent_status_ok; then
    finish refused agent_contract_not_ready "$anchor" "$count" "$height" ""
    exit 1
fi

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
archive="$MARKER.cleared-$stamp"
if [ "$DRY_RUN" = "1" ]; then
    finish would_clear stale_marker_proven "$anchor" "$count" "$height" "$archive"
    exit 0
fi
mv "$MARKER" "$archive"
finish cleared stale_marker_proven "$anchor" "$count" "$height" "$archive"
