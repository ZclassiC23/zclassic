#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Prove offline vendor mode refuses a cache miss before invoking a downloader.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/zcl-vendor-offline-selftest.XXXXXX")"
trap 'rm -rf "$SANDBOX"' EXIT HUP INT TERM

fail()
{
    printf 'build_vendor_offline_selftest: FAIL: %s\n' "$*" >&2
    exit 1
}

mkdir -p "$SANDBOX/tools/scripts" "$SANDBOX/vendor/.cache" \
    "$SANDBOX/vendor/lib" "$SANDBOX/vendor/include" "$SANDBOX/bin"
cp "$ROOT/tools/scripts/build_vendor.sh" \
    "$ROOT/tools/scripts/vendor_provenance_lib.sh" "$SANDBOX/tools/scripts/"

for tool in curl wget; do
    printf '%s\n' '#!/usr/bin/env bash' \
        'printf "%s\\n" "$0 $*" >>"$DOWNLOADER_CONTACT_LOG"' \
        'exit 97' > "$SANDBOX/bin/$tool"
    chmod +x "$SANDBOX/bin/$tool"
done

contact_log="$SANDBOX/downloader-contact.log"
: > "$contact_log"
if output="$(cd "$SANDBOX" && \
        DOWNLOADER_CONTACT_LOG="$contact_log" \
        PATH="$SANDBOX/bin:$PATH" ZCL_VENDOR_OFFLINE=1 \
        tools/scripts/build_vendor.sh libz.a 2>&1)"; then
    fail 'missing archive unexpectedly built in offline mode'
fi
[[ "$output" == *'offline cache miss or checksum failure: zlib-1.3.1.tar.gz'* ]] ||
    fail 'cache-miss refusal did not name the missing pinned archive'
[ ! -s "$contact_log" ] || fail 'offline mode invoked a downloader'

if ZCL_VENDOR_OFFLINE=invalid "$ROOT/tools/scripts/build_vendor.sh" \
        --check-provenance >/dev/null 2>&1; then
    fail 'invalid offline policy value was accepted'
fi

printf '%s\n' \
    'build_vendor_offline_selftest: PASS downloader_contacted=false cache_miss_refused=true'
