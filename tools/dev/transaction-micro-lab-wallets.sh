#!/usr/bin/env bash
# Create or inspect the two persistent, isolated receive-only wallets used by
# the live transaction micro lab. Public output is deliberately address-free.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
# shellcheck source=tools/scripts/sh_str.sh
. "$REPO/tools/scripts/sh_str.sh"

ACTION="${1:-status}"
shift || true
BASE_DIR="${ZCL_TRANSACTION_LAB_WALLETS_DIR:-$HOME/.local/state/zclassic23-transaction-lab-wallets}"
NODE_BIN="${ZCL_TRANSACTION_LAB_NODE_BIN:-build/bin/zclassic23-dev}"
ACTIVE_NODE_PID=""

cleanup_active_node() {
    if [ -n "$ACTIVE_NODE_PID" ] && kill -0 "$ACTIVE_NODE_PID" 2>/dev/null; then
        kill -TERM "$ACTIVE_NODE_PID" 2>/dev/null || true
    fi
}
trap cleanup_active_node EXIT INT TERM

while [ $# -gt 0 ]; do
    case "$1" in
        --base-dir=*) BASE_DIR="${1#*=}" ;;
        -h|--help)
            cat <<'USAGE'
usage: tools/dev/transaction-micro-lab-wallets.sh {setup|status|selftest} [--base-dir=ABSOLUTE_DIR]

Creates two persistent mainnet-address wallets on isolated loopback ports,
then stops them. Addresses stay in mode-0600 private manifests and are never
printed. This command never funds, reserves, signs, or broadcasts.
USAGE
            exit 0
            ;;
        *)
            echo "transaction-micro-lab-wallets: unknown argument" >&2
            exit 2
            ;;
    esac
    shift
done

fail() {
    echo "transaction-micro-lab-wallets: $*" >&2
    exit 1
}

assert_private_base() {
    case "$BASE_DIR" in
        /*) ;;
        *) fail "base directory must be absolute" ;;
    esac
    case "$BASE_DIR" in
        "$REPO"|"$REPO"/*) fail "base directory must stay outside the repository" ;;
        /|/home|/home/*/github|/tmp) fail "base directory is too broad" ;;
    esac
}

wallet_manifest_valid() {
    local file="$1"
    [ -f "$file" ] && [ "$(stat -c '%a' "$file" 2>/dev/null || true)" = 600 ] &&
        jq -e '.schema == "zcl.transaction_micro_lab_recipient_wallet.v1" and
               (.alias == "lab-a" or .alias == "lab-b") and
               (.transparent | type == "string" and length > 20) and
               (.sapling | type == "string" and length > 20)' \
            "$file" >/dev/null 2>&1
}

emit_status() {
    assert_private_base
    local ready=0 alias file
    for alias in lab-a lab-b; do
        file="$BASE_DIR/$alias/recipient.json"
        wallet_manifest_valid "$file" && ready=$((ready + 1))
    done
    local state="blocked"
    [ "$ready" -eq 2 ] && state="ready"
    printf 'transaction-micro-lab-wallets: [%s%s] %d/2 state=%s addresses=redacted keys=never-exported funded=false\n' \
        "$([ "$ready" -ge 1 ] && printf '#' || printf '-')" \
        "$([ "$ready" -ge 2 ] && printf '#' || printf '-')" \
        "$ready" "$state"
}

port_free() {
    local port="$1"
    local listeners
    listeners="$(ss -tlnH "sport = :$port" 2>/dev/null || true)"
    [ -z "$listeners" ]
}

create_one() {
    local alias="$1" base_port="$2" dd
    dd="$BASE_DIR/$alias"
    local manifest="$dd/recipient.json"
    wallet_manifest_valid "$manifest" && return 0
    [ ! -e "$manifest" ] ||
        fail "an existing recipient manifest is invalid; refusing to replace it"

    local p2p="$base_port" rpc=$((base_port + 1))
    local fs=$((base_port + 2)) https=$((base_port + 3)) p
    for p in "$p2p" "$rpc" "$fs" "$https"; do
        port_free "$p" || fail "an isolated wallet port is already in use"
    done

    install -d -m 700 "$dd"
    local boot_log="$dd/.setup-node.log" status_json="$dd/.setup-status.json"
    local t_json="$dd/.setup-transparent.json" z_json="$dd/.setup-sapling.json"
    rm -f -- "$boot_log" "$status_json" "$t_json" "$z_json"

    setsid "$NODE_BIN" -datadir="$dd" -operator-lane=test \
        -port="$p2p" -rpcport="$rpc" -fsport="$fs" -httpsport="$https" \
        -connect=127.0.0.1:39999 -nolegacyimport -nobgvalidation \
        -wallet-no-phrase-backup -showmetrics=0 >"$boot_log" 2>&1 &
    local pid=$! deadline=$(( $(date +%s) + 180 )) ready=false
    ACTIVE_NODE_PID="$pid"
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f -- "$boot_log" "$status_json" "$t_json" "$z_json"
            fail "an isolated wallet node exited during setup"
        fi
        if "$NODE_BIN" -datadir="$dd" -rpcport="$rpc" core wallet status \
                >"$status_json" 2>/dev/null &&
           jq -e '.ok == true' "$status_json" >/dev/null 2>&1; then
            ready=true
            break
        fi
        sleep 1
    done
    if [ "$ready" != true ]; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -f -- "$boot_log" "$status_json" "$t_json" "$z_json"
        fail "an isolated wallet node did not become ready"
    fi

    "$NODE_BIN" -datadir="$dd" -rpcport="$rpc" core wallet address new \
        >"$t_json" 2>/dev/null || true
    "$NODE_BIN" -datadir="$dd" -rpcport="$rpc" core wallet shielded address \
        >"$z_json" 2>/dev/null || true
    local transparent sapling
    transparent="$(jq -er '.data.address' "$t_json" 2>/dev/null || true)"
    sapling="$(jq -er '.data.address' "$z_json" 2>/dev/null || true)"
    [ "${#transparent}" -gt 20 ] && [ "${#sapling}" -gt 20 ] || {
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -f -- "$boot_log" "$status_json" "$t_json" "$z_json"
        fail "isolated wallet address derivation failed"
    }

    local candidate="$dd/.recipient.json.tmp"
    jq -n --arg alias "$alias" --arg transparent "$transparent" \
        --arg sapling "$sapling" \
        '{schema:"zcl.transaction_micro_lab_recipient_wallet.v1",
          alias:$alias,transparent:$transparent,sapling:$sapling}' \
        >"$candidate"
    chmod 600 "$candidate"
    mv -f -- "$candidate" "$manifest"

    kill -TERM "$pid" 2>/dev/null || true
    local stop_deadline=$(( $(date +%s) + 60 ))
    while kill -0 "$pid" 2>/dev/null &&
          [ "$(date +%s)" -lt "$stop_deadline" ]; do
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    ACTIVE_NODE_PID=""
    rm -f -- "$boot_log" "$status_json" "$t_json" "$z_json"
}

selftest() {
    local temp setup_temp fake_node
    temp="$(mktemp -d /tmp/zcl23-micro-wallet-selftest-XXXXXX)"
    case "$temp" in /tmp/zcl23-micro-wallet-selftest-*) ;; *) return 1 ;; esac
    trap 'rm -r -- "$temp"' RETURN
    install -d -m 700 "$temp/lab-a" "$temp/lab-b"
    printf '%s\n' '{"schema":"zcl.transaction_micro_lab_recipient_wallet.v1","alias":"lab-a","transparent":"t1aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","sapling":"zs1aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}' >"$temp/lab-a/recipient.json"
    printf '%s\n' '{"schema":"zcl.transaction_micro_lab_recipient_wallet.v1","alias":"lab-b","transparent":"t1bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","sapling":"zs1bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}' >"$temp/lab-b/recipient.json"
    chmod 600 "$temp/lab-a/recipient.json" "$temp/lab-b/recipient.json"
    local out
    out="$(ZCL_TRANSACTION_LAB_WALLETS_DIR="$temp" "$0" status)"
    str_contains "$out" '2/2 state=ready' || return 1
    if str_contains "$out" 't1aaaa' || str_contains "$out" 'zs1aaaa' ||
       str_contains "$out" 'lab-a' || str_contains "$out" '/tmp/'; then
        return 1
    fi

    # Exercise the setup/create path with a process-compatible fake node. The
    # public harness output must remain redacted while both private manifests
    # are created and the two fake node processes are stopped.
    setup_temp="$temp/setup"
    fake_node="$temp/fake-node"
    install -d -m 700 "$setup_temp"
    printf '%s\n' \
        '#!/bin/sh' \
        'case " $* " in' \
        '  *" core wallet status "*) printf '\''%s\n'\'' '\''{"ok":true}'\''; exit 0 ;;' \
        '  *" core wallet address new "*) printf '\''%s\n'\'' '\''{"data":{"address":"t1ccccccccccccccccccccccccccccccccc"}}'\''; exit 0 ;;' \
        '  *" core wallet shielded address "*) printf '\''%s\n'\'' '\''{"data":{"address":"zs1cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}}'\''; exit 0 ;;' \
        'esac' \
        'trap '\''exit 0'\'' TERM INT' \
        'while :; do sleep 1; done' >"$fake_node"
    chmod 700 "$fake_node"
    out="$(ZCL_TRANSACTION_LAB_WALLETS_DIR="$setup_temp" \
        ZCL_TRANSACTION_LAB_NODE_BIN="$fake_node" "$0" setup)"
    str_contains "$out" '2/2 state=ready' || return 1
    wallet_manifest_valid "$setup_temp/lab-a/recipient.json" || return 1
    wallet_manifest_valid "$setup_temp/lab-b/recipient.json" || return 1
    if str_contains "$out" 't1cccc' || str_contains "$out" 'zs1cccc' ||
       str_contains "$out" 'lab-a' || str_contains "$out" '/tmp/'; then
        return 1
    fi
    echo "transaction-micro-lab-wallets selftest: PASS"
}

command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v ss >/dev/null 2>&1 || fail "ss is required"

case "$ACTION" in
    setup)
        assert_private_base
        [ -x "$NODE_BIN" ] || fail "zclassic23-dev binary is not built"
        umask 077
        install -d -m 700 "$BASE_DIR"
        create_one lab-a 39310
        create_one lab-b 39320
        emit_status
        ;;
    status) emit_status ;;
    selftest) selftest ;;
    *) fail "usage: setup, status, or selftest" ;;
esac
