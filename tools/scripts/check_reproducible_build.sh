#!/usr/bin/env bash
# tools/scripts/check_reproducible_build.sh — build-twice byte-identity gate.
#
# The release tool (tools/release.sh) sets deterministic INPUTS (pinned
# SOURCE_DATE_EPOCH, -march=x86-64-v3, -Wl,--build-id=none, deterministic
# tarball) but its --verify only checks SHA3+GPG of an EXISTING archive — it
# never proves two builds of the same source produce byte-identical binaries.
# This gate closes that gap: it builds the zclassic23 binary TWICE, in two
# fully isolated temp build dirs, under the IDENTICAL release flag profile,
# and asserts the two artifacts are byte-for-byte identical.
#
# This is the project's reproducibility PROOF, not a smoke test: it must FAIL
# LOUD on any byte drift and point at the cause (embedded build path,
# __DATE__/__TIME__, nondeterministic link order, build-id, etc.).
#
# Usage:
#   tools/scripts/check_reproducible_build.sh        # build zclassic23 twice, compare
#   BINARY=zclassic-cli tools/scripts/check_reproducible_build.sh
#   KEEP=1 tools/scripts/check_reproducible_build.sh  # keep the two build dirs for forensics
#
# Design notes:
#   * Flag REUSE: sources tools/scripts/repro_build_vars.sh (the SAME file
#     tools/release.sh sources), so the gate tests the real release config,
#     not an approximation. The Makefile's default -march is x86-64-v3 when
#     ZCL_NATIVE is unset; we do NOT set ZCL_NATIVE, so the test reflects the
#     portable release baseline exactly.
#   * Isolation: each build gets its own BUILD_DIR (overriding the Makefile's
#     `BUILD_DIR = build` on the command line). Neither build sees the other's
#     object files, depfiles, or build_commit stamp. No `make clean` is needed
#     between them — the two temp dirs ARE the clean rooms.
#   * Hash: SHA3-256 (the project standard — release.sh attests SHA3-256). We
#     use openssl dgst -sha3-256 to match release.sh exactly, with a sha256sum
#     fallback for hosts whose openssl lacks SHA3. `cmp` is the authoritative
#     byte-identity verdict; the hash is for reporting/recording.
#   * Exit 0 = byte-identical (and prints the hash). Exit 1 = mismatch, with a
#     diagnosing diff (first differing offset + likely-cause hints).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# --- helpers -----------------------------------------------------------------
die()  { printf '\033[31m[repro] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m[repro]\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m[repro]\033[0m %s\n' "$*"; }

# --- config ------------------------------------------------------------------
BINARY="${BINARY:-zclassic23}"          # which Makefile binary target to compare
KEEP="${KEEP:-0}"                       # 1 = keep the two build dirs for forensics
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# --- prerequisites -----------------------------------------------------------
command -v make >/dev/null 2>&1 || die "make not found on PATH"
command -v git  >/dev/null 2>&1 || die "git not found on PATH"
command -v cmp  >/dev/null 2>&1 || die "cmp not found on PATH"

# SHA3-256 is the project standard (release.sh). Fall back to sha256sum on hosts
# whose openssl lacks SHA3 — the byte-identity verdict comes from `cmp`, so this
# only affects the reported hash algorithm.
hash_bin=""
hash_algo=""
if openssl dgst -sha3-256 /dev/null >/dev/null 2>&1; then
    hash_algo="SHA3-256"
    hash_bin() { openssl dgst -sha3-256 "$1" | awk '{print $NF}'; }
elif command -v sha256sum >/dev/null 2>&1; then
    hash_algo="SHA-256"
    hash_bin() { sha256sum "$1" | awk '{print $1}'; }
else
    die "neither 'openssl dgst -sha3-256' nor 'sha256sum' is available"
fi

# --- load the shared release flag profile ------------------------------------
# This is the single source of truth shared with tools/release.sh. Sourcing it
# exports SOURCE_DATE_EPOCH, REL_CFLAGS, REL_LDFLAGS. We pin SOURCE_DATE_EPOCH
# to the HEAD commit time (the release default) so BOTH builds see the same
# embedded-timestamp base — the test then reflects the real release config.
# shellcheck source=tools/scripts/repro_build_vars.sh
. "$SCRIPT_DIR/repro_build_vars.sh"

[ -n "$REL_CFLAGS" ]  || die "REL_CFLAGS resolved empty — repro_build_vars.sh failed"
[ -n "$REL_LDFLAGS" ] || die "REL_LDFLAGS resolved empty — repro_build_vars.sh failed"

info "Binary target : $BINARY"
info "Hash algorithm: $hash_algo"
info "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH (pinned across both builds)"
info "CFLAGS         : $REL_CFLAGS"
info "LDFLAGS        : $REL_LDFLAGS"

# --- two isolated build dirs -------------------------------------------------
WORK="$(mktemp -d -t zcl-repro.XXXXXX)"
BUILD_A="$WORK/build_a"
BUILD_B="$WORK/build_b"
BIN_A="$BUILD_A/bin/$BINARY"
BIN_B="$BUILD_B/bin/$BINARY"
trap 'rc=$?; if [ "$KEEP" = "1" ] && [ $rc -ne 0 ]; then info "KEEP=1: leaving build dirs at $WORK for forensics"; else rm -rf "$WORK"; fi' EXIT

build_once() {
    # build_once <label> <build_dir>
    local label="$1" bdir="$2"
    info "── Build $label: BUILD_DIR=$bdir ──"
    local t0 t1
    t0=$(date +%s)
    # BUILD_DIR on the command line overrides the Makefile's `BUILD_DIR = build`
    # wholesale, redirecting BIN_DIR + OBJ_DIR + the build_commit stamp into the
    # isolated temp dir. CFLAGS/LDFLAGS are the resolved release profile.
    if ! make -j"$JOBS" BUILD_DIR="$bdir" \
            CFLAGS="$REL_CFLAGS" LDFLAGS="$REL_LDFLAGS" \
            "$BINARY" >&2; then
        die "Build $label failed (see stderr above)"
    fi
    t1=$(date +%s)
    [ -f "$bdir/bin/$BINARY" ] || die "Build $label finished but $bdir/bin/$BINARY not found"
    info "Build $label done in $((t1 - t0))s ($(stat -c%s "$bdir/bin/$BINARY" 2>/dev/null || stat -f%z "$bdir/bin/$BINARY") bytes)"
}

build_once A "$BUILD_A"
build_once B "$BUILD_B"

# --- compare -----------------------------------------------------------------
HASH_A="$(hash_bin "$BIN_A")"
HASH_B="$(hash_bin "$BIN_B")"
SIZE_A=$(stat -c%s "$BIN_A" 2>/dev/null || stat -f%z "$BIN_A")
SIZE_B=$(stat -c%s "$BIN_B" 2>/dev/null || stat -f%z "$BIN_B")

info "Build A: $hash_algo=$HASH_A  size=$SIZE_A"
info "Build B: $hash_algo=$HASH_B  size=$SIZE_B"

if [ "$HASH_A" = "$HASH_B" ] && cmp -s "$BIN_A" "$BIN_B"; then
    ok "BYTE-IDENTICAL: two independent builds of '$BINARY' match exactly."
    echo ""
    echo "  $hash_algo : $HASH_A"
    echo "  size       : $SIZE_A bytes"
    echo "  This is the project's first build-twice reproducibility proof."
    exit 0
fi

# --- diagnose the mismatch ---------------------------------------------------
echo "" >&2
die_printf() { printf '\033[31m[repro] MISMATCH:\033[0m %s\n' "$*" >&2; }
die_printf "two builds of '$BINARY' are NOT byte-identical."
die_printf "  A $hash_algo=$HASH_A  size=$SIZE_A"
die_printf "  B $hash_algo=$HASH_B  size=$SIZE_B"

if [ "$SIZE_A" != "$SIZE_B" ]; then
    die_printf "sizes differ by $((SIZE_B - SIZE_A)) bytes — a different amount of content was embedded."
fi

# First differing byte offset. `cmp -n` with a large cap avoids reading huge
# files fully when the divergence is early; fall back to a full cmp.
FIRST_DIFF=""
if FIRST_DIFF="$(cmp -n 100000000 "$BIN_A" "$BIN_B" 2>&1 || true)"; then
    :
fi
if [ -z "$FIRST_DIFF" ] || ! printf '%s' "$FIRST_DIFF" | grep -qi 'differ\|byte'; then
    FIRST_DIFF="$(cmp "$BIN_A" "$BIN_B" 2>&1 || true)"
fi
die_printf "first divergence: $FIRST_DIFF"

# Dump the strings that most often carry nondeterminism, from BOTH binaries, so
# the cause is visible without a separate forensic pass. These are the usual
# suspects: absolute build paths, __DATE__/__TIME__ macros, build-id, random
# temp-dir names baked in by a configure step.
section() { printf '\n\033[33m[repro] —— %s ——\033[0m\n' "$1" >&2; }
dump_likely() {
    local f="$1" label="$2"
    section "$label: paths / dates / temp-dir leakage"
    strings "$f" \
        | grep -aE '/tmp/|/var/tmp/|\.XXXXXX|build_[ab]|/__DATE__/|__TIME__' \
        | sort -u | head -20 || true
    section "$label: embedded build-id / GNU ld version"
    strings "$f" \
        | grep -aE 'GNU[[:space:]].*ld|BuildID|build.id' \
        | sort -u | head -10 || true
}
dump_likely "$BIN_A" "Build A"
dump_likely "$BIN_B" "Build B"

cat >&2 <<HINTS

[repro] Likely-cause checklist (build config, NOT node code):
  1. Embedded absolute build path — a configure/cmake step baked \$PWD or a
     temp dir into a vendored .a (OpenSSL OPENSSLDIR, libevent, leveldb). Fix
     in tools/scripts/build_vendor.sh with a neutral --prefix.
  2. __DATE__ / __TIME__ macros in source — non-reproducible by construction;
     SOURCE_DATE_EPOCH can't reach them unless the compiler's -Wdate-time is
     honored and the macro replaced.
  3. Nondeterministic link order — LTO partitioning or an unstable glob in the
     link line. Sort the source list, or pin ARFLAGS=Dcr (build_vendor.sh
     already does for .a archives).
  4. build-id — should already be dropped by -Wl,--build-id=none in REL_LDFLAGS;
     if it appears, the flag did not reach the final link.
  5. generated inputs or toolchain drift outside the captured source inventory.
     Git commit ids are intentionally not embedded in the sovereign binary;
     GitHub trace metadata belongs in an external publication sidecar.

[repro] KEEP=1 retains the two build dirs under $WORK for byte-level forensics.
HINTS

if [ "$KEEP" = "1" ]; then
    info "KEEP=1: retaining build dirs at $WORK (remove manually when done)"
    trap - EXIT
fi

exit 1
