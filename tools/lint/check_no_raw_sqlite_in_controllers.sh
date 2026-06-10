#!/usr/bin/env bash
# Gate #20: controllers should not prepare/exec SQLite directly.
# Mode: WARN | RATCHET | FAIL (controlled by ZCL_LINT_MODE; default WARN).
#   WARN    — report violations, always exit 0 (Phase 0 measurement).
#   RATCHET — fail only on a violation in a file NOT listed in
#             no_raw_sqlite_in_controllers_baseline.txt. Baselined files
#             are tolerated; the baseline may only shrink. (E10 graduation.)
#   FAIL    — fail on ANY violation, baseline ignored.
set -euo pipefail

MODE="${ZCL_LINT_MODE:-WARN}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BASELINE="$SCRIPT_DIR/no_raw_sqlite_in_controllers_baseline.txt"

cd "$ROOT"

# Load baseline (set of relative file paths allowed to carry raw sqlite).
declare -A BASELINED=()
if [[ -f "$BASELINE" ]]; then
    while IFS= read -r line; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [[ -z "$line" ]] && continue
        BASELINED["$line"]=1
    done < "$BASELINE"
fi

roots=()
for root in app/controllers tools/mcp/controllers; do
    [[ -d "$root" ]] && roots+=("$root")
done

matches=$(
    grep -rn --include='*.c' --include='*.h' \
        -E '\bsqlite3_prepare_v2\s*\(|\bsqlite3_exec\s*\(' \
        "${roots[@]}" 2>/dev/null \
    | grep -v '// raw-controller-sql-ok' \
    || true
)

violations=0
if [[ -n "${matches//[[:space:]]/}" ]]; then
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        file="${line%%:*}"
        # In RATCHET mode a baselined file is tolerated. In FAIL mode the
        # baseline is ignored. In WARN mode everything is reported anyway.
        if [[ "$MODE" == "RATCHET" && -n "${BASELINED[$file]:-}" ]]; then
            continue
        fi
        violations=$((violations + 1))
        echo "$line" >&2
    done <<< "$matches"
fi

echo "[check_no_raw_sqlite_in_controllers] $violations violation(s) found (mode: $MODE)"
echo "[check_no_raw_sqlite_in_controllers] use projection_* or models, or add // raw-controller-sql-ok for a documented exception"
if [[ "$MODE" == "RATCHET" ]]; then
    echo "[check_no_raw_sqlite_in_controllers] baselined files in tools/lint/no_raw_sqlite_in_controllers_baseline.txt (ratchet may only shrink)"
fi

if (( violations > 0 )) && [[ "$MODE" == "FAIL" || "$MODE" == "RATCHET" ]]; then
    exit 1
fi
exit 0
