#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Byte-identity and fail-closed tests for the batched dirty-source identity.

set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/source-identity.sh"
REAL_GIT="$(command -v git)"
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
printf '%s\n' '_module-origin/' 'vendor/lib/*.a' 'vendor/include/' \
    'app/controllers/src/ignored-*.c' \
    'app/controllers/include/controllers/ignored-*.h' \
    >> .git/info/exclude

# A real gitlink proves the source id follows current submodule bytes rather
# than the parent's SHA-1 gitlink object id. The local origin is Git-ignored.
git init -q _module-origin
git -C _module-origin config user.email source-identity-selftest@example.invalid
git -C _module-origin config user.name source-identity-selftest
printf 'submodule input\n' > _module-origin/sub.c
git -C _module-origin add sub.c
git -C _module-origin commit -qm base
git -c protocol.file.allow=always submodule add -q ./_module-origin vendor/sub

# Ignored archives selected by the build remain exact build inputs.
mkdir -p vendor/lib
printf 'archive input\n' > vendor/lib/fixture.a
mkdir -p vendor/include/openssl
printf 'generated header input\n' > vendor/include/openssl/fixture.h

# These tracked anchors create real Make wildcard/include roots. Later tests
# add locally ignored compiler inputs beside them; Git must not control whether
# those bytes enter the authoritative source inventory.
mkdir -p app/controllers/src app/controllers/include/controllers
printf 'int app_fixture(void) { return 1; }\n' \
    > app/controllers/src/fixture.c
printf '#define APP_FIXTURE 1\n' \
    > app/controllers/include/controllers/fixture.h

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

submodule_before="$($SCRIPT capture)" || fail 'submodule baseline failed'
git -C vendor/sub commit --allow-empty -qm history-only
git add vendor/sub
git commit -qm gitlink-history-only
submodule_after="$($SCRIPT capture)" || fail 'submodule history capture failed'
[ "$submodule_before" = "$submodule_after" ] ||
    fail 'identity inherited the parent/submodule Git object id'

printf 'submodule edit\n' >> vendor/sub/sub.c
submodule_changed="$($SCRIPT capture)" || fail 'submodule edit capture failed'
[ "$submodule_changed" != "$submodule_after" ] ||
    fail 'current submodule bytes did not supersede source identity'
printf 'submodule input\n' > vendor/sub/sub.c
[ "$($SCRIPT capture)" = "$submodule_after" ] ||
    fail 'restoring submodule bytes did not restore source identity'

mv vendor/sub vendor/sub-initialized
mkdir vendor/sub
printf 'unowned replacement bytes\n' > vendor/sub/rogue.c
if "$SCRIPT" capture > /dev/null 2>&1; then
    fail 'nonempty uninitialized gitlink bytes were omitted'
fi
rm -rf vendor/sub
mv vendor/sub-initialized vendor/sub
[ "$($SCRIPT capture)" = "$submodule_after" ] ||
    fail 'restoring initialized gitlink did not restore source identity'

printf 'archive changed\n' > vendor/lib/fixture.a
[ "$($SCRIPT capture)" != "$submodule_after" ] ||
    fail 'ignored linked archive bytes did not supersede source identity'
printf 'archive input\n' > vendor/lib/fixture.a

printf 'generated header changed\n' > vendor/include/openssl/fixture.h
[ "$($SCRIPT capture)" != "$submodule_after" ] ||
    fail 'ignored generated header bytes did not supersede source identity'
printf 'generated header input\n' > vendor/include/openssl/fixture.h
[ "$($SCRIPT capture)" = "$submodule_after" ] ||
    fail 'restoring generated header bytes did not restore source identity'
[ "$($SCRIPT capture)" = "$submodule_after" ] ||
    fail 'restoring linked archive bytes did not restore source identity'

printf 'int ignored_source(void) { return 1; }\n' \
    > app/controllers/src/ignored-fixture.c
git check-ignore -q app/controllers/src/ignored-fixture.c ||
    fail 'ignored source fixture is not actually ignored'
ignored_source_a="$($SCRIPT capture)" ||
    fail 'ignored wildcard source capture failed'
[ "$ignored_source_a" != "$submodule_after" ] ||
    fail 'ignored wildcard-selected C source was omitted'
printf 'int ignored_source(void) { return 2; }\n' \
    > app/controllers/src/ignored-fixture.c
ignored_source_b="$($SCRIPT capture)" ||
    fail 'mutated ignored wildcard source capture failed'
[ "$ignored_source_b" != "$ignored_source_a" ] ||
    fail 'ignored wildcard-selected C source mutation was omitted'
rm app/controllers/src/ignored-fixture.c
[ "$($SCRIPT capture)" = "$submodule_after" ] ||
    fail 'removing ignored wildcard source did not restore identity'

printf '#define IGNORED_FIXTURE 1\n' \
    > app/controllers/include/controllers/ignored-fixture.h
git check-ignore -q app/controllers/include/controllers/ignored-fixture.h ||
    fail 'ignored header fixture is not actually ignored'
ignored_header_a="$($SCRIPT capture)" ||
    fail 'ignored include-root header capture failed'
[ "$ignored_header_a" != "$submodule_after" ] ||
    fail 'ignored include-root header was omitted'
printf '#define IGNORED_FIXTURE 2\n' \
    > app/controllers/include/controllers/ignored-fixture.h
ignored_header_b="$($SCRIPT capture)" ||
    fail 'mutated ignored include-root header capture failed'
[ "$ignored_header_b" != "$ignored_header_a" ] ||
    fail 'ignored include-root header mutation was omitted'
rm app/controllers/include/controllers/ignored-fixture.h
[ "$($SCRIPT capture)" = "$submodule_after" ] ||
    fail 'removing ignored include-root header did not restore identity'

rm vendor/lib/fixture.a
ln -s ../../fixture-1.c vendor/lib/fixture.a
if "$SCRIPT" capture > /dev/null 2>&1; then
    fail 'symlinked linked archive was not rejected'
fi
rm vendor/lib/fixture.a
printf 'archive input\n' > vendor/lib/fixture.a

# A history-only Git commit changes the repository's SHA-1 HEAD but not source
# bytes. The sovereign v2 identity must therefore remain exactly unchanged.
tree_before_history="$($SCRIPT capture)" || fail 'clean-tree capture failed'
git commit --allow-empty -qm history-only
tree_after_history="$($SCRIPT capture)" || fail 'post-history capture failed'
[ "$tree_before_history" = "$tree_after_history" ] ||
    fail 'identity inherited a Git commit/object id instead of source bytes'
[ -z "$($SCRIPT paths)" ] || fail 'history-only commit appeared as source dirt'
clean_record="$($SCRIPT capture-record)" || fail 'clean record capture failed'
read -r clean_id clean_bit clean_mutation <<< "$clean_record"
[ "$clean_id" = "$tree_after_history" ] && [ "$clean_bit" = 1 ] &&
    [[ "$clean_mutation" =~ ^[0-9a-f]{64}$ ]] ||
    fail 'clean record did not bind identity and clean state together'

for i in $(seq 1 160); do
    printf '/* dirty %s */\n' "$i" >> "fixture-$i.c"
done
printf 'untracked with spaces\n' > 'new source file.c'
printf 'untracked with backslash\n' > 'new\source.c'
printf 'untracked with newline\n' > $'new\nsource.c'
rm deleted.c
ln -sfn fixture-2.c source-link
chmod +x fixture-160.c
git add fixture-1.c fixture-2.c

fast="$("$SCRIPT" capture)" || fail 'batched capture failed'
dirty_record="$("$SCRIPT" capture-record)" || fail 'dirty record capture failed'
read -r dirty_id dirty_bit dirty_mutation <<< "$dirty_record"
[ "$dirty_id" = "$fast" ] && [ "$dirty_bit" = 1 ] &&
    [[ "$dirty_mutation" =~ ^[0-9a-f]{64}$ ]] ||
    fail 'dirty record did not include non-committed source state'
portable="$(ZCL_SOURCE_IDENTITY_FORCE_PORTABLE=1 "$SCRIPT" capture)" ||
    fail 'portable capture failed'
[ "$fast" = "$portable" ] ||
    fail "batched identity differs from canonical portable identity fast=$fast portable=$portable"
[[ "$fast" =~ ^[0-9a-f]{64}$ ]] || fail 'capture was not lowercase SHA-256'

# A trailing newline is a legal symlink-target byte. It must supersede the
# otherwise-identical target, and fast/portable collectors must agree exactly.
ln -sfn $'fixture-2.c\n' source-link
newline_fast="$("$SCRIPT" capture)" ||
    fail 'batched trailing-newline symlink capture failed'
newline_portable="$(ZCL_SOURCE_IDENTITY_FORCE_PORTABLE=1 "$SCRIPT" capture)" ||
    fail 'portable trailing-newline symlink capture failed'
[ "$newline_fast" = "$newline_portable" ] ||
    fail 'fast/portable identities differ for trailing-newline symlink target'
[ "$newline_fast" != "$fast" ] ||
    fail 'trailing-newline symlink target did not supersede source identity'
ln -sfn fixture-2.c source-link
restored="$("$SCRIPT" capture)" || fail 'restored symlink capture failed'
[ "$restored" = "$fast" ] ||
    fail 'restoring exact symlink target did not restore source identity'

# Edit/revert ABA preserves content but changes the build-session mutation
# token, so an object compiled during the transient state cannot be published.
aba_record="$($SCRIPT capture-record)" || fail 'ABA baseline capture failed'
read -r aba_id aba_clean aba_mutation <<< "$aba_record"
printf 'transient compiler input\n' > fixture-170.c
printf 'int fixture_170(void) { return 170; }\n' > fixture-170.c
[ "$($SCRIPT capture)" = "$aba_id" ] ||
    fail 'ABA restore did not recover the portable content identity'
if "$SCRIPT" verify-record "$aba_id" "$aba_clean" "$aba_mutation" \
        > /dev/null 2>&1; then
    fail 'edit/revert ABA verified against its stale mutation token'
fi

current_record="$($SCRIPT capture-record)" || fail 'current record capture failed'
read -r current_id current_clean current_mutation <<< "$current_record"
[ "$current_id" = "$fast" ] && [ "$current_clean" = 1 ] ||
    fail 'current record changed content/completeness state after ABA restore'
verified="$($SCRIPT verify "$fast")" || fail 'exact identity did not verify'
[ "$verified" = "$fast" ] || fail 'verify returned a different identity'
"$SCRIPT" verify-record "$fast" 1 "$current_mutation" > /dev/null ||
    fail 'verify-record rejected the current identity/completeness record'

# The metadata token operates on the enumerated set, so independently refresh
# that set before accepting a record. Inject a new untracked file during the
# first SHA-256 pass and prove a single capture-record cannot omit it.
real_sha256sum="$(command -v sha256sum)"
mkdir inventory-race-bin
printf '%s\n' '#!/usr/bin/env bash' \
    'if [ "$#" -eq 0 ] && [ ! -e "$INVENTORY_RACE_FLAG" ]; then' \
    '  : > "$INVENTORY_RACE_FLAG"' \
    '  printf "late inventory input\\n" > "$INVENTORY_RACE_SOURCE"' \
    'fi' \
    'exec "$REAL_SHA256SUM" "$@"' > inventory-race-bin/sha256sum
chmod +x inventory-race-bin/sha256sum
if PATH="$PWD/inventory-race-bin:$PATH" \
        REAL_SHA256SUM="$real_sha256sum" \
        INVENTORY_RACE_FLAG="$PWD/inventory-race-fired" \
        INVENTORY_RACE_SOURCE="$PWD/late-inventory-source.c" \
        "$SCRIPT" capture-record > /dev/null 2>&1; then
    fail 'new source created during capture escaped the inventory guard'
fi
rm -rf inventory-race-bin inventory-race-fired late-inventory-source.c

printf 'superseding edit\n' >> fixture-161.c
if "$SCRIPT" verify "$fast" > /dev/null 2>&1; then
    fail 'superseded identity verified'
fi
if "$SCRIPT" verify-record "$fast" 1 "$current_mutation" \
        > /dev/null 2>&1; then
    fail 'superseded identity/clean record verified'
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

# A process-substitution failure used to be invisible to the parent shell.
# Inject a `git ls-files --stage` failure and prove the full tracked inventory
# can never collapse to a successful partial identity.
mkdir fail-git-bin
printf '%s\n' '#!/usr/bin/env bash' \
    'if [ "${1:-}" = ls-files ] && [[ " $* " = *" --stage "* ]]; then exit 19; fi' \
    'exec "$REAL_GIT" "$@"' > fail-git-bin/git
chmod +x fail-git-bin/git
if PATH="$PWD/fail-git-bin:$PATH" REAL_GIT="$REAL_GIT" \
        "$SCRIPT" capture > /dev/null 2>&1; then
    fail 'tracked-inventory command failure produced a partial identity'
fi

printf 'source-identity-selftest: PASS identity=%s sha1_independent=true submodule_bytes=true ignored_compiler_inputs=true path_bytes=true inventory_fail_closed=true\n' "$fast"
