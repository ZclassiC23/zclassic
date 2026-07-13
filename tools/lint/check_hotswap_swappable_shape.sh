#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_hotswap_swappable_shape.sh — THE HARD LINE for the REAL (activatable)
# Tier-1 hot-swap module ABI. Every handler on the swappable allowlist
# (config/hotswap_swappable.def) must be owned by a shape-LEAF translation unit:
# a controller, view, or condition. It may NEVER resolve under a reducer stage,
# consensus validation, the storage engine, a supervisor, or any state root —
# a dlopen'd module of any of those could silently diverge the node's consensus
# state or the reducer fold. This gate refuses any swappable source_tu that is
# not under an allowed shape folder, or that sits under a forbidden root.
#
# Manifest path is overridable via ZCL_HOTSWAP_SWAPPABLE_MANIFEST so the
# lint-gate self-test can point it at a seeded-violation fixture.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

MANIFEST="${ZCL_HOTSWAP_SWAPPABLE_MANIFEST:-config/hotswap_swappable.def}"

echo "══ LINT: hot-swap swappable allowlist shape (controllers/views/conditions only) ══"

if [ ! -r "$MANIFEST" ]; then
    echo "check_hotswap_swappable_shape: FATAL — manifest '$MANIFEST' missing/unreadable." >&2
    echo "  Refusing to report 'clean' with no manifest to scan." >&2
    exit 2
fi

# Extract (handler, source) from each HOTSWAP_SWAPPABLE("h","s") invocation.
mapfile -t PAIRS < <(sed -n \
    's/^[[:space:]]*HOTSWAP_SWAPPABLE("\([^"]*\)"[[:space:]]*,[[:space:]]*"\([^"]*\)").*/\1\t\2/p' \
    "$MANIFEST")

# Fail-loud: a manifest that parses to zero entries means the format drifted.
gate_require_scanned "${#PAIRS[@]}" 1 check_hotswap_swappable_shape \
    "no HOTSWAP_SWAPPABLE(\"h\",\"s\") entries parsed from $MANIFEST"

# A shape LEAF the module ABI may re-point.
ALLOWED='^(app/controllers|app/views|app/conditions)/'
# Belt-and-suspenders: never under any of these, even if mislabeled a shape.
FORBIDDEN='^(core|lib/consensus|lib/validation|lib/storage|lib/net|lib/coins|lib/chain|lib/mining|app/jobs|lib/kernel|lib/supervisor|app/supervisors|domain/consensus)/'

violations=""
declare -A seen_handlers=()
for pair in "${PAIRS[@]}"; do
    h="${pair%%$'\t'*}"
    s="${pair#*$'\t'}"
    case "$h" in
        ""|*[!A-Za-z0-9_.]*)
            violations="${violations}  $h (invalid handler name)"$'\n' ;;
    esac
    if [ -n "${seen_handlers[$h]:-}" ]; then
        violations="${violations}  $h (duplicate swappable row)"$'\n'
    fi
    seen_handlers[$h]=1
    if printf '%s\n' "$s" | grep -qE "$FORBIDDEN"; then
        violations="${violations}  $h -> $s (under a forbidden consensus/state/supervisor root)"$'\n'
        continue
    fi
    if ! printf '%s\n' "$s" | grep -qE "$ALLOWED"; then
        violations="${violations}  $h -> $s (not under an allowed shape-leaf folder: app/controllers, app/views, app/conditions)"$'\n'
        continue
    fi
    case "$s" in
        *.c) ;;
        *) violations="${violations}  $h -> $s (not a .c translation unit)"$'\n'; continue ;;
    esac
    if [ ! -f "$s" ]; then
        violations="${violations}  $h -> $s (manifest references a nonexistent file)"$'\n'
    fi
done

if [ -n "${violations//[[:space:]]/}" ]; then
    printf '%s' "$violations"
    echo "FAIL: hot-swap swappable allowlist lists an out-of-shape handler."
    echo "  Every swappable source_tu must be a controller/view/condition LEAF,"
    echo "  NEVER a reducer stage, consensus validation, storage engine, or"
    echo "  supervisor. This is the hard line that makes activation safe."
    exit 1
fi

echo "  OK: ${#PAIRS[@]} swappable handler(s), all shape-leaf surfaces"
exit 0
