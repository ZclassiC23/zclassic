#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# S6 acceptance: seven isolated regtest nodes anchor independent ZID masters,
# provision chain-bound DHT delegations, then prove hostile-frame rejection,
# sparse iterative FIND_NODE, fair concurrency, persistence, and cold reauth.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

DHT_LIVE_PORTS="8023 8033 8034 8035 8043 8044 8045 8046 8232 8443 \
18034 18232 18234 18243 18244 18245 18246"
# P2P reconnects pass the production reachable-port policy; use two of its
# explicit test-safe ports rather than arbitrary high ports that only the
# initial operator-directed -connect dial may bypass.
A_PORT=20022; A_RPC=39211; A_FS=39212; A_HTTPS=39213
B_PORT=18033; B_RPC=39221; B_FS=39222; B_HTTPS=39223
DEAD_SINK=39999
DHT_WAIT="${DHT_WAIT:-90}"
DHT_PACKAGEHOST="${DHT_PACKAGEHOST:-0}"
DHT_BUILDWORKERS="${DHT_BUILDWORKERS:-0}"
DHT_AFTER_SPARSE_HOOK="${DHT_AFTER_SPARSE_HOOK:-}"
DHT_WORK_PARENT="${DHT_WORK_PARENT:-$REPO_ROOT/test-tmp}"
DHT_PARAMS_DIR="${DHT_PARAMS_DIR:-}"
DHT_WORK=""; DHT_DD_A=""; DHT_DD_B=""; DHT_PGID_A=""; DHT_PGID_B=""
declare -A DHT_OWNED_PGIDS=()
declare -A DHT_OWNED_START=()
DHT_OWNED_PORTS=()
DHT_CLEANED=0
DHT_KEEP="${DHT_KEEP:-0}"
# Throwaway passphrases for the wallet-custody recipe (never argv: they
# ride the wallet-passphrase credential file and --input=- stdin only).
DHT_WALLET_PASS="zcode-dht-acceptance-wallet-pass"
DHT_BACKUP_PASS="zcode-dht-acceptance-backup-pass"

dht_die() {
    echo "zcode-dht-acceptance: FATAL: $*" >&2
    if [ -n "$DHT_WORK" ] && [ -d "$DHT_WORK" ]; then
        printf '%s\n' "$*" >"$DHT_WORK/FAILURE"
    fi
    exit 2
}
dht_note() { echo "zcode-dht-acceptance: $*"; }

dht_make_work() {
    local prefix="$1" parent
    mkdir -p "$DHT_WORK_PARENT"
    parent="$(cd "$DHT_WORK_PARENT" && pwd -P)"
    [ "$parent" != / ] || dht_die "DHT_WORK_PARENT must not be /"
    DHT_WORK_PARENT="$parent"
    DHT_WORK="$(mktemp -d "$DHT_WORK_PARENT/$prefix-XXXXXX")" ||
        dht_die "could not create isolated work directory under $parent"
}

dht_assert_port() {
    local p="$1" live owned
    for live in $DHT_LIVE_PORTS; do
        [ "$p" = "$live" ] && dht_die "port $p is in the live refuse-set"
    done
    ss -tlnH "sport = :$p" 2>/dev/null | grep -q . &&
        dht_die "port $p is already listening"
    for owned in "${DHT_OWNED_PORTS[@]:-}"; do
        [ "$owned" = "$p" ] && return 0
    done
    DHT_OWNED_PORTS+=("$p")
    return 0
}

dht_kill_group() {
    local pgid="$1" sig="${2:-TERM}" i state
    [ -n "$pgid" ] || return 0
    [ "${DHT_OWNED_PGIDS[$pgid]:-0}" = 1 ] || return 0
    kill -"$sig" "-$pgid" 2>/dev/null || true
    for i in $(seq 1 50); do
        if ! kill -0 "-$pgid" 2>/dev/null; then
            wait "$pgid" 2>/dev/null || true
            unset "DHT_OWNED_PGIDS[$pgid]"
            return 0
        fi
        state="$(awk '{print $3}' "/proc/$pgid/stat" 2>/dev/null || true)"
        [ "$state" = Z ] && break
        sleep 0.2
    done
    kill -KILL "-$pgid" 2>/dev/null || true
    wait "$pgid" 2>/dev/null || true
    unset "DHT_OWNED_PGIDS[$pgid]"
    ! kill -0 "-$pgid" 2>/dev/null
}

dht_register_owned_group() {
    local pid="$1" start
    start="$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null || true)"
    if [ -z "$start" ]; then
        wait "$pid" 2>/dev/null || true
        dht_die "spawned process group $pid exited before ownership registration"
    fi
    DHT_OWNED_PGIDS[$pid]=1
    DHT_OWNED_START[$pid]="$start"
}

dht_assert_no_owned_processes() {
    local pid expected current
    for pid in "${!DHT_OWNED_START[@]}"; do
        expected="${DHT_OWNED_START[$pid]}"
        current="$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null || true)"
        [ -z "$current" ] || [ "$current" != "$expected" ] ||
            dht_die "owned process $pid remained after cleanup"
    done
}

dht_assert_ports_rebindable() {
    [ "${#DHT_OWNED_PORTS[@]}" -gt 0 ] || return 0
    if ! python3 - "${DHT_OWNED_PORTS[@]}" <<'PY'
import socket,sys
sockets=[]
try:
    for text in sys.argv[1:]:
        sock=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        sock.bind(("0.0.0.0",int(text)))
        sockets.append(sock)
finally:
    for sock in sockets:
        sock.close()
PY
    then
        dht_die "owned ports could not be rebound immediately after cleanup"
    fi
}

dht_cleanup() {
    [ "$DHT_CLEANED" = 1 ] && return 0
    DHT_CLEANED=1
    local pgid failed=0
    for pgid in "${!DHT_OWNED_PGIDS[@]}"; do
        dht_kill_group "$pgid" || failed=1
    done
    if [ "$DHT_KEEP" = 1 ] && [ -n "$DHT_WORK" ]; then
        dht_note "preserved acceptance artifacts: $DHT_WORK"
    elif [ -n "$DHT_WORK" ] && [ -d "$DHT_WORK" ]; then
        case "$DHT_WORK" in
            "$DHT_WORK_PARENT"/zcl23-dhtacc-*|\
            "$DHT_WORK_PARENT"/zcl23-dhtprobe-*)
                rm -rf "$DHT_WORK"
                ;;
            *) dht_note "WARN refusing to remove non-scratch $DHT_WORK" ;;
        esac
    fi
    [ "$failed" -eq 0 ]
}

dht_exit() {
    local rc="$?"
    trap - EXIT INT TERM
    if ! dht_cleanup && [ "$rc" -eq 0 ]; then rc=2; fi
    exit "$rc"
}
trap dht_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

dht_rpc() {
    local dd="$1" port="$2"; shift 2
    ZCL_DATADIR="$dd" ZCL_RPCPORT="$port" "$RPC_BIN" "$@" 2>/dev/null
}
a_rpc() { dht_rpc "$DHT_DD_A" "$A_RPC" "$@"; }
b_rpc() { dht_rpc "$DHT_DD_B" "$B_RPC" "$@"; }
dht_result() {
    python3 -c 'import json,sys
d=json.load(sys.stdin)
if d.get("error") is not None: raise SystemExit(2)
v=d.get("result")
print(json.dumps(v,separators=(",",":")) if isinstance(v,(dict,list)) else v)'
}
dht_jget() {
    local expr="$1"
    python3 -c "import json,sys; d=json.load(sys.stdin); print($expr)"
}
dht_native() {
    local dd="$1" rpc="$2"; shift 2
    "$NODE_BIN" -datadir="$dd" -rpcport="$rpc" "$@" 2>/dev/null | tail -1
}
dht_status() { dht_native "$1" "$2" zcode network status; }

dht_spawn() {
    local out_name="$1" dd="$2" p2p="$3" rpc="$4" fs="$5" https="$6"
    shift 6
    local args=() connect pid
    for connect in "$@"; do args+=("-connect=$connect"); done
    [ "${#args[@]}" -gt 0 ] || args+=("-connect=127.0.0.1:$DEAD_SINK")
    # No -allow-plaintext-wallet: the ZID anchor's overlay-intent custody
    # gate refuses a plaintext-at-rest wallet. The wallet-passphrase
    # credential (CREDENTIALS_DIRECTORY, exported below) encrypts key
    # writes at rest (WKS1); -operator-lane=dev arms the dev wallet scope.
    case "$DHT_PACKAGEHOST" in 0|1) ;; *) dht_die "DHT_PACKAGEHOST must be 0 or 1" ;; esac
    case "$DHT_BUILDWORKERS" in 0|1) ;; *) dht_die "DHT_BUILDWORKERS must be 0 or 1" ;; esac
    local worker_args=() params_args=()
    [ "$DHT_BUILDWORKERS" = 1 ] && worker_args+=("-buildworker")
    [ -z "$DHT_PARAMS_DIR" ] || params_args+=("-paramsdir=$DHT_PARAMS_DIR")
    setsid "$NODE_BIN" -datadir="$dd" -regtest -port="$p2p" \
        -rpcport="$rpc" -fsport="$fs" -httpsport="$https" \
        "${args[@]}" -packagehost="$DHT_PACKAGEHOST" -v2transport \
        "${worker_args[@]}" "${params_args[@]}" \
        -operator-lane=dev -wallet-no-phrase-backup \
        -nobgvalidation -nolegacyimport -showmetrics=0 \
        >>"$dd/node.log" 2>&1 &
    pid="$!"
    dht_register_owned_group "$pid"
    printf -v "$out_name" '%s' "$pid"
}

dht_spawn_owned_command() {
    local out_name="$1" log="$2" pid
    shift 2
    setsid "$@" >>"$log" 2>&1 &
    pid="$!"
    dht_register_owned_group "$pid"
    printf -v "$out_name" '%s' "$pid"
}

dht_wait_owned_exit() {
    local pid="$1" expected="$2" label="$3" rc
    if wait "$pid"; then rc=0; else rc="$?"; fi
    unset "DHT_OWNED_PGIDS[$pid]"
    [ "$rc" -eq "$expected" ] ||
        dht_die "$label exited $rc (expected $expected)"
}

dht_wait_file() {
    local path="$1" owner="$2" deadline
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ -s "$path" ] && return 0
        kill -0 "$owner" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

dht_process_identity_alive() {
    local pid="$1" expected="$2" current
    current="$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null || true)"
    [ -n "$current" ] && [ "$current" = "$expected" ]
}

dht_probe_read_report() {
    local path="$1" pid_name="$2" port_name="$3" start_name="$4"
    local probe_pid probe_port probe_start extra
    read -r probe_pid probe_port probe_start extra <"$path"
    [ -n "$probe_pid" ] && [ -n "$probe_port" ] &&
        [ -n "$probe_start" ] && [ -z "${extra:-}" ] ||
        dht_die "invalid lifecycle probe report $path"
    printf -v "$pid_name" '%s' "$probe_pid"
    printf -v "$port_name" '%s' "$probe_port"
    printf -v "$start_name" '%s' "$probe_start"
}

dht_lifecycle_probe_child() {
    local report="${DHT_PROBE_REPORT:?}" release="${DHT_PROBE_RELEASE:?}"
    local outcome="${DHT_PROBE_OUTCOME:?}" listener="" listener_port
    dht_make_work zcl23-dhtprobe
    setsid python3 - "$report" >>"$DHT_WORK/listener.log" 2>&1 <<'PY' &
import os,pathlib,signal,socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",0)); s.listen(1)
start=pathlib.Path(f"/proc/{os.getpid()}/stat").read_text().split()[21]
path=pathlib.Path(sys.argv[1])
tmp=path.with_suffix(path.suffix+".tmp")
tmp.write_text(f"{os.getpid()} {s.getsockname()[1]} {start}\n")
tmp.replace(path)
signal.pause()
PY
    listener="$!"
    dht_register_owned_group "$listener"
    dht_wait_file "$report" "$listener" ||
        dht_die "lifecycle probe listener failed before readiness"
    read -r _ listener_port _ <"$report"
    DHT_OWNED_PORTS+=("$listener_port")
    dht_wait_file "$release" "$$" ||
        dht_die "lifecycle probe release was not observed"
    [ "$outcome" = success ] || dht_die "forced middle-of-run failure"
    if ! dht_cleanup; then
        echo "zcode-dht-acceptance: FATAL: lifecycle probe cleanup failed" >&2
        exit 2
    fi
    dht_assert_no_owned_processes
    dht_assert_ports_rebindable
    dht_note "PASS lifecycle probe; owned_processes_remaining=0 ports_rebindable=true"
}

dht_lifecycle_selftest() {
    local one_shell two_shell three_shell signal_shell
    local one_pid one_port one_start two_pid two_port two_start
    local three_pid three_port three_start signal_pid signal_port signal_start
    dht_make_work zcl23-dhtprobe

    dht_spawn_owned_command one_shell "$DHT_WORK/one.log" env \
        DHT_LIFECYCLE_MODE=probe DHT_PROBE_OUTCOME=success \
        DHT_PROBE_REPORT="$DHT_WORK/one.report" \
        DHT_PROBE_RELEASE="$DHT_WORK/one.release" \
        bash "$SCRIPT_DIR/zcode_dht_acceptance.sh"
    dht_spawn_owned_command two_shell "$DHT_WORK/two.log" env \
        DHT_LIFECYCLE_MODE=probe DHT_PROBE_OUTCOME=failure \
        DHT_PROBE_REPORT="$DHT_WORK/two.report" \
        DHT_PROBE_RELEASE="$DHT_WORK/two.release" \
        bash "$SCRIPT_DIR/zcode_dht_acceptance.sh"
    dht_wait_file "$DHT_WORK/one.report" "$one_shell" ||
        dht_die "first concurrent lifecycle probe did not become ready"
    dht_wait_file "$DHT_WORK/two.report" "$two_shell" ||
        dht_die "second concurrent lifecycle probe did not become ready"
    dht_probe_read_report "$DHT_WORK/one.report" one_pid one_port one_start
    dht_probe_read_report "$DHT_WORK/two.report" two_pid two_port two_start
    [ "$one_port" != "$two_port" ] || dht_die "concurrent probes shared a port"
    DHT_OWNED_PORTS+=("$one_port" "$two_port")

    printf '%s\n' release >"$DHT_WORK/two.release"
    dht_wait_owned_exit "$two_shell" 2 "forced-failure lifecycle probe"
    ! dht_process_identity_alive "$two_pid" "$two_start" ||
        dht_die "failed probe left its owned listener alive"
    dht_process_identity_alive "$one_pid" "$one_start" ||
        dht_die "failed probe cleaned the other probe's listener"

    printf '%s\n' release >"$DHT_WORK/one.release"
    dht_wait_owned_exit "$one_shell" 0 "successful lifecycle probe"
    ! dht_process_identity_alive "$one_pid" "$one_start" ||
        dht_die "successful probe left its owned listener alive"
    dht_assert_ports_rebindable

    dht_spawn_owned_command three_shell "$DHT_WORK/three.log" env \
        DHT_LIFECYCLE_MODE=probe DHT_PROBE_OUTCOME=success \
        DHT_PROBE_REPORT="$DHT_WORK/three.report" \
        DHT_PROBE_RELEASE="$DHT_WORK/three.release" \
        bash "$SCRIPT_DIR/zcode_dht_acceptance.sh"
    dht_wait_file "$DHT_WORK/three.report" "$three_shell" ||
        dht_die "immediate rerun lifecycle probe did not become ready"
    dht_probe_read_report "$DHT_WORK/three.report" three_pid three_port three_start
    DHT_OWNED_PORTS+=("$three_port")
    printf '%s\n' release >"$DHT_WORK/three.release"
    dht_wait_owned_exit "$three_shell" 0 "immediate-rerun lifecycle probe"
    ! dht_process_identity_alive "$three_pid" "$three_start" ||
        dht_die "immediate rerun left its owned listener alive"

    dht_spawn_owned_command signal_shell "$DHT_WORK/signal.log" env \
        DHT_LIFECYCLE_MODE=probe DHT_PROBE_OUTCOME=success \
        DHT_PROBE_REPORT="$DHT_WORK/signal.report" \
        DHT_PROBE_RELEASE="$DHT_WORK/signal.release" \
        bash "$SCRIPT_DIR/zcode_dht_acceptance.sh"
    dht_wait_file "$DHT_WORK/signal.report" "$signal_shell" ||
        dht_die "signal lifecycle probe did not become ready"
    dht_probe_read_report "$DHT_WORK/signal.report" \
        signal_pid signal_port signal_start
    DHT_OWNED_PORTS+=("$signal_port")
    kill -TERM "-$signal_shell"
    dht_wait_owned_exit "$signal_shell" 143 "interrupted lifecycle probe"
    ! dht_process_identity_alive "$signal_pid" "$signal_start" ||
        dht_die "interrupted probe left its owned listener alive"

    if ! dht_cleanup; then
        echo "zcode-dht-acceptance: FATAL: lifecycle selftest cleanup failed" >&2
        exit 2
    fi
    dht_assert_no_owned_processes
    dht_assert_ports_rebindable
    dht_note "PASS lifecycle ownership: concurrent isolation, failure, interruption, immediate rerun"
}

case "${DHT_LIFECYCLE_MODE:-scenario}" in
    probe) dht_lifecycle_probe_child; exit 0 ;;
    selftest) dht_lifecycle_selftest; exit 0 ;;
    scenario) ;;
    *) dht_die "unknown DHT_LIFECYCLE_MODE=${DHT_LIFECYCLE_MODE:-}" ;;
esac

dht_height() {
    dht_rpc "$1" "$2" getblockcount | dht_result
}
dht_wait_rpc() {
    local dd="$1" rpc="$2" pid="$3" deadline
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        kill -0 "$pid" 2>/dev/null || return 1
        [ -f "$dd/.cookie" ] && dht_height "$dd" "$rpc" >/dev/null 2>&1 && return 0
        sleep 0.5
    done
    return 1
}
dht_wait_height() {
    local dd="$1" rpc="$2" target="$3" deadline h
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        h="$(dht_height "$dd" "$rpc" 2>/dev/null || true)"
        [ "$h" = "$target" ] && return 0
        sleep 0.5
    done
    return 1
}

# The regtest miner stamps blocks from whole-second wall time.  More than six
# consecutive blocks with one timestamp becomes <= the peer's 11-block MTP
# even though the local submission path accepted the batch.  Mine in groups
# of five with a wall-clock step so the second node validates the same chain.
dht_mine_to_address() {
    local count="$1" address="$2" chunk
    while [ "$count" -gt 0 ]; do
        chunk=5
        [ "$count" -lt "$chunk" ] && chunk="$count"
        a_rpc generatetoaddress "$chunk" "\"$address\"" | dht_result >/dev/null
        count=$((count - chunk))
        [ "$count" -eq 0 ] || sleep 1
    done
}
dht_mine_empty() {
    local count="$1" chunk
    while [ "$count" -gt 0 ]; do
        chunk=5
        [ "$count" -lt "$chunk" ] && chunk="$count"
        a_rpc generate "$chunk" | dht_result >/dev/null
        count=$((count - chunk))
        [ "$count" -eq 0 ] || sleep 1
    done
}
dht_wait_auth() {
    local dd="$1" rpc="$2" want="${3:-1}" deadline out enabled auth accepted
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        out="$(dht_status "$dd" "$rpc" 2>/dev/null || true)"
        enabled="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("enabled",False)' 2>/dev/null || true)"
        auth="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("connected_authenticated",0)' 2>/dev/null || true)"
        accepted="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("frames_accepted",0)' 2>/dev/null || true)"
        [ "$enabled" = True ] && [ "${auth:-0}" -ge "$want" ] &&
            [ "${accepted:-0}" -ge "$want" ] && return 0
        sleep 0.5
    done
    return 1
}
dht_wait_cold_load() {
    local dd="$1" rpc="$2" want="${3:-1}" deadline out loaded cold
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        out="$(dht_status "$dd" "$rpc" 2>/dev/null || true)"
        loaded="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("persistence_loaded",False)' 2>/dev/null || true)"
        cold="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("cold_contacts",0)' 2>/dev/null || true)"
        [ "$loaded" = True ] && [ "${cold:-0}" -ge "$want" ] && return 0
        sleep 0.5
    done
    return 1
}

# ── Wallet-custody helpers (the ZID anchor's overlay-intent gate) ─────
# The anchor's custody gate requires the wallet encrypted at rest (the
# wallet-passphrase credential armed at first boot), unlocked, and covered
# by a current-key encrypted backup; its money gate requires an OUTBOUND
# peer with a live sync state and a positive vault spendable. This is the
# metaverse-tour recipe (tools/dev/metaverse_tour.sh) adapted to dht_*
# style; passphrases ride --input=- stdin only, never argv.
dht_wait_connected() {
    local dd="$1" rpc="$2" deadline n
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        n="$(dht_rpc "$dd" "$rpc" getconnectioncount 2>/dev/null | dht_result 2>/dev/null || true)"
        [ "${n:-0}" -ge 1 ] 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
# The money freshness classifier fails closed on finding_peers; the sync
# FSM only leaves it behind a peer it can sync FROM (outbound).
dht_wait_sync_live() {
    local dd="$1" rpc="$2" deadline state
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        state="$(dht_rpc "$dd" "$rpc" downloadstats 2>/dev/null \
            | dht_jget 'd["result"]["sync_state"]' 2>/dev/null || true)"
        case "$state" in
            blocks_download|connecting_blocks|at_tip) return 0 ;;
        esac
        sleep 0.5
    done
    return 1
}
# The money gate reads the REDUCER pipeline, not the active chain: the
# authoritative coins tip AND H* must both reach the mined height.
dht_wait_fold() {
    local dd="$1" rpc="$2" tip="$3" deadline dump coins hstar
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        dump="$(dht_native "$dd" "$rpc" dumpstate reducer_frontier || true)"
        coins="$(printf '%s' "$dump" | dht_jget 'd["state"]["coins_best_height"]' 2>/dev/null || true)"
        hstar="$(printf '%s' "$dump" | dht_jget 'd["state"]["hstar"]' 2>/dev/null || true)"
        [ "$coins" = "$tip" ] && [ "$hstar" = "$tip" ] && return 0
        sleep 1
    done
    echo "zcode-dht-acceptance: reducer_frontier at stall: $dump" >&2
    return 1
}
# RPC-ready != chain-loaded: the anchor's runtime gate needs the active
# chain index, which loads after the RPC starts serving.
dht_wait_chain_loaded() {
    local dd="$1" rpc="$2" tip="$3" deadline loaded
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        loaded="$(dht_rpc "$dd" "$rpc" getblockchaininfo 2>/dev/null | python3 -c '
import json,sys
tip = int(sys.argv[1])
try:
    d = json.load(sys.stdin).get("result")
except Exception:
    d = None
print(isinstance(d, dict) and d.get("blocks") == tip and d.get("initialblockdownload") is not True)' \
            "$tip" 2>/dev/null || true)"
        [ "$loaded" = "True" ] && return 0
        sleep 1
    done
    return 1
}
# The fee-reserve rung reads the vault read model's zcl_spendable, which
# lags the reducer fold while the wallet re-derives its spendable coins.
dht_wait_spendable() {
    local dd="$1" rpc="$2" deadline spend
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        spend="$(dht_native "$dd" "$rpc" dumpstate vault 2>/dev/null \
            | dht_jget 'd["state"]["zcl"]["spendable"]' 2>/dev/null || true)"
        case "$spend" in
            ''|*[!0-9]*) ;;
            *) [ "$spend" -gt 0 ] && return 0 ;;
        esac
        sleep 1
    done
    return 1
}
dht_unlock_wallet() {
    local dd="$1" rpc="$2" status unlock
    status="$(dht_native "$dd" "$rpc" core wallet security status || true)"
    [ "$(printf '%s' "$status" | dht_jget 'd.get("ok",False)' 2>/dev/null || true)" = "True" ] || {
        printf '%s\n' "$status" >&2; return 1; }
    if [ "$(printf '%s' "$status" | dht_jget 'd["data"]["unlocked"]' 2>/dev/null || true)" != "True" ]; then
        unlock="$(printf '%s' "{\"passphrase\":\"$DHT_WALLET_PASS\",\"timeout_seconds\":3600}" \
            | dht_native "$dd" "$rpc" core wallet security unlock --input=- || true)"
        [ "$(printf '%s' "$unlock" | dht_jget 'd["data"]["unlocked"]' 2>/dev/null || true)" = "True" ] || {
            printf '%s\n' "$unlock" >&2; return 1; }
    fi
    return 0
}
dht_backup_wallet() {
    local dd="$1" rpc="$2" out
    out="$(printf '%s' "{\"confirm\":true,\"password\":\"$DHT_BACKUP_PASS\"}" \
        | dht_native "$dd" "$rpc" core wallet backup now --input=- || true)"
    [ "$(printf '%s' "$out" | dht_jget 'd.get("ok",False)' 2>/dev/null || true)" = "True" ] || {
        printf '%s\n' "$out" >&2; return 1; }
}
# Plan (retrying ONLY the transient OVERLAY_INTENT_REFUSED money-currency
# skew — the idempotency key makes a repeated plan safe), then commit the
# returned plan_id. Prints the commit reply; nonzero on any refusal.
dht_anchor() {
    local dd="$1" rpc="$2" pubkey="$3" key="$4" plan plan_id commit try
    plan=""
    for try in $(seq 1 20); do
        plan="$(dht_native "$dd" "$rpc" core identity anchor \
            --input="{\"wallet_scope\":\"dev\",\"pubkey\":\"$pubkey\",\"idempotency_key\":\"$key\"}" || true)"
        case "$plan" in
            *OVERLAY_INTENT_REFUSED*) sleep 1 ;;
            *) break ;;
        esac
    done
    [ "$(printf '%s' "$plan" | dht_jget 'd.get("ok",False)' 2>/dev/null || true)" = "True" ] &&
    [ "$(printf '%s' "$plan" | dht_jget 'd["data"]["stage"]' 2>/dev/null || true)" = "plan" ] || {
        printf '%s\n' "$plan" >&2; return 1; }
    plan_id="$(printf '%s' "$plan" | dht_jget 'd["data"]["plan_id"]' 2>/dev/null)" || return 1
    commit="$(dht_native "$dd" "$rpc" core identity anchor \
        --input="{\"wallet_scope\":\"dev\",\"plan_id\":\"$plan_id\",\"confirm\":true}" || true)"
    [ "$(printf '%s' "$commit" | dht_jget 'd.get("ok",False)' 2>/dev/null || true)" = "True" ] &&
    [ "$(printf '%s' "$commit" | dht_jget 'd["data"]["stage"]' 2>/dev/null || true)" = "committed" ] || {
        printf '%s\n' "$commit" >&2; return 1; }
    printf '%s\n' "$commit"
}

dht_build_helper() {
    cc -std=c23 -O1 -w -D_GNU_SOURCE -ffunction-sections -fdata-sections \
        -Wl,--gc-sections -I"$REPO_ROOT/lib/base/include" \
        -I"$REPO_ROOT/lib/sha3/include" -I"$REPO_ROOT/lib/crypto/include" \
        -I"$REPO_ROOT/lib/support/include" \
        -I"$REPO_ROOT/lib/util/include" -I"$REPO_ROOT/lib/platform/include" \
        -I"$REPO_ROOT/lib/json/include" -I"$REPO_ROOT/lib/core/include" \
        -I"$REPO_ROOT/lib/net/include" -I"$REPO_ROOT/lib/noise/include" \
        -I"$REPO_ROOT/lib/vcs/include" -I"$REPO_ROOT/lib/zid/include" \
        -I"$REPO_ROOT/core/math/include" -o "$DHT_WORK/dht-peer" \
        "$REPO_ROOT/tools/zcode_dht_acceptance_peer.c" \
        "$REPO_ROOT/lib/net/src/v2_transport.c" \
        "$REPO_ROOT/lib/noise/src/noise_handshake.c" \
        "$REPO_ROOT/lib/noise/src/session_transport.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_delegation.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_identity.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_msgs.c" \
        "$REPO_ROOT/lib/zid/src/zid.c" \
        "$REPO_ROOT/lib/zid/src/zendp.c" \
        "$REPO_ROOT/lib/crypto/src/ed25519.c" \
        "$REPO_ROOT/lib/crypto/src/sha512.c" \
        "$REPO_ROOT/lib/crypto/src/sha256.c" \
        "$REPO_ROOT/lib/sha3/src/sha3.c" \
        "$REPO_ROOT/lib/crypto/src/hmac_sha256.c" \
        "$REPO_ROOT/lib/crypto/src/hkdf_sha256.c" \
        "$REPO_ROOT/lib/crypto/src/chacha20poly1305.c" \
        "$REPO_ROOT/lib/crypto/src/curve25519.c" \
        "$REPO_ROOT/lib/crypto/src/x25519_safe.c" \
        "$REPO_ROOT/lib/crypto/src/random_secret.c" \
        "$REPO_ROOT/core/math/src/hash.c" \
        "$REPO_ROOT/lib/core/src/utiltime.c" \
        "$REPO_ROOT/lib/core/src/random.c" \
        "$REPO_ROOT/lib/base/src/safe_alloc.c" \
        "$REPO_ROOT/lib/base/src/log_level.c" \
        "$REPO_ROOT/lib/base/src/result.c" \
        "$REPO_ROOT/lib/base/src/cleanse.c" \
        "$REPO_ROOT/lib/platform/src/clock.c" \
        "$REPO_ROOT/lib/platform/src/rng.c" \
        "$REPO_ROOT/lib/util/src/write_all.c" \
        "$REPO_ROOT/lib/json/src/json.c" \
        "$REPO_ROOT/lib/util/src/hw_profile.c" \
        "$REPO_ROOT/lib/util/src/cpu_topology.c" ||
        dht_die "acceptance helper compile failed"
}

dht_check_find() {
    local reply="$1" target="$2" a_id="$3" b_id="$4"
    printf '%s' "$reply" | python3 -c '
import json,sys
d=json.load(sys.stdin)
target=int(sys.argv[1],16); expected={sys.argv[2],sys.argv[3]}
assert d["ok"] is True, d
rows=d["data"]["node_ids"]
assert len(rows) == 2 and set(rows) == expected, rows
assert rows == sorted(rows,key=lambda x:(int(x,16)^target,x)), rows
' "$target" "$a_id" "$b_id" || dht_die "FIND_NODE result/order mismatch"
}

dht_check_contacts_file() {
    local path="$1" self_id="$2" expected="${3:-1}"
    python3 -c '
import pathlib,struct,sys
p=pathlib.Path(sys.argv[1]); b=p.read_bytes(); self_id=bytes.fromhex(sys.argv[2])
assert b[:8] == b"ZCDHTC\r\n"
assert struct.unpack_from("<H",b,8)[0] == 2
assert b[42:74] == self_id
n=struct.unpack_from("<I",b,74)[0]
entry_bytes=303 # node id + canonical delegation + local time/failures
assert n == int(sys.argv[3]) and len(b) == 78 + n*entry_bytes
ids=[b[78+i*entry_bytes:110+i*entry_bytes] for i in range(n)]
assert ids == sorted(ids) and len(ids) == len(set(ids))
' "$path" "$self_id" "$expected" || dht_die "non-canonical contacts file: $path"
}

dht_check_attack_deltas() {
    local before="$1" after="$2"
    python3 -c '
import json,sys
b=json.loads(sys.argv[1])["data"]; a=json.loads(sys.argv[2])["data"]
br=b["frames_rejected"]; ar=a["frames_rejected"]
want={"malformed":2,"identity":1,"replay":1,"unsolicited":1,
      "expired":1,"poisoned-contacts":1}
for key,delta in want.items():
    assert ar[key]-br[key] == delta,(key,br[key],ar[key])
for key in set(ar)-set(want):
    assert ar[key]-br[key] == 0,(key,br[key],ar[key])
assert a["frames_accepted"]-b["frames_accepted"] >= 1
' "$before" "$after" || dht_die "hostile Noise-frame counter deltas differ"
}

for port in $A_PORT $A_RPC $A_FS $A_HTTPS $B_PORT $B_RPC $B_FS $B_HTTPS; do
    dht_assert_port "$port"
done
[ -x "$NODE_BIN" ] && [ -x "$RPC_BIN" ] || dht_die "build node and RPC binaries first"
dht_make_work zcl23-dhtacc
DHT_DD_A="$DHT_WORK/a"; DHT_DD_B="$DHT_WORK/b"
mkdir -p "$DHT_DD_A" "$DHT_DD_B"
dht_build_helper

SEED_A=1111111111111111111111111111111111111111111111111111111111111111
SEED_B=2222222222222222222222222222222222222222222222222222222222222222
install -m 600 /dev/null "$DHT_WORK/master-a.hex"
install -m 600 /dev/null "$DHT_WORK/master-b.hex"
printf '%s\n' "$SEED_A" >"$DHT_WORK/master-a.hex"
printf '%s\n' "$SEED_B" >"$DHT_WORK/master-b.hex"
PUB_A="$("$DHT_WORK/dht-peer" pubkey "$SEED_A")"
PUB_B="$("$DHT_WORK/dht-peer" pubkey "$SEED_B")"

declare -a DDS RPCS PORTS FSPORTS HTTPSPORTS SEEDS PUBS NODES PIDS DOCS
DDS=("$DHT_DD_A" "$DHT_DD_B")
RPCS=("$A_RPC" "$B_RPC")
PORTS=("$A_PORT" "$B_PORT" 20023 20024 20025 20026 20027)
FSPORTS=("$A_FS" "$B_FS" 39232 39242 39252 39262 39272)
HTTPSPORTS=("$A_HTTPS" "$B_HTTPS" 39233 39243 39253 39263 39273)
SEEDS=("$SEED_A" "$SEED_B"
  3333333333333333333333333333333333333333333333333333333333333333
  4444444444444444444444444444444444444444444444444444444444444444
  5555555555555555555555555555555555555555555555555555555555555555
  6666666666666666666666666666666666666666666666666666666666666666
  7777777777777777777777777777777777777777777777777777777777777777)
PUBS=("$PUB_A" "$PUB_B")
for port in 20023 20024 20025 20026 20027 \
    39231 39232 39233 39241 39242 39243 39251 39252 39253 \
    39261 39262 39263 39271 39272 39273; do
    dht_assert_port "$port"
done
for i in 2 3 4 5 6; do
    DDS[$i]="$DHT_WORK/node-$i"
    RPCS[$i]=$((39211 + i * 10))
    mkdir -p "${DDS[$i]}"
    seed_file="$DHT_WORK/master-$i.hex"
    install -m 600 /dev/null "$seed_file"
    printf '%s\n' "${SEEDS[$i]}" >"$seed_file"
    PUBS[$i]="$("$DHT_WORK/dht-peer" pubkey "${SEEDS[$i]}")"
done

# Wallet custody: boot every node with a passphrase credential so key
# writes encrypt at rest (WKS1) — the ZID anchor's overlay-intent custody
# gate refuses a plaintext-at-rest wallet. The current-key encrypted
# backup itself happens after mining below, once the spend key exists.
DHT_CRED_DIR="$DHT_WORK/cred"
install -d -m 700 "$DHT_CRED_DIR"
install -m 600 /dev/null "$DHT_CRED_DIR/wallet-passphrase"
printf '%s\n' "$DHT_WALLET_PASS" >"$DHT_CRED_DIR/wallet-passphrase"
export CREDENTIALS_DIRECTORY="$DHT_CRED_DIR"

dht_note "booting two clean packagehost=$DHT_PACKAGEHOST regtest nodes"
dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
    "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "node A RPC warmup failed"
dht_spawn DHT_PGID_B "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" \
    "$B_HTTPS" "127.0.0.1:$A_PORT"
dht_wait_rpc "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" || dht_die "node B RPC warmup failed"
! rg -q "unrecognized flag '-v2transport'" "$DHT_DD_A/node.log" "$DHT_DD_B/node.log" ||
    dht_die "v2transport was not recognized"

dht_note "mining spendable regtest funds"
ADDR="$(a_rpc getnewaddress | dht_result)"
dht_mine_to_address 101 "$ADDR"
dht_wait_height "$DHT_DD_B" "$B_RPC" 101 || dht_die "B did not sync funding chain"
dht_wait_fold "$DHT_DD_A" "$A_RPC" 101 || dht_die "A reducer fold did not reach the funding tip"

# The ZID anchor's overlay-intent custody gate requires the wallet
# encrypted at rest, unlocked, and covered by a current-key encrypted
# backup; its money gate requires A to hold an OUTBOUND peer with a live
# sync state (the money-freshness classifier fails closed on
# finding_peers, and the sync FSM only leaves it behind an outbound peer).
# Mirror the metaverse-tour recipe: bounce B onto the dead sink so the
# pair's only post-restart link is A's outbound onetry below (B's own
# redial-backoff was measured >60s — deterministic, no already-connected
# skip), then restart A so the forward-folded coins set stamps its
# authority (the coins_kv authority stamps land only at boot).
dht_note "bouncing B onto the dead sink (A will own the custody-phase link)"
dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
dht_spawn DHT_PGID_B "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" \
    "$B_HTTPS" "127.0.0.1:$DEAD_SINK"
dht_wait_rpc "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" || dht_die "B dead-sink bounce failed"
dht_note "restarting A so the forward-folded coins set stamps its authority"
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
    "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "A custody restart failed"
dht_wait_fold "$DHT_DD_A" "$A_RPC" 101 || dht_die "A reducer fold did not survive the restart"
# Operator-directed onetry: bypasses the reachable-port policy and lands
# immediately — B is up, listening, and not connected to us.
a_rpc addnode "\"127.0.0.1:$B_PORT\"" "\"onetry\"" >/dev/null || true
dht_wait_connected "$DHT_DD_A" "$A_RPC" || dht_die "A never connected outbound to B"
dht_wait_sync_live "$DHT_DD_A" "$A_RPC" || dht_die "A sync never left finding_peers"
dht_wait_chain_loaded "$DHT_DD_A" "$A_RPC" 101 || dht_die "A active chain index did not load"

# The restart re-locks the encrypted-at-rest wallet; the anchor's
# funding-input build draws from the key pool, which a locked wallet
# refuses. Unlock explicitly (passphrase via --input=- only), re-top the
# RAM-only keypool bookkeeping with one getnewaddress, then take the
# current-key encrypted backup the custody gate demands (AFTER the top-up,
# so the backup covers the key the anchor spends from).
dht_note "unlocking the wallet and taking the current-key encrypted backup"
dht_unlock_wallet "$DHT_DD_A" "$A_RPC" || dht_die "A wallet unlock failed"
a_rpc getnewaddress | dht_result >/dev/null || dht_die "post-restart keypool top-up failed"
dht_backup_wallet "$DHT_DD_A" "$A_RPC" || dht_die "A custody backup failed"
dht_wait_spendable "$DHT_DD_A" "$A_RPC" || dht_die "A vault spendable never became positive"

dht_note "anchoring seven masters (plan/commit under identity custody)"
ANCHOR_A="$(dht_anchor "$DHT_DD_A" "$A_RPC" "$PUB_A" "dht-anchor-a")" || dht_die "A anchor failed"
dht_mine_empty 1; sleep 1
ANCHOR_B="$(dht_anchor "$DHT_DD_A" "$A_RPC" "$PUB_B" "dht-anchor-b")" || dht_die "B anchor failed"
dht_mine_empty 1; sleep 1
for i in 2 3 4 5 6; do
    dht_anchor "$DHT_DD_A" "$A_RPC" "${PUBS[$i]}" "dht-anchor-$i" >/dev/null ||
        dht_die "anchor $i failed"
    dht_mine_empty 1; sleep 1
done
dht_mine_empty 21
dht_wait_height "$DHT_DD_B" "$B_RPC" 129 || dht_die "B did not sync final beacon chain"

dht_note "provisioning independent delegations through the operator leaf"
DELEGATE_A="$(dht_native "$DHT_DD_A" "$A_RPC" zcode network delegate --input="{\"seed_file\":\"$DHT_WORK/master-a.hex\"}")"
DELEGATE_B="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network delegate --input="{\"seed_file\":\"$DHT_WORK/master-b.hex\"}")"
[ "$(printf '%s' "$DELEGATE_A" | dht_jget 'd["ok"]')" = True ] || dht_die "A delegation failed: $DELEGATE_A"
[ "$(printf '%s' "$DELEGATE_B" | dht_jget 'd["ok"]')" = True ] || dht_die "B delegation failed: $DELEGATE_B"
NODE_A="$(printf '%s' "$DELEGATE_A" | dht_jget 'd["data"]["node_id"]')"
NODE_B="$(printf '%s' "$DELEGATE_B" | dht_jget 'd["data"]["node_id"]')"
NODES=("$NODE_A" "$NODE_B")
[ "$NODE_A" != "$NODE_B" ] || dht_die "independent masters derived one node ID"

# Close A to obtain a coherent chain fixture while B remains on its original
# boot with a provable RPC tip.  The delegate leaf can authorize a distinct
# target datadir explicitly: it reads that clone's chain projection, uses B's
# authenticated chain RPC for genesis/beacon, and creates new target-local
# Noise and online keys.  No DHT contacts or endpoint records are copied.
dht_note "preparing five closed-chain fixtures with independent identities"
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
for i in 2 3 4 5 6; do
    cp -a "$DHT_DD_A/." "${DDS[$i]}/"
    rm -rf "${DDS[$i]}/zcode"
    rm -f "${DDS[$i]}/v2_identity.key" "${DDS[$i]}/.cookie" \
        "${DDS[$i]}/.rpcport" "${DDS[$i]}/zclassic23.pid" \
        "${DDS[$i]}/node.log" "${DDS[$i]}/peers.dat" \
        "${DDS[$i]}/peers.dat.sha3" "${DDS[$i]}/anchors.dat" \
        "${DDS[$i]}/anchors.dat.sha3" "${DDS[$i]}/banlist.dat"
    delegated="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network delegate \
        --input="{\"seed_file\":\"$DHT_WORK/master-$i.hex\",\"datadir\":\"${DDS[$i]}\"}")"
    [ "$(printf '%s' "$delegated" | dht_jget 'd["ok"]')" = True ] ||
        dht_die "delegation $i failed: $delegated"
    NODES[$i]="$(printf '%s' "$delegated" | dht_jget 'd["data"]["node_id"]')"
    [ -s "${DDS[$i]}/v2_identity.key" ] &&
    [ -s "${DDS[$i]}/zcode/dht/online_ed25519.key" ] &&
    [ -s "${DDS[$i]}/zcode/dht/delegation.v1" ] ||
        dht_die "independent identity files missing for node $i"
done
python3 - "${NODES[@]}" <<'PY' || dht_die "seven identities were not independent"
import sys
ids=sys.argv[1:]
assert len(ids)==7 and len(set(ids))==7 and all(len(x)==64 for x in ids)
PY

dht_note "restarting to prove capability learning, Noise, and DHT bootstrap"
dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
    "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "A restart failed"
dht_spawn DHT_PGID_B "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" \
    "$B_HTTPS" "127.0.0.1:$A_PORT"
dht_wait_rpc "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" || dht_die "B restart failed"
dht_wait_auth "$DHT_DD_A" "$A_RPC" || dht_die "A never authenticated B over DHT"
dht_wait_auth "$DHT_DD_B" "$B_RPC" || dht_die "B never authenticated A over DHT"
rg -q 'controlled Noise reconnect requested' "$DHT_DD_B/node.log" ||
    dht_die "plaintext capability-learning reconnect was not observed"

TARGET_A=0101010101010101010101010101010101010101010101010101010101010101
TARGET_B=fefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefe
FIND_A="$(dht_native "$DHT_DD_A" "$A_RPC" zcode network find --input="{\"node_id\":\"$TARGET_A\"}")"
FIND_B="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network find --input="{\"node_id\":\"$TARGET_B\"}")"
dht_check_find "$FIND_A" "$TARGET_A" "$NODE_A" "$NODE_B"
dht_check_find "$FIND_B" "$TARGET_B" "$NODE_A" "$NODE_B"

dht_note "rejecting hostile frames inside an authenticated Noise session"
ATTACK_BEFORE="$(dht_status "$DHT_DD_A" "$A_RPC")"
# Use the already-anchored but currently offline third identity. Reusing B's
# live node ID would intentionally trigger the S6 duplicate-session eviction
# path and mix B's late frames into this hostile-payload counter proof.
"$DHT_WORK/dht-peer" attack 127.0.0.1 "$A_PORT" "${DDS[2]}" |
    grep -qx 'attack-sequence-sent' || dht_die "hostile Noise peer failed"
ATTACK_AFTER="$(dht_status "$DHT_DD_A" "$A_RPC")"
dht_check_attack_deltas "$ATTACK_BEFORE" "$ATTACK_AFTER"

dht_note "clean shutdown, canonical-file check, and cold reload"
dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
dht_check_contacts_file "$DHT_DD_A/zcode/dht/contacts.v2" "$NODE_A" 2
dht_check_contacts_file "$DHT_DD_B/zcode/dht/contacts.v2" "$NODE_B"

# Start each node without its peer: the loaded contact must be visible as
# cold before any network refresh can authenticate it.
dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
    "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "A cold-load boot failed"
dht_wait_cold_load "$DHT_DD_A" "$A_RPC" ||
    dht_die "A persisted contact did not publish cold"
dht_spawn DHT_PGID_B "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" \
    "$B_HTTPS" "127.0.0.1:$A_PORT"
dht_wait_rpc "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" || dht_die "B reload boot failed"
dht_wait_auth "$DHT_DD_A" "$A_RPC" || dht_die "A reload did not refresh B"
dht_wait_auth "$DHT_DD_B" "$B_RPC" || dht_die "B reload did not refresh A"

dht_note "short disconnect retains incumbent, then reconnect resets it"
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
sleep 2
PEERS="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network peers --input='{"limit":64}')"
[ "$(printf '%s' "$PEERS" | dht_jget 'd["data"]["count"]')" -eq 1 ] || dht_die "B evicted A during a short disconnect"
dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
    "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "A recovery boot failed"
dht_wait_auth "$DHT_DD_B" "$B_RPC" || dht_die "B did not reauthenticate A"
FINAL_FIND="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network find --input="{\"node_id\":\"$TARGET_A\"}")"
dht_check_find "$FINAL_FIND" "$TARGET_A" "$NODE_A" "$NODE_B"

dht_note "expanding to seven independent daemons for iterative sparse lookup"
PIDS[0]="$DHT_PGID_A"
PIDS[1]="$DHT_PGID_B"

# The five closed-chain fixtures and their distinct identities were prepared
# above while B still exposed the original boot's provable tip.
dht_note "closing the two-node phase before seven-node isolated boot"
dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
# The two-node persistence proof above is complete.  Reset only its learned
# contact files so the multi-node phase starts from topology edges, not from a
# historical A<->B shortcut.  Signed delegations and online keys stay intact.
rm -f "$DHT_DD_A/zcode/dht/contacts.v2" \
    "$DHT_DD_B/zcode/dht/contacts.v2"

dht_note "booting seven isolated identities on the common chain fixture"
for i in 0 1 2 3 4 5 6; do
    dht_spawn "PIDS[$i]" "${DDS[$i]}" "${PORTS[$i]}" "${RPCS[$i]}" \
        "${FSPORTS[$i]}" "${HTTPSPORTS[$i]}" \
        "127.0.0.1:$DEAD_SINK"
    dht_wait_rpc "${DDS[$i]}" "${RPCS[$i]}" "${PIDS[$i]}" ||
        dht_die "isolated node $i warmup failed"
    dht_wait_height "${DDS[$i]}" "${RPCS[$i]}" 129 ||
        dht_die "isolated node $i lost the common verified chain"
done
DHT_PGID_A="${PIDS[0]}"; DHT_PGID_B="${PIDS[1]}"

dht_note "publishing seven chain-bound ZENDP records without DHT contacts"
for i in 0 1 2 3 4 5 6; do
    seed_file="$DHT_WORK/master-$i.hex"
    if [ "$i" -eq 0 ]; then
        seed_file="$DHT_WORK/master-a.hex"
    elif [ "$i" -eq 1 ]; then
        seed_file="$DHT_WORK/master-b.hex"
    fi
    published="$(dht_native "${DDS[$i]}" "${RPCS[$i]}" zcode endpoint publish \
        --input="{\"ipv4\":\"127.0.0.1\",\"ipv4_port\":\"${PORTS[$i]}\",\"seed_file\":\"$seed_file\",\"seq\":\"1\",\"height\":129}")"
    [ "$(printf '%s' "$published" | dht_jget 'd["ok"]')" = True ] ||
        dht_die "endpoint publish $i failed: $published"
    DOCS[$i]="$(printf '%s' "$published" | dht_jget 'd["data"]["doc_hex"]')"
done

# Choose a deterministic XOR-progress path ending at node 6. Only neighbours
# (plus B-D) are connected initially; learned ZENDP hints create later edges.
read -r -a ORDER <<<"$(python3 - "${NODES[@]}" <<'PY'
import sys
ids=[int(x,16) for x in sys.argv[1:]]
t=ids[6]
print(*sorted(range(6),key=lambda i:(ids[i]^t,ids[i]),reverse=True),6)
PY
)"
# Only the lookup origin needs reachability hints: responders return
# address-free IDs and never initiate a learned dial. Filing each signed doc
# once at that origin is the genuine minimum and avoids manufacturing an
# irrelevant all-to-all directory projection on the other six nodes.
ORIGIN="${ORDER[0]}"
dht_note "filing signed endpoints at the future lookup origin only"
for publisher in 0 1 2 3 4 5 6; do
    [ "$ORIGIN" -eq "$publisher" ] && continue
    accepted="$(dht_native "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" \
        zcode endpoint accept --input="{\"doc\":\"${DOCS[$publisher]}\"}")"
    [ "$(printf '%s' "$accepted" | dht_jget 'd["ok"]')" = True ] ||
        dht_die "origin refused endpoint $publisher: $accepted"
done
for i in 0 1 2 3 4 5 6; do dht_kill_group "${PIDS[$i]}"; PIDS[$i]=""; done
DHT_PGID_A=""; DHT_PGID_B=""

for pos in 0 1 2 3 4 5 6; do
    idx="${ORDER[$pos]}"
    connects=()
    if [ "$pos" -eq 0 ]; then
        connects=("127.0.0.1:$DEAD_SINK")
    else
        prev="${ORDER[$((pos - 1))]}"
        connects=("127.0.0.1:${PORTS[$prev]}")
        if [ "$pos" -eq 3 ]; then
            alt="${ORDER[1]}"
            connects+=("127.0.0.1:${PORTS[$alt]}")
        fi
    fi
    dht_spawn "PIDS[$idx]" "${DDS[$idx]}" "${PORTS[$idx]}" "${RPCS[$idx]}" \
        "${FSPORTS[$idx]}" "${HTTPSPORTS[$idx]}" "${connects[@]}"
    dht_wait_rpc "${DDS[$idx]}" "${RPCS[$idx]}" "${PIDS[$idx]}" ||
        dht_die "sparse restart failed for node $idx"
done
# Do not infer topology readiness from one authenticated edge: that races the
# alternate route's Noise upgrade and turns a real recovery proof into a
# single dead-candidate wait. Wait for the exact sparse graph degree first.
EXPECTED_AUTH=(1 3 2 3 2 2 1)
for pos in 0 1 2 3 4 5 6; do
    idx="${ORDER[$pos]}"
    dht_wait_auth "${DDS[$idx]}" "${RPCS[$idx]}" \
        "${EXPECTED_AUTH[$pos]}" ||
        dht_die "sparse node $idx did not authenticate all declared edges"
done

NEXT="${ORDER[1]}"; BROKEN="${ORDER[2]}"; TARGET=6
origin_status="$(dht_status "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}")"
[ "$(printf '%s' "$origin_status" | dht_jget 'd["data"]["connected_authenticated"]')" -eq 1 ] ||
    dht_die "origin was not sparse before lookup: $origin_status"

dht_note "breaking the nearest path; FIND_NODE must recover through B-D"
dht_kill_group "${PIDS[$BROKEN]}"; PIDS[$BROKEN]=""
before_find="$(dht_status "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}")"
iterative="$(dht_native "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" zcode network find \
    --input="{\"node_id\":\"${NODES[$TARGET]}\"}" || true)"
after_find="$(dht_status "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}")"
printf '%s\n' "$iterative" >"$DHT_WORK/iterative.json"
if ! python3 - "$iterative" "$before_find" "$after_find" "${NODES[$TARGET]}" <<'PY'
import json,sys
r=json.loads(sys.argv[1]); b=json.loads(sys.argv[2])["data"]; a=json.loads(sys.argv[3])["data"]
target=sys.argv[4]
assert r["ok"] is True,r
d=r["data"]
assert d["termination"]=="target_authenticated",d
assert d["rounds"]>=3 and d["xor_progress"]>=3,d
assert target in d["node_ids"],d
assert a["find_node_sent"]-b["find_node_sent"] <= 24,(b,a)
assert a["frames_accepted"]-b["frames_accepted"] <= 64,(b,a)
PY
then
    dht_die "iterative sparse proof failed: $iterative"
fi

dht_note "admitting eight external callers while exactly three queries stall"
concurrent_dir="$DHT_WORK/concurrent"; mkdir -p "$concurrent_dir"
# Freeze every remote daemon after authentication. TCP/Noise sessions remain
# live, but no NODES reply can race the status sample below. This proves eight
# separate CLI processes occupy all eight service lookup slots at once while
# the global network-query cap remains exactly three.
for i in 0 1 2 3 4 5 6; do
    [ "$i" -eq "$ORIGIN" ] && continue
    [ -n "${PIDS[$i]:-}" ] && kill -STOP "-${PIDS[$i]}"
done
jobs=()
for i in 1 2 3 4 5 6 7 8; do
    target="$(printf '%064x' "$i")"
    (dht_native "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" \
        zcode network find begin --input="{\"node_id\":\"$target\"}" \
        >"$concurrent_dir/$i.begin.json") &
    jobs+=("$!")
done
for job in "${jobs[@]}"; do wait "$job" || dht_die "lookup admission process failed"; done
burst="$(dht_status "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}")"
python3 - "$concurrent_dir" "$burst" <<'PY' || dht_die "true eight-caller admission proof failed"
import json,pathlib,sys
p=pathlib.Path(sys.argv[1])
rows=[json.loads((p/f"{i}.begin.json").read_text()) for i in range(1,9)]
assert all(r.get("ok") is True and r["data"]["state"]=="pending" for r in rows),rows
ids=[r["data"]["lookup_id"] for r in rows]
owners=[r["data"]["owner_token"] for r in rows]
assert len(set(ids))==8 and len(set(owners))==8,(ids,owners)
s=json.loads(sys.argv[2])["data"]
assert s["queued_lookups"]==8,s
assert s["active_queries"]==3,s
PY
for i in 1 2 3 4 5 6 7 8; do
    read -r lookup owner <<<"$(python3 - "$concurrent_dir/$i.begin.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))["data"]
print(d["lookup_id"],d["owner_token"])
PY
)"
    polled="$(dht_native "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" \
        zcode network find poll \
        --input="{\"lookup_id\":\"$lookup\",\"owner_token\":\"$owner\"}")"
    [ "$(printf '%s' "$polled" | dht_jget 'd["data"]["state"]')" = pending ] ||
        dht_die "stalled lookup $i was not pending: $polled"
    canceled="$(dht_native "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" \
        zcode network find cancel \
        --input="{\"lookup_id\":\"$lookup\",\"owner_token\":\"$owner\"}")"
    [ "$(printf '%s' "$canceled" | dht_jget 'd["ok"]')" = True ] ||
        dht_die "lookup cancel $i failed: $canceled"
done
for i in 0 1 2 3 4 5 6; do
    [ "$i" -eq "$ORIGIN" ] && continue
    [ -n "${PIDS[$i]:-}" ] && kill -CONT "-${PIDS[$i]}"
done

dht_note "cold-loading with no peer database, then reconnecting autonomously"
for i in 0 1 2 3 4 5 6; do
    dht_kill_group "${PIDS[$i]:-}"
    PIDS[$i]=""
done
python3 - "${DDS[$ORIGIN]}/zcode/dht/contacts.v2" "${NODES[$NEXT]}" <<'PY' || dht_die "multi contact file is not canonical"
import pathlib,struct,sys
p=pathlib.Path(sys.argv[1]); b=p.read_bytes(); assert b[:8]==b"ZCDHTC\r\n"
n=struct.unpack_from("<I",b,74)[0]; size=303
entries=[b[78+i*size:78+(i+1)*size] for i in range(n)]
ids=[entry[:32] for entry in entries]
assert n>=3 and len(b)==78+n*size and ids==sorted(ids) and len(ids)==len(set(ids))
keep=bytes.fromhex(sys.argv[2])
selected=[entry for entry in entries if entry[:32]==keep]
assert len(selected)==1
# Canonical one-contact fixture built only from an already authenticated
# contacts.v2 entry. No address is introduced; the untouched header remains
# bound to the same network genesis and local node ID.
p.write_bytes(b[:74]+struct.pack("<I",1)+selected[0])
PY

# peers.dat is deliberately absent. The only origin-side network facts are
# one address-free authenticated-history ID plus the independently accepted,
# chain-bound ZENDP files already created above.
rm -f "${DDS[$ORIGIN]}/peers.dat" "${DDS[$ORIGIN]}/peers.dat.sha3"
[ "$(find "${DDS[$ORIGIN]}/zcode/endpoints" -maxdepth 1 -type f -name '*.zid' | wc -l)" -ge 6 ] ||
    dht_die "origin lost its accepted ZENDP records"

# Rebuild a six-node chain that has no edge to the origin. Start from the far
# end so every explicit -connect target is already listening. None of these
# nodes accepted the origin's directory, so no DHT hint can create a shortcut.
for pos in 6 5 4 3 2 1; do
    idx="${ORDER[$pos]}"
    connects=("127.0.0.1:$DEAD_SINK")
    if [ "$pos" -lt 6 ]; then
        farther="${ORDER[$((pos + 1))]}"
        connects=("127.0.0.1:${PORTS[$farther]}")
    fi
    dht_spawn "PIDS[$idx]" "${DDS[$idx]}" "${PORTS[$idx]}" \
        "${RPCS[$idx]}" "${FSPORTS[$idx]}" "${HTTPSPORTS[$idx]}" \
        "${connects[@]}"
    dht_wait_rpc "${DDS[$idx]}" "${RPCS[$idx]}" "${PIDS[$idx]}" ||
        dht_die "cold-bootstrap remote node $idx failed"
done
for pos in 1 2 3 4 5 6; do
    idx="${ORDER[$pos]}"
    dht_wait_auth "${DDS[$idx]}" "${RPCS[$idx]}" 1 ||
        dht_die "remote sparse chain node $idx did not authenticate"
done

dht_spawn "PIDS[$ORIGIN]" "${DDS[$ORIGIN]}" "${PORTS[$ORIGIN]}" \
    "${RPCS[$ORIGIN]}" "${FSPORTS[$ORIGIN]}" "${HTTPSPORTS[$ORIGIN]}" \
    "127.0.0.1:$DEAD_SINK"
dht_wait_rpc "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" "${PIDS[$ORIGIN]}" ||
    dht_die "origin zero-peer cold restart failed"
dht_wait_cold_load "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" 1 ||
    dht_die "origin did not load its address-free cold contact"
cold="$(dht_status "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}")"
connections="$(dht_rpc "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" getconnectioncount | dht_result)"
[ "$connections" -eq 0 ] &&
[ "$(printf '%s' "$cold" | dht_jget 'd["data"]["connected_authenticated"]')" -eq 0 ] &&
[ "$(printf '%s' "$cold" | dht_jget 'd["data"]["cold_contacts"]')" -eq 1 ] ||
    dht_die "origin did not start with exactly zero peers: $cold connections=$connections"

before_cold_find="$cold"
cold_find="$(dht_native "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}" \
    zcode network find --input="{\"node_id\":\"${NODES[$TARGET]}\"}" || true)"
after_cold_find="$(dht_status "${DDS[$ORIGIN]}" "${RPCS[$ORIGIN]}")"
python3 - "$cold_find" "$before_cold_find" "$after_cold_find" \
    "${NODES[$TARGET]}" <<'PY' || dht_die "autonomous cold-bootstrap lookup failed"
import json,sys
r=json.loads(sys.argv[1]); b=json.loads(sys.argv[2])["data"]; a=json.loads(sys.argv[3])["data"]
assert r["ok"] is True,r
d=r["data"]
assert d["termination"]=="target_authenticated" and d["rounds"]>=2,d
assert sys.argv[4] in d["node_ids"],d
br=b["reachability"]; ar=a["reachability"]
assert ar["entries"]>=6,ar
requests=ar["requests_enqueued"]-br["requests_enqueued"]
dials=ar["dials_queued"]-br["dials_queued"]
# One persisted cold seed creates the first connection; standard iterative
# Kademlia then directly authenticates further address-free IDs returned by
# each hop. Every queued request must produce at most one dial, and the whole
# proof stays bounded by the independently accepted endpoint index.
assert 2<=dials<=ar["entries"]-1,(br,ar)
assert requests==dials,(br,ar)
assert a["connected_authenticated"]>=1,a
PY

# Optional composition point for larger real-process acceptances. Run it only
# after this owner's sparse/recovery assertions are complete: a composed proof
# may legitimately publish records, dial discovered providers, and restart
# roles, none of which may retroactively perturb the DHT fixture's topology
# assertions. All seven independent identities are live and authenticated here,
# so the hook still reuses the production DHT/Noise ceremony above. Restrict it
# to this repository's tools/dev directory: an ambient path must never become
# executable acceptance input.
if [ -n "$DHT_AFTER_SPARSE_HOOK" ]; then
    hook_real="$(readlink -f "$DHT_AFTER_SPARSE_HOOK" 2>/dev/null || true)"
    case "$hook_real" in
        "$REPO_ROOT"/tools/dev/*.sh) ;;
        *) dht_die "after-sparse hook must be a tools/dev shell script" ;;
    esac
    [ -f "$hook_real" ] || dht_die "after-sparse hook is not a regular file"
    dht_note "running composed after-sparse acceptance hook $(basename "$hook_real")"
    # shellcheck source=/dev/null
    . "$hook_real"
fi

if ! dht_cleanup; then
    dht_die "owned process groups did not terminate during success cleanup"
fi
dht_assert_no_owned_processes
dht_assert_ports_rebindable
dht_note "PASS: seven-node sparse lookup, true async admission, persistence, autonomous cold bootstrap; owned_processes_remaining=0 ports_rebindable=true"
