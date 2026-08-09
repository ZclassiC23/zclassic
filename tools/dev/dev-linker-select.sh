#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# purpose: Select a verified host-local dev linker, with portable fallback.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RECEIPT="${ZCL_DEV_LINKER_RECEIPT:-$ROOT/.cache/zcl-dev-loop/linker-shootout.json}"

if command -v jq >/dev/null 2>&1 && [ -r "$RECEIPT" ]; then
    selected="$(jq -r '
      if .schema == "zcl.dev_linker_shootout.v1" and
         .status == "complete" and .selected.verified == true
      then .selected.name else "" end' "$RECEIPT" 2>/dev/null || true)"
    case "$selected" in
        mold) command -v mold >/dev/null 2>&1 && printf '%s' '-fuse-ld=mold' && exit 0;;
        lld) command -v ld.lld >/dev/null 2>&1 && printf '%s' '-fuse-ld=lld' && exit 0;;
        gold) command -v ld.gold >/dev/null 2>&1 && printf '%s' '-fuse-ld=gold' && exit 0;;
        bfd) command -v ld.bfd >/dev/null 2>&1 && printf '%s' '-fuse-ld=bfd' && exit 0;;
    esac
fi

if command -v mold >/dev/null 2>&1; then
    printf '%s' '-fuse-ld=mold'
elif command -v ld.lld >/dev/null 2>&1; then
    printf '%s' '-fuse-ld=lld'
elif command -v ld.gold >/dev/null 2>&1; then
    printf '%s' '-fuse-ld=gold'
fi
