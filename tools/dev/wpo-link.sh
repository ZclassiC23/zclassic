#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Serialize whole-program LTO links under one checkout-wide flock. Make's
# jobserver keeps per-TU compiles (and the link's own LTRANS phase) parallel;
# this lock only stops two concurrent makes from running two WPA links at the
# same time. The command's exit status propagates unchanged.

set -euo pipefail

fail()
{
    printf 'wpo-link: %s\n' "$*" >&2
    exit 2
}

[ "$#" -ge 2 ] || fail 'usage: wpo-link.sh LOCKFILE CMD [ARG...]'

LOCKFILE="$1"
shift

mkdir -p -- "$(dirname -- "$LOCKFILE")"

if command -v flock >/dev/null 2>&1; then
    exec flock -x "$LOCKFILE" "$@"
fi

printf 'wpo-link: flock unavailable; running WITHOUT link serialization\n' >&2
exec "$@"
