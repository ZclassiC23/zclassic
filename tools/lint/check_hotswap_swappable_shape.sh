#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_hotswap_swappable_shape.sh — THE HARD LINE for the REAL (activatable)
# Tier-1 hot-swap module ABI. config/hotswap_swappable.def carries ONE row per
# swappable source TU plus the space-separated set of command leaves that TU's
# module may re-point in a single all-or-nothing batch. This gate enforces both
# halves of the line:
#
#   SHAPE  Every source_tu must be a shape-LEAF translation unit: a controller,
#          view, or condition. It may NEVER resolve under a reducer stage,
#          consensus validation, the storage engine, a supervisor, or any state
#          root — a dlopen'd module of any of those could silently diverge the
#          node's consensus state or the reducer fold.
#
#   LEAF   Every leaf must be declared with ZCL_COMMAND_READY_READ in
#          config/commands/*.def — the READY, read-only, effect=EFFECT_READ
#          macro form. A leaf declared with any COMMAND/PLANNED/COMPAT/DEV form
#          (which can carry EFFECT_MUTATE or a non-READY availability) is
#          refused here, BEFORE it can reach the runtime. A leaf may be claimed
#          by exactly ONE source file, which is what makes a duplicate leaf
#          across two modules unrepresentable.
#
# Before the def grew a leaf column this gate checked shape FOLDER roots only,
# and the READY/read-only property was asserted nowhere but at runtime — a
# silent hole the moment the allowlist widened past the six files that happened
# to match config/hotswap_eligible.def.
#
# Build-free text parse (same paren-depth, string-literal-aware walk as
# tools/lint/check_privileged_transition_receipt.sh): macro invocations are
# recognized only at COLUMN 1, so the prose in each .def's header comment (which
# spells the macro signature out) is never mistaken for a row.
#
# Manifest and command-catalog paths are overridable via
# ZCL_HOTSWAP_SWAPPABLE_MANIFEST / ZCL_HOTSWAP_COMMAND_DEF_DIR so the lint-gate
# self-test can point them at seeded-violation fixtures.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

MANIFEST="${ZCL_HOTSWAP_SWAPPABLE_MANIFEST:-config/hotswap_swappable.def}"
DEFDIR="${ZCL_HOTSWAP_COMMAND_DEF_DIR:-config/commands}"

echo "══ LINT: hot-swap swappable allowlist (shape leaf + READY read-only) ══"

if [ ! -r "$MANIFEST" ]; then
    echo "check_hotswap_swappable_shape: FATAL — manifest '$MANIFEST' missing/unreadable." >&2
    echo "  Refusing to report 'clean' with no manifest to scan." >&2
    exit 2
fi

# Walk every COLUMN-1 invocation of a macro by paren depth (string-literal
# aware) and print its first ARGN string literals, tab separated. TOK is the
# macro token including its '('.
MACRO_AWK='
{ buf = buf $0 "\n" }
END {
    n = length(buf); L = length(TOK)
    i = 1
    while (i <= n) {
        if (substr(buf, i, L) != TOK || (i > 1 && substr(buf, i - 1, 1) != "\n")) {
            i++; continue
        }
        j = i + L; depth = 1; in_str = 0; esc = 0; spec = ""
        while (j <= n && depth > 0) {
            c = substr(buf, j, 1)
            if (in_str) {
                if (esc) { esc = 0 }
                else if (c == "\\") { esc = 1 }
                else if (c == "\"") { in_str = 0 }
            } else {
                if (c == "\"") { in_str = 1 }
                else if (c == "(") { depth++ }
                else if (c == ")") { depth-- }
            }
            if (depth > 0) spec = spec c
            j++
        }
        out = ""; rest = spec
        for (k = 0; k < ARGN; k++) {
            v = ""
            if (match(rest, /"[^"]*"/)) {
                v = substr(rest, RSTART + 1, RLENGTH - 2)
                rest = substr(rest, RSTART + RLENGTH)
            }
            out = out (k ? "\t" : "") v
        }
        print out
        i = j
    }
}'

mapfile -t ROWS < <(awk -v TOK='HOTSWAP_SWAPPABLE(' -v ARGN=2 "$MACRO_AWK" "$MANIFEST")

# Fail-loud: a manifest that parses to zero entries means the format drifted.
gate_require_scanned "${#ROWS[@]}" 1 check_hotswap_swappable_shape \
    "no HOTSWAP_SWAPPABLE(\"source\",\"leaves\") rows parsed from $MANIFEST"

# ── Enumerate every READY read-only leaf from the command catalog ──────────
declare -A ready_read=()
def_count=0
ready_count=0
if [ -d "$DEFDIR" ]; then
    shopt -s nullglob
    for f in "$DEFDIR"/*.def; do
        def_count=$((def_count + 1))
        while IFS= read -r leaf; do
            [ -n "$leaf" ] || continue
            ready_read["$leaf"]=1
            ready_count=$((ready_count + 1))
        done < <(awk -v TOK='ZCL_COMMAND_READY_READ(' -v ARGN=1 "$MACRO_AWK" "$f")
    done
    shopt -u nullglob
fi
if [ "$def_count" -eq 0 ] || [ "$ready_count" -eq 0 ]; then
    echo "check_hotswap_swappable_shape: FATAL — no ZCL_COMMAND_READY_READ leaves enumerated from $DEFDIR/*.def." >&2
    echo "  The command-catalog scan is hollow; refusing to certify any leaf as" >&2
    echo "  READY read-only off an empty enumeration." >&2
    exit 2
fi

# A shape LEAF the module ABI may re-point.
ALLOWED='^(app/controllers|app/views|app/conditions)/'
# Belt-and-suspenders: never under any of these, even if mislabeled a shape.
FORBIDDEN='^(core|lib/consensus|lib/validation|lib/storage|lib/net|lib/coins|lib/chain|lib/mining|app/jobs|lib/kernel|lib/supervisor|app/supervisors|domain/consensus)/'

# Pre-pass: realized leaf population across every row. Asserted against a floor
# BEFORE the violation walk, so a manifest whose leaf column silently emptied is
# a loud exit 2 rather than a clean pass — and so a row rejected early (e.g. a
# forbidden source root) still contributes to the scan count.
leaf_total=0
for row in "${ROWS[@]}"; do
    row_leaves="${row#*$'\t'}"
    [ "$row_leaves" = "$row" ] && continue
    for _h in $row_leaves; do
        leaf_total=$((leaf_total + 1))
    done
done
gate_require_scanned "$leaf_total" 1 check_hotswap_swappable_shape \
    "no swappable leaves parsed out of ${#ROWS[@]} row(s) in $MANIFEST"

violations=""
declare -A seen_sources=()
declare -A leaf_owner=()
for row in "${ROWS[@]}"; do
    s="${row%%$'\t'*}"
    leaves="${row#*$'\t'}"
    [ "$leaves" = "$row" ] && leaves=""

    if [ -z "$s" ]; then
        violations="${violations}  (row with no source_tu string literal)"$'\n'
        continue
    fi
    if [ -n "${seen_sources[$s]:-}" ]; then
        violations="${violations}  $s (duplicate source row; one row per file)"$'\n'
    fi
    seen_sources[$s]=1

    if printf '%s\n' "$s" | grep -qE "$FORBIDDEN"; then
        violations="${violations}  $s (under a forbidden consensus/state/supervisor root)"$'\n'
        continue
    fi
    if ! printf '%s\n' "$s" | grep -qE "$ALLOWED"; then
        violations="${violations}  $s (not under an allowed shape-leaf folder: app/controllers, app/views, app/conditions)"$'\n'
        continue
    fi
    case "$s" in
        *.c) ;;
        *) violations="${violations}  $s (not a .c translation unit)"$'\n'; continue ;;
    esac
    if [ ! -f "$s" ]; then
        violations="${violations}  $s (manifest references a nonexistent file)"$'\n'
        continue
    fi
    if [ -z "${leaves//[[:space:]]/}" ]; then
        violations="${violations}  $s (row declares no swappable leaves)"$'\n'
        continue
    fi

    for h in $leaves; do
        case "$h" in
            *[!A-Za-z0-9_.]*)
                violations="${violations}  $s -> $h (invalid leaf name)"$'\n'
                continue ;;
        esac
        if [ -n "${leaf_owner[$h]:-}" ]; then
            violations="${violations}  $h (claimed by both ${leaf_owner[$h]} and $s; a leaf belongs to exactly one file)"$'\n'
            continue
        fi
        leaf_owner[$h]="$s"
        if [ -z "${ready_read[$h]:-}" ]; then
            violations="${violations}  $s -> $h (not declared with ZCL_COMMAND_READY_READ in $DEFDIR/*.def — swappable leaves must be READY and read-only)"$'\n'
        fi
    done
done

if [ -n "${violations//[[:space:]]/}" ]; then
    printf '%s' "$violations"
    echo "FAIL: the hot-swap swappable allowlist violates the hard line."
    echo "  Every source_tu must be a controller/view/condition LEAF, NEVER a"
    echo "  reducer stage, consensus validation, storage engine, or supervisor."
    echo "  Every leaf must be declared ZCL_COMMAND_READY_READ (READY +"
    echo "  read-only) in $DEFDIR/*.def and be owned by exactly one file."
    echo "  This is what makes activation safe."
    exit 1
fi

echo "  OK: ${#ROWS[@]} swappable file(s), $leaf_total READY read-only shape leaf/leaves"
echo "      (cross-checked against $ready_count ZCL_COMMAND_READY_READ leaves in $def_count catalog file(s))"
exit 0
