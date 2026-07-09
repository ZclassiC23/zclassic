#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Shared deterministic stamp primitives for vendored static archives.
# This file deliberately knows nothing about dependency versions or recipes;
# build_vendor.sh supplies the checked expected descriptor, and dep_audit.sh
# asks that same builder contract to verify the installed archive byte-for-byte.

VP_SCHEMA="zclassic23.vendor-provenance.v1"

vp_sha256_file() {
    sha256sum "$1" | awk '{print $1}'
}

vp_sha256_text() {
    printf '%s' "$1" | sha256sum | awk '{print $1}'
}

# Hash a tool's stable identity without assuming it implements compiler-only
# options.  In particular, `perl -dumpmachine` means "start the debugger for
# umpmachine" and can wedge an unattended vendor build at a DB<1> prompt.
vp_tool_identity_sha() {
    local command_name="$1" version
    local -a command_argv
    read -r -a command_argv <<<"$command_name"
    [[ ${#command_argv[@]} -gt 0 ]] || return 1
    version="$("${command_argv[@]}" --version 2>/dev/null |
        sed -n '/[^[:space:]]/{p;q;}' || true)"
    vp_sha256_text "command=$command_name
version=$version"
}

# Compiler identity additionally binds the target triple.  Keep this separate
# from vp_tool_identity_sha so generic build tools are never probed with
# compiler flags.
vp_compiler_identity_sha() {
    local command_name="$1" version machine
    local -a command_argv
    read -r -a command_argv <<<"$command_name"
    [[ ${#command_argv[@]} -gt 0 ]] || return 1
    version="$("${command_argv[@]}" --version 2>/dev/null |
        sed -n '/[^[:space:]]/{p;q;}' || true)"
    machine="$("${command_argv[@]}" -dumpmachine 2>/dev/null |
        sed -n '1p' || true)"
    vp_sha256_text "command=$command_name
version=$version
machine=$machine"
}

vp_stamp_path() {
    local lib_dir="$1" archive="$2"
    printf '%s/.provenance/%s.stamp' "$lib_dir" "$archive"
}

vp_render_stamp() {
    local archive_path="$1" descriptor="$2"
    printf '%s\n' "$descriptor"
    printf 'artifact_sha256=%s\n' "$(vp_sha256_file "$archive_path")"
    printf 'artifact_size=%s\n' "$(wc -c <"$archive_path" | tr -d ' ')"
}

vp_write_stamp() {
    local lib_dir="$1" archive="$2" descriptor="$3"
    local archive_path="$lib_dir/$archive" stamp tmp
    [[ -f "$archive_path" ]] || return 1
    stamp="$(vp_stamp_path "$lib_dir" "$archive")"
    mkdir -p "$(dirname "$stamp")"
    tmp="${stamp}.tmp.$$"
    vp_render_stamp "$archive_path" "$descriptor" >"$tmp"
    chmod 0644 "$tmp"
    mv -f "$tmp" "$stamp"
}

vp_verify_stamp() {
    local lib_dir="$1" archive="$2" descriptor="$3"
    local archive_path="$lib_dir/$archive" stamp expected
    archive_path="$lib_dir/$archive"
    stamp="$(vp_stamp_path "$lib_dir" "$archive")"
    [[ -f "$archive_path" && -f "$stamp" ]] || return 1
    expected="${stamp}.verify.$$"
    vp_render_stamp "$archive_path" "$descriptor" >"$expected"
    if cmp -s "$expected" "$stamp"; then
        rm -f "$expected"
        return 0
    fi
    rm -f "$expected"
    return 1
}

vp_stamp_sha256() {
    local stamp
    stamp="$(vp_stamp_path "$1" "$2")"
    [[ -f "$stamp" ]] || return 1
    vp_sha256_file "$stamp"
}

# Integrity lock for the one prebuilt archive committed to git. Its manifest
# is intentionally explicit when upstream source provenance is unresolved;
# changing the bytes requires a reviewed manifest change, and the manifest's
# replacement policy forbids blessing new bytes without proven source inputs.
vp_verify_locked_manifest() {
    local archive_path="$1" manifest="$2" expected_archive="$3"
    [[ -f "$archive_path" && -f "$manifest" ]] || return 1
    grep -qx "schema=zclassic23.committed-vendor-manifest.v1" "$manifest" ||
        return 1
    grep -qx "archive=$expected_archive" "$manifest" || return 1
    grep -qx 'replacement_policy=source-proof-required' "$manifest" ||
        return 1
    local want_sha want_size got_sha got_size
    want_sha="$(sed -n 's/^archive_sha256=//p' "$manifest")"
    want_size="$(sed -n 's/^archive_size=//p' "$manifest")"
    [[ "$want_sha" =~ ^[0-9a-f]{64}$ && "$want_size" =~ ^[0-9]+$ ]] ||
        return 1
    got_sha="$(vp_sha256_file "$archive_path")"
    got_size="$(wc -c <"$archive_path" | tr -d ' ')"
    [[ "$got_sha" == "$want_sha" && "$got_size" == "$want_size" ]]
}
