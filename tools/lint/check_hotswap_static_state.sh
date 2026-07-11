#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_hotswap_static_state.sh — every TU in the Tier-1 hot-swap eligibility
# manifest (config/hotswap_eligible.def) must define NO mutable file-scope
# statics. A generation .so recompiles the whole TU, so any mutable file-scope
# static becomes a fresh zero-initialized copy inside the .so — the live
# process state (registered routes, boot-populated main_state, atomic provider
# slots) is lost. Resident state MUST live in a sibling non-eligible trampoline
# TU. This gate greps each eligible TU for the offending pattern.
#
# Heuristic (documented as acceptable): a file-scope line matching `^static`,
# NOT `const`, NOT a function declarator (no `(`), that either carries an
# initializer (`=`), declares an array (`[`), or opens an aggregate (ends `{`).
# A provably swap-safe static may carry a same-line escape comment:
#   hotswap-static-ok: <reason>
#
# Manifest path is overridable via ZCL_HOTSWAP_MANIFEST so the lint-gate
# self-test can point it at a seeded-violation manifest.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

MANIFEST="${ZCL_HOTSWAP_MANIFEST:-config/hotswap_eligible.def}"

echo "══ LINT: hot-swap eligible TUs hold no mutable file-scope statics ══"

if [ ! -r "$MANIFEST" ]; then
    echo "check_hotswap_static_state: FATAL — manifest '$MANIFEST' missing/unreadable." >&2
    echo "  Refusing to report 'clean' with no manifest to scan." >&2
    exit 2
fi

mapfile -t PATHS < <(gate_grep -oE 'HOTSWAP_ELIGIBLE\("[^"]+"\)' "$MANIFEST" \
    | sed -E 's/HOTSWAP_ELIGIBLE\("([^"]+)"\)/\1/')

gate_require_scanned "${#PATHS[@]}" 1 check_hotswap_static_state \
    "no HOTSWAP_ELIGIBLE(\"...\") entries parsed from $MANIFEST"

# awk detector for one file: flag mutable file-scope statics per the heuristic.
detect_statics() {
    awk '
        /hotswap-static-ok:/ { next }         # explicit allowlist escape
        /^static[ \t]/ {
            line = $0
            if (line ~ /\<const\>/)   next     # immutable
            if (line ~ /\(/)          next     # function declarator/prototype
            if (line ~ /=/ || line ~ /\[/ || line ~ /\{[ \t]*$/)
                printf "%s:%d: %s\n", FILENAME, FNR, line
        }
    ' "$1"
}

violations=""
scanned=0
for p in "${PATHS[@]}"; do
    if [ ! -f "$p" ]; then
        echo "check_hotswap_static_state: FATAL — eligible TU '$p' does not exist." >&2
        echo "  Manifest drifted; refusing to pass off an unscannable file." >&2
        exit 2
    fi
    scanned=$((scanned + 1))
    hits="$(detect_statics "$p")"
    if [ -n "$hits" ]; then
        violations="${violations}${hits}"$'\n'
    fi
done

gate_require_scanned "$scanned" 1 check_hotswap_static_state "no eligible TU scanned"

if [ -n "${violations//[[:space:]]/}" ]; then
    printf '%s' "$violations"
    echo "FAIL: a hot-swap eligible TU defines a mutable file-scope static."
    echo "  Move it to a sibling NON-eligible resident trampoline TU (a .so"
    echo "  gets its own zero copy), or, if provably swap-safe, annotate the"
    echo "  declaration line with a comment reading  hotswap-static-ok: <reason>."
    exit 1
fi

echo "  OK: $scanned eligible TU(s) free of mutable file-scope statics"
exit 0
