#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Lint gate — no NEW production caller of stage_repair_coin_backfill_try.
#
# The coin-backfill repair ladder is a borrowed-seed-era cure path that should
# shrink after the sovereign refold cutover. Keep the runtime entry point owned
# by reducer_frontier only; new production callers widen the repair fabric and
# must fail review. A second call in the allowed file also fails so the allowed
# surface remains exact, not "anything from this file".
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$ROOT"

SYMBOL='stage_repair_coin_backfill_try('
DEF_FILE='app/jobs/src/stage_repair_coin_backfill.c'
ALLOWED_FILE='app/jobs/src/stage_repair_reducer_frontier_coin.c'

if [ ! -f "$DEF_FILE" ] || ! grep -qF "$SYMBOL" "$DEF_FILE"; then
    echo "check_no_new_coin_backfill_caller: FATAL — '$SYMBOL' no longer found in $DEF_FILE."
    echo "  - If the coin-backfill ladder was deleted, remove this gate and its Makefile wiring."
    echo "  - If it moved or was renamed, update DEF_FILE/SYMBOL so the ratchet keeps firing."
    exit 2
fi

bad=()
allowed_count=0
SCAN_ROOTS=(app config lib tools domain application adapters/outbound/persistence)
while IFS= read -r f; do
    [ -n "$f" ] || continue
    [ "$f" = "$DEF_FILE" ] && continue
    case "$f" in lib/test/*) continue ;; esac

    count=$(grep -oF "$SYMBOL" "$f" | wc -l | tr -d ' ')
    if [ "$f" = "$ALLOWED_FILE" ]; then
        allowed_count=$((allowed_count + count))
    else
        bad+=("$f:$count")
    fi
done < <(grep -rlF --include='*.c' "$SYMBOL" "${SCAN_ROOTS[@]}" 2>/dev/null | sort -u)

if [ "${#bad[@]}" = "0" ] && [ "$allowed_count" = "1" ]; then
    echo "check_no_new_coin_backfill_caller: clean — one allowed production caller"
    exit 0
fi

echo ""
if [ "$allowed_count" != "1" ]; then
    echo "check_no_new_coin_backfill_caller: expected exactly 1 call in $ALLOWED_FILE, found $allowed_count"
fi
if [ "${#bad[@]}" -gt 0 ]; then
    echo "check_no_new_coin_backfill_caller: NEW production caller(s) of $SYMBOL:"
    for v in "${bad[@]}"; do echo "  $v"; done
fi
echo ""
echo "Do NOT add another coin-backfill repair entry caller. Route reducer-frontier"
echo "repair evidence through the existing dispatcher, or delete/shrink this ladder"
echo "after the sovereign refold cure."
exit 1
