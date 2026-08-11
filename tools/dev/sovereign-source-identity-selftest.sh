#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Fail-closed tests for the Git-free ZVCS Make identity adapter.

set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/source-identity.sh"
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/zcl-sovereign-identity-selftest.XXXXXX")"
trap 'rm -rf "$SANDBOX"' EXIT HUP INT TERM
ROOT=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef

fail()
{
    echo "sovereign-source-identity-selftest: FAIL: $*" >&2
    exit 1
}

mkdir -p "$SANDBOX/source/app" "$SANDBOX/bin"
printf 'int sovereign(void) { return 23; }\n' > "$SANDBOX/source/app/a.c"
printf 'all:\n\t@true\n' > "$SANDBOX/source/Makefile"
printf '%s\n' '#!/usr/bin/env bash' \
    'set -euo pipefail' \
    'printf "{\"schema\":\"zcl.result.v1\",\"ok\":true,\"data\":{\"source_root\":\"%s\"}}\n" "$FAKE_SOURCE_ROOT"' \
    > "$SANDBOX/bin/bootstrap"
printf '%s\n' '#!/usr/bin/env bash' 'exit 97' > "$SANDBOX/bin/git"
chmod +x "$SANDBOX/bin/bootstrap" "$SANDBOX/bin/git"

cd "$SANDBOX/source"
export ZCL_SOVEREIGN_SOURCE_ROOT="$ROOT"
export ZCL_SOVEREIGN_VERIFY_BIN="$SANDBOX/bin/bootstrap"
export FAKE_SOURCE_ROOT="$ROOT"
export PATH="$SANDBOX/bin:$PATH"

[ ! -e .git ] || fail 'fixture unexpectedly has Git metadata'
[ "$($SCRIPT capture)" = "$ROOT" ] || fail 'capture did not use ZVCS authority'
record="$($SCRIPT capture-record)" || fail 'capture-record failed'
read -r identity complete mutation <<< "$record"
[ "$identity" = "$ROOT" ] && [ "$complete" = 1 ] &&
    [[ "$mutation" =~ ^[0-9a-f]{64}$ ]] ||
    fail 'capture-record shape is invalid'
$SCRIPT verify-record "$identity" "$complete" "$mutation" >/dev/null ||
    fail 'unchanged sovereign record did not verify'

printf 'transient\n' >> app/a.c
printf 'int sovereign(void) { return 23; }\n' > app/a.c
if $SCRIPT verify-record "$identity" "$complete" "$mutation" \
        >/dev/null 2>&1; then
    fail 'edit/revert ABA retained a stale mutation receipt'
fi

record="$($SCRIPT capture-record)" || fail 'refreshed capture-record failed'
read -r identity complete mutation <<< "$record"
$SCRIPT verify-mutation "$mutation" >/dev/null ||
    fail 'current sovereign mutation did not verify'

FAKE_SOURCE_ROOT=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
export FAKE_SOURCE_ROOT
if $SCRIPT capture >/dev/null 2>&1; then
    fail 'bootstrap/source authority disagreement was accepted'
fi
FAKE_SOURCE_ROOT="$ROOT"
export FAKE_SOURCE_ROOT

ZCL_SOVEREIGN_VERIFY_BIN="$SANDBOX/bin/missing" \
    $SCRIPT capture >/dev/null 2>&1 &&
    fail 'missing bootstrap verifier was accepted'

printf 'sovereign-source-identity-selftest: PASS root=%s git_contacted=false aba_refused=true\n' "$ROOT"
