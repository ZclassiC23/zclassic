#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Build the ordinary C23 node at the project's supported Linux ABI floor.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CC_WRAPPER="$($SCRIPT_DIR/c23_portable_sysroot.sh prepare)"

echo "c23-portable-release: rebuilding linked archives through $CC_WRAPPER" >&2
VENDOR_CC="$CC_WRAPPER" "$SCRIPT_DIR/build_vendor.sh" \
    libtor_stub.a libsqlite3.a libz.a libcrypto.a libssl.a \
    libevent.a libevent_openssl.a libevent_pthreads.a

# The portable baseline is the ordinary offline-friendly node. A checkout
# containing an optional host-built full-Tor archive must not silently import
# that host ABI into this artifact; the stub leaves Tor explicitly disabled.
products=(zclassic23 zcl-rpc zclassic23-package-sign zclassic23-package-verify)
# The two tiny stable-name helpers are FORCE-built by their canonical rules;
# changing this compiler also changes vendor provenance, which invalidates the
# two whole-program products without making every source prerequisite phony.
make -C "$REPO_ROOT" CC="$CC_WRAPPER" VENDOR_CC="$CC_WRAPPER" \
    ZCL_C23_PORTABLE_RELEASE=1 TOR_FULL= "${products[@]}"
for product in "${products[@]}"; do
    "$SCRIPT_DIR/check_c23_node_binary.sh" "$REPO_ROOT/build/bin/$product" >/dev/null
done

max="$(objdump -T "$REPO_ROOT/build/bin/zclassic23" 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -V | tail -1)"
sha="$(sha256sum "$REPO_ROOT/build/bin/zclassic23" | awk '{print $1}')"
echo "c23-portable-release: PASS products=${#products[@]} node=build/bin/zclassic23 sha256=$sha max_abi=$max" >&2
