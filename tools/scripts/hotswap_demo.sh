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
# Every eligible TU carries its proof route on the same manifest row, so the
# harness, planners, and live transport cannot maintain divergent side tables.
#
# Pure POSIX sh + coreutils; no third-party tools.
set -eu

cd "$(dirname "$0")/../.."

BIN=build/bin/zclassic23-dev
DEMO_DATADIR=${HOME}/.zclassic-c23-dev
MANIFEST=config/hotswap_eligible.def

# Parse the canonical source:probe pairs from the manifest. Colons are not
# legal in either repo-relative source paths or MCP tool names.
ELIGIBLE=$(sed -n \
    's/^[[:space:]]*HOTSWAP_ELIGIBLE("\([^"]*\)")[[:space:]]*HOTSWAP_PROBE("\([^"]*\)").*/\1:\2/p' \
    "$MANIFEST")
if [ -z "$ELIGIBLE" ]; then
    echo "FAIL: no HOTSWAP_ELIGIBLE/HOTSWAP_PROBE pairs parsed from $MANIFEST" >&2
    exit 1
fi

echo "== [sync] every eligible TU has one canonical proof route =="

echo "== [1/2] build dev binary =="
make --no-print-directory dev-bin

pass=0
fail=0
for entry in $ELIGIBLE; do
    src=${entry%%:*}
    probe=${entry#*:}
    if [ -z "$src" ] || [ -z "$probe" ] || [ "$src" = "$probe" ]; then
        echo "FAIL: malformed eligible source/probe pair '$entry'" >&2
        fail=$((fail + 1))
        continue
    fi
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
