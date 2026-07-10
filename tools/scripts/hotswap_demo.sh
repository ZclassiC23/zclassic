#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Tier-1 hot-swap end-to-end demo — proves, WITHOUT any running node and with
# nothing but the stock C toolchain, that an edited MCP handler goes live via a
# dlopen'd generation .so.
#
# Steps:
#   1. build build/bin/zclassic23-dev (the -rdynamic dev binary),
#   2. build a generation .so from the demo controller (a re-pointed
#      zcl_name_list whose new handler returns a marker string),
#   3. in ONE short-lived process (`mcpcall`, the in-process dispatcher — not
#      the live node): register the real routes, dlopen the .so via the loader,
#      re-point the slot, and probe zcl_name_list,
#   4. assert the probe result carries the new handler's marker.
#
# Pure POSIX sh + coreutils; no third-party tools.
set -eu

cd "$(dirname "$0")/../.."

BIN=build/bin/zclassic23-dev
DEMO_SRC=tools/scripts/hotswap_demo_controller.c
DEMO_DATADIR=/tmp/zcl-hotswap-demo   # a dev datadir (loader refuses the live one)
MARKER=NEW-GENERATION-HANDLER

echo "== [1/4] build dev binary =="
make --no-print-directory dev-bin

echo "== [2/4] build generation .so from $DEMO_SRC =="
SO=$(make --no-print-directory hotswap-so FILES="$DEMO_SRC" | tail -1)
if [ -z "$SO" ] || [ ! -f "$SO" ]; then
    echo "FAIL: hotswap-so did not produce a .so" >&2
    exit 1
fi
# The loader requires an absolute path; make it so (pure POSIX).
case "$SO" in
    /*) : ;;
    *)  SO="$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO")" ;;
esac
echo "gen .so: $SO"

echo "== [3/4] dlopen + re-point + probe (one in-process dispatch, no node) =="
ARGS="{\"so_path\":\"$SO\",\"probe_tool\":\"zcl_name_list\"}"
OUT=$("$BIN" -datadir="$DEMO_DATADIR" mcpcall zcl_agent_hotswap "$ARGS")
echo "$OUT"

echo "== [4/4] assert the swapped-in handler ran =="
# The load report must show the route was replaced, and the embedded probe
# must carry the new handler's marker.
if printf '%s' "$OUT" | grep -q "\"replaced\":\[\"zcl_name_list\"\]" \
   && printf '%s' "$OUT" | grep -q "$MARKER"; then
    echo "PASS: zcl_name_list dispatch returned the new generation handler ($MARKER)"
    exit 0
fi

echo "FAIL: expected replaced=[zcl_name_list] and marker '$MARKER' in output" >&2
exit 1
