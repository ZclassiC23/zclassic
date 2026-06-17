#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# ci_symbol_floor_gate.sh — the hermetic MVP C1 portability-floor gate.
#
# MVP criterion #1 ("single-binary install on clean Ubuntu/Debian") rests on
# the headline claim that the ~15 MB binary is self-contained — it runs on a
# clean OS with no extra install. That is true only up to a SYMBOL-VERSION
# FLOOR: the binary dynamically links libc, libstdc++ and libgcc_s, so its real
# floor is a TRIPLE, not glibc alone:
#
#     GLIBC   <= 2.38      (libc.so.6)
#     GLIBCXX <= 3.4.30    (libstdc++.so.6 — the C++ runtime, e.g. LevelDB)
#     CXXABI  <= 1.3.9     (libstdc++.so.6 — the C++ ABI)
#
# A clean Ubuntu 24.04 (glibc 2.39 / GLIBCXX 3.4.33) satisfies all three; an
# Ubuntu 20.04 (GLIBCXX 3.4.28) would NOT. This gate is the regression guard:
# it inspects the ALREADY-BUILT artifact with `objdump -T` + `ldd` only (no
# node, no network, no params, no docker, no wall-clock — pure static
# inspection), so unlike ci-install / ci-install-container it CAN live inside
# hermetic `make ci`. A future change that pulls a NEWER libstdc++/glibc symbol
# (silently raising the floor and breaking clean-OS portability) fails the build
# LOUD instead of rotting until a user on an older distro hits it.
#
# Raising a floor below is a DELIBERATE portability decision (it drops support
# for older distros) — change the constants here on purpose, never to silence a
# red.  Override the inspected artifact with ZCL_SYMBOL_FLOOR_BIN (cross-build).
#
# Exit: 0 PASS · 2 SKIP (objdump/ldd absent) · 1 FAIL (a floor exceeded / unmet dep).

set -uo pipefail

GATE_SRC="${BASH_SOURCE[0]}"
GATE_DIR="$(cd "$(dirname "$GATE_SRC")" && pwd)"
REPO_ROOT="$(cd "$GATE_DIR/../.." && pwd)"

gate_skip() { echo "ci-symbol-floor: SKIP ($*)"; exit 2; }
gate_die()  { echo "ci-symbol-floor: FAIL: $*" >&2; exit 1; }

BIN="${ZCL_SYMBOL_FLOOR_BIN:-$REPO_ROOT/build/bin/zclassic23}"
[ -r "$BIN" ] || gate_die "binary not found/readable: $BIN"

command -v objdump >/dev/null 2>&1 || gate_skip "objdump absent (install binutils to run this gate)"
command -v ldd     >/dev/null 2>&1 || gate_skip "ldd absent"

# ── The documented portability floor (raise only on purpose) ────────
FLOOR_GLIBC="GLIBC_2.38"
FLOOR_GLIBCXX="GLIBCXX_3.4.30"
FLOOR_CXXABI="CXXABI_1.3.9"

# ── (1) Every shared-lib dependency must resolve ────────────────────
# A "=> not found" means the binary cannot start on this host at all.
missing="$(ldd "$BIN" 2>/dev/null | grep -c 'not found' || true)"
[ "${missing:-0}" -eq 0 ] || gate_die "$missing shared-lib dependency(ies) unresolved (ldd '=> not found')"

# ── (2) Max required symbol version per family <= documented floor ──
# `family_max FAMILY_` prints the highest FAMILY_x.y.z token the binary
# requires (version-sorted), or empty if the family is unused.
family_max() {
    objdump -T "$BIN" 2>/dev/null | grep -oE "$1[0-9.]+" | sort -V | tail -1
}

# `le A B` is true iff version A <= version B (same family prefix), via sort -V.
le() {
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]
}

check_family() {
    local label="$1" prefix="$2" floor="$3"
    local max
    max="$(family_max "$prefix")"
    if [ -z "$max" ]; then
        echo "ci-symbol-floor: $label not required by the binary (OK)"
        return 0
    fi
    if le "$max" "$floor"; then
        echo "ci-symbol-floor: $label max required = $max  <= floor $floor  (OK)"
        return 0
    fi
    gate_die "$label max required = $max EXCEEDS documented floor $floor — this raises the clean-OS portability floor; either lower the build's symbol use or deliberately bump the floor in $GATE_SRC"
}

# A dynamically-linked ELF MUST require at least one GLIBC symbol. An empty
# GLIBC family means objdump could not parse the artifact (non-ELF / corrupt /
# a mis-pointed ZCL_SYMBOL_FLOOR_BIN) — refuse to PASS a garbage binary, which
# would otherwise sail through with every family reporting "not required".
[ -n "$(family_max "GLIBC_")" ] || gate_die "no GLIBC symbols found in $BIN — not a parseable dynamically-linked ELF (refusing to PASS a garbage artifact)"

check_family "GLIBC"   "GLIBC_"   "$FLOOR_GLIBC"
check_family "GLIBCXX" "GLIBCXX_" "$FLOOR_GLIBCXX"
check_family "CXXABI"  "CXXABI_"  "$FLOOR_CXXABI"

echo "=== ci-symbol-floor: PASSED (triple floor $FLOOR_GLIBC / $FLOOR_GLIBCXX / $FLOOR_CXXABI honored; all deps resolve) ==="
exit 0
