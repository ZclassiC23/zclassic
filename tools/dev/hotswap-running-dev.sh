#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Phase-0 resident hot-swap containment boundary.  Source/event path lists are
# not publication authority: an event subset cannot prove the complete source
# epoch, and Git HEAD/object ids (including SHA-1 ids) are display trace only.
# Reopening this entry point requires the Phase-3 immutable build epoch,
# receipts, exact SHA-256 source-id CAS, resident CAS, and durable rollback.

set -uo pipefail

fail()
{
    printf '[dev-hotswap] FATAL: %s\n' "$*" >&2
    exit 2
}

# Refuse before parsing caller paths, building, probing, opening RPC, or
# touching a resident registry.  `make hotswap-so` and simulation/tests remain
# the non-publishing development surfaces.
fail 'runtime publication and resident probing are contained; use make hotswap-so plus build/test verification'
