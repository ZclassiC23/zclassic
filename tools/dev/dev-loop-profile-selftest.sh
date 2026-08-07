#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Prove that ordinary development profiles cannot acquire release-only LTO.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
profiles="$(make -s -C "$ROOT" dev-loop-profile-flags)"

fail()
{
    printf 'check-dev-loop-profiles: FAIL: %s\n' "$*" >&2
    exit 1
}

profile_line()
{
    local name="$1"
    printf '%s\n' "$profiles" | awk -F '\t' -v wanted="$name" \
        '$1 == wanted { print; found = 1 } END { if (!found) exit 1 }'
}

for name in DEV_LIVE DEV_RESTART INTEGRATION; do
    line="$(profile_line "$name")" || fail "missing $name profile"
    case " $line " in
        *' -flto'*|*' -fuse-linker-plugin'*)
            fail "$name contains release-only LTO flags: $line"
            ;;
    esac
done

release="$(profile_line RELEASE)" || fail 'missing RELEASE profile'
case " $release " in
    *' -flto'*) ;;
    *) fail 'RELEASE no longer carries whole-program LTO' ;;
esac

git -C "$ROOT" grep -q '\$(DEV_RESTART_CFLAGS) \$(DEV_RESTART_LDFLAGS)' -- Makefile ||
    fail 'incremental dev link is not owned by DEV_RESTART'
git -C "$ROOT" grep -q '\$(DEV_LIVE_CFLAGS) -fPIC' -- Makefile ||
    fail 'resident module compiler is not owned by DEV_LIVE'
git -C "$ROOT" grep -q 'resident action plan contains release-only LTO flags' -- \
    tools/dev/devloop_hotswap_build.c ||
    fail 'resident action-plan LTO refusal is missing'

printf '%s\n' \
    'check-dev-loop-profiles: PASS — DEV_LIVE/DEV_RESTART/INTEGRATION are non-LTO; RELEASE retains LTO'
