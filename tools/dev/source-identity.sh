#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Supersession identity for HEAD plus the visible dirty Git overlay. This is a
# verify-loop diagnostic, not `zcl.dev_source_epoch.v1` and not publication
# authority: ignored generated/vendor inputs, compiler, and full configuration
# are intentionally deferred to the immutable Phase-3 epoch schema. Any hidden
# index bit, discovery error, unsupported dirty type, or concurrent visible edit
# still fails closed for the narrower contract.

set -euo pipefail

MODE="${1:-capture}"
EXPECTED="${2:-}"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "source-identity: not inside a Git worktree" >&2
    exit 2
}
cd "$ROOT"

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
        sha256sum -- "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 -- "$1" | awk '{print $1}'
    else
        fail "no SHA-256 implementation is available"
    fi
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

git diff --name-only --no-renames -z HEAD -- > "$WORK/tracked" ||
    fail "tracked dirty-set discovery failed"
git ls-files --others --exclude-standard -z -- > "$WORK/untracked" ||
    fail "untracked dirty-set discovery failed"
{
    cat "$WORK/tracked"
    cat "$WORK/untracked"
} | LC_ALL=C sort -zu > "$WORK/paths"

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
    local head path mode digest target
    head="$(git rev-parse --verify HEAD 2>/dev/null)" ||
        fail "could not resolve HEAD"
    {
        printf 'zcl.dev_source_identity.v1\0%s\0' "$head"
        while IFS= read -r -d '' path; do
            printf 'P\0%s\0' "$path"
            if [ -L "$path" ]; then
                mode="$(stat -c '%f' -- "$path" 2>/dev/null)" ||
                    fail "could not stat symlink: $path"
                target="$(readlink -- "$path")" ||
                    fail "could not read symlink: $path"
                digest="$(printf '%s' "$target" | sha256_stream)" ||
                    fail "could not hash symlink: $path"
                printf 'L\0%s\0%s\0' "$mode" "$digest"
            elif [ -f "$path" ]; then
                mode="$(stat -c '%f' -- "$path" 2>/dev/null)" ||
                    fail "could not stat source: $path"
                digest="$(sha256_file "$path")" ||
                    fail "could not hash source: $path"
                printf 'F\0%s\0%s\0' "$mode" "$digest"
            elif [ ! -e "$path" ]; then
                printf 'D\0'
            else
                fail "unsupported dirty source type: $path"
            fi
        done < "$WORK/paths"
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
    local head path target digest record emitted mode
    local path_i=0 existing_i=0 regular_i=0 hash_i=0 batch_start=0
    local batch_size=128
    local -a paths=() types=() existing_paths=() existing_modes=()
    local -a regular_paths=() regular_digests=() batch=()

    head="$(git rev-parse --verify HEAD 2>/dev/null)" ||
        fail "could not resolve HEAD"

    while IFS= read -r -d '' path; do
        paths+=("$path")
        if [ -L "$path" ]; then
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
    done < "$WORK/paths"

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
        printf 'zcl.dev_source_identity.v1\0%s\0' "$head"
        for ((path_i = 0; path_i < ${#paths[@]}; path_i++)); do
            path="${paths[$path_i]}"
            printf 'P\0%s\0' "$path"
            case "${types[$path_i]}" in
                L)
                    mode="${existing_modes[$existing_i]}"
                    existing_i=$((existing_i + 1))
                    target="$(readlink -- "$path")" ||
                        fail "could not read symlink: $path"
                    digest="$(printf '%s' "$target" | sha256_stream)" ||
                        fail "could not hash symlink: $path"
                    printf 'L\0%s\0%s\0' "$mode" "$digest"
                    ;;
                F)
                    mode="${existing_modes[$existing_i]}"
                    existing_i=$((existing_i + 1))
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

case "$MODE" in
    paths)
        emit_paths
        ;;
    capture)
        capture
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
    *)
        echo "usage: tools/dev/source-identity.sh paths|capture|verify EXPECTED" >&2
        exit 2
        ;;
esac
