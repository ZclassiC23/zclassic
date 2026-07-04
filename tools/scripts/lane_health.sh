#!/usr/bin/env bash
# Read-only health summary for the live/soak/dev node lanes.
#
# This is an operational guard, not a failover controller. It makes the
# three-lane topology visible so development restarts do not accidentally
# consume the public node or the long-running soak evidence lane.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

JSON=0
STRICT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --json) JSON=1 ;;
        --strict) STRICT=1 ;;
        -h|--help)
            cat <<'USAGE'
usage: tools/scripts/lane_health.sh [--json] [--strict]

Reports live, soak, and dev lane systemd/RPC/socket health without mutating
any service. --strict exits non-zero when any lane has status=fail.
USAGE
            exit 0
            ;;
        *)
            echo "lane-health: unknown arg '$1'" >&2
            exit 2
            ;;
    esac
    shift
done

ZCL_CLI="${ZCL_CLI:-$REPO_ROOT/build/bin/zclassic-cli}"
RPC_TIMEOUT="${ZCL_LANE_RPC_TIMEOUT:-3}"
LAG_WARN="${ZCL_LANE_LAG_WARN:-10}"

json_escape() {
    printf '%s' "$1" \
        | sed 's/\\/\\\\/g; s/"/\\"/g; s/	/\\t/g; s/\r/\\r/g' \
        | tr '\n' ' '
}

json_bool() {
    if [ "$1" = "1" ]; then
        printf 'true'
    else
        printf 'false'
    fi
}

systemd_show() {
    local unit="$1" prop="$2"
    command -v systemctl >/dev/null 2>&1 || return 1
    systemctl --user show "$unit" -p "$prop" --value 2>/dev/null || true
}

listen_state() {
    local port="$1"
    command -v ss >/dev/null 2>&1 || return 1
    ss -ltn 2>/dev/null | grep -Eq "[:.]${port}[[:space:]]"
}

rpc_call() {
    local datadir="$1" rpcport="$2" method="$3"
    [ -x "$ZCL_CLI" ] || return 1
    timeout "$RPC_TIMEOUT" "$ZCL_CLI" -datadir="$datadir" -rpcport="$rpcport" "$method" 2>/dev/null
}

peer_count_from_json() {
    grep -o '"addr"[[:space:]]*:' 2>/dev/null | wc -l | tr -d ' '
}

report_lane() {
    local lane="$1" unit="$2" datadir="$3" rpcport="$4" p2p_port="$5" role="$6"
    local active mainpid restarts cmdline rpc_up height peers p2p_listening rpc_listening reindex
    local tip_lag status reason

    active="$(systemd_show "$unit" ActiveState)"
    [ -n "$active" ] || active="unknown"
    mainpid="$(systemd_show "$unit" MainPID)"
    [ -n "$mainpid" ] || mainpid="0"
    restarts="$(systemd_show "$unit" NRestarts)"
    [ -n "$restarts" ] || restarts="null"

    cmdline=""
    if [ "$mainpid" != "0" ] && [ -r "/proc/$mainpid/cmdline" ]; then
        cmdline="$(tr '\0' ' ' <"/proc/$mainpid/cmdline")"
    fi

    reindex=0
    case "$cmdline" in
        *" -reindex-chainstate"*|*"-reindex-chainstate "*) reindex=1 ;;
    esac

    p2p_listening=0
    rpc_listening=0
    listen_state "$p2p_port" && p2p_listening=1
    listen_state "$rpcport" && rpc_listening=1

    rpc_up=0
    height=""
    if height="$(rpc_call "$datadir" "$rpcport" getblockcount)"; then
        case "$height" in
            ''|*[!0-9]*) height="" ;;
            *) rpc_up=1 ;;
        esac
    else
        height=""
    fi

    peers=""
    if [ "$rpc_up" = "1" ]; then
        peers="$(rpc_call "$datadir" "$rpcport" getpeerinfo | peer_count_from_json)"
    fi
    [ -n "$peers" ] || peers="null"

    tip_lag="null"
    if [ -n "$height" ] && [ -n "$LIVE_REFERENCE_HEIGHT" ]; then
        if [ "$height" -le "$LIVE_REFERENCE_HEIGHT" ]; then
            tip_lag=$((LIVE_REFERENCE_HEIGHT - height))
        else
            tip_lag=0
        fi
    fi

    status="ok"
    reason="healthy"
    if [ "$active" != "active" ]; then
        status="fail"
        reason="unit_not_active"
    elif [ "$reindex" = "1" ]; then
        status="fail"
        reason="forced_reindex_flag_present"
    elif [ "$rpc_up" != "1" ]; then
        if [ "$lane" = "dev" ]; then
            status="warn"
            reason="dev_booting_rpc_down"
        else
            status="fail"
            reason="rpc_down"
        fi
    elif [ "$tip_lag" != "null" ] && [ "$tip_lag" -gt "$LAG_WARN" ]; then
        status="warn"
        reason="lag_to_live_${tip_lag}"
    elif [ "$p2p_listening" != "1" ] || [ "$rpc_listening" != "1" ]; then
        status="warn"
        reason="listener_missing"
    fi

    case "$status" in
        ok) ok_count=$((ok_count + 1)) ;;
        warn) warn_count=$((warn_count + 1)) ;;
        *) fail_count=$((fail_count + 1)) ;;
    esac

    if [ "$JSON" = "1" ]; then
        printf '{"lane":"%s","unit":"%s","datadir":"%s","rpcport":%s,"p2p_port":%s,"role":"%s","active_state":"%s","mainpid":%s,"restarts":%s,"rpc_up":%s,"height":%s,"tip_lag_to_live":%s,"peer_count":%s,"p2p_listening":%s,"rpc_listening":%s,"reindex_chainstate":%s,"status":"%s","reason":"%s"}\n' \
            "$(json_escape "$lane")" \
            "$(json_escape "$unit")" \
            "$(json_escape "$datadir")" \
            "$rpcport" \
            "$p2p_port" \
            "$(json_escape "$role")" \
            "$(json_escape "$active")" \
            "$mainpid" \
            "$restarts" \
            "$(json_bool "$rpc_up")" \
            "${height:-null}" \
            "$tip_lag" \
            "$peers" \
            "$(json_bool "$p2p_listening")" \
            "$(json_bool "$rpc_listening")" \
            "$(json_bool "$reindex")" \
            "$(json_escape "$status")" \
            "$(json_escape "$reason")"
    else
        printf 'lane-health: %-4s status=%-4s reason=%-28s unit=%-20s active=%-8s pid=%-7s rpc=%-4s height=%-8s lag=%-8s peers=%-4s p2p_listen=%s rpc_listen=%s reindex=%s\n' \
            "$lane" "$status" "$reason" "$unit" "$active" "$mainpid" \
            "$([ "$rpc_up" = "1" ] && printf up || printf down)" \
            "${height:-null}" "$tip_lag" "$peers" \
            "$([ "$p2p_listening" = "1" ] && printf yes || printf no)" \
            "$([ "$rpc_listening" = "1" ] && printf yes || printf no)" \
            "$([ "$reindex" = "1" ] && printf yes || printf no)"
    fi
}

ok_count=0
warn_count=0
fail_count=0

LIVE_REFERENCE_HEIGHT=""
if live_height="$(rpc_call "$HOME/.zclassic-c23" 18232 getblockcount)"; then
    case "$live_height" in
        ''|*[!0-9]*) LIVE_REFERENCE_HEIGHT="" ;;
        *) LIVE_REFERENCE_HEIGHT="$live_height" ;;
    esac
fi

report_lane live zclassic23 "$HOME/.zclassic-c23" 18232 8033 "public daily-driver"
report_lane soak zclassic23-soak "$HOME/.zclassic-c23-soak" 18242 8043 "long-uptime evidence"
report_lane dev zcl23-dev "$HOME/.zclassic-c23-dev" 18252 8053 "fresh-build development"

if [ "$JSON" != "1" ]; then
    echo "lane-health: SUMMARY ok=$ok_count warn=$warn_count fail=$fail_count"
fi

if [ "$STRICT" = "1" ] && [ "$fail_count" -gt 0 ]; then
    exit 1
fi
exit 0
