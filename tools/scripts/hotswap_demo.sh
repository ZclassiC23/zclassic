#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Tier-1 hot-swap v2 proof — for EVERY manifest-eligible stateless MCP
# controller, proves (WITHOUT mutating a running service, with only the stock C
# toolchain) that its generation .so loads, validates, self-tests, and commits
# one atomic route snapshot re-pointing a route the TU owns.
#
# Steps, per eligible TU in config/hotswap_eligible.def:
#   1. build build/bin/zclassic23-dev (the -rdynamic dev binary) once,
#   2. build an immutable input-addressed generation .so from the real TU,
#   3. in ONE short-lived process (`mcpcall`, the in-process dispatcher — not
#      the live node): register the real routes, dlopen the .so via the loader,
#      commit all of its routes atomically,
#   4. assert v2 provenance (mcp.routes provider + both content hashes) and that
#      a read-only route the TU owns appears in the atomic replacement report.
#
# Every eligible TU MUST have a proof entry here (probe_for_source); an eligible
# TU with no entry, or a proof entry for a non-eligible TU, fails the sync
# check below — so the harness cannot silently skip a newly-admitted TU.
#
# Pure POSIX sh + coreutils; no third-party tools.
set -eu

cd "$(dirname "$0")/../.."

BIN=build/bin/zclassic23-dev
DEMO_DATADIR=${HOME}/.zclassic-c23-dev
MANIFEST=config/hotswap_eligible.def

# A read-only route each eligible TU owns, used to prove its generation
# committed. Keep in sync with config/hotswap_eligible.def.
probe_for_source() {
    case "$1" in
        tools/mcp/controllers/app_controller.c)    echo "zcl_name_list" ;;
        tools/mcp/controllers/meta_controller.c)   echo "zcl_tools_list" ;;
        tools/mcp/controllers/chain_controller.c)  echo "zcl_getblockcount" ;;
        tools/mcp/controllers/net_controller.c)    echo "zcl_peers" ;;
        tools/mcp/controllers/wallet_controller.c) echo "zcl_balance" ;;
        *) echo "" ;;
    esac
}

# Parse eligible sources from the manifest.
ELIGIBLE=$(sed -n 's/^[[:space:]]*HOTSWAP_ELIGIBLE("\([^"]*\)").*/\1/p' "$MANIFEST")
if [ -z "$ELIGIBLE" ]; then
    echo "FAIL: no HOTSWAP_ELIGIBLE(...) entries parsed from $MANIFEST" >&2
    exit 1
fi

echo "== [sync] every eligible TU has a proof entry =="
sync_fail=0
for src in $ELIGIBLE; do
    if [ -z "$(probe_for_source "$src")" ]; then
        echo "FAIL: eligible TU '$src' has no probe route in hotswap_demo.sh" >&2
        sync_fail=1
    fi
done
[ "$sync_fail" -eq 0 ] || exit 1

echo "== [1/2] build dev binary =="
make --no-print-directory dev-bin

pass=0
fail=0
for src in $ELIGIBLE; do
    probe=$(probe_for_source "$src")
    echo "== [2/2] $src (probe $probe) =="

    SO=$(make --no-print-directory hotswap-so FILES="$src" | tail -1)
    if [ -z "$SO" ] || [ ! -f "$SO" ]; then
        echo "FAIL: hotswap-so did not produce a .so for $src" >&2
        fail=$((fail + 1))
        continue
    fi
    case "$SO" in
        /*) : ;;
        *)  SO="$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO")" ;;
    esac

    ARGS="{\"so_path\":\"$SO\"}"
    OUT=$("$BIN" -datadir="$DEMO_DATADIR" mcpcall zcl_agent_hotswap "$ARGS")

    if printf '%s' "$OUT" | grep -q '"ok"[[:space:]]*:[[:space:]]*true' \
       && printf '%s' "$OUT" | grep -q '"provider_id"[[:space:]]*:[[:space:]]*"mcp.routes"' \
       && printf '%s' "$OUT" | grep -q "\"$probe\"" \
       && printf '%s' "$OUT" | grep -Eq '"input_content_sha256"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"' \
       && printf '%s' "$OUT" | grep -Eq '"artifact_sha256"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"'; then
        echo "PASS: $src committed a v2 generation re-pointing $probe"
        pass=$((pass + 1))
    else
        echo "FAIL: $src did not commit a v2 generation exposing $probe" >&2
        echo "$OUT" >&2
        fail=$((fail + 1))
    fi
done

echo "== hotswap_demo: $pass passed, $fail failed over eligible TUs =="
[ "$fail" -eq 0 ] || exit 1
exit 0
