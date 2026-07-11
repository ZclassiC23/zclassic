#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_release_no_dev_symbols.sh — prove the RELEASE binary contains none of
# the dev-only mutation entry points. The dev command dispatcher, the
# hot-swap/reload cycle, the persistent watcher, and the subprocess runner live
# in DEV_ONLY_SRCS (Makefile) and are compiled ONLY into the DEV binary; the
# release binary must link none of them. See
# docs/NATIVE_COMMAND_INTERFACE.md §20 ("Release builds contain no dev mutation
# or loader command path").
#
# Two layers, so the gate is authoritative from source AND from the artifact:
#   1. STRUCTURAL (always): main.c guards the devloop dispatch under
#      ZCL_DEV_BUILD, and the Makefile excludes the executors from ALL_SRCS.
#      These two facts guarantee absence in any fresh release build.
#   2. ARTIFACT (when a fresh release binary exists): `nm -D` must not export
#      any forbidden dev-executor symbol. The release binary is stripped, but
#      -rdynamic keeps extern symbols in .dynsym, so nm -D is definitive.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

BIN="${1:-build/bin/zclassic23}"

# The extern dev-mutation entry points that must never reach the release binary.
FORBIDDEN=(
    zcl_devloop_cli_main
    zcl_devloop_is_method
    zcl_devloop_run_cycle
    zcl_devloop_run_sim
    zcl_devloop_watch
    zcl_devloop_process_run
    zcl_devloop_print_status
)

echo "══ LINT: release binary contains no dev-only mutation symbols ══"

rc=0

# ── layer 1: structural (source-level, always runs) ──────────────────────
# main.c must guard the devloop dispatch under ZCL_DEV_BUILD.
if ! gate_grep -qE '#ifdef[[:space:]]+ZCL_DEV_BUILD' src/main.c >/dev/null; then
    echo "FAIL: src/main.c has no ZCL_DEV_BUILD guard at all" >&2
    rc=1
fi
guard_block="$(awk '
    /#ifdef[[:space:]]+ZCL_DEV_BUILD/ { depth++; inblk=1 }
    inblk { print }
    /#endif/ { if (depth>0) depth-- ; if (depth==0) inblk=0 }
' src/main.c)"
if ! printf '%s' "$guard_block" | gate_grep -q 'zcl_devloop_cli_main' >/dev/null; then
    echo "FAIL: the zcl_devloop_cli_main dispatch in src/main.c is not inside a" >&2
    echo "      #ifdef ZCL_DEV_BUILD block — it would link into the release binary" >&2
    rc=1
fi

# Makefile must place the executors in DEV_ONLY_SRCS and exclude them from
# ALL_SRCS (via the filter-out into DEVLOOP_SRCS).
mk_dev_only="$(gate_grep -A2 '^DEV_ONLY_SRCS' Makefile || true)"
mk_filter="$(gate_grep -E '^DEVLOOP_SRCS[[:space:]]*=.*filter-out.*DEV_ONLY_SRCS' Makefile || true)"
scanned=0
for f in devloop_cli.c devloop_cycle.c devloop_watch.c devloop_process.c; do
    scanned=$((scanned + 1))
    if ! printf '%s' "$mk_dev_only" | gate_grep -q "tools/dev/$f" >/dev/null; then
        echo "FAIL: tools/dev/$f is not in the Makefile DEV_ONLY_SRCS group" >&2
        rc=1
    fi
done
gate_require_scanned "$scanned" 4 "check-release-no-dev-symbols" \
    "the DEV_ONLY_SRCS executor list changed shape unexpectedly"
if [ -z "$mk_filter" ]; then
    echo "FAIL: DEVLOOP_SRCS must filter-out DEV_ONLY_SRCS so the release ALL_SRCS excludes them" >&2
    rc=1
fi

# ── layer 2: artifact (nm on a FRESH release binary, when present) ────────
if [ -x "$BIN" ] && [ "$BIN" -nt src/main.c ] && [ "$BIN" -nt Makefile ]; then
    syms="$(nm -D --defined-only "$BIN" 2>/dev/null | awk '{print $NF}')"
    if [ -z "$syms" ]; then
        echo "NOTE: nm -D exported no symbols from $BIN (unexpected for -rdynamic);" >&2
        echo "      relying on the structural proof above." >&2
    else
        for sym in "${FORBIDDEN[@]}"; do
            if printf '%s\n' "$syms" | gate_grep -qx "$sym" >/dev/null; then
                echo "FAIL: release binary $BIN exports dev-only symbol '$sym'" >&2
                rc=1
            fi
        done
        [ "$rc" -eq 0 ] && echo "  nm -D $BIN: all ${#FORBIDDEN[@]} dev-executor symbols ABSENT ✓"
    fi
else
    echo "NOTE: no fresh release binary at $BIN — structural proof above is authoritative."
    echo "      (build it with 'make zclassic23' for the artifact-level nm proof.)"
fi

if [ "$rc" -ne 0 ]; then
    echo "check-release-no-dev-symbols: FAIL" >&2
    exit 1
fi
echo "check-release-no-dev-symbols: OK"
exit 0
