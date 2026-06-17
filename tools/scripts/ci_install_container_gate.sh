#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# ci_install_container_gate.sh — the CLEAN-CONTAINER MVP C1 proof.
#
# MVP criterion #1 (docs/MVP.md) is "single-binary install on clean
# Ubuntu/Debian". The hermetic on-host proxy (ci_install_gate.sh) proves
# the install MECHANISM, but it runs on THIS dev host — contaminated with
# every build/runtime dependency the project has ever touched. This gate
# proves the half the on-host proxy structurally CANNOT: that the
# self-contained ~15 MB binary actually RUNS on a *clean* Ubuntu image
# with NO build toolchain and NO repo source — i.e. it carries no hidden
# host dependency. The headline vision is "one ~15 MB self-contained
# binary"; this is the gate that holds that claim honest.
#
# What it does:
#   1. Preflight docker (docker group OR passwordless sudo). SKIP (exit 2)
#      when docker is unreachable — the SAME SKIP-not-FAIL discipline as
#      mvp-onion-local / mvp-coldstart-local, so a docker-less runner never
#      false-FAILs.
#   2. Run a clean ubuntu:24.04 container (glibc 2.39 / GLIBCXX 3.4.33 — at or
#      above the binary's real TRIPLE symbol-version floor GLIBC_2.38 /
#      GLIBCXX_3.4.30 / CXXABI_1.3.9, the floor that ci_symbol_floor_gate.sh
#      pins) with ONLY build/bin mounted READ-ONLY — no repo checkout, no
#      compiler.
#   3. Log the image identity + `ldd` for the record, and note if a
#      compiler is unexpectedly present (image not minimal).
#   4. Spawn the node as an ISOLATED regtest node with the SAME proven flag
#      set the audited chokepoint (isolated_node_env.sh) uses — -regtest,
#      a dead -connect sink, -nolegacyimport, -nobgvalidation — and poll
#      RPC readiness over the env-var path (ZCL_DATADIR/ZCL_RPCPORT, the
#      cookie-file gate), then ASSERT the INSTALLED binary answers
#      getblockcount.
#
# Isolation: the container's network namespace is separate from the host,
# so the in-container ports never collide with / touch the live node; the
# datadir is the container's ephemeral /tmp and dies with --rm. Nothing on
# the host is read-write mounted.
#
# A missing runtime lib makes the loader/node fail to start -> this gate
# FAILs LOUD with the captured error (a real portability finding). The
# escape hatch ZCL_CI_CONTAINER_APT="libstdc++6 ..." apt-get installs named
# runtime packages first; it defaults EMPTY, so the default run asserts the
# strict self-containment claim (binary + base image only).
#
# Exit: 0 PASS · 2 SKIP (docker unreachable / image unobtainable) · 1 FAIL.
#
# The inner node-spawn+poll logic is parametrized by ZCL_BINDIR so it can be
# exercised WITHOUT docker for development:
#     ZCL_BINDIR="$PWD/build/bin" bash -c "$(sed -n '/^INNER=/,/^'\''$/p' \
#         tools/scripts/ci_install_container_gate.sh | ...)"
# but the supported entry point is `make ci-install-container`.

set -uo pipefail

GATE_SRC="${BASH_SOURCE[0]}"
GATE_DIR="$(cd "$(dirname "$GATE_SRC")" && pwd)"
REPO_ROOT="$(cd "$GATE_DIR/../.." && pwd)"

gate_skip() { echo "ci-install-container: SKIP ($*)"; exit 2; }
gate_die()  { echo "ci-install-container: FAIL: $*" >&2; exit 1; }

BINDIR="$REPO_ROOT/build/bin"
[ -x "$BINDIR/zclassic23" ] || gate_die "built node missing/!executable: $BINDIR/zclassic23"
[ -x "$BINDIR/zcl-rpc" ]    || gate_die "built zcl-rpc missing/!executable: $BINDIR/zcl-rpc"

# ── Resolve a usable docker (group membership OR passwordless sudo) ──
# Never prompt: a password-gated sudo is treated as "docker unreachable"
# and SKIPs, exactly like a docker-less host — this gate must never block.
DOCKER=""
if docker info >/dev/null 2>&1; then
    DOCKER="docker"
elif sudo -n docker info >/dev/null 2>&1; then
    DOCKER="sudo -n docker"
else
    gate_skip "docker unreachable (need docker-group membership or passwordless sudo; the daemon may be up but this user cannot reach the socket)"
fi
echo "ci-install-container: using '$DOCKER'"

# ── Ensure the clean base image is available (cache or pull) ─────────
IMG="${ZCL_CI_CONTAINER_IMAGE:-ubuntu:24.04}"
if ! $DOCKER image inspect "$IMG" >/dev/null 2>&1; then
    echo "ci-install-container: pulling $IMG ..."
    $DOCKER pull "$IMG" >/dev/null 2>&1 || gate_skip "cannot obtain image $IMG (no local cache and pull failed — offline?)"
fi

APT_PKGS="${ZCL_CI_CONTAINER_APT:-}"

# ── The in-container proof (only bash + coreutils — no ss/pkill) ─────
# Single-quoted so the HOST shell does not expand it; values arrive via
# `-e` env vars. ZCL_BINDIR points at the read-only binary mount (/zcl).
INNER='
set -uo pipefail
BIN="${ZCL_BINDIR:-/zcl}"
APT="${ZCL_CI_CONTAINER_APT:-}"

. /etc/os-release 2>/dev/null || true
echo "[container] image: ${PRETTY_NAME:-unknown}  glibc: $(ldd --version 2>/dev/null | head -1 | grep -oE "[0-9]+\.[0-9]+$")"
if command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; then
    echo "[container] NOTE: a compiler is present — base image is not minimal (proof still valid)"
else
    echo "[container] clean: no cc/gcc on PATH"
fi

if [ -n "$APT" ]; then
    echo "[container] apt-get runtime libs: $APT"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq $APT >/dev/null 2>&1 \
        || { echo "[container] apt-get of runtime libs failed"; exit 1; }
fi

echo "[container] ldd zclassic23:"
ldd "$BIN/zclassic23" 2>&1 | sed "s/^/[container]   /" || true

DD="$(mktemp -d /tmp/zcl-clean.XXXXXX)" || { echo "[container] mktemp failed"; exit 1; }
echo "[container] spawning isolated regtest node (datadir=$DD)"
"$BIN/zclassic23" \
    -datadir="$DD" -regtest \
    -port=39111 -rpcport=39112 -fsport=39113 -httpsport=39114 \
    -connect=127.0.0.1:39999 \
    -nobgvalidation -nolegacyimport -showmetrics=0 \
    >"$DD/node.log" 2>&1 &
NODE=$!

ok=0
for _ in $(seq 1 90); do
    if ! kill -0 "$NODE" 2>/dev/null; then
        echo "[container] node exited during warmup — tail of node.log:"
        tail -n 30 "$DD/node.log" 2>/dev/null | sed "s/^/[container]   /"
        break
    fi
    if [ -f "$DD/.cookie" ]; then
        raw="$(ZCL_DATADIR="$DD" ZCL_RPCPORT=39112 "$BIN/zcl-rpc" getblockcount 2>/dev/null)"
        # Readiness signal (the audited chokepoint pattern): any digits means
        # the RPC answered. tr -dc over the JSON concatenates ALL digits, so
        # use it ONLY as a non-empty gate, never as the value.
        sig="$(printf "%s" "$raw" | tr -dc "0-9-")"
        if [ -n "$sig" ]; then
            # Report the REAL result for the log (fresh regtest tip == 0),
            # falling back to the raw signal if the JSON shape is unexpected.
            bc="$(printf "%s" "$raw" | sed -E "s/.*\"result\":(-?[0-9]+).*/\1/")"
            printf "%s" "$bc" | grep -qE "^-?[0-9]+$" || bc="$sig"
            echo "[container] installed binary answered getblockcount=$bc"
            ok=1
            break
        fi
    fi
    sleep 0.5
done

kill -9 "$NODE" 2>/dev/null || true
if [ "$ok" = 1 ]; then
    echo "[container] PASS: self-contained binary ran + served RPC on a clean image"
    exit 0
fi
echo "[container] FAIL: node never served RPC — tail of node.log:"
tail -n 40 "$DD/node.log" 2>/dev/null | sed "s/^/[container]   /"
exit 1
'

echo "ci-install-container: running clean-image proof on $IMG (binaries mounted RO)"
$DOCKER run --rm \
    -e ZCL_BINDIR=/zcl \
    -e ZCL_CI_CONTAINER_APT="$APT_PKGS" \
    -v "$BINDIR":/zcl:ro \
    "$IMG" bash -c "$INNER"
rc=$?

if [ "$rc" -eq 0 ]; then
    echo "=== ci-install-container: PASSED ==="
else
    echo "=== ci-install-container: FAILED (rc=$rc) ==="
fi
exit "$rc"
