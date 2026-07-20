#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Two-builder reproducible-build gate for the zclassic23 node binary.
#
# Builds build/bin/zclassic23 TWICE from the current working tree, in two
# isolated build directories whose absolute paths differ in both value AND
# length (to expose any embedded absolute-path or padding nondeterminism), then
# SHA3-256- and byte-compares the two shipped (stripped) binaries.
#
# The shipped binary is stripped (`strip -s`), so .text/.rodata/.data carry no
# debug metadata. This gate compares the shipped artifacts AS SHIPPED — it does
# NOT strip anything itself. Byte identity is therefore proven over exactly the
# bytes a user would receive. If a future toolchain change reintroduces a
# divergence, the gate reports the exact differing ELF sections and byte
# offsets rather than papering over them with aggressive stripping; a residual
# diff is printed in full so the operator sees precisely what is not yet proven.
#
# Determinism is supplied by REPRO_CFLAGS in the Makefile (behavior-identical
# -ffile-prefix-map + -gno-record-gcc-switches). See
# docs/SECURITY_AND_INTEGRITY.md "Reproducible build gate".
#
# Cost: two full whole-program LTO builds (~2x a normal `make zclassic23`).
# This is intentionally NOT on the default lint/ci path — run it on demand.
#
# Usage:  make repro-verify            (or)   tools/scripts/repro-verify.sh
# Env:    ZCL_REPRO_JOBS=<n>   parallelism for each build (default: nproc)
#         ZCL_REPRO_KEEP=1     keep the two build trees for inspection
set -euo pipefail

JOBS="${ZCL_REPRO_JOBS:-$(nproc 2>/dev/null || echo 4)}"
SRC="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "repro-verify: not inside a git worktree" >&2
    exit 2
}

for t in rsync git openssl cmp readelf; do
    command -v "$t" >/dev/null 2>&1 || { echo "repro-verify: missing required tool: $t" >&2; exit 2; }
done

BASE="$(mktemp -d "${TMPDIR:-/tmp}/zcl-repro.XXXXXX")"
# Two build roots with DIFFERENT absolute-path values and lengths.
A="$BASE/a"
B="$BASE/builder-two-deliberately-longer-path"

cleanup() {
    if [ "${ZCL_REPRO_KEEP:-0}" = "1" ]; then
        echo "repro-verify: build trees kept at $BASE" >&2
    else
        rm -rf "$BASE"
    fi
}
trap cleanup EXIT HUP INT TERM

snapshot() {
    # Copy the current working tree (tracked + build inputs like vendor/lib)
    # into a self-contained git worktree so tools/dev/source-identity.sh can
    # enumerate it. The build/ tree and the linked-worktree .git pointer are
    # excluded; a fresh throwaway git repo is initialized in their place.
    local dst="$1"
    mkdir -p "$dst"
    rsync -a --delete \
        --exclude='/build' --exclude='/.git' \
        "$SRC"/./ "$dst"/
    # A throwaway git repo makes the copy a valid worktree so
    # tools/dev/source-identity.sh can enumerate it. The commit is best-effort;
    # source-identity inventories untracked files too, so a no-op commit is
    # harmless — both snapshots are treated identically, which is all the gate
    # needs. All git chatter is suppressed.
    ( cd "$dst"
      git init -q >/dev/null 2>&1
      git add -A >/dev/null 2>&1 || true
      git -c user.email=repro@localhost -c user.name=repro \
          commit -q -m 'repro-verify snapshot' >/dev/null 2>&1 || true )
}

build_one() {
    local dst="$1" log="$2"
    if ! ( cd "$dst" && make zclassic23 -j"$JOBS" ) >"$log" 2>&1; then
        echo "repro-verify: build FAILED in $dst (tail of log):" >&2
        tail -25 "$log" >&2
        return 1
    fi
}

echo "repro-verify: source tree     = $SRC"
echo "repro-verify: build root A     = $A"
echo "repro-verify: build root B     = $B"
echo "repro-verify: jobs             = $JOBS"
echo "repro-verify: snapshotting ..."
snapshot "$A"
snapshot "$B"

echo "repro-verify: building A (this is ~1x a full node build) ..."
build_one "$A" "$BASE/build-a.log"
echo "repro-verify: building B ..."
build_one "$B" "$BASE/build-b.log"

BA="$A/build/bin/zclassic23"
BB="$B/build/bin/zclassic23"
[ -f "$BA" ] && [ -f "$BB" ] || { echo "repro-verify: FAIL — a node binary is missing" >&2; exit 1; }

HA="$(openssl dgst -sha3-256 "$BA" | awk '{print $NF}')"
HB="$(openssl dgst -sha3-256 "$BB" | awk '{print $NF}')"
SA="$(stat -c%s "$BA")"; SB="$(stat -c%s "$BB")"

echo "repro-verify: A  sha3-256=$HA  size=$SA"
echo "repro-verify: B  sha3-256=$HB  size=$SB"

if cmp -s "$BA" "$BB"; then
    echo "repro-verify: PASS — build/bin/zclassic23 is byte-identical across two builders (sha3-256=$HA)"
    exit 0
fi

# ── Residual-diff report (honest; no aggressive stripping) ────────────────
echo "repro-verify: FAIL — the two node binaries are NOT byte-identical" >&2
echo "repro-verify: differing byte count = $(cmp -l "$BA" "$BB" | wc -l)" >&2
echo "repro-verify: first differing offsets (hex):" >&2
cmp -l "$BA" "$BB" | awk '{printf "  0x%x\n",$1-1}' | head -20 >&2
echo "repro-verify: differing ELF sections:" >&2
for s in $(readelf -SW "$BA" | sed -n 's/.*\] \(\.[^ ]*\).*/\1/p' | sort -u); do
    ha="$(objcopy -O binary --only-section="$s" "$BA" /dev/stdout 2>/dev/null | sha256sum | awk '{print $1}')"
    hb="$(objcopy -O binary --only-section="$s" "$BB" /dev/stdout 2>/dev/null | sha256sum | awk '{print $1}')"
    [ "$ha" = "$hb" ] || echo "  DIFF section $s" >&2
done
echo "repro-verify: (re-run with ZCL_REPRO_KEEP=1 to inspect $BASE)" >&2
exit 1
