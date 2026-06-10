#!/usr/bin/env bash
# Gate #19: no direct clock_gettime/time(NULL)/getrandom outside lib/platform.
# Mode: WARN | FAIL (controlled by ZCL_LINT_MODE; default FAIL for Phase 1)
set -euo pipefail

MODE="${ZCL_LINT_MODE:-FAIL}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$ROOT"

roots=()
for root in app lib config tools; do
    [[ -d "$root" ]] && roots+=("$root")
done

matches=$(
    grep -rn --include='*.c' --include='*.h' \
        -E '\bclock_gettime\s*\(|\btime\s*\(\s*NULL\s*\)|\bgetrandom\s*\(' \
        "${roots[@]}" 2>/dev/null \
    | grep -v '^lib/platform/' \
    | grep -v '^tools/lint/check_no_raw_clock_outside_platform.sh:' \
    | grep -v '// platform-ok' \
    || true
)

violations=0
if [[ -n "${matches//[[:space:]]/}" ]]; then
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        violations=$((violations + 1))
        echo "$line" >&2
    done <<< "$matches"
fi

echo "[check_no_raw_clock_outside_platform] $violations violation(s) found (mode: $MODE)"
echo "[check_no_raw_clock_outside_platform] ratchet now FAIL -- no new raw clock calls allowed"
echo "[check_no_raw_clock_outside_platform] use platform.clock/platform.rng or add // platform-ok for a documented exception"

if (( violations > 0 )) && [[ "$MODE" == "FAIL" ]]; then
    exit 1
fi
exit 0
