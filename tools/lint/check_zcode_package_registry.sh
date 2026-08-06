#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Purpose: verify package roots and exact-once monolith source ownership.
set -euo pipefail

checker="build/bin/zcode-package-registry-check"
if [[ ! -x "$checker" ]]; then
    echo "check-zcode-package-registry: FAIL — missing $checker" >&2
    exit 1
fi

"$checker"

mapfile -t package_sources < <(
    git ls-files 'lib/base/src/*.c' 'lib/sha3/src/*.c' 'lib/codec/src/*.c' |
        LC_ALL=C sort
)
mapfile -t monolith_sources < <(
    make -s --no-print-directory print-zcode-monolith-lib-sources |
        sed -n '/^lib\/.*\/src\/.*\.c$/p'
)

if (( ${#package_sources[@]} == 0 || ${#monolith_sources[@]} == 0 )); then
    echo "check-zcode-package-registry: FAIL — empty package or monolith source projection" >&2
    exit 1
fi

for source in "${package_sources[@]}"; do
    count=0
    for compiled in "${monolith_sources[@]}"; do
        [[ "$compiled" == "$source" ]] && ((count += 1))
    done
    if (( count != 1 )); then
        echo "check-zcode-package-registry: FAIL — $source appears $count times in LIB_SRCS" >&2
        exit 1
    fi
done

codec_consumers=(
    lib/vcs/src/package_release.c
    lib/vcs/src/package_recipe.c
    lib/vcs/src/package_deps.c
)
for source in "${codec_consumers[@]}"; do
    if ! git grep -q '#include "codec/cursor.h"' -- "$source"; then
        echo "check-zcode-package-registry: FAIL — $source does not use the bounded codec cursor" >&2
        exit 1
    fi
done
if git grep -n -E 'vcs_(wr|rd)_u(16|32|64)le|#include "vcs_priv.h"' -- \
        "${codec_consumers[@]}"; then
    echo "check-zcode-package-registry: FAIL — package release/recipe/lock restored a private codec" >&2
    exit 1
fi

echo "zcode package registry: ${#package_sources[@]} authoritative package sources occur exactly once in monolith LIB_SRCS"
echo "zcode package registry: release, recipe and lock wires use codec/cursor.h exclusively"
