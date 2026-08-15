#!/usr/bin/env bash
# commons_install_batch.sh — install drafted C23 packages into the Commons.
#
#   tools/dev/commons_install_batch.sh <name> <draft-dir> [<name> <draft-dir>]...
#
# Per package this performs the proven pipeline steps:
#   1. copy the draft (include/, src/, tests/, app/, README.md,
#      zcode-package.json) into packages/<name>/, plus the MIT LICENSE;
#   2. rebuild and run its test suite in-tree under sanitizers;
#   3. generate (or reuse) a publisher key in ~/.config/zclassic23;
#   4. run the package factory: exact package, dual independent
#      reproduction (stores A and B), corpus registration, report.
#
# After it exits green, commit packages/, corpus/scopes.def and the
# corpus/factory reports together, then run one corpus census for the
# batch (see docs/work/FORWARD_PLAN.md for the census invocation).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

SIGN=build/bin/zclassic23-package-sign
FACTORY=build/bin/package-factory
KEYDIR="$HOME/.config/zclassic23"
STORE_A="$HOME/.zclassic-c23-commons-factory-a"
STORE_B="$HOME/.zclassic-c23-commons-factory-b"
CUTOFF_HEIGHT=3203194
CUTOFF_MTP=1754524800

if [ "$#" -lt 2 ] || [ $(( $# % 2 )) -ne 0 ]; then
    echo "usage: $0 <name> <draft-dir> [...]" >&2
    exit 2
fi

while [ "$#" -ge 2 ]; do
    name="$1"; draft="$2"; shift 2

    for part in include src tests app README.md zcode-package.json; do
        if [ ! -e "$draft/$part" ]; then
            echo "commons_install_batch: $draft missing $part" >&2
            exit 1
        fi
    done
    if [ -e "packages/$name" ]; then
        echo "commons_install_batch: packages/$name already exists" >&2
        exit 1
    fi

    mkdir -p "packages/$name"
    cp -r "$draft/include" "$draft/src" "$draft/tests" "$draft/app" \
          "$draft/README.md" "$draft/zcode-package.json" "packages/$name/"
    cp packages/zglob/LICENSE "packages/$name/LICENSE"

    # Dependency sources join the local test build (the factory resolves
    # them from the store for the pinned reproduction builds).
    dep_includes=()
    dep_srcs=()
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        dep_includes+=("-Ipackages/$dep/include")
        dep_srcs+=("packages/$dep/src/$dep.c")
    done < <(python3 -c "
import json
m = json.load(open('packages/$name/zcode-package.json'))
for d in m.get('dependencies', []):
    print(d['name'].split('/')[0])
")

    cc -std=c23 -O1 -g -Wall -Wextra -Werror -pedantic \
       -fsanitize=address,undefined \
       -I"packages/$name/include" "${dep_includes[@]}" \
       "packages/$name/src/$name.c" "packages/$name/tests/test_$name.c" \
       "${dep_srcs[@]}" \
       -o "/tmp/commons-install-$name"
    "/tmp/commons-install-$name"

    key="$KEYDIR/commons-publisher-$name.key"
    mkdir -p "$KEYDIR"
    if [ ! -f "$key" ]; then
        "$SIGN" --generate "$key" >/dev/null
        chmod 600 "$key"
    fi
    pubkey=$("$SIGN" --public --key-fd 9 9<"$key")

    "$FACTORY" run --package "packages/$name" \
        --publisher-key-file "$key" --publisher-pubkey "$pubkey" \
        --store-a "$STORE_A" --store-b "$STORE_B" \
        --report "corpus/factory/$name.report.json" \
        --kind human \
        --cutoff-height "$CUTOFF_HEIGHT" --cutoff-mtp "$CUTOFF_MTP" \
        --register-corpus --census-def corpus/scopes.def

    echo "commons_install_batch: $name installed"
done

echo "commons_install_batch: batch complete"
