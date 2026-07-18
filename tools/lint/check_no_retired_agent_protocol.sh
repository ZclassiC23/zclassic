#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# The typed native command registry is the sole agent interface. Reject the
# retired three-letter protocol as a semantic token in tracked paths or file
# contents. Embedded byte-copy and Tor configuration words are not tokens.

set -euo pipefail

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
REPO_ROOT="$(cd "$(dirname "$SCRIPT_PATH")/../.." && pwd)"
SCAN_ROOT="${ZCL_RETIRED_PROTOCOL_ROOT:-$REPO_ROOT}"
retired="$(printf '%s%s%s' 'm' 'c' 'p')"

run_scan()
{
    local root="$1" path lower_path raw matches rc=0 regular=0 path_violation=0
    local content_violation=0
    local -a tracked=()

    if ! git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "check_no_retired_agent_protocol: FATAL — '$root' is not a git worktree" >&2
        return 2
    fi

    mapfile -d '' -t tracked < <(git -C "$root" ls-files -z)
    if [[ "${#tracked[@]}" -eq 0 ]]; then
        echo "check_no_retired_agent_protocol: FATAL — tracked-file scan is empty" >&2
        return 2
    fi

    for path in "${tracked[@]}"; do
        [[ -f "$root/$path" ]] || continue
        regular=$((regular + 1))
        lower_path="${path,,}"
        if [[ "$lower_path" == *"$retired"* ]]; then
            printf 'FAIL: retired agent protocol in tracked path: %s\n' "$path"
            path_violation=1
        fi
    done
    if [[ "$regular" -eq 0 ]]; then
        echo "check_no_retired_agent_protocol: FATAL — no tracked regular files were scanned" >&2
        return 2
    fi

    set +e
    raw="$(git -C "$root" grep -n -I -i -F "$retired" -- .)"
    rc=$?
    set -e
    if [[ "$rc" -ge 2 ]]; then
        echo "check_no_retired_agent_protocol: FATAL — tracked-content scan failed (exit $rc)" >&2
        return 2
    fi
    if [[ "$rc" -eq 0 ]]; then
        # The byte-copy family and Tor's NumCPUs option contain the same three
        # letters incidentally. Remove only those complete, known identifiers;
        # any remaining occurrence (including Open<X>Client-style embedding)
        # is a retired-interface reference.
        matches="$(printf '%s\n' "$raw" |
            sed -E 's/__builtin_memcpy|memcpy_uses_blob_var|memcpys|memcpy|numcpus//gI' |
            awk -v needle="$retired" '
                index(tolower($0), needle) != 0 { print }
            ')"
    fi
    if [[ -n "$matches" ]]; then
        printf '%s\n' "$matches"
        content_violation=1
    fi

    if [[ "$path_violation" -ne 0 || "$content_violation" -ne 0 ]]; then
        echo "FAIL: retired agent protocol remains in tracked files."
        return 1
    fi
    printf '  OK: %d tracked regular files contain no retired protocol token\n' "$regular"
    return 0
}

selftest_tmp=""
cleanup_selftest()
{
    if [[ -n "$selftest_tmp" && -d "$selftest_tmp" ]]; then
        rm -rf -- "$selftest_tmp"
    fi
}

selftest()
{
    local bad_name
    selftest_tmp="$(mktemp -d)"
    trap cleanup_selftest EXIT HUP INT TERM
    git -C "$selftest_tmp" init -q

    printf '%s\n' 'memcpy(buffer, source, length);' 'numcpus=4' >"$selftest_tmp/clean.c"
    git -C "$selftest_tmp" add clean.c
    ZCL_RETIRED_PROTOCOL_ROOT="$selftest_tmp" "$SCRIPT_PATH" >/dev/null || {
        echo "selftest: clean embedded substrings were rejected" >&2
        return 1
    }

    printf 'Open%sClient\n' "${retired^}" >"$selftest_tmp/untracked.txt"
    ZCL_RETIRED_PROTOCOL_ROOT="$selftest_tmp" "$SCRIPT_PATH" >/dev/null || {
        echo "selftest: untracked fixture entered the production scan" >&2
        return 1
    }

    git -C "$selftest_tmp" add untracked.txt
    if ZCL_RETIRED_PROTOCOL_ROOT="$selftest_tmp" "$SCRIPT_PATH" >/dev/null; then
        echo "selftest: tracked content violation was not detected" >&2
        return 1
    fi
    git -C "$selftest_tmp" rm -q --cached untracked.txt
    rm -f -- "$selftest_tmp/untracked.txt"

    bad_name="old_${retired}_surface.txt"
    printf '%s\n' 'clean body' >"$selftest_tmp/$bad_name"
    git -C "$selftest_tmp" add "$bad_name"
    if ZCL_RETIRED_PROTOCOL_ROOT="$selftest_tmp" "$SCRIPT_PATH" >/dev/null; then
        echo "selftest: tracked path violation was not detected" >&2
        return 1
    fi

    echo "check_no_retired_agent_protocol: selftest PASS"
}

if [[ "${1:-}" == "--selftest" ]]; then
    selftest
    exit $?
fi
if [[ "$#" -ne 0 ]]; then
    echo "usage: $0 [--selftest]" >&2
    exit 2
fi

echo "══ LINT: retired agent protocol absent from tracked files ══"
run_scan "$SCAN_ROOT"
