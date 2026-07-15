#!/usr/bin/env bash
# Gate: sandbox wired (HARD).
#
# The os_sandbox node steady-state profile is only a defense if boot actually
# ENTERS it. This gate asserts that config/src/boot.c both (a) registers a
# SYSINIT boundary record named "sandbox" and (b) calls os_sandbox_enter(),
# so the confinement wiring cannot silently regress to zero-sandbox while the
# -sandbox=steady flag still advertises confinement.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

SRC="config/src/boot.c"
[[ -f "$SRC" ]] || { echo "check_sandbox_wired: FATAL — missing $SRC" >&2; exit 2; }

fail=0

if ! grep -Eq '\.name[[:space:]]*=[[:space:]]*"sandbox"' "$SRC"; then
    echo "check_sandbox_wired: FAIL — no SYSINIT record named \"sandbox\" in $SRC" >&2
    fail=1
fi

if ! grep -Eq '\.stage[[:space:]]*=[[:space:]]*BOOT_STAGE_SERVICES_RUNNING.*"sandbox"|"sandbox".*BOOT_STAGE_SERVICES_RUNNING' "$SRC"; then
    echo "check_sandbox_wired: FAIL — the sandbox record is not on the SERVICES_RUNNING boundary in $SRC" >&2
    fail=1
fi

if ! grep -Eq '\bos_sandbox_enter[[:space:]]*\(' "$SRC"; then
    echo "check_sandbox_wired: FAIL — boot never calls os_sandbox_enter() in $SRC" >&2
    fail=1
fi

if (( fail )); then
    echo "check_sandbox_wired: the -sandbox=steady confinement wiring is missing or moved." >&2
    exit 1
fi

echo "[check_sandbox_wired] OK — boot registers the sandbox record and enters os_sandbox"
exit 0
