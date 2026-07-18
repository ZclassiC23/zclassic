#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# rom-bundle-replicate.sh — copy a two-builder-verified consensus-state bundle
# + its independent replay receipt (config/consensus_state_replay_receipt.h)
# + a record of the producing binary's own SHA3 image digest to a second,
# operator-chosen directory, and prove the copy is byte-identical by SHA3-256
# before reporting success. This is the "second disk" half of ROM bundle
# replication (docs/ROM_DELIVERY.md): today the verified bundle lives on ONE
# disk; pointing a node's -rombundlereplicadir=DEST at this script's output
# (config/rom_bundle_admission.h) turns that copy into a servable ROM catalog
# entry, and pointing ANOTHER node's DEST at the same replicated directory (or
# a further copy of it) turns that peer into a recovery source too.
#
# Fails closed: any missing input, a failed copy, or a post-copy digest
# mismatch exits non-zero and leaves nothing half-written (copies land via a
# temp name + atomic rename inside DEST, never in place at the final name
# until content is proven).
#
# Usage:
#   tools/scripts/rom-bundle-replicate.sh \
#     --bundle=PATH_TO_consensus-state-bundle-<anchor>.sqlite \
#     --receipt=PATH_TO_consensus_state_replay_receipt.v1 \
#     --dest=DIR \
#     [--binary=PATH]   (default: build/bin/zclassic23; the producing binary
#                         whose SHA3 image digest gets recorded alongside the
#                         copy for operator audit — this is NOT re-checked by
#                         the rom_bundle_admission reader, which intentionally
#                         does not require the serving node to run the exact
#                         verifying binary; see config/rom_bundle_admission.h)
#     [--sha3-tool=PATH] (default: build/bin/rom_bundle_sha3)
#
# Exit codes: 0 PASS (copy verified byte-identical by SHA3), 1 FAIL
# (mismatch/copy error), 2 usage/precondition error.
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

BUNDLE=""
RECEIPT=""
DEST=""
BINARY="$REPO_ROOT/build/bin/zclassic23"
SHA3_TOOL="$REPO_ROOT/build/bin/rom_bundle_sha3"

RECEIPT_BASENAME="consensus_state_replay_receipt.v1"
BINARY_HASH_BASENAME="producing_binary_sha3.txt"

usage() {
    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bundle=*)    BUNDLE="${1#--bundle=}" ;;
        --receipt=*)   RECEIPT="${1#--receipt=}" ;;
        --dest=*)      DEST="${1#--dest=}" ;;
        --binary=*)    BINARY="${1#--binary=}" ;;
        --sha3-tool=*) SHA3_TOOL="${1#--sha3-tool=}" ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "rom-bundle-replicate: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

[ -n "$BUNDLE" ]  || { echo "rom-bundle-replicate: --bundle=PATH is required" >&2; exit 2; }
[ -n "$RECEIPT" ] || { echo "rom-bundle-replicate: --receipt=PATH is required" >&2; exit 2; }
[ -n "$DEST" ]    || { echo "rom-bundle-replicate: --dest=DIR is required" >&2; exit 2; }
[ -s "$BUNDLE" ]  || { echo "rom-bundle-replicate: bundle not found or empty: $BUNDLE" >&2; exit 2; }
[ -s "$RECEIPT" ] || { echo "rom-bundle-replicate: receipt not found or empty: $RECEIPT" >&2; exit 2; }
[ -x "$SHA3_TOOL" ] || {
    echo "rom-bundle-replicate: $SHA3_TOOL not built (run: make tools/rom_bundle_sha3)" >&2
    exit 2
}

BUNDLE_BASENAME="$(basename -- "$BUNDLE")"
DEST_BUNDLE="$DEST/$BUNDLE_BASENAME"
DEST_RECEIPT="$DEST/$RECEIPT_BASENAME"
DEST_BINARY_HASH="$DEST/$BINARY_HASH_BASENAME"

mkdir -p "$DEST"

echo "======================================================================"
echo "  rom-bundle-replicate plan"
echo "  bundle:  $BUNDLE"
echo "  receipt: $RECEIPT"
echo "  binary:  ${BINARY:-<none — hash step skipped>}"
echo "  dest:    $DEST"
echo "  steps:"
echo "    1. sha3 of source bundle + receipt"
echo "    2. cp bundle  -> $DEST_BUNDLE (via temp + atomic rename)"
echo "       cp receipt -> $DEST_RECEIPT (via temp + atomic rename, canonical name)"
echo "    3. record producing binary SHA3 -> $DEST_BINARY_HASH"
echo "    4. sha3 of the DEST copies; require exact match to step 1"
echo "======================================================================"

# ── step 1: source digests ─────────────────────────────────────────────

src_bundle_sha3="$("$SHA3_TOOL" "$BUNDLE" | awk '{print $1}')"
src_receipt_sha3="$("$SHA3_TOOL" "$RECEIPT" | awk '{print $1}')"
echo "[rom-bundle-replicate] source bundle  sha3: $src_bundle_sha3"
echo "[rom-bundle-replicate] source receipt sha3: $src_receipt_sha3"

# ── step 2: copy via temp + atomic rename (never a half-written final name) ─

tmp_bundle="$DEST_BUNDLE.tmp.$$"
tmp_receipt="$DEST_RECEIPT.tmp.$$"
cleanup() { rm -f "$tmp_bundle" "$tmp_receipt"; }
trap cleanup EXIT INT TERM

cp -- "$BUNDLE" "$tmp_bundle"
cp -- "$RECEIPT" "$tmp_receipt"
mv -f -- "$tmp_bundle" "$DEST_BUNDLE"
mv -f -- "$tmp_receipt" "$DEST_RECEIPT"

# ── step 3: producing-binary hash record (audit trail, not re-verified) ────

if [ -n "$BINARY" ] && [ -x "$BINARY" ]; then
    binary_sha3="$("$SHA3_TOOL" "$BINARY" | awk '{print $1}')"
    {
        echo "# rom-bundle-replicate producing-binary record"
        echo "binary_path: $BINARY"
        echo "binary_sha3: $binary_sha3"
        echo "bundle: $BUNDLE_BASENAME"
        echo "bundle_sha3: $src_bundle_sha3"
        echo "replicated_at_unix: $(date +%s)"
    } > "$DEST_BINARY_HASH.tmp.$$"
    mv -f -- "$DEST_BINARY_HASH.tmp.$$" "$DEST_BINARY_HASH"
    echo "[rom-bundle-replicate] producing binary sha3: $binary_sha3 ($BINARY)"
else
    echo "[rom-bundle-replicate] NOTE: producing binary not found/executable at" \
         "'$BINARY' — skipping the binary-hash record" >&2
fi

# ── step 4: verify the copy byte-for-byte by SHA3 ──────────────────────────

dest_bundle_sha3="$("$SHA3_TOOL" "$DEST_BUNDLE" | awk '{print $1}')"
dest_receipt_sha3="$("$SHA3_TOOL" "$DEST_RECEIPT" | awk '{print $1}')"

fail=0
if [ "$dest_bundle_sha3" != "$src_bundle_sha3" ]; then
    echo "FAIL: bundle copy digest mismatch: dest=$dest_bundle_sha3 want=$src_bundle_sha3" >&2
    fail=1
fi
if [ "$dest_receipt_sha3" != "$src_receipt_sha3" ]; then
    echo "FAIL: receipt copy digest mismatch: dest=$dest_receipt_sha3 want=$src_receipt_sha3" >&2
    fail=1
fi

if [ "$fail" != "0" ]; then
    echo "FAIL: rom-bundle-replicate — copy not verified, see mismatches above" >&2
    exit 1
fi

echo "PASS: rom-bundle-replicate — copy verified byte-identical by SHA3-256"
echo "  $DEST_BUNDLE"
echo "  $DEST_RECEIPT"
[ -f "$DEST_BINARY_HASH" ] && echo "  $DEST_BINARY_HASH"
echo ""
echo "Point a node at this directory to serve it: -rombundlereplicadir=$DEST"
exit 0
