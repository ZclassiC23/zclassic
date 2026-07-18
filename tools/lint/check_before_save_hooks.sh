#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_before_save_hooks.sh — critical models must WIRE before_save hooks
# (Makefile `check-before-save-hooks` gate): each of utxo/block/wallet_key/
# wallet_tx must contain an ar_register_before_save(...) call (a bare
# 'before_save' comment does not count). Extracted verbatim from the former
# inline Makefile recipe for tools/lint/run_lint.sh + standalone use.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

for model in utxo block wallet_key wallet_tx; do
    f=app/models/src/$model.c
    test -f "$f" \
    || { echo "FAIL: $f missing (model file moved/renamed)"; exit 1; }
    grep -qE 'ar_register_before_save[[:space:]]*\(' "$f" \
    || { echo "FAIL: $f does not WIRE a before_save hook (no ar_register_before_save(...) call; a bare 'before_save' comment does not count)"; exit 1; }
done
echo "  OK: critical models have before_save hooks"
