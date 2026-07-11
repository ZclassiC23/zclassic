#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Tier-1 hot-swap v2 smoke test — proves, WITHOUT mutating a running service
# and with only the stock C toolchain, that an allowlisted stateless MCP
# provider loads, validates, self-tests, and commits one atomic route snapshot.
#
# Steps:
#   1. build build/bin/zclassic23-dev (the -rdynamic dev binary),
#   2. build an immutable input-addressed generation .so from the real
#      allowlisted app controller,
#   3. in ONE short-lived process (`mcpcall`, the in-process dispatcher — not
#      the live node): register the real routes, dlopen the .so via the loader,
#      commit all of its routes atomically,
#   4. assert the v2 provenance and replacement report.
#
# Pure POSIX sh + coreutils; no third-party tools.
set -eu

cd "$(dirname "$0")/../.."

BIN=build/bin/zclassic23-dev
DEMO_SRC=tools/mcp/controllers/app_controller.c
DEMO_DATADIR=${HOME}/.zclassic-c23-dev

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

echo "== [3/4] validate + atomically commit (one short-lived process) =="
ARGS="{\"so_path\":\"$SO\"}"
OUT=$("$BIN" -datadir="$DEMO_DATADIR" mcpcall zcl_agent_hotswap "$ARGS")
echo "$OUT"

echo "== [4/4] assert v2 atomic generation evidence =="
if printf '%s' "$OUT" | grep -q '"ok"[[:space:]]*:[[:space:]]*true' \
   && printf '%s' "$OUT" | grep -q '"provider_id"[[:space:]]*:[[:space:]]*"mcp.routes"' \
   && printf '%s' "$OUT" | grep -q '"zcl_name_list"' \
   && printf '%s' "$OUT" | grep -Eq '"input_content_sha256"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"' \
   && printf '%s' "$OUT" | grep -Eq '"artifact_sha256"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"'; then
    echo "PASS: manifest v2 validated and the complete app route generation committed"
    exit 0
fi

echo "FAIL: expected a committed v2 generation with route and hash provenance" >&2
exit 1
