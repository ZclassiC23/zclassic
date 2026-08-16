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
# Store pair and declared provenance kind are overridable so a poisoned
# package name (a failed publish claims the name in that store) can be
# rerouted to a fresh store pair, and AI-drafted batches are labeled ai.
STORE_A="${ZCL_COMMONS_STORE_A:-$HOME/.zclassic-c23-commons-factory-a}"
STORE_B="${ZCL_COMMONS_STORE_B:-$HOME/.zclassic-c23-commons-factory-b}"
KIND="${ZCL_COMMONS_KIND:-human}"
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
    # them from the store for the pinned reproduction builds). The closure
    # is TRANSITIVE (a pinned dep's own pins are compiled too, e.g.
    # zotp -> zsha1) and every src/*.c of a multi-file dep participates.
    dep_includes=()
    dep_srcs=()
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        dep_includes+=("-Ipackages/$dep/include")
        for s in "packages/$dep"/src/*.c; do
            dep_srcs+=("$s")
        done
    done < <(python3 -c "
import json
seen = []
def visit(name):
    short = name.split('/')[0]
    if short in seen:
        return
    try:
        m = json.load(open('packages/%s/zcode-package.json' % short))
    except OSError:
        return
    for d in m.get('dependencies', []):
        visit(d['name'])
    seen.append(short)
m = json.load(open('packages/$name/zcode-package.json'))
for d in m.get('dependencies', []):
    visit(d['name'])
for short in seen:
    print(short)
")

    # The package itself may be multi-file (src/*.c).
    pkg_srcs=()
    for s in "packages/$name"/src/*.c; do
        pkg_srcs+=("$s")
    done

    cc -std=c23 -O1 -g -Wall -Wextra -Werror -pedantic \
       -fsanitize=address,undefined \
       -I"packages/$name/include" "${dep_includes[@]}" \
       "${pkg_srcs[@]}" "packages/$name/tests/test_$name.c" \
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
        --kind "$KIND" \
        --cutoff-height "$CUTOFF_HEIGHT" --cutoff-mtp "$CUTOFF_MTP" \
        --register-corpus --census-def corpus/scopes.def

    echo "commons_install_batch: $name installed"
done

echo "commons_install_batch: batch complete"
