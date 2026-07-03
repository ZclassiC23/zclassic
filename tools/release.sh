#!/usr/bin/env bash
# tools/release.sh — Build a tagged release of zclassic23.
#
# Creates:
#   build/release/zclassic23-v{VERSION}-linux-x86_64.tar.gz
#   build/release/zclassic23-v{VERSION}-linux-x86_64.sha3
#   build/release/zclassic23-v{VERSION}-linux-x86_64.sha3.sig  (if GPG key available)
#
# Usage:
#   ./tools/release.sh              # auto-detect version from clientversion.h
#   ./tools/release.sh v0.1.0       # explicit tag
#   ./tools/release.sh --verify     # verify an existing release archive
#   ./tools/release.sh --unsigned   # allow an unsigned release (else hard-fail)
#
# By default a release MUST be GPG-signed: if no secret key is available the
# script aborts. Pass --unsigned (or set ZCL_ALLOW_UNSIGNED=1) to override.
#
# Reproducible: records compiler, build flags (CFLAGS/LDFLAGS), git rev, and
# uname in BUILDINFO.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ---------- helpers ----------------------------------------------------------

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ---------- version detection ------------------------------------------------

VERSION_H="lib/util/include/util/clientversion.h"
[ -f "$VERSION_H" ] || die "Cannot find $VERSION_H"

V_MAJOR=$(grep '#define CLIENT_VERSION_MAJOR'    "$VERSION_H" | awk '{print $3}')
V_MINOR=$(grep '#define CLIENT_VERSION_MINOR'    "$VERSION_H" | awk '{print $3}')
V_REV=$(grep   '#define CLIENT_VERSION_REVISION' "$VERSION_H" | awk '{print $3}')
V_BUILD=$(grep '#define CLIENT_VERSION_BUILD'    "$VERSION_H" | awk '{print $3}')

AUTO_VERSION="v${V_MAJOR}.${V_MINOR}.${V_REV}-b${V_BUILD}"

# ---------- mode selection ---------------------------------------------------

MODE="build"
TAG=""
ALLOW_UNSIGNED="${ZCL_ALLOW_UNSIGNED:-0}"

# Consume the --unsigned flag wherever it appears; it does not affect --verify.
ARGS=()
for arg in "$@"; do
    if [ "$arg" = "--unsigned" ]; then
        ALLOW_UNSIGNED=1
    else
        ARGS+=("$arg")
    fi
done
set -- "${ARGS[@]+"${ARGS[@]}"}"

if [ "${1:-}" = "--verify" ]; then
    MODE="verify"
    shift
    ARCHIVE="${1:-}"
    [ -n "$ARCHIVE" ] || die "Usage: $0 --verify <archive.tar.gz>"
    [ -f "$ARCHIVE" ]  || die "File not found: $ARCHIVE"
elif [ -n "${1:-}" ]; then
    TAG="$1"
else
    TAG="$AUTO_VERSION"
fi

# ---------- verify mode ------------------------------------------------------

if [ "$MODE" = "verify" ]; then
    info "Verifying release archive: $ARCHIVE"

    SHA3_FILE="${ARCHIVE%.tar.gz}.sha3"
    SIG_FILE="${SHA3_FILE}.sig"

    # Check SHA3-256
    if [ -f "$SHA3_FILE" ]; then
        EXPECTED=$(awk '{print $1}' "$SHA3_FILE")
        ACTUAL=$(openssl dgst -sha3-256 "$ARCHIVE" | awk '{print $NF}')
        if [ "$EXPECTED" = "$ACTUAL" ]; then
            info "SHA3-256: OK ($ACTUAL)"
        else
            die "SHA3-256 MISMATCH!\n  expected: $EXPECTED\n  actual:   $ACTUAL"
        fi
    else
        echo "WARN: No .sha3 file found at $SHA3_FILE"
    fi

    # Check GPG signature
    if [ -f "$SIG_FILE" ]; then
        if command -v gpg >/dev/null 2>&1; then
            if gpg --verify "$SIG_FILE" "$SHA3_FILE" 2>/dev/null; then
                info "GPG signature: OK"
            else
                die "GPG signature verification FAILED"
            fi
        else
            echo "WARN: gpg not installed, cannot verify signature"
        fi
    else
        echo "WARN: No .sig file found at $SIG_FILE (unsigned release)"
    fi

    # Extract and check BUILDINFO
    if tar -tzf "$ARCHIVE" 2>/dev/null | grep -q BUILDINFO; then
        info "BUILDINFO:"
        tar -xzf "$ARCHIVE" --to-stdout "*/BUILDINFO" 2>/dev/null || true
    fi

    info "Verification complete."
    exit 0
fi

# ---------- build mode -------------------------------------------------------

info "Building release: $TAG"

ARCH=$(uname -m)
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
RELEASE_NAME="zclassic23-${TAG}-${OS}-${ARCH}"
RELEASE_DIR="$REPO_ROOT/build/release"
STAGING="$RELEASE_DIR/$RELEASE_NAME"
NODE_BIN="$REPO_ROOT/build/bin/zclassic23"
CLI_BIN="$REPO_ROOT/build/bin/zclassic-cli"

# ---------- reproducible build profile ---------------------------------------
#
# The Makefile DEV default is -march=native + a non-deterministic build-id, both
# of which make the artifact machine-specific (the dev binary stays fast — we do
# NOT touch the Makefile). For a *release* we must be byte-reproducible so the
# .sha3 attestation is stable across machines, so we override the flags ONLY for
# this invocation via command-line make overrides:
#
#   (a) pin the ISA baseline to x86-64-v3 instead of the host's native arch;
#   (b) drop the linker build-id (-Wl,--build-id=none) so two builds of the
#       same source produce byte-identical binaries;
#   (c) export SOURCE_DATE_EPOCH from the HEAD commit time so any embedded
#       timestamps (and the staged-file mtimes below) are fixed;
#   (d) the tarball itself is created deterministically further down.
#
# We start from make's *resolved* CFLAGS/LDFLAGS (which already contain every
# -I include path) and rewrite only the reproducibility-hostile tokens, then
# hand the result back to make on the command line. A command-line assignment
# overrides the Makefile's `CFLAGS =` / `LDFLAGS =` definitions wholesale, so
# reconstructing from the resolved value is what keeps the include paths intact.
#
# The flag computation is shared with the build-twice byte-identity gate
# (tools/scripts/check_reproducible_build.sh) via tools/scripts/repro_build_vars.sh
# so the release artifact and the reproducibility gate provably use the SAME
# determinism profile — they cannot drift apart.
# shellcheck source=tools/scripts/repro_build_vars.sh
. "$SCRIPT_DIR/scripts/repro_build_vars.sh"
info "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH (HEAD commit time)"
info "Release CFLAGS:  $REL_CFLAGS"
info "Release LDFLAGS: $REL_LDFLAGS"

# Build from clean (deterministic flags overridden on the command line)
info "Running: make clean && make zclassic23 zclassic-cli (reproducible flags)"
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" CFLAGS="$REL_CFLAGS" LDFLAGS="$REL_LDFLAGS" \
    zclassic23 zclassic-cli 2>&1 | tail -3

# Verify binaries exist
[ -f "$NODE_BIN" ] || die "Build failed: $NODE_BIN not found"
[ -f "$CLI_BIN" ]  || die "Build failed: $CLI_BIN not found"

# Clean previous staging after make clean has removed build/.
rm -rf "$STAGING"
mkdir -p "$STAGING"

# Collect build metadata
GIT_REV=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY=$(git diff --quiet 2>/dev/null && echo "clean" || echo "dirty")

# Record the *release* flags actually used (computed above), not the dev
# defaults. The load-bearing reproducibility fields are -march (pinned to
# x86-64-v3) and -flto (CFLAGS + LDFLAGS), plus the dropped build-id.
CC="${CC:-$(_repro_make_var CC)}"
CC="${CC:-cc}"
COMPILER=$($CC --version 2>/dev/null | head -1 || echo "unknown")
# Build date is derived from the commit time so BUILDINFO stays reproducible.
BUILD_DATE=$(date -u -d "@${SOURCE_DATE_EPOCH}" +"%Y-%m-%dT%H:%M:%SZ" 2>/dev/null \
             || date -u -r "${SOURCE_DATE_EPOCH}" +"%Y-%m-%dT%H:%M:%SZ" 2>/dev/null \
             || echo "unknown")

cat > "$STAGING/BUILDINFO" <<BUILDINFO
ZClassic23 Release: $TAG
Build date:   $BUILD_DATE
Source epoch: $SOURCE_DATE_EPOCH
Git revision: $GIT_REV ($GIT_BRANCH, $GIT_DIRTY)
Compiler:     $COMPILER
CFLAGS:       $REL_CFLAGS
LDFLAGS:      $REL_LDFLAGS
Platform:     $(uname -srm)
Binary size:  $(stat -c%s "$NODE_BIN" 2>/dev/null || stat -f%z "$NODE_BIN") bytes
CLI size:     $(stat -c%s "$CLI_BIN" 2>/dev/null || stat -f%z "$CLI_BIN") bytes
BUILDINFO

info "BUILDINFO:"
cat "$STAGING/BUILDINFO"

# Copy binaries
cp "$NODE_BIN" "$STAGING/zclassic23"
cp "$CLI_BIN" "$STAGING/zclassic-cli"

# Strip debug symbols for release (keep a copy)
if command -v strip >/dev/null 2>&1; then
    strip "$STAGING/zclassic23"
    strip "$STAGING/zclassic-cli"
    info "Stripped binaries"
fi

# Copy essential files
cp LICENSE "$STAGING/" 2>/dev/null || true
cp README.md "$STAGING/" 2>/dev/null || true

# Create tarball — deterministically, so the .sha3 is stable across machines.
#   --sort=name        : stable member order (independent of readdir order)
#   --mtime=@0         : fix every member's mtime (no wall-clock leakage)
#   --owner/--group=0  : strip the building user's uid/gid
#   --numeric-owner    : never resolve names from the local passwd/group db
#   gzip -n            : omit the original name + timestamp from the gzip header
TARBALL="$RELEASE_DIR/${RELEASE_NAME}.tar.gz"
info "Creating archive (deterministic): $TARBALL"
(cd "$RELEASE_DIR" \
    && tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
           -cf - "$RELEASE_NAME" | gzip -n > "${RELEASE_NAME}.tar.gz")

# SHA3-256 hash
SHA3_FILE="$RELEASE_DIR/${RELEASE_NAME}.sha3"
HASH=$(openssl dgst -sha3-256 "$TARBALL" | awk '{print $NF}')
echo "$HASH  ${RELEASE_NAME}.tar.gz" > "$SHA3_FILE"
info "SHA3-256: $HASH"

# GPG detached signature (required unless explicitly waived)
SIG_FILE="${SHA3_FILE}.sig"
if command -v gpg >/dev/null 2>&1 && gpg --list-secret-keys 2>/dev/null | grep -q sec; then
    info "Signing with GPG..."
    gpg --detach-sign --armor -o "$SIG_FILE" "$SHA3_FILE"
    info "Signature: $SIG_FILE"
elif [ "$ALLOW_UNSIGNED" = "1" ]; then
    echo "WARN: No GPG secret key found; producing an UNSIGNED release (waived)."
    echo "      To sign: gpg --detach-sign --armor -o ${SIG_FILE} ${SHA3_FILE}"
else
    die "No GPG secret key found — refusing to produce an unsigned release.\n  Install/import a signing key, or re-run with --unsigned (or ZCL_ALLOW_UNSIGNED=1) to override."
fi

# Git tag (if not already tagged). Tagging is a publishing convenience, not
# part of producing the reproducible artifact, so a failure here (e.g. no git
# identity configured on a CI runner) must NOT abort an otherwise-good release.
if git rev-parse "$TAG" >/dev/null 2>&1; then
    info "Tag $TAG already exists, skipping git tag"
else
    info "Creating git tag: $TAG"
    git tag -a "$TAG" -m "Release $TAG" \
        || echo "WARN: could not create git tag $TAG (continuing; artifact is unaffected)"
fi

# Clean up staging directory
rm -rf "$STAGING"

# Summary
echo ""
echo "============================================"
echo "  Release $TAG complete"
echo "============================================"
echo "  Archive:   $TARBALL"
echo "  SHA3-256:  $SHA3_FILE"
[ -f "$SIG_FILE" ] && echo "  Signature: $SIG_FILE"
echo "  Tag:       $TAG"
echo ""
echo "To verify:   ./tools/release.sh --verify $TARBALL"
echo "To push tag: git push origin $TAG"
echo "============================================"
