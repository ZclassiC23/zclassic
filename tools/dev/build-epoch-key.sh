#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Produce the fail-closed compiler/profile keys used by Make's cached object
# epochs.  The portable source authority remains independent of mtimes and Git
# history.  This host-local compile-session key intentionally also binds the
# mutation token (including nanosecond mtime/ctime metadata), so edit/revert
# ABA sessions cannot late-publish objects compiled during a transient epoch.
# Exact source bytes/completeness, mutation, compiler/tool bytes, and effective
# flags together select the disposable local object/candidate namespace.

set -euo pipefail

WORK=""
cleanup()
{
    [ -z "$WORK" ] || rm -rf -- "$WORK"
}
trap cleanup EXIT
trap 'exit 2' HUP INT TERM

fail()
{
    printf 'build-epoch-key: %s\n' "$*" >&2
    exit 2
}

sha256_file()
{
    sha256sum < "$1" | awk '{print $1}'
}

is_sha256()
{
    [[ "${1:-}" =~ ^[0-9a-f]{64}$ ]]
}

MODE="${1:-}"
shift || true

case "$MODE" in
compiler-id)
    CC_COMMAND="${1:-}"
    CXX_COMMAND="${2:-${1:-}}"
    [ -n "$CC_COMMAND" ] || fail 'compiler-id requires the effective CC command'

    # Make's compiler commands are a whitespace-separated argv (normally
    # `cc`, `ccache cc`, or `sccache cc`).  Accept only tokens whose shell
    # interpretation is identical to `read -a`: quotes, escapes, expansions,
    # comments, globs, redirects, assignments in argv[0], and control syntax
    # all fail closed instead of being fingerprinted differently from Make.
    safe_command()
    {
        local label="$1" command="$2"
        [[ "$command" =~ ^[[:space:]]*[A-Za-z0-9_./:+,=%-]+([[:space:]]+[A-Za-z0-9_./:+,=%-]+)*[[:space:]]*$ ]] ||
            fail "$label contains unsupported shell syntax"
    }
    safe_command CC "$CC_COMMAND"
    safe_command CXX "$CXX_COMMAND"
    read -r -a CC_ARGV <<< "$CC_COMMAND"
    read -r -a CXX_ARGV <<< "$CXX_COMMAND"
    [ "${#CC_ARGV[@]}" -gt 0 ] || fail 'CC parsed to an empty argv'
    [ "${#CXX_ARGV[@]}" -gt 0 ] || fail 'CXX parsed to an empty argv'
    case "${CC_ARGV[0]}" in -*|*=*) fail 'CC argv[0] is not an executable token' ;; esac
    case "${CXX_ARGV[0]}" in -*|*=*) fail 'CXX argv[0] is not an executable token' ;; esac
    command -v "${CC_ARGV[0]}" >/dev/null 2>&1 ||
        fail "compiler command not found: ${CC_ARGV[0]}"
    command -v "${CXX_ARGV[0]}" >/dev/null 2>&1 ||
        fail "C++ compiler command not found: ${CXX_ARGV[0]}"

    # GNU Make exports scheduling/control state to recipes but not uniformly to
    # parse-time $(shell ...). GCC -v echoes MAKEFLAGS even though it does not
    # affect generated code. Normalize those orchestration-only variables so a
    # -j change cannot impersonate a toolchain replacement.
    unset MAKEFLAGS MFLAGS MAKELEVEL

    WORK="$(mktemp -d "${TMPDIR:-/tmp}/zcl-build-compiler.XXXXXX")" ||
        fail 'could not create compiler fingerprint workspace'
    PREIMAGE="$WORK/compiler.preimage"
    : > "$PREIMAGE"
    printf 'zcl.build_compiler_identity.v2\0cc_command\0%s\0cxx_command\0%s\0' \
        "$CC_COMMAND" "$CXX_COMMAND" \
        >> "$PREIMAGE"

    # These variables can redirect headers, compiler subprograms, libraries,
    # or an SDK without changing argv.  Bind unset-vs-set and the exact value.
    for env_name in CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH COMPILER_PATH \
            GCC_EXEC_PREFIX LIBRARY_PATH SDKROOT LD_LIBRARY_PATH \
            SOURCE_DATE_EPOCH; do
        if [[ -v "$env_name" ]]; then
            printf 'environment\0%s\0set\0%s\0' "$env_name" "${!env_name}" \
                >> "$PREIMAGE"
        else
            printf 'environment\0%s\0unset\0' "$env_name" >> "$PREIMAGE"
        fi
    done
    while IFS= read -r env_name; do
        case "$env_name" in CCACHE_*|SCCACHE_*)
            printf 'environment\0%s\0set\0%s\0' "$env_name" "${!env_name}" \
                >> "$PREIMAGE"
            ;;
        esac
    done < <(compgen -e | LC_ALL=C sort -u)

    declare -A SEEN_TOOL=()
    fingerprint_tool()
    {
        local label="$1" requested="$2" resolved digest metadata
        [ -n "$requested" ] || return 0
        if [[ "$requested" == */* ]]; then
            resolved="$(readlink -f -- "$requested" 2>/dev/null || true)"
        else
            resolved="$(command -v -- "$requested" 2>/dev/null || true)"
            [ -n "$resolved" ] &&
                resolved="$(readlink -f -- "$resolved" 2>/dev/null || true)"
        fi
        [ -n "$resolved" ] && [ -f "$resolved" ] || return 0
        case "${SEEN_TOOL[$resolved]+seen}" in seen) return 0 ;; esac
        SEEN_TOOL["$resolved"]=1
        digest="$(sha256_file "$resolved")" ||
            fail "could not hash compiler tool: $resolved"
        metadata="$(stat -Lc '%d:%i:%s:%Y:%Z:%y:%z' "$resolved" 2>/dev/null)" ||
            fail "could not stat compiler tool: $resolved"
        printf 'tool\0%s\0%s\0%s\0%s\0' \
            "$label" "$resolved" "$digest" "$metadata" \
            >> "$PREIMAGE"
    }

    # Fingerprint wrappers and explicit compiler argv programs.
    for token in "${CC_ARGV[@]}"; do
        case "$token" in -*) continue ;; esac
        fingerprint_tool argv "$token"
    done
    for token in "${CXX_ARGV[@]}"; do
        case "$token" in -*) continue ;; esac
        fingerprint_tool cxx-argv "$token"
    done

    probe()
    {
        local label="$1" output rc
        shift
        set +e
        output="$("${CC_ARGV[@]}" "$@" </dev/null 2>&1)"
        rc=$?
        set -e
        printf 'probe\0%s\0%d\0%s\0' "$label" "$rc" "$output" \
            >> "$PREIMAGE"
    }

    probe version --version
    probe machine -dumpmachine
    probe compiler-version -dumpfullversion -dumpversion
    probe search-dirs -print-search-dirs
    probe c-include-search -E -x c -v -
    probe c-builtins -dM -E -x c -

    wrapper_base="$(basename -- "${CC_ARGV[0]}")"
    case "$wrapper_base" in
        ccache)
            set +e
            wrapper_config="$("${CC_ARGV[0]}" --show-config 2>&1)"
            rc=$?
            set -e
            printf 'wrapper-config\0ccache\0%d\0%s\0' "$rc" "$wrapper_config" \
                >> "$PREIMAGE"
            ;;
        sccache)
            # sccache has no stable show-config command; its admitted SCCACHE_*
            # environment is bound above and its conventional config file is
            # hashed below. Never bind live cache statistics/counters.
            printf 'wrapper-config\0sccache\0environment-plus-file\0' >> "$PREIMAGE"
            ;;
    esac
    for wrapper_config_path in \
            "${CCACHE_CONFIGPATH:-}" \
            "${HOME:-}/.config/ccache/ccache.conf" \
            "${HOME:-}/.ccache/ccache.conf" \
            "${SCCACHE_CONF:-}" \
            "${HOME:-}/.config/sccache/config"; do
        [ -n "$wrapper_config_path" ] || continue
        [ -f "$wrapper_config_path" ] &&
            fingerprint_tool wrapper-config-file "$wrapper_config_path"
    done

    probe_cxx()
    {
        local label="$1" output rc
        shift
        set +e
        output="$("${CXX_ARGV[@]}" "$@" </dev/null 2>&1)"
        rc=$?
        set -e
        printf 'cxx-probe\0%s\0%d\0%s\0' "$label" "$rc" "$output" \
            >> "$PREIMAGE"
    }
    probe_cxx version --version
    probe_cxx machine -dumpmachine
    probe_cxx include-search -E -x c++ -v -
    probe_cxx builtins -dM -E -x c++ -

    # GCC/Clang drivers dispatch to these programs. Hash the resolved bytes,
    # not just a marketing version line, so an in-place toolchain replacement
    # cannot silently reuse old cached objects.
    for program in cc1 cc1plus collect2 lto1 as ld; do
        set +e
        resolved="$("${CC_ARGV[@]}" "-print-prog-name=$program" 2>/dev/null)"
        rc=$?
        set -e
        [ "$rc" -eq 0 ] || continue
        fingerprint_tool "driver-$program" "$resolved"
    done

    # Bind every linker implementation the Make profile may auto-select and
    # the runtime/startup archives selected by the drivers.
    for program in ld ld.lld mold; do
        resolved="$(command -v -- "$program" 2>/dev/null || true)"
        [ -n "$resolved" ] && fingerprint_tool "linker-$program" "$resolved"
    done
    # GCC accepts arbitrary -fuse-ld=<name> values and searches for ld.<name>
    # in its program path. Fingerprint every available linker-shaped executable
    # in the admitted driver/PATH search, not only today's bfd/lld/mold names.
    program_dirs="$("${CC_ARGV[@]}" -print-search-dirs 2>/dev/null |
        sed -n 's/^programs: *=//p')"
    IFS=: read -r -a linker_dirs <<< "${program_dirs}:${PATH:-}"
    for linker_dir in "${linker_dirs[@]}"; do
        [ -n "$linker_dir" ] || linker_dir=.
        [ -d "$linker_dir" ] || continue
        while IFS= read -r -d '' resolved; do
            [ -x "$resolved" ] && fingerprint_tool arbitrary-linker "$resolved"
        done < <(find "$linker_dir" -maxdepth 1 \( -type f -o -type l \) \
            -name 'ld*' -print0 2>/dev/null | LC_ALL=C sort -z)
    done
    for asset in libgcc.a libgcc_s.so libgcc_s.so.1 libstdc++.a libstdc++.so \
            libc.so libm.so libpthread.so libdl.so Scrt1.o crt1.o crti.o \
            crtn.o crtbegin.o crtbeginS.o crtend.o crtendS.o; do
        set +e
        resolved="$("${CXX_ARGV[@]}" "-print-file-name=$asset" 2>/dev/null)"
        rc=$?
        set -e
        [ "$rc" -eq 0 ] || continue
        fingerprint_tool "driver-asset-$asset" "$resolved"
    done

    # Header/search-root bytes are too expensive to hash on every Make parse,
    # but their complete host metadata inventory is cheap and ABA-sensitive:
    # an edit/revert changes ctime even when bytes and mtime are restored.  The
    # include-search probes above bind order and compiler preprocessing state.
    : > "$WORK/search-roots"
    collect_search_roots()
    {
        local -n argv_ref="$1"
        local output in_list=0 line root
        output="$("${argv_ref[@]}" -E -x c -v - </dev/null 2>&1 || true)"
        while IFS= read -r line; do
            case "$line" in
                '#include <...> search starts here:') in_list=1; continue ;;
                'End of search list.') in_list=0; continue ;;
            esac
            [ "$in_list" -eq 1 ] || continue
            root="${line# }"
            root="${root% (framework directory)}"
            [ -d "$root" ] && printf '%s\0' "$root" >> "$WORK/search-roots"
        done <<< "$output"
    }
    collect_search_roots CC_ARGV
    collect_search_roots CXX_ARGV
    for env_name in CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH COMPILER_PATH \
            LIBRARY_PATH SDKROOT; do
        [[ -v "$env_name" ]] || continue
        IFS=: read -r -a env_roots <<< "${!env_name}"
        for root in "${env_roots[@]}"; do
            [ -n "$root" ] || root=.
            [ -d "$root" ] && printf '%s\0' "$root" >> "$WORK/search-roots"
        done
    done
    LC_ALL=C sort -zu "$WORK/search-roots" -o "$WORK/search-roots"
    while IFS= read -r -d '' root; do
        resolved="$(readlink -f -- "$root" 2>/dev/null || true)"
        [ -n "$resolved" ] && [ -d "$resolved" ] || continue
        printf 'search-root\0%s\0%s\0' "$root" "$resolved" >> "$PREIMAGE"
        find -L "$resolved" \( -type f -o -type l \) \
            -printf '%P\0%D:%i:%s:%T@:%C@:%m:%y:%l\0' 2>/dev/null |
            LC_ALL=C sort -z >> "$PREIMAGE" ||
            fail "could not inventory compiler search root: $resolved"
        while IFS= read -r -d '' linked; do
            target="$(readlink -f -- "$linked" 2>/dev/null || true)"
            [ -f "$target" ] && fingerprint_tool search-symlink-target "$target"
        done < <(find "$resolved" -type l -print0 2>/dev/null)
        printf '\0search-root-end\0' >> "$PREIMAGE"
    done < "$WORK/search-roots"

    sha256_file "$PREIMAGE"
    ;;

key)
    SOURCE_ID="${1:-}"
    COMPLETE="${2:-}"
    MUTATION="${3:-}"
    COMPILER_ID="${4:-}"
    PROFILE="${5:-}"
    COMPILE_FLAGS="${6:-}"
    LINK_FLAGS="${7:-}"
    is_sha256 "$SOURCE_ID" || fail 'key requires a lowercase SHA-256 source id'
    [ "$COMPLETE" = 1 ] || fail 'key requires capture completeness bit 1'
    is_sha256 "$MUTATION" || fail 'key requires a source mutation token'
    is_sha256 "$COMPILER_ID" || fail 'key requires a compiler fingerprint'
    [ -n "$PROFILE" ] || fail 'key requires a nonempty profile name'
    case "$PROFILE" in *$'\n'*|*$'\r'*) fail 'profile contains a control line' ;; esac
    case "$COMPILE_FLAGS $LINK_FLAGS" in
        *@*) fail 'response-file syntax is forbidden in compile/link flags' ;;
    esac

    WORK="$(mktemp -d "${TMPDIR:-/tmp}/zcl-build-epoch.XXXXXX")" ||
        fail 'could not create build epoch workspace'
    {
        printf 'zcl.build_compile_epoch.v1\0'
        printf 'source_id_sha256\0%s\0' "$SOURCE_ID"
        printf 'capture_complete\0%s\0' "$COMPLETE"
        printf 'source_mutation_sha256\0%s\0' "$MUTATION"
        printf 'compiler_id_sha256\0%s\0' "$COMPILER_ID"
        printf 'profile\0%s\0' "$PROFILE"
        printf 'compile_flags\0%s\0' "$COMPILE_FLAGS"
        printf 'link_flags\0%s\0' "$LINK_FLAGS"
    } > "$WORK/epoch.preimage"
    sha256_file "$WORK/epoch.preimage"
    ;;

*)
    fail 'usage: build-epoch-key.sh compiler-id CC [CXX] | key SOURCE COMPLETE MUTATION COMPILER PROFILE COMPILE_FLAGS LINK_FLAGS'
    ;;
esac
