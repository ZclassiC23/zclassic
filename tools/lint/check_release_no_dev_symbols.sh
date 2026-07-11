#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_release_no_dev_symbols.sh — prove the native dev-lane activation engine
# is ABSENT from the release binary.
#
# The engine (tools/dev/dev_activation*.c) can stop/start the systemd unit,
# flip the `current` generation symlink, and exec `systemctl --user ...`. None
# of that belongs in the shipped node: it is a developer control-plane, not an
# operator surface. The engine is therefore compiled ONLY under a dev OR test
# build — the whole TU body sits inside
#     #if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)
# and the real process-exec ops sit inside a nested #ifdef ZCL_DEV_BUILD — so a
# release compile (neither macro defined) yields an empty object with none of
# these symbols.
#
# This gate is the regression guard. Two independent proofs:
#   (1) SOURCE: each FORBIDDEN entry point is defined inside a dev/test-only
#       preprocessor guard region (never in release-visible code).
#   (2) BINARY: if build/bin/zclassic23 exists, nm must NOT report any FORBIDDEN
#       symbol as a defined symbol. (Absence — however achieved, including a
#       stripped binary — satisfies the invariant; a positive find fails.)
#
# Exit: 0 PASS · 1 FAIL · 2 hollow-scan abort.
set -euo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

echo "══ LINT: release binary carries no dev-activation symbols ══"

# The forbidden entry points — the extern surface of the dev-lane activation
# engine. Extend this list when adding a new dev-only control-plane entry point.
FORBIDDEN=(
    dev_activation_run
    dev_activation_activate_generation
    dev_activation_default_ops
)

SRCS=(tools/dev/dev_activation.c tools/dev/dev_activation_ops.c)

# ── (1) SOURCE proof: every forbidden definition is under a dev/test guard ──
scanned_defs=0
for sym in "${FORBIDDEN[@]}"; do
    found_def=0
    for f in "${SRCS[@]}"; do
        [ -r "$f" ] || continue
        # awk tracks the preprocessor guard nesting; a definition of `sym` is
        # OK only when at least one enclosing #if/#ifdef requires ZCL_DEV_BUILD
        # or ZCL_TESTING (i.e. the line is compiled out of a release build).
        result="$(awk -v sym="$sym" '
            /^[[:space:]]*#[[:space:]]*if(n?def)?([^a-zA-Z0-9_]|$)/ {
                is_guard = ($0 ~ /ZCL_DEV_BUILD/ || $0 ~ /ZCL_TESTING/) ? 1 : 0
                depth++; guard[depth] = is_guard
                if (is_guard) open_guards++
                next
            }
            /^[[:space:]]*#[[:space:]]*endif/ {
                if (depth > 0) { if (guard[depth]) open_guards--; depth-- }
                next
            }
            $0 ~ ("^(int|void|bool)[[:space:]]+" sym "[[:space:]]*\\(") {
                if (open_guards > 0) print "GUARDED"
                else print "UNGUARDED:" NR
            }
        ' "$f")"
        if printf '%s\n' "$result" | grep -q '^UNGUARDED:'; then
            line="$(printf '%s\n' "$result" | sed -n 's/^UNGUARDED://p' | head -1)"
            echo "FAIL: $sym is defined OUTSIDE a dev/test guard in $f:$line"
            echo "  release compiles would export this symbol — wrap it in"
            echo "  '#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)'."
            exit 1
        fi
        if printf '%s\n' "$result" | grep -q '^GUARDED'; then
            found_def=1
            scanned_defs=$((scanned_defs + 1))
        fi
    done
    if [ "$found_def" -eq 0 ]; then
        echo "FAIL: forbidden entry point '$sym' has no definition in ${SRCS[*]}"
        echo "  the gate's FORBIDDEN list drifted from the engine — update one."
        exit 1
    fi
done
gate_require_scanned "$scanned_defs" "${#FORBIDDEN[@]}" check_release_no_dev_symbols \
    "no guarded dev-activation definitions found — were the engine files moved?"
echo "  OK: all ${#FORBIDDEN[@]} entry points are dev/test-guarded in source"

# ── (2) BINARY proof (best-effort): nm the release artifact if present ──
BIN="${ZCL_RELEASE_NO_DEV_SYMBOLS_BIN:-build/bin/zclassic23}"
if [ -r "$BIN" ] && command -v nm >/dev/null 2>&1; then
    # A defined symbol in nm is any line whose type is not 'U' (undefined).
    hits="$(nm "$BIN" 2>/dev/null | awk '$2 != "U" {print $3}' \
        | grep -Ewf <(printf '%s\n' "${FORBIDDEN[@]}") || true)"
    if [ -n "$hits" ]; then
        echo "FAIL: release binary $BIN defines dev-activation symbol(s):"
        printf '  %s\n' $hits
        exit 1
    fi
    echo "  OK: release binary $BIN defines none of the forbidden symbols"
else
    echo "  NOTE: $BIN absent (or nm missing) — source proof stands alone"
fi

echo "=== check-release-no-dev-symbols: PASSED ==="
exit 0
