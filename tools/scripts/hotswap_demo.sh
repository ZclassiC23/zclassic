#!/usr/bin/env sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Legacy resident commit harness, retained only as a typed compatibility path.
# Phase 0 contains both MCP/native apply and resident probing before dlopen.
set -eu

printf '%s\n' \
  'hotswap_demo: REFUSING — resident generation loading/publication is contained.' \
  'Use make hotswap-so, make t ONLY=hotswap_loader, and make hotswap-sim.' >&2
exit 3
