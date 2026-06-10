#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_coins_lookup_nullcheck.sh — ensure controller-level coins lookups
# are gated by the P24.14 chainstate safety guard.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

hits=$(grep -rl --include='*.c' 'coins_view_cache_get_coins\s*(' \
    app/controllers/src 2>/dev/null || true)

if [[ -z "$hits" ]]; then
    echo "check_coins_lookup_nullcheck: clean — no controller coin lookups found"
    exit 0
fi

missing=""
while IFS= read -r file; do
    [[ -z "$file" ]] && continue
    if ! grep -q 'rpc_require_chainstate_lookup_ready\s*(' "$file"; then
        missing+="$file"$'\n'
    fi
done <<< "$hits"

missing="${missing%$'\n'}"

if [[ -n "$missing" ]]; then
    echo "check_coins_lookup_nullcheck: missing rpc_require_chainstate_lookup_ready in:"
    echo
    echo "$missing"
    exit 1
fi

echo "check_coins_lookup_nullcheck: clean — all controller coin lookups guarded"
exit 0
