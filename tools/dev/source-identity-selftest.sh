#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Byte-identity and fail-closed tests for the batched dirty-source identity.

set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/source-identity.sh"
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/zcl-source-identity-selftest.XXXXXX")"
trap 'rm -rf "$SANDBOX"' EXIT HUP INT TERM

fail()
{
    printf 'source-identity-selftest: FAIL: %s\n' "$*" >&2
    exit 1
}

cd "$SANDBOX"
git init -q
git config user.email source-identity-selftest@example.invalid
git config user.name source-identity-selftest

# Enough files to exercise the batched path rather than accidentally testing
# only the zero/single-file envelope.
for i in $(seq 1 180); do
    printf 'int fixture_%s(void) { return %s; }\n' "$i" "$i" > "fixture-$i.c"
done
printf 'unchanged\n' > hidden.c
printf 'delete me\n' > deleted.c
ln -s fixture-1.c source-link
git add .
git commit -qm base

for i in $(seq 1 160); do
    printf '/* dirty %s */\n' "$i" >> "fixture-$i.c"
done
printf 'untracked with spaces\n' > 'new source file.c'
rm deleted.c
ln -sfn fixture-2.c source-link
chmod +x fixture-160.c
git add fixture-1.c fixture-2.c

fast="$("$SCRIPT" capture)" || fail 'batched capture failed'
portable="$(ZCL_SOURCE_IDENTITY_FORCE_PORTABLE=1 "$SCRIPT" capture)" ||
    fail 'portable capture failed'
[ "$fast" = "$portable" ] ||
    fail "batched identity differs from canonical portable identity fast=$fast portable=$portable"
[[ "$fast" =~ ^[0-9a-f]{64}$ ]] || fail 'capture was not lowercase SHA-256'

verified="$($SCRIPT verify "$fast")" || fail 'exact identity did not verify'
[ "$verified" = "$fast" ] || fail 'verify returned a different identity'

printf 'superseding edit\n' >> fixture-161.c
if "$SCRIPT" verify "$fast" > /dev/null 2>&1; then
    fail 'superseded identity verified'
fi

# Hidden index bits can suppress dirty discovery and must remain a hard error
# on both implementations.
git update-index --skip-worktree hidden.c
if "$SCRIPT" capture > /dev/null 2>&1; then
    fail 'batched capture accepted skip-worktree state'
fi
if ZCL_SOURCE_IDENTITY_FORCE_PORTABLE=1 "$SCRIPT" capture > /dev/null 2>&1; then
    fail 'portable capture accepted skip-worktree state'
fi
git update-index --no-skip-worktree hidden.c

printf 'source-identity-selftest: PASS identity=%s dirty_files=162\n' "$fast"
