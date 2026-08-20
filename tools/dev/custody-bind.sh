#!/usr/bin/env bash
# Owner-only provisioning for a private custody binding.
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
WALLET_SCOPE="portfolio"

while [ $# -gt 0 ]; do
    case "$1" in
        --broker-dir=*) BROKER_DIR="${1#*=}" ;;
        --dev-datadir=*) DEV_DATADIR="${1#*=}" ;;
        --prod-datadir=*) PROD_DATADIR="${1#*=}" ;;
        --dev-rpcport=*) DEV_RPCPORT="${1#*=}" ;;
        --prod-rpcport=*) PROD_RPCPORT="${1#*=}" ;;
        --wallet-scope=dev|--wallet-scope=prod|--wallet-scope=portfolio)
            WALLET_SCOPE="${1#*=}"
            ;;
        --wallet-scope=*)
            echo "custody-bind: --wallet-scope must be dev, prod, or portfolio" >&2
            exit 2
            ;;
        -h|--help)
            cat <<'USAGE'
usage: tools/dev/custody-bind.sh {setup|selftest} [options]

Options:
  --broker-dir=ABSOLUTE_DIR
  --dev-datadir=ABSOLUTE_DIR
  --prod-datadir=ABSOLUTE_DIR
  --dev-rpcport=PORT
  --prod-rpcport=PORT
  --wallet-scope=dev|prod|portfolio

Discovers only the requested persistent wallet identity (both identities when
portfolio is selected), creates an owner-private broker binding, and verifies
the requested money snapshot is CURRENT. The default is portfolio. No key,
address, wallet id, endpoint or path is printed. No funds are reserved, signed,
broadcast or moved.
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
    case "$WALLET_SCOPE" in
        dev|portfolio)
            case "$DEV_DATADIR" in /*) ;; *) fail "development wallet datadir must be absolute" ;; esac
            is_port "$DEV_RPCPORT" || fail "development RPC port is invalid"
            ;;
    esac
    case "$WALLET_SCOPE" in
        prod|portfolio)
            case "$PROD_DATADIR" in /*) ;; *) fail "production wallet datadir must be absolute" ;; esac
            is_port "$PROD_RPCPORT" || fail "production RPC port is invalid"
            ;;
    esac
    if [ "$WALLET_SCOPE" = portfolio ]; then
        [ "$DEV_DATADIR:$DEV_RPCPORT" != "$PROD_DATADIR:$PROD_RPCPORT" ] ||
            fail "development and production endpoints must be distinct"
    fi
}

custody_snapshot() {
    local scope="$1" datadir="$2" port="$3" raw snapshot attempt
    for attempt in 1 2 3 4 5; do
        raw="$(ZCL_DATADIR="$datadir" ZCL_RPCPORT="$port" \
            timeout 12s "$RPC_BIN" agentsession '"custody"' \
            "{\"wallet_scope\":\"$scope\"}" 2>/dev/null || true)"
        snapshot="$(printf '%s\n' "$raw" | jq -cer \
            --arg scope "$scope" '
            .result.snapshot |
            select(.wallet_scope == $scope and .status == "CURRENT" and
                   .complete == true and
                   (.wallet_instance_id | type == "string" and
                    test("^[0-9a-fA-F]{32}$")) and
                   (.network_genesis | type == "string" and
                    test("^[0-9a-fA-F]{64}$")))' 2>/dev/null || true)"
        if [ -n "$snapshot" ]; then
            printf '%s\n' "$snapshot"
            return 0
        fi
        [ "$attempt" -eq 5 ] || sleep 2
    done
    fail "$scope wallet money state is not complete and CURRENT"
}

write_spec() {
    local dev_snapshot="$1" prod_snapshot="$2" candidate="$3"
    local dev_id prod_id dev_genesis prod_genesis dev_rpc_arg prod_rpc_arg
    dev_id=""
    prod_id=""
    dev_genesis=""
    prod_genesis=""
    if [ -n "$dev_snapshot" ]; then
        dev_id="$(jq -er '.wallet_instance_id' <<<"$dev_snapshot")"
        dev_genesis="$(jq -er '.network_genesis' <<<"$dev_snapshot")"
    fi
    if [ -n "$prod_snapshot" ]; then
        prod_id="$(jq -er '.wallet_instance_id' <<<"$prod_snapshot")"
        prod_genesis="$(jq -er '.network_genesis' <<<"$prod_snapshot")"
    fi
    if [ "$WALLET_SCOPE" = portfolio ]; then
        [ "$dev_id" != "$prod_id" ] ||
            fail "duplicate wallet identity across active endpoints"
        [ "$dev_genesis" = "$prod_genesis" ] ||
            fail "development and production wallets report different networks"
    fi
    dev_rpc_arg=1
    prod_rpc_arg=1
    case "$WALLET_SCOPE" in dev|portfolio) dev_rpc_arg="$DEV_RPCPORT" ;; esac
    case "$WALLET_SCOPE" in prod|portfolio) prod_rpc_arg="$PROD_RPCPORT" ;; esac

    jq -n --arg requested_scope "$WALLET_SCOPE" \
        --arg dev_id "$dev_id" --arg prod_id "$prod_id" \
        --arg dev_genesis "$dev_genesis" --arg prod_genesis "$prod_genesis" \
        --arg dev_datadir "$DEV_DATADIR" \
        --arg prod_datadir "$PROD_DATADIR" \
        --argjson dev_rpcport "$dev_rpc_arg" \
        --argjson prod_rpcport "$prod_rpc_arg" '
        {holder:"local-owner-custody",issuer:"local-owner-custody",
         scope:"kinds",kinds:"content",queries:"inspect_property",
         max_value_zat:0,
         wallets:([
           {scope:"dev",wallet_instance_id:$dev_id,
            network_genesis:$dev_genesis,node_datadir:$dev_datadir,
            rpc_port:$dev_rpcport},
           {scope:"prod",wallet_instance_id:$prod_id,
            network_genesis:$prod_genesis,node_datadir:$prod_datadir,
            rpc_port:$prod_rpcport}
         ] | if $requested_scope == "portfolio" then .
              else map(select(.scope == $requested_scope)) end)}' >"$candidate"
    chmod 600 "$candidate"
}

verify_private_file() {
    local path="$1"
    [ -f "$path" ] && [ "$(stat -c '%a' "$path" 2>/dev/null || true)" = 600 ]
}

binding_matches_spec() {
    local binding="$1" spec="$2"
    jq -e --slurpfile requested "$spec" '
        (.wallets | sort_by(.scope)) ==
        ($requested[0].wallets | sort_by(.scope))' "$binding" \
        >/dev/null 2>&1
}

verify_money_current() {
    local money="" attempt
    for attempt in 1 2 3; do
        money="$(timeout 25s "$NODE_BIN" metaverse agent money \
            --dir="$BROKER_DIR" 2>/dev/null || true)"
        if [ -n "$money" ] &&
           ! str_contains "$money" 'node_datadir' &&
           ! str_contains "$money" 'rpc_port' &&
           ! str_contains "$money" "$DEV_DATADIR" &&
           ! str_contains "$money" "$PROD_DATADIR" &&
           printf '%s\n' "$money" | jq -e --arg scope "$WALLET_SCOPE" '
               .ok == true and
               ((.data.wallets // []) | if $scope == "portfolio" then
                 (([.[] | select(.wallet_scope == "dev" and
                                 .status == "CURRENT")] | length) == 1 and
                  ([.[] | select(.wallet_scope == "prod" and
                                 .status == "CURRENT")] | length) == 1)
                else
                  ([.[] | select(.wallet_scope == $scope and
                                 .status == "CURRENT")] | length) == 1
                end)' \
               >/dev/null 2>&1; then
            return 0
        fi
        [ "$attempt" -eq 3 ] || sleep 2
    done
    fail "the requested identity-bound wallet snapshot is not CURRENT"
}

setup_binding() {
    assert_private_target
    command -v jq >/dev/null 2>&1 || fail "jq is required"
    [ -x "$NODE_BIN" ] || fail "current development node binary is not built"
    [ -x "$RPC_BIN" ] || fail "RPC helper is not built"

    local dev_snapshot prod_snapshot spec candidate log generation gen_spec
    local binding_candidate
    dev_snapshot=""
    prod_snapshot=""
    case "$WALLET_SCOPE" in
        dev|portfolio)
            dev_snapshot="$(custody_snapshot dev "$DEV_DATADIR" "$DEV_RPCPORT")"
            ;;
    esac
    case "$WALLET_SCOPE" in
        prod|portfolio)
            prod_snapshot="$(custody_snapshot prod "$PROD_DATADIR" "$PROD_RPCPORT")"
            ;;
    esac

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
    binding_candidate="$BROKER_DIR/.money-bindings.json.tmp.$$"
    trap 'rm -f -- "$candidate" "$binding_candidate"' RETURN
    write_spec "$dev_snapshot" "$prod_snapshot" "$candidate"

    if verify_private_file "$BROKER_DIR/money-bindings.json" &&
       binding_matches_spec "$BROKER_DIR/money-bindings.json" "$candidate"; then
        mv -f -- "$candidate" "$spec"
    else
        install -d -m 700 "$BROKER_DIR/generations"
        generation="$(mktemp -d "$BROKER_DIR/generations/gen-XXXXXXXX")"
        chmod 700 "$generation"
        gen_spec="$generation/grant-spec.json"
        install -m 600 "$candidate" "$gen_spec"
        if ! "$NODE_BIN" -datadir="$DEV_DATADIR" --metaverse-broker \
                --broker-dir="$generation" --grant-spec="$gen_spec" \
                --script=inspect --requests=1 >"$generation/setup.log" 2>&1; then
            chmod 600 "$generation/setup.log" 2>/dev/null || true
            install -m 600 "$generation/setup.log" "$log" 2>/dev/null || true
            fail "broker refused the private custody specification"
        fi
        chmod 600 "$generation/setup.log" 2>/dev/null || true
        verify_private_file "$generation/money-bindings.json" ||
            fail "broker did not persist a private money binding"
        install -m 600 "$generation/money-bindings.json" "$binding_candidate"
        mv -f -- "$binding_candidate" "$BROKER_DIR/money-bindings.json"
        mv -f -- "$candidate" "$spec"
        install -m 600 "$generation/setup.log" "$log"
    fi
    verify_private_file "$spec" || fail "private grant specification mode drifted"
    verify_private_file "$BROKER_DIR/money-bindings.json" ||
        fail "broker did not persist a private money binding"
    binding_matches_spec "$BROKER_DIR/money-bindings.json" "$spec" ||
        fail "persisted money binding differs from the owner specification"
    verify_money_current

    case "$WALLET_SCOPE" in
        dev) readiness="dev=CURRENT prod=not_required" ;;
        prod) readiness="dev=not_required prod=CURRENT" ;;
        portfolio) readiness="dev=CURRENT prod=CURRENT" ;;
    esac
    echo "custody-bind: ready scope=$WALLET_SCOPE $readiness identities=bound endpoints=private funds_moved=false"
}

selftest() {
    local temp fake_rpc fake_node call_log rpc_log out rerun_out rotate_out
    local duplicate_out dev_out dev_rerun_out
    temp="$(mktemp -d /tmp/zcl23-custody-bind-selftest-XXXXXX)"
    case "$temp" in
        /tmp/zcl23-custody-bind-selftest-*) ;;
        *) return 1 ;;
    esac
    trap 'rm -r -- "$temp"' RETURN
    fake_rpc="$temp/fake-rpc"
    fake_node="$temp/fake-node"
    call_log="$temp/broker-calls"
    rpc_log="$temp/rpc-calls"
    printf '%s\n' \
        '#!/bin/sh' \
        'id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' \
        '[ "${ZCL_RPCPORT:-}" != 1 ] && id=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' \
        '[ "${ZCL_CUSTODY_BIND_DUPLICATE_FIXTURE:-0}" = 1 ] && id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' \
        'scope=dev' \
        '[ "${ZCL_RPCPORT:-}" != 1 ] && scope=prod' \
        '[ -z "${ZCL_CUSTODY_BIND_RPC_LOG:-}" ] || printf '\''%s\n'\'' "$scope" >>"$ZCL_CUSTODY_BIND_RPC_LOG"' \
        'printf '\''{"result":{"ok":true,"snapshot":{"wallet_scope":"%s","wallet_instance_id":"%s","network_genesis":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","status":"CURRENT","complete":true,"confirmed_zcl":"0.00000000"}}}\n'\'' "$scope" "$id"' \
        >"$fake_rpc"
    printf '%s\n' \
        '#!/bin/sh' \
        'dir=' \
        'spec=' \
        'for arg in "$@"; do case "$arg" in --broker-dir=*) dir=${arg#*=} ;; --grant-spec=*) spec=${arg#*=} ;; esac; done' \
        'case " $* " in' \
        '  *" --metaverse-broker "*)' \
        '    [ -z "${ZCL_CUSTODY_BIND_CALL_LOG:-}" ] || printf x >>"$ZCL_CUSTODY_BIND_CALL_LOG"' \
        '    jq '\''{schema:"zcl.agent_money_bindings.v1",wallets:.wallets}'\'' "$spec" >"$dir/money-bindings.json"' \
        '    chmod 600 "$dir/money-bindings.json"' \
        '    printf '\''{}\n'\'' >"$dir/broker.json"; chmod 600 "$dir/broker.json"; exit 0 ;;' \
        '  *" metaverse agent money "*)' \
        '    printf '\''{"ok":true,"data":{"wallets":[{"wallet_scope":"dev","status":"CURRENT"},{"wallet_scope":"prod","status":"CURRENT"}],"portfolio_total_known":true}}\n'\''; exit 0 ;;' \
        'esac' \
        'exit 1' >"$fake_node"
    chmod 700 "$fake_rpc" "$fake_node"

    out="$(ZCL_CUSTODY_BIND_CALL_LOG="$call_log" ZCL_CUSTODY_BIND_RPC_LOG="$rpc_log" \
        ZCL_CUSTODY_BIN="$fake_node" ZCL_CUSTODY_RPC_BIN="$fake_rpc" \
        ZCL_CUSTODY_DEV_DATADIR="$temp/dev" \
        ZCL_CUSTODY_PROD_DATADIR="$temp/prod" \
        ZCL_CUSTODY_DEV_RPCPORT=1 ZCL_CUSTODY_PROD_RPCPORT=2 \
        "$0" setup --broker-dir="$temp/broker")"
    str_contains "$out" 'ready scope=portfolio dev=CURRENT prod=CURRENT' || return 1
    verify_private_file "$temp/broker/grant-spec.json" || return 1
    verify_private_file "$temp/broker/money-bindings.json" || return 1
    [ "$(wc -c <"$call_log")" = 1 ] || return 1
    if str_contains "$out" 'aaaaaaaa' || str_contains "$out" 'cccccccc' ||
       str_contains "$out" "$temp" || str_contains "$out" 'rpc_port'; then
        return 1
    fi

    rerun_out="$(ZCL_CUSTODY_BIND_CALL_LOG="$call_log" ZCL_CUSTODY_BIND_RPC_LOG="$rpc_log" \
        ZCL_CUSTODY_BIN="$fake_node" ZCL_CUSTODY_RPC_BIN="$fake_rpc" \
        ZCL_CUSTODY_DEV_DATADIR="$temp/dev" \
        ZCL_CUSTODY_PROD_DATADIR="$temp/prod" \
        ZCL_CUSTODY_DEV_RPCPORT=1 ZCL_CUSTODY_PROD_RPCPORT=2 \
        "$0" setup --broker-dir="$temp/broker")"
    str_contains "$rerun_out" 'ready scope=portfolio dev=CURRENT prod=CURRENT' || return 1
    [ "$(wc -c <"$call_log")" = 1 ] || return 1

    rotate_out="$(ZCL_CUSTODY_BIND_CALL_LOG="$call_log" ZCL_CUSTODY_BIND_RPC_LOG="$rpc_log" \
        ZCL_CUSTODY_BIN="$fake_node" ZCL_CUSTODY_RPC_BIN="$fake_rpc" \
        ZCL_CUSTODY_DEV_DATADIR="$temp/dev" \
        ZCL_CUSTODY_PROD_DATADIR="$temp/prod" \
        ZCL_CUSTODY_DEV_RPCPORT=1 ZCL_CUSTODY_PROD_RPCPORT=3 \
        "$0" setup --broker-dir="$temp/broker")"
    str_contains "$rotate_out" 'ready scope=portfolio dev=CURRENT prod=CURRENT' || return 1
    [ "$(wc -c <"$call_log")" = 2 ] || return 1
    [ "$(jq -r '.wallets[] | select(.scope == "prod") | .rpc_port' \
        "$temp/broker/money-bindings.json")" = 3 ] || return 1

    : >"$rpc_log"
    dev_out="$(ZCL_CUSTODY_BIND_CALL_LOG="$call_log" ZCL_CUSTODY_BIND_RPC_LOG="$rpc_log" \
        ZCL_CUSTODY_BIN="$fake_node" ZCL_CUSTODY_RPC_BIN="$fake_rpc" \
        ZCL_CUSTODY_DEV_DATADIR="$temp/dev" \
        ZCL_CUSTODY_PROD_DATADIR=relative-prod-must-not-be-read \
        ZCL_CUSTODY_DEV_RPCPORT=1 ZCL_CUSTODY_PROD_RPCPORT=invalid \
        "$0" setup --wallet-scope=dev --broker-dir="$temp/dev-broker")"
    str_contains "$dev_out" 'ready scope=dev dev=CURRENT prod=not_required' || return 1
    [ "$(jq -r '.wallets | length' "$temp/dev-broker/money-bindings.json")" = 1 ] || return 1
    [ "$(jq -r '.wallets[0].scope' "$temp/dev-broker/money-bindings.json")" = dev ] || return 1
    [ "$(cat "$rpc_log")" = dev ] || return 1
    if str_contains "$dev_out" 'aaaaaaaa' || str_contains "$dev_out" "$temp" ||
       str_contains "$dev_out" 'rpc_port'; then
        return 1
    fi
    dev_rerun_out="$(ZCL_CUSTODY_BIND_CALL_LOG="$call_log" ZCL_CUSTODY_BIND_RPC_LOG="$rpc_log" \
        ZCL_CUSTODY_BIN="$fake_node" ZCL_CUSTODY_RPC_BIN="$fake_rpc" \
        ZCL_CUSTODY_DEV_DATADIR="$temp/dev" \
        ZCL_CUSTODY_PROD_DATADIR=relative-prod-must-not-be-read \
        ZCL_CUSTODY_DEV_RPCPORT=1 ZCL_CUSTODY_PROD_RPCPORT=invalid \
        "$0" setup --wallet-scope=dev --broker-dir="$temp/dev-broker")"
    str_contains "$dev_rerun_out" 'ready scope=dev dev=CURRENT prod=not_required' || return 1
    [ "$(grep -c '^dev$' "$rpc_log")" = 2 ] || return 1

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
