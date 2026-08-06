#!/usr/bin/env bash
# Owner-only provisioning for the private dev/prod custody binding.
#
# The command discovers wallet identities through each node's typed custody
# reader, writes the endpoint-bearing grant spec under a mode-0700 directory,
# and lets the production broker mint money-bindings.json.  Public output is
# deliberately free of wallet ids, genesis hashes, ports and filesystem paths.
# It never reserves, signs, broadcasts or moves funds.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
# shellcheck source=tools/scripts/sh_str.sh
. "$REPO/tools/scripts/sh_str.sh"

ACTION="${1:-setup}"
shift || true

BROKER_DIR="${ZCL_CUSTODY_BROKER_DIR:-$HOME/.local/state/zclassic23-custody-broker}"
DEV_DATADIR="${ZCL_CUSTODY_DEV_DATADIR:-$HOME/.zclassic-c23-dev}"
PROD_DATADIR="${ZCL_CUSTODY_PROD_DATADIR:-$HOME/.zclassic-c23}"
DEV_RPCPORT="${ZCL_CUSTODY_DEV_RPCPORT:-18252}"
PROD_RPCPORT="${ZCL_CUSTODY_PROD_RPCPORT:-18232}"
NODE_BIN="${ZCL_CUSTODY_BIN:-build/bin/zclassic23-dev}"
RPC_BIN="${ZCL_CUSTODY_RPC_BIN:-build/bin/zcl-rpc}"

while [ $# -gt 0 ]; do
    case "$1" in
        --broker-dir=*) BROKER_DIR="${1#*=}" ;;
        --dev-datadir=*) DEV_DATADIR="${1#*=}" ;;
        --prod-datadir=*) PROD_DATADIR="${1#*=}" ;;
        --dev-rpcport=*) DEV_RPCPORT="${1#*=}" ;;
        --prod-rpcport=*) PROD_RPCPORT="${1#*=}" ;;
        -h|--help)
            cat <<'USAGE'
usage: tools/dev/custody-bind.sh {setup|selftest} [options]

Options:
  --broker-dir=ABSOLUTE_DIR
  --dev-datadir=ABSOLUTE_DIR
  --prod-datadir=ABSOLUTE_DIR
  --dev-rpcport=PORT
  --prod-rpcport=PORT

Discovers both persistent wallet identities from their local typed readers,
creates an owner-private broker binding, and verifies both money snapshots are
CURRENT. No key, address, wallet id, endpoint or path is printed. No funds are
reserved, signed, broadcast or moved.
USAGE
            exit 0
            ;;
        *)
            echo "custody-bind: unknown argument" >&2
            exit 2
            ;;
    esac
    shift
done

fail() {
    echo "custody-bind: $*" >&2
    exit 1
}

is_port() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$1" -ge 1 ] && [ "$1" -le 65535 ]
}

assert_private_target() {
    case "$BROKER_DIR" in
        /*) ;;
        *) fail "broker directory must be absolute" ;;
    esac
    case "$BROKER_DIR" in
        "$REPO"|"$REPO"/*)
            fail "broker directory must stay outside the repository"
            ;;
        /|/home|"$HOME"|/tmp)
            fail "broker directory is too broad"
            ;;
    esac
    case "$DEV_DATADIR:$PROD_DATADIR" in
        /*:/*) ;;
        *) fail "both wallet datadirs must be absolute" ;;
    esac
    is_port "$DEV_RPCPORT" || fail "development RPC port is invalid"
    is_port "$PROD_RPCPORT" || fail "production RPC port is invalid"
    [ "$DEV_DATADIR:$DEV_RPCPORT" != "$PROD_DATADIR:$PROD_RPCPORT" ] ||
        fail "development and production endpoints must be distinct"
}

custody_snapshot() {
    local scope="$1" datadir="$2" port="$3" raw snapshot
    if ! raw="$(ZCL_DATADIR="$datadir" ZCL_RPCPORT="$port" \
            "$RPC_BIN" agentsession '"custody"' \
            "{\"wallet_scope\":\"$scope\"}" 2>/dev/null)"; then
        fail "$scope wallet custody reader is unreachable"
    fi
    snapshot="$(printf '%s\n' "$raw" | jq -cer \
        --arg scope "$scope" '
        .result.snapshot |
        select(.wallet_scope == $scope and .status == "CURRENT" and
               .complete == true and
               (.wallet_instance_id | type == "string" and
                test("^[0-9a-fA-F]{32}$")) and
               (.network_genesis | type == "string" and
                test("^[0-9a-fA-F]{64}$")))' 2>/dev/null || true)"
    [ -n "$snapshot" ] ||
        fail "$scope wallet money state is not complete and CURRENT"
    printf '%s\n' "$snapshot"
}

write_spec() {
    local dev_snapshot="$1" prod_snapshot="$2" candidate="$3"
    local dev_id prod_id dev_genesis prod_genesis
    dev_id="$(jq -er '.wallet_instance_id' <<<"$dev_snapshot")"
    prod_id="$(jq -er '.wallet_instance_id' <<<"$prod_snapshot")"
    dev_genesis="$(jq -er '.network_genesis' <<<"$dev_snapshot")"
    prod_genesis="$(jq -er '.network_genesis' <<<"$prod_snapshot")"
    [ "$dev_id" != "$prod_id" ] ||
        fail "duplicate wallet identity across active endpoints"
    [ "$dev_genesis" = "$prod_genesis" ] ||
        fail "development and production wallets report different networks"

    jq -n \
        --arg dev_id "$dev_id" --arg prod_id "$prod_id" \
        --arg genesis "$dev_genesis" \
        --arg dev_datadir "$DEV_DATADIR" \
        --arg prod_datadir "$PROD_DATADIR" \
        --argjson dev_rpcport "$DEV_RPCPORT" \
        --argjson prod_rpcport "$PROD_RPCPORT" '
        {holder:"local-owner-custody",issuer:"local-owner-custody",
         scope:"kinds",kinds:"content",queries:"inspect_property",
         max_value_zat:0,
         wallets:[
           {scope:"dev",wallet_instance_id:$dev_id,
            network_genesis:$genesis,node_datadir:$dev_datadir,
            rpc_port:$dev_rpcport},
           {scope:"prod",wallet_instance_id:$prod_id,
            network_genesis:$genesis,node_datadir:$prod_datadir,
            rpc_port:$prod_rpcport}
         ]}' >"$candidate"
    chmod 600 "$candidate"
}

verify_private_file() {
    local path="$1"
    [ -f "$path" ] && [ "$(stat -c '%a' "$path" 2>/dev/null || true)" = 600 ]
}

setup_binding() {
    assert_private_target
    command -v jq >/dev/null 2>&1 || fail "jq is required"
    [ -x "$NODE_BIN" ] || fail "current development node binary is not built"
    [ -x "$RPC_BIN" ] || fail "RPC helper is not built"

    local dev_snapshot prod_snapshot spec candidate log money
    dev_snapshot="$(custody_snapshot dev "$DEV_DATADIR" "$DEV_RPCPORT")"
    prod_snapshot="$(custody_snapshot prod "$PROD_DATADIR" "$PROD_RPCPORT")"

    umask 077
    install -d -m 700 "$BROKER_DIR"
    [ "$(stat -c '%a' "$BROKER_DIR" 2>/dev/null || true)" = 700 ] ||
        fail "broker directory is not mode 0700"
    case "$(readlink -f "$BROKER_DIR")" in
        "$REPO"|"$REPO"/*)
            fail "resolved broker directory must stay outside the repository"
            ;;
    esac

    spec="$BROKER_DIR/grant-spec.json"
    candidate="$BROKER_DIR/.grant-spec.json.tmp.$$"
    log="$BROKER_DIR/setup.log"
    trap 'rm -f -- "$candidate"' RETURN
    write_spec "$dev_snapshot" "$prod_snapshot" "$candidate"
    mv -f -- "$candidate" "$spec"

    if ! "$NODE_BIN" -datadir="$DEV_DATADIR" --metaverse-broker \
            --broker-dir="$BROKER_DIR" --grant-spec="$spec" \
            --script=inspect --requests=1 >"$log" 2>&1; then
        chmod 600 "$log" 2>/dev/null || true
        fail "broker refused the private custody specification"
    fi
    chmod 600 "$log" 2>/dev/null || true
    verify_private_file "$spec" || fail "private grant specification mode drifted"
    verify_private_file "$BROKER_DIR/money-bindings.json" ||
        fail "broker did not persist a private money binding"

    if ! money="$("$NODE_BIN" metaverse agent money \
            --dir="$BROKER_DIR" 2>/dev/null)"; then
        fail "identity-bound money reader is unreachable"
    fi
    if str_contains "$money" 'node_datadir' || str_contains "$money" 'rpc_port' ||
       str_contains "$money" "$DEV_DATADIR" || str_contains "$money" "$PROD_DATADIR"; then
        fail "public money output exposed a private endpoint"
    fi
    printf '%s\n' "$money" | jq -e '
        .ok == true and
        ((.data.wallets // []) |
          ([.[] | select(.wallet_scope == "dev" and .status == "CURRENT")] |
           length) == 1 and
          ([.[] | select(.wallet_scope == "prod" and .status == "CURRENT")] |
           length) == 1)' >/dev/null 2>&1 ||
        fail "one or both identity-bound wallet snapshots are not CURRENT"

    echo "custody-bind: ready dev=CURRENT prod=CURRENT identities=bound endpoints=private funds_moved=false"
}

selftest() {
    local temp fake_rpc fake_node out duplicate_out
    temp="$(mktemp -d /tmp/zcl23-custody-bind-selftest-XXXXXX)"
    case "$temp" in
        /tmp/zcl23-custody-bind-selftest-*) ;;
        *) return 1 ;;
    esac
    trap 'rm -r -- "$temp"' RETURN
    fake_rpc="$temp/fake-rpc"
    fake_node="$temp/fake-node"
    printf '%s\n' \
        '#!/bin/sh' \
        'id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' \
        '[ "${ZCL_RPCPORT:-}" = 2 ] && id=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' \
        '[ "${ZCL_CUSTODY_BIND_DUPLICATE_FIXTURE:-0}" = 1 ] && id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' \
        'scope=dev' \
        '[ "${ZCL_RPCPORT:-}" = 2 ] && scope=prod' \
        'printf '\''{"result":{"ok":true,"snapshot":{"wallet_scope":"%s","wallet_instance_id":"%s","network_genesis":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","status":"CURRENT","complete":true,"confirmed_zcl":"0.00000000"}}}\n'\'' "$scope" "$id"' \
        >"$fake_rpc"
    printf '%s\n' \
        '#!/bin/sh' \
        'dir=' \
        'spec=' \
        'for arg in "$@"; do case "$arg" in --broker-dir=*) dir=${arg#*=} ;; --grant-spec=*) spec=${arg#*=} ;; esac; done' \
        'case " $* " in' \
        '  *" --metaverse-broker "*)' \
        '    jq '\''{schema:"zcl.agent_money_bindings.v1",wallets:.wallets}'\'' "$spec" >"$dir/money-bindings.json"' \
        '    chmod 600 "$dir/money-bindings.json"' \
        '    printf '\''{}\n'\'' >"$dir/broker.json"; chmod 600 "$dir/broker.json"; exit 0 ;;' \
        '  *" metaverse agent money "*)' \
        '    printf '\''{"ok":true,"data":{"wallets":[{"wallet_scope":"dev","status":"CURRENT"},{"wallet_scope":"prod","status":"CURRENT"}],"portfolio_total_known":true}}\n'\''; exit 0 ;;' \
        'esac' \
        'exit 1' >"$fake_node"
    chmod 700 "$fake_rpc" "$fake_node"

    out="$(ZCL_CUSTODY_BIN="$fake_node" ZCL_CUSTODY_RPC_BIN="$fake_rpc" \
        ZCL_CUSTODY_DEV_DATADIR="$temp/dev" \
        ZCL_CUSTODY_PROD_DATADIR="$temp/prod" \
        ZCL_CUSTODY_DEV_RPCPORT=1 ZCL_CUSTODY_PROD_RPCPORT=2 \
        "$0" setup --broker-dir="$temp/broker")"
    str_contains "$out" 'ready dev=CURRENT prod=CURRENT' || return 1
    verify_private_file "$temp/broker/grant-spec.json" || return 1
    verify_private_file "$temp/broker/money-bindings.json" || return 1
    if str_contains "$out" 'aaaaaaaa' || str_contains "$out" 'cccccccc' ||
       str_contains "$out" "$temp" || str_contains "$out" 'rpc_port'; then
        return 1
    fi

    if duplicate_out="$(ZCL_CUSTODY_BIND_DUPLICATE_FIXTURE=1 \
        ZCL_CUSTODY_BIN="$fake_node" ZCL_CUSTODY_RPC_BIN="$fake_rpc" \
        ZCL_CUSTODY_DEV_DATADIR="$temp/dev" \
        ZCL_CUSTODY_PROD_DATADIR="$temp/prod" \
        ZCL_CUSTODY_DEV_RPCPORT=1 ZCL_CUSTODY_PROD_RPCPORT=2 \
        "$0" setup --broker-dir="$temp/duplicate" 2>&1)"; then
        return 1
    fi
    str_contains "$duplicate_out" 'duplicate wallet identity' || return 1
    [ ! -e "$temp/duplicate/money-bindings.json" ] || return 1
    echo "custody-bind selftest: PASS"
}

case "$ACTION" in
    setup) setup_binding ;;
    selftest) selftest ;;
    *) fail "usage: setup or selftest" ;;
esac
