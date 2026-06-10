#!/usr/bin/env bash
# Gate #18: every app/*.c file lives under a framework shape folder.
# Mode: WARN | RATCHET | FAIL (controlled by ZCL_LINT_MODE; default WARN).
#   WARN    — report violations, always exit 0 (Phase 0 measurement).
#   RATCHET — fail only on violations NOT in framework_shape_allowlist.txt
#             (the baseline). Allowlisted violations are tolerated; the
#             allowlist may only shrink. This is the E10 graduation mode.
#   FAIL    — fail on ANY violation, allowlist ignored.
set -euo pipefail

MODE="${ZCL_LINT_MODE:-WARN}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ALLOWLIST="$SCRIPT_DIR/framework_shape_allowlist.txt"

cd "$ROOT"

declare -A ALLOWED=()
if [[ -f "$ALLOWLIST" ]]; then
    while IFS= read -r line; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [[ -z "$line" ]] && continue
        ALLOWED["$line"]=1
    done < "$ALLOWLIST"
fi

is_known_shape_path() {
    local path="$1"
    case "$path" in
        app/controllers/src/*.c|\
        app/services/src/*.c|\
        app/models/src/*.c|\
        app/jobs/src/*.c|\
        app/supervisors/src/*.c|\
        app/conditions/src/*.c|\
        app/events/src/*.c|\
        app/views/src/*.c)
            return 0
            ;;
    esac
    return 1
}

scanned=0
violations=0
allowlisted=0

# In RATCHET mode the allowlist IS the baseline: allowlisted files are
# tolerated, non-allowlisted violations fail. In FAIL mode the allowlist
# is ignored (any violation fails).
while IFS= read -r file; do
    scanned=$((scanned + 1))
    if is_known_shape_path "$file"; then
        continue
    fi
    if [[ "$MODE" != "FAIL" && -n "${ALLOWED[$file]:-}" ]]; then
        allowlisted=$((allowlisted + 1))
        continue
    fi
    violations=$((violations + 1))
    echo "$file: not in a known shape folder (expected one of: controllers, services, models, jobs, supervisors, conditions, events, views)" >&2
done < <(find app -type f -name '*.c' | sort)

echo "[framework_shape_check] scanned $scanned .c files in app/"
echo "[framework_shape_check] $violations violation(s) found (mode: $MODE)"
if (( allowlisted > 0 )); then
    echo "[framework_shape_check] $allowlisted allowlisted violation(s) ignored"
fi
echo "[framework_shape_check] write to tools/lint/framework_shape_allowlist.txt to allowlist existing violations"

if (( violations > 0 )) && [[ "$MODE" == "FAIL" || "$MODE" == "RATCHET" ]]; then
    exit 1
fi
exit 0
