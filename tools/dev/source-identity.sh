#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Supersession identity for the complete current build-source inventory. Git is
# used only to enumerate tracked/untracked paths and inspect index flags; its
# object ids and commit history are never hashed or trusted. This is a
# verify-loop diagnostic, not the immutable Phase-3 publication epoch schema.
# Initialized gitlinks are inventoried recursively. Exact static archives and
# generated vendor headers selected by the current build are included even when
# Git-ignored. Every file beneath the C23 source/include/template roots is also
# inventoried independently of `.gitignore` and `.git/info/exclude`, because
# Make wildcards and compiler includes do not honor Git ignore rules. The
# compiler/toolchain state, environment, and full build configuration remain
# deferred. Hidden index bits, discovery errors, and unsupported source types
# fail closed. Linked binaries must also `verify-record` after linking; capture
# alone is not a filesystem snapshot or publication receipt.

set -euo pipefail

MODE="${1:-capture}"
EXPECTED="${2:-}"
EXPECTED_CLEAN="${3:-}"
EXPECTED_MUTATION="${4:-}"
NEED_DIRTY_PATHS=0
[ "$MODE" = paths ] && NEED_DIRTY_PATHS=1
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || exit 2
SELF="$SELF_DIR/$(basename "${BASH_SOURCE[0]}")"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "source-identity: not inside a Git worktree" >&2
    exit 2
}
cd "$ROOT"

# Optional host-local, per-Make-invocation memoization for capture-record and
# verify-record (see capture_record_cached() below). One plain `make
# build-only` or `make t-fast` calls one of those two modes 4-5 times even
# with zero source changes (Makefile parse-time BUILD_SOURCE_RECORD, the
# mutation/identity stamps, and every build-epoch-session.sh acquire/verify),
# and each call is a full git-ls-files+find+sha256 walk of every build input.
# A caller opts in by setting ZCL_SOURCE_IDENTITY_SESSION to a `pid:start`
# token identifying the ONE live Make process driving the whole invocation
# (Make's own pid plus its /proc start-time in clock ticks, so a later
# process that reuses the same pid never collides with a stale entry). Unset
# or malformed disables memoization for that call -- always safe, just slower.
ZCL_SOURCE_IDENTITY_SESSION="${ZCL_SOURCE_IDENTITY_SESSION:-}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/zcl-source-identity.XXXXXX")" || exit 2
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

fail()
{
    echo "source-identity: $*" >&2
    exit 3
}

sha256_stream()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | awk '{print $1}'
    else
        fail "no SHA-256 implementation is available"
    fi
}

sha256_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum < "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 < "$1" | awk '{print $1}'
    else
        fail "no SHA-256 implementation is available"
    fi
}

# Collapse host permission details into the modes Git can represent. Special
# bits and group/other write bits are not source identity; executable intent is.
canonical_source_mode()
{
    local kind="$1" raw="$2" out_name="$3" value canonical
    [[ "$raw" =~ ^[0-9a-fA-F]+$ ]] || return 1
    value=$((16#$raw))
    case "$kind" in
        L)
            [ $((value & 0170000)) -eq $((0120000)) ] || return 1
            canonical=120000
            ;;
        F)
            [ $((value & 0170000)) -eq $((0100000)) ] || return 1
            if [ $((value & 0111)) -ne 0 ]; then
                canonical=100755
            else
                canonical=100644
            fi
            ;;
        *) return 1 ;;
    esac
    printf -v "$out_name" '%s' "$canonical"
}

# readlink writes one record-terminating newline. Command substitution normally
# strips every trailing newline, including legal newlines that are part of the
# symlink target. Append a non-newline sentinel, then remove exactly the
# sentinel and readlink's one delimiter while preserving target bytes.
read_symlink_target()
{
    local link_path="$1" out_name="$2" raw sentinel=$'\x1f'
    raw="$(
        readlink -- "$link_path" || exit 1
        printf '%s' "$sentinel"
    )" || return 1
    [ "${raw: -1}" = "$sentinel" ] || return 1
    raw="${raw%"$sentinel"}"
    [ "${raw: -1}" = $'\n' ] || return 1
    raw="${raw%$'\n'}"
    printf -v "$out_name" '%s' "$raw"
}

# `git diff` deliberately does not reveal content hidden by assume-unchanged
# or skip-worktree.  Refuse the entire publication epoch if either bit exists;
# silently trusting those flags would let a sealed edit evade the dirty set.
git ls-files -v -z > "$WORK/index-tags" ||
    fail "could not inspect Git index flags"
while IFS= read -r -d '' record; do
    tag="${record:0:1}"
    path="${record:2}"
    case "$tag" in
        S|[a-z]) fail "hidden Git index bit on path: $path (clear skip-worktree/assume-unchanged before publication)" ;;
    esac
done < "$WORK/index-tags"

git ls-files --others --exclude-standard -z -- > "$WORK/untracked" ||
    fail "untracked dirty-set discovery failed"
: > "$WORK/paths"
if [ "$NEED_DIRTY_PATHS" = 1 ]; then
    git diff --name-only --no-renames -z HEAD -- > "$WORK/tracked" ||
        fail "tracked dirty-set discovery failed"
    {
        cat "$WORK/tracked"
        cat "$WORK/untracked"
    } | LC_ALL=C sort -zu > "$WORK/paths"
fi

# Build the COMPLETE current source inventory separately from the dirty-path
# API above. A clean tree derives its identity from file bytes, never a Git
# object id. An initialized gitlink is recursively inventoried from its current
# tracked and nonignored-untracked bytes; a missing gitlink gets a distinct
# marker. Direct vendored archive and generated-header inputs are included even
# when ignored, so an active vendor input change supersedes the build identity.
: > "$WORK/tracked-source"
declare -A GITLINK_STATE=()
GITLINK_SEQ=0

append_prefixed_nul()
{
    local input="$1" prefix="$2" output="$3" relative
    while IFS= read -r -d '' relative; do
        printf '%s/%s\0' "$prefix" "$relative" >> "$output"
    done < "$input"
}

collect_gitlink()
{
    local prefix="$1" top physical record meta relative mode stage tag path
    local seq=$GITLINK_SEQ
    GITLINK_SEQ=$((GITLINK_SEQ + 1))
    printf '%s\0' "$prefix" >> "$WORK/tracked-source"
    GITLINK_STATE["$prefix"]=absent
    if [ ! -e "$prefix" ] && [ ! -L "$prefix" ]; then
        return 0
    fi
    [ -d "$prefix" ] && [ ! -L "$prefix" ] ||
        fail "invalid gitlink worktree: $prefix"
    if [ ! -e "$prefix/.git" ]; then
        if find "$prefix" -mindepth 1 -maxdepth 1 -print -quit \
                > "$WORK/gitlink-uninitialized-$seq" 2>/dev/null &&
           [ ! -s "$WORK/gitlink-uninitialized-$seq" ]; then
            GITLINK_STATE["$prefix"]=uninitialized-empty
            return 0
        fi
        fail "nonempty uninitialized gitlink would omit bytes: $prefix"
    fi
    top="$(git -C "$prefix" rev-parse --show-toplevel 2>/dev/null)" ||
        fail "could not resolve gitlink worktree: $prefix"
    top="$(cd "$top" && pwd -P)" ||
        fail "could not canonicalize gitlink worktree: $prefix"
    physical="$(cd "$prefix" && pwd -P)" ||
        fail "could not enter gitlink worktree: $prefix"
    [ "$top" = "$physical" ] ||
        fail "gitlink resolves to a foreign worktree: $prefix"
    GITLINK_STATE["$prefix"]=present

    git -C "$prefix" ls-files -v -z > "$WORK/gitlink-tags-$seq" ||
        fail "could not inspect gitlink index flags: $prefix"
    while IFS= read -r -d '' record; do
        tag="${record:0:1}"
        path="${record:2}"
        case "$tag" in
            S|[a-z]) fail "hidden Git index bit in gitlink path: $prefix/$path" ;;
        esac
    done < "$WORK/gitlink-tags-$seq"

    git -C "$prefix" ls-files --others --exclude-standard -z -- \
        > "$WORK/gitlink-untracked-$seq" ||
        fail "gitlink untracked discovery failed: $prefix"
    if [ "$NEED_DIRTY_PATHS" = 1 ]; then
        git -C "$prefix" diff --name-only --no-renames -z HEAD -- \
            > "$WORK/gitlink-dirty-$seq" ||
            fail "gitlink dirty-set discovery failed: $prefix"
        append_prefixed_nul "$WORK/gitlink-dirty-$seq" "$prefix" \
            "$WORK/paths"
        append_prefixed_nul "$WORK/gitlink-untracked-$seq" "$prefix" \
            "$WORK/paths"
    fi
    append_prefixed_nul "$WORK/gitlink-untracked-$seq" "$prefix" \
        "$WORK/tracked-source"

    git -C "$prefix" ls-files --stage -z -- \
        > "$WORK/gitlink-index-$seq" ||
        fail "gitlink tracked-source discovery failed: $prefix"
    while IFS= read -r -d '' record; do
        meta="${record%%$'\t'*}"
        relative="${record#*$'\t'}"
        mode="${meta%% *}"
        stage="${meta##* }"
        [ "$stage" = 0 ] ||
            fail "unmerged gitlink index stage $stage for path: $prefix/$relative"
        case "$mode" in
            100644|100755|120000)
                printf '%s/%s\0' "$prefix" "$relative" \
                    >> "$WORK/tracked-source"
                ;;
            160000) collect_gitlink "$prefix/$relative" ;;
            *) fail "unsupported gitlink index mode $mode for path: $prefix/$relative" ;;
        esac
    done < "$WORK/gitlink-index-$seq"
}

git ls-files --stage -z -- > "$WORK/tracked-index" ||
    fail "tracked source discovery failed"
while IFS= read -r -d '' record; do
    meta="${record%%$'\t'*}"
    path="${record#*$'\t'}"
    mode="${meta%% *}"
    stage="${meta##* }"
    [ "$stage" = 0 ] ||
        fail "unmerged index stage $stage for path: $path"
    case "$mode" in
        100644|100755|120000) printf '%s\0' "$path" >> "$WORK/tracked-source" ;;
        160000) collect_gitlink "$path" ;;
        *) fail "unsupported tracked index mode $mode for path: $path" ;;
    esac
done < "$WORK/tracked-index"

# Git ignore policy is not build policy. GNU Make selects C sources with
# wildcards and the compiler recursively opens headers/templates beneath these
# roots even when `.gitignore` or the local `.git/info/exclude` hides them from
# `git ls-files --others --exclude-standard`. Inventory every regular file in
# those roots independently. Reject symlinks, sockets, FIFOs, and other special
# nodes there: a compiler-followed symlink would bind only its target pathname,
# not the mutable target bytes. `.git` metadata inside a future nested worktree
# is pruned; initialized gitlinks are handled by collect_gitlink() above.
BUILD_INPUT_ROOTS=(adapters app application config core domain lib ports sdk src tools)
existing_build_roots=()
for path in "${BUILD_INPUT_ROOTS[@]}"; do
    if [ -e "$path" ] || [ -L "$path" ]; then
        [ -d "$path" ] && [ ! -L "$path" ] ||
            fail "build input root is not a real directory: $path"
        existing_build_roots+=("$path")
    fi
done
if [ "${#existing_build_roots[@]}" -gt 0 ]; then
    find "${existing_build_roots[@]}" -name .git -prune -o \
        ! -type d ! -type f -print0 \
        > "$WORK/build-root-unsupported" 2>/dev/null ||
        fail "build input root discovery failed"
    if [ -s "$WORK/build-root-unsupported" ]; then
        IFS= read -r -d '' path < "$WORK/build-root-unsupported" || true
        fail "unsupported compiler input beneath build roots: ${path:-unknown}"
    fi
    find "${existing_build_roots[@]}" -name .git -prune -o \
        -type f -print0 >> "$WORK/tracked-source" 2>/dev/null ||
        fail "build input inventory failed"
fi

# Gitlink discovery appends its own paths after the root diagnostic list was
# sorted. Restore one canonical set for the explicit `paths` mode. Authority
# modes never execute the HEAD-relative diagnostic at all.
if [ "$NEED_DIRTY_PATHS" = 1 ]; then
    LC_ALL=C sort -zu "$WORK/paths" > "$WORK/paths.sorted" ||
        fail "could not canonicalize dirty source inventory"
    mv -- "$WORK/paths.sorted" "$WORK/paths" ||
        fail "could not publish dirty source inventory"
fi

for path in vendor/lib/*.a \
            vendor/tor/libtor.a \
            vendor/tor/src/ext/ed25519/donna/libed25519_donna.a \
            vendor/tor/src/ext/ed25519/ref10/libed25519_ref10.a \
            vendor/tor/src/ext/keccak-tiny/libkeccak-tiny.a; do
    [ -L "$path" ] && fail "linked archive must not be a symlink: $path"
    if [ -e "$path" ] && [ ! -f "$path" ]; then
        fail "linked archive is not a regular file: $path"
    fi
    [ -f "$path" ] && printf '%s\0' "$path" >> "$WORK/tracked-source"
done

# `-Ivendor/include` is global. Several OpenSSL/zlib headers are generated and
# ignored, but they are compiler inputs just as surely as the linked archives.
# Recursively inventory their exact current bytes; never allow a socket/FIFO or
# another unsupported type to hide beneath the include root.
if [ -e vendor/include ] || [ -L vendor/include ]; then
    [ -d vendor/include ] && [ ! -L vendor/include ] ||
        fail "vendor include root is not a real directory"
    find vendor/include -mindepth 1 ! -type d ! -type f \
        -print -quit > "$WORK/vendor-include-unsupported" 2>/dev/null ||
        fail "vendor include input discovery failed"
    [ ! -s "$WORK/vendor-include-unsupported" ] ||
        fail "unsupported compiler input beneath vendor/include"
    find vendor/include -mindepth 1 -type f -print0 \
        >> "$WORK/tracked-source" 2>/dev/null ||
        fail "vendor include inventory failed"
fi
{
    cat "$WORK/tracked-source"
    cat "$WORK/untracked"
} | LC_ALL=C sort -zu > "$WORK/source-paths"

emit_paths()
{
    local path
    while IFS= read -r -d '' path; do
        case "$path" in
            *$'\n'*|*$'\r'*) fail "control character in dirty path" ;;
        esac
        printf '%s\n' "$path"
    done < "$WORK/paths"
}

capture_portable()
{
    local path mode raw_mode digest target state
    {
        printf 'zcl.dev_source_identity.v2\0'
        while IFS= read -r -d '' path; do
            printf 'P\0%s\0' "$path"
            if [ "${GITLINK_STATE[$path]+known}" = known ]; then
                state="${GITLINK_STATE[$path]}"
                printf 'G\0%s\0' "$state"
            elif [ -L "$path" ]; then
                raw_mode="$(stat -c '%f' -- "$path" 2>/dev/null)" ||
                    fail "could not stat symlink: $path"
                canonical_source_mode L "$raw_mode" mode ||
                    fail "noncanonical symlink mode: $path"
                read_symlink_target "$path" target ||
                    fail "could not read symlink: $path"
                digest="$(printf '%s' "$target" | sha256_stream)" ||
                    fail "could not hash symlink: $path"
                printf 'L\0%s\0%s\0' "$mode" "$digest"
            elif [ -f "$path" ]; then
                raw_mode="$(stat -c '%f' -- "$path" 2>/dev/null)" ||
                    fail "could not stat source: $path"
                canonical_source_mode F "$raw_mode" mode ||
                    fail "noncanonical regular-file mode: $path"
                digest="$(sha256_file "$path")" ||
                    fail "could not hash source: $path"
                printf 'F\0%s\0%s\0' "$mode" "$digest"
            elif [ ! -e "$path" ]; then
                printf 'D\0'
            else
                fail "unsupported dirty source type: $path"
            fi
        done < "$WORK/source-paths"
    } > "$WORK/preimage"
    sha256_file "$WORK/preimage"
}

# GNU coreutils can hash every regular dirty file in one process and stat every
# extant path in one process.  The portable implementation above spawned both
# tools once per file; on a 200-file working overlay that consumed ~0.75 s per
# identity, and a verify cycle needs at least capture + compare.  Keep the
# canonical preimage byte-for-byte identical while removing that process storm.
capture_batched()
{
    local path target digest record emitted mode raw_mode
    local path_i=0 existing_i=0 regular_i=0 hash_i=0 batch_start=0
    local batch_size=128
    local -a paths=() types=() existing_paths=() existing_modes=()
    local -a regular_paths=() regular_digests=() batch=()

    while IFS= read -r -d '' path; do
        paths+=("$path")
        if [ "${GITLINK_STATE[$path]+known}" = known ]; then
            types+=(G)
        elif [ -L "$path" ]; then
            types+=(L)
            existing_paths+=("$path")
        elif [ -f "$path" ]; then
            types+=(F)
            existing_paths+=("$path")
            regular_paths+=("$path")
        elif [ ! -e "$path" ]; then
            types+=(D)
        else
            fail "unsupported dirty source type: $path"
        fi
    done < "$WORK/source-paths"

    if [ "${#existing_paths[@]}" -gt 0 ]; then
        : > "$WORK/modes"
        for ((batch_start = 0; batch_start < ${#existing_paths[@]};
              batch_start += batch_size)); do
            batch=("${existing_paths[@]:batch_start:batch_size}")
            stat -c '%f' -- "${batch[@]}" >> "$WORK/modes" 2>/dev/null ||
                fail "dirty source changed while collecting file modes"
        done
        mapfile -t existing_modes < "$WORK/modes"
        [ "${#existing_modes[@]}" -eq "${#existing_paths[@]}" ] ||
            fail "file-mode batch was incomplete"
    fi

    if [ "${#regular_paths[@]}" -gt 0 ]; then
        : > "$WORK/hashes"
        for ((batch_start = 0; batch_start < ${#regular_paths[@]};
              batch_start += batch_size)); do
            batch=("${regular_paths[@]:batch_start:batch_size}")
            sha256sum --zero -- "${batch[@]}" >> "$WORK/hashes" ||
                fail "dirty source changed while hashing regular files"
        done
        while IFS= read -r -d '' record; do
            [ "$hash_i" -lt "${#regular_paths[@]}" ] ||
                fail "regular-file hash batch returned extra rows"
            digest="${record:0:64}"
            emitted="${record:66}"
            [[ "$digest" =~ ^[0-9a-f]{64}$ ]] &&
                [ "${record:64:2}" = "  " ] &&
                [ "$emitted" = "${regular_paths[$hash_i]}" ] ||
                fail "regular-file hash batch was malformed or reordered"
            regular_digests+=("$digest")
            hash_i=$((hash_i + 1))
        done < "$WORK/hashes"
        [ "$hash_i" -eq "${#regular_paths[@]}" ] ||
            fail "regular-file hash batch was incomplete"
    fi

    {
        printf 'zcl.dev_source_identity.v2\0'
        for ((path_i = 0; path_i < ${#paths[@]}; path_i++)); do
            path="${paths[$path_i]}"
            printf 'P\0%s\0' "$path"
            case "${types[$path_i]}" in
                G)
                    printf 'G\0%s\0' "${GITLINK_STATE[$path]}"
                    ;;
                L)
                    raw_mode="${existing_modes[$existing_i]}"
                    existing_i=$((existing_i + 1))
                    canonical_source_mode L "$raw_mode" mode ||
                        fail "noncanonical symlink mode: $path"
                    read_symlink_target "$path" target ||
                        fail "could not read symlink: $path"
                    digest="$(printf '%s' "$target" | sha256_stream)" ||
                        fail "could not hash symlink: $path"
                    printf 'L\0%s\0%s\0' "$mode" "$digest"
                    ;;
                F)
                    raw_mode="${existing_modes[$existing_i]}"
                    existing_i=$((existing_i + 1))
                    canonical_source_mode F "$raw_mode" mode ||
                        fail "noncanonical regular-file mode: $path"
                    digest="${regular_digests[$regular_i]}"
                    regular_i=$((regular_i + 1))
                    printf 'F\0%s\0%s\0' "$mode" "$digest"
                    ;;
                D)
                    printf 'D\0'
                    ;;
                *)
                    fail "internal dirty-source classification failure: $path"
                    ;;
            esac
        done
    } > "$WORK/preimage"
    [ "$existing_i" -eq "${#existing_modes[@]}" ] &&
        [ "$regular_i" -eq "${#regular_digests[@]}" ] ||
        fail "dirty-source batch consumption was incomplete"
    sha256_file "$WORK/preimage"
}

capture()
{
    local help
    if [ "${ZCL_SOURCE_IDENTITY_FORCE_PORTABLE:-0}" != 1 ] &&
       command -v sha256sum >/dev/null 2>&1; then
        help="$(sha256sum --help 2>/dev/null || true)"
        if [[ "$help" == *"--zero"* ]]; then
            capture_batched
            return
        fi
    fi
    capture_portable
}

# Build-session mutation token. Unlike the portable content identity, this is
# deliberately host-local and short-lived: inode + nanosecond mtime/ctime make
# an edit/revert (ABA) visible between pre-build capture and post-link verify.
# It is never baked as a release/source identifier.
mutation_token()
{
    local path record path_i=0 existing_i=0 batch_start=0
    local batch_size=128
    local -a paths=() types=() existing_paths=() metadata=() batch=()
    while IFS= read -r -d '' path; do
        paths+=("$path")
        if [ "${GITLINK_STATE[$path]+known}" = known ]; then
            types+=(G)
        elif [ -L "$path" ] || [ -f "$path" ]; then
            types+=(E)
            existing_paths+=("$path")
        elif [ ! -e "$path" ]; then
            types+=(D)
        else
            fail "unsupported source type while capturing mutation token: $path"
        fi
    done < "$WORK/source-paths"

    : > "$WORK/mutation-metadata"
    for ((batch_start = 0; batch_start < ${#existing_paths[@]};
          batch_start += batch_size)); do
        batch=("${existing_paths[@]:batch_start:batch_size}")
        stat --printf='%d:%i:%s:%f:%y:%z\0' -- "${batch[@]}" \
            >> "$WORK/mutation-metadata" 2>/dev/null ||
            fail "source changed while collecting mutation metadata"
    done
    mapfile -d '' -t metadata < "$WORK/mutation-metadata"
    [ "${#metadata[@]}" -eq "${#existing_paths[@]}" ] ||
        fail "mutation metadata batch was incomplete"

    {
        printf 'zcl.dev_source_mutation.v1\0'
        for ((path_i = 0; path_i < ${#paths[@]}; path_i++)); do
            path="${paths[$path_i]}"
            printf 'P\0%s\0' "$path"
            case "${types[$path_i]}" in
                G) printf 'G\0%s\0' "${GITLINK_STATE[$path]}" ;;
                E)
                    record="${metadata[$existing_i]}"
                    existing_i=$((existing_i + 1))
                    printf 'E\0%s\0' "$record"
                    ;;
                D) printf 'D\0' ;;
                *) fail "internal mutation-token classification failure" ;;
            esac
        done
    } > "$WORK/mutation-preimage"
    [ "$existing_i" -eq "${#metadata[@]}" ] ||
        fail "mutation metadata consumption was incomplete"
    sha256_file "$WORK/mutation-preimage"
}

# Hash the enumerated source set and gitlink presence separately from
# file bytes. The normal mutation token deliberately operates on the captured
# path set; this guard lets capture-record detect a new untracked file, newly
# selected ignored archive, or submodule inventory change that appeared after
# that set was collected.
inventory_token()
{
    local path
    {
        printf 'zcl.dev_source_inventory.v1\0'
        while IFS= read -r -d '' path; do
            printf 'S\0%s\0' "$path"
            if [ "${GITLINK_STATE[$path]+known}" = known ]; then
                printf 'G\0%s\0' "${GITLINK_STATE[$path]}"
            fi
        done < "$WORK/source-paths"
    } > "$WORK/inventory-preimage"
    sha256_file "$WORK/inventory-preimage"
}

capture_record()
{
    local identity clean inventory_before inventory_after
    local mutation_before mutation_after
    inventory_before="$(inventory_token)" || exit $?
    mutation_before="$(mutation_token)" || exit $?
    identity="$(capture)" || exit $?
    mutation_after="$(mutation_token)" || exit $?
    [ "$mutation_before" = "$mutation_after" ] ||
        fail "source mutated during identity capture"
    inventory_after="$("$SELF" inventory-token)" || exit $?
    [ "$inventory_before" = "$inventory_after" ] ||
        fail "source inventory changed during identity capture"
    # The legacy-named `clean` slot is a v2 capture-completeness bit, not a Git
    # cleanliness claim. Exact current bytes already identify dirty worktrees;
    # deriving authority from Git HEAD/gitlink object ids would reintroduce the
    # legacy object-hash dependency this v2 record removes.
    clean=1
    printf '%s %s %s\n' "$identity" "$clean" "$mutation_after"
}

# See ZCL_SOURCE_IDENTITY_SESSION above. Returns the cache file path for a
# token, or fails (caller falls back to an uncached capture) when the token is
# unset or does not match the exact `pid:start` shape this script mints --
# never trust an unrecognized shape as a cache key.
session_cache_path()
{
    local token="$1"
    [[ "$token" =~ ^[1-9][0-9]*:[0-9]+$ ]] || return 1
    printf '%s/build/identity/.session-cache/capture.%s.record' \
        "$ROOT" "${token/:/.}"
}

# A session's cache entry outlives the Make process that minted it (a new
# token every invocation), so entries for dead sessions would otherwise
# accumulate forever. Prune any entry whose encoded pid is no longer the
# exact live process that token identifies -- the same liveness test
# tools/dev/build-epoch-session.sh uses for its epoch leases.
prune_session_cache()
{
    local dir="$1" f base token pid start actual
    [ -d "$dir" ] || return 0
    for f in "$dir"/capture.*.record; do
        [ -e "$f" ] || continue
        base="$(basename -- "$f")"
        token="${base#capture.}"
        token="${token%.record}"
        pid="${token%%.*}"
        start="${token#*.}"
        actual=""
        if [[ "$pid" =~ ^[1-9][0-9]*$ ]] && [[ "$start" =~ ^[0-9]+$ ]]; then
            actual="$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null)" || actual=""
        fi
        [ -n "$actual" ] && [ "$actual" = "$start" ] && continue
        rm -f -- "$f" 2>/dev/null || true
    done
}

# Read-through memoization wrapper around capture_record(). The FIRST call in
# a given ZCL_SOURCE_IDENTITY_SESSION pays the full walk and caches it; every
# later call in the SAME session (the same live Make process) reads the cache
# instead of re-walking. A new session (a new `make` invocation) always misses
# and re-derives from scratch, so this never weakens cross-invocation
# supersession detection -- it only removes repeat work within one process.
capture_record_cached()
{
    local cache tmp record
    cache="$(session_cache_path "$ZCL_SOURCE_IDENTITY_SESSION")" || {
        capture_record
        return
    }
    if [ -f "$cache" ]; then
        cat -- "$cache"
        return
    fi
    record="$(capture_record)" || return $?
    if mkdir -p "$(dirname -- "$cache")" 2>/dev/null; then
        prune_session_cache "$(dirname -- "$cache")"
        tmp="$(mktemp "$(dirname -- "$cache")/.tmp.XXXXXX" 2>/dev/null)" || tmp=""
        if [ -n "$tmp" ]; then
            printf '%s\n' "$record" > "$tmp" && mv -f -- "$tmp" "$cache" ||
                rm -f -- "$tmp"
        fi
    fi
    printf '%s\n' "$record"
}

case "$MODE" in
    paths)
        emit_paths
        ;;
    capture)
        capture
        ;;
    capture-record)
        capture_record_cached
        ;;
    inventory-token)
        inventory_token
        ;;
    verify)
        [[ "$EXPECTED" =~ ^[0-9a-fA-F]{64}$ ]] ||
            fail "verify requires a 64-hex expected identity"
        actual="$(capture)" || exit $?
        if [ "${actual,,}" != "${EXPECTED,,}" ]; then
            fail "source epoch superseded: expected=${EXPECTED,,} actual=${actual,,}"
        fi
        printf '%s\n' "${actual,,}"
        ;;
    verify-record)
        [[ "$EXPECTED" =~ ^[0-9a-fA-F]{64}$ ]] ||
            fail "verify-record requires a 64-hex expected identity"
        [ "$EXPECTED_CLEAN" = 1 ] ||
            fail "verify-record requires v2 capture-completeness bit 1"
        [[ "$EXPECTED_MUTATION" =~ ^[0-9a-fA-F]{64}$ ]] ||
            fail "verify-record requires a 64-hex mutation token"
        actual_record="$(capture_record_cached)" || exit $?
        read -r actual actual_clean actual_mutation <<< "$actual_record"
        if [ "${actual,,}" != "${EXPECTED,,}" ] ||
           [ "$actual_clean" != "$EXPECTED_CLEAN" ] ||
           [ "${actual_mutation,,}" != "${EXPECTED_MUTATION,,}" ]; then
            fail "source build superseded: expected=${EXPECTED,,}/clean=${EXPECTED_CLEAN}/mutation=${EXPECTED_MUTATION,,} actual=${actual,,}/clean=${actual_clean}/mutation=${actual_mutation,,}"
        fi
        printf '%s %s %s\n' "${actual,,}" "$actual_clean" \
            "${actual_mutation,,}"
        ;;
    *)
        echo "usage: tools/dev/source-identity.sh paths|capture|capture-record|verify EXPECTED|verify-record EXPECTED CLEAN MUTATION" >&2
        exit 2
        ;;
esac
