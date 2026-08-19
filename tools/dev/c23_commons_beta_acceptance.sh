#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# purpose: Run the C23 Commons stranger journey from a clean installed prefix.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ROOT="$(mktemp -d /tmp/zcl23-c23-beta.XXXXXX)"
KEEP="${C23_BETA_KEEP:-0}"
PRODUCT_LABEL="${C23_BETA_LABEL:-c23-commons-beta}"
PRODUCT_HOOK="${C23_BETA_HOOK:-$SCRIPT_DIR/c23_commons_beta_hook.sh}"

beta_ui_host_pids() {
    local proc cmd environment
    [ "${C23_BETA_NATIVE_UI_JOURNEY:-0}" = 1 ] || return 0
    for proc in /proc/[0-9]*; do
        [ -r "$proc/cmdline" ] && [ -r "$proc/environ" ] || continue
        cmd="$(tr '\0' ' ' < "$proc/cmdline" 2>/dev/null || true)"
        [ "$cmd" = "$PREFIX/bin/zclassic23 --ui-present-host " ] || continue
        environment="$(tr '\0' '\n' < "$proc/environ" 2>/dev/null || true)"
        case "$environment" in
            *"XDG_RUNTIME_DIR=$RUN_ROOT/native-ui-runtime"*)
                printf '%s\n' "${proc##*/}" ;;
        esac
    done
    return 0
}

beta_cleanup() {
    local rc="$?" pid
    trap - EXIT INT TERM
    while IFS= read -r pid; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done < <(beta_ui_host_pids)
    case "$RUN_ROOT" in
        /tmp/zcl23-c23-beta.*)
            if [ "$rc" -eq 0 ] && [ "$KEEP" != 1 ]; then
                rm -rf "$RUN_ROOT"
            else
                echo "$PRODUCT_LABEL: preserved run at $RUN_ROOT" >&2
            fi
            ;;
        *) echo "$PRODUCT_LABEL: refusing cleanup outside guarded root" >&2 ;;
    esac
    exit "$rc"
}
trap beta_cleanup EXIT INT TERM

PREFIX="$RUN_ROOT/install"
WORK_PARENT="$RUN_ROOT/work"
PARAMS_DIR="$RUN_ROOT/no-zk-params"
mkdir -p "$PREFIX" "$WORK_PARENT" "$PARAMS_DIR"

if [ "${C23_BETA_NATIVE_UI_JOURNEY:-0}" = 1 ]; then
    [ -n "${DISPLAY:-}" ] || {
        echo "c23-commons-beta: native journey requires DISPLAY" >&2
        exit 2
    }
    [ -x "${C23_BETA_NATIVE_UI_DRIVER:-}" ] || {
        echo "c23-commons-beta: native journey driver is unavailable" >&2
        exit 2
    }
    mkdir -m 0700 "$RUN_ROOT/native-ui-runtime"
    # c23-portable-install deliberately rebuilds build/bin from scratch. Keep
    # the already-validated input driver inside this guarded run so that clean
    # product build cannot invalidate the later physical acceptance path.
    install -m 0755 "$C23_BETA_NATIVE_UI_DRIVER" \
        "$RUN_ROOT/native-ui-driver"
    export C23_BETA_NATIVE_UI_DRIVER="$RUN_ROOT/native-ui-driver"
    export XDG_RUNTIME_DIR="$RUN_ROOT/native-ui-runtime"
fi

echo "$PRODUCT_LABEL: installing ordinary product into $PREFIX"
make -C "$REPO_ROOT" c23-portable-install DESTDIR="$PREFIX" PREFIX= >/dev/null
for product in zclassic23 zcl-rpc zclassic23-package-sign \
        zclassic23-package-verify; do
    [ -x "$PREFIX/bin/$product" ] || {
        echo "c23-commons-beta: installed product missing: $product" >&2
        exit 2
    }
done
# Unconditional, because DHT_ACCEPTANCE_C23 below points at the install
# prefix unconditionally and the DHT harness checks that binary before it
# starts a single node, in every composition — it is that harness's own
# assertion tool, not arena scenery. Installing it behind the arena flag
# while redirecting the variable regardless left the default composition
# dying on its binary precondition before any hook could run.
make -C "$REPO_ROOT" tools/arena-product-journey-c23 >/dev/null
install -m 0755 "$REPO_ROOT/build/bin/arena_product_journey_c23" \
    "$PREFIX/bin/arena_product_journey_c23"
# arena_runner and the dev binary are read only by the arena journey hook,
# so they stay behind its flag and cost the default composition nothing.
if [ "${C23_BETA_INSTALL_ARENA_RUNNER:-0}" = 1 ]; then
    make -C "$REPO_ROOT" tools/arena-runner dev-bin >/dev/null
    install -m 0755 "$REPO_ROOT/build/bin/arena_runner" \
        "$PREFIX/bin/arena_runner"
    install -m 0755 "$REPO_ROOT/build/bin/zclassic23-dev" \
        "$PREFIX/bin/zclassic23-dev"
fi
[ -r "$PRODUCT_HOOK" ] || {
    echo "$PRODUCT_LABEL: composition hook is unavailable: $PRODUCT_HOOK" >&2
    exit 2
}

# The physical-node owner remains zcode_dht_acceptance.sh. This product
# runner supplies only an installed binary and an external scratch root; it
# does not create another transport, process owner, or node topology.
export ZCL_NODE_BIN="$PREFIX/bin/zclassic23"
export ZCL_RPC_BIN="$PREFIX/bin/zcl-rpc"
export C23_BETA_INSTALL_BIN="$PREFIX/bin"
export C23_BETA_DEV_BIN="$PREFIX/bin/zclassic23-dev"
export DHT_ACCEPTANCE_C23="$PREFIX/bin/arena_product_journey_c23"
export C23_BETA_FIXTURE_SOURCE="$REPO_ROOT"
export DHT_WORK_PARENT="$WORK_PARENT"
# This regtest scenario never proves or validates a shielded transaction.
# Naming an empty params directory makes that unavailable capability explicit
# and avoids parsing a 50 MiB prover key independently in seven package nodes.
export DHT_PARAMS_DIR="$PARAMS_DIR"
export DHT_PACKAGEHOST=1
# The composed proof phase uses the canonical signer-owned worker service. The
# package-fetch assertions below still prove import is inert before any work
# request exists; enabling a worker is capacity, never execution authority.
export DHT_BUILDWORKERS=1
export DHT_KEEP=1
export DHT_AFTER_SPARSE_HOOK="$PRODUCT_HOOK"

# Every child inherits an outside-checkout cwd. The only repository paths the
# driver retains are its harness and the two pre-existing Commons fixtures;
# none is supplied to a consumer command or node process.
cd "$RUN_ROOT"
bash "$SCRIPT_DIR/zcode_dht_acceptance.sh"

echo "=== $PRODUCT_LABEL: PASSED ==="
