#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_silent_error_returns.sh — silent `return -1;` gate for one app layer
# (Makefile `check-silent-errors-{services,controllers,jobs,conditions}`).
# Every error return must be paired with a LOG_ERR/LOG_FAIL/LOG_RETURN (or an
# error log on the immediately preceding line), or carry a
# `// raw-return-ok:<reason>` marker. One parameterized script backs the four
# per-layer gates; extracted verbatim from the former inline Makefile recipes
# for tools/lint/run_lint.sh + standalone use.
#
# Usage: check_silent_error_returns.sh <scan-dir> <fail-label> <ok-label> <fix-hint>
#   fail-label: layer name used in the FAIL line ("found in services")
#   ok-label:   layer name used in the OK line ("all service error returns")

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

DIR="${1:?usage: check_silent_error_returns.sh <scan-dir> <fail-label> <ok-label> <fix-hint>}"
FAIL_LABEL="${2:?usage: check_silent_error_returns.sh <scan-dir> <fail-label> <ok-label> <fix-hint>}"
OK_LABEL="${3:?usage: check_silent_error_returns.sh <scan-dir> <fail-label> <ok-label> <fix-hint>}"
HINT="${4:?usage: check_silent_error_returns.sh <scan-dir> <fail-label> <ok-label> <fix-hint>}"

HITS=$(grep -rn -B1 'return -1;' "$DIR" --include='*.c' \
    | grep 'return -1;' \
    | grep -v 'LOG_ERR\|LOG_FAIL\|LOG_RETURN\|log_json' \
    | grep -vE '(//|/\*) raw-return-ok:[A-Za-z][A-Za-z0-9_-]+' \
    | while read -r line; do
        file=$(echo "$line" | cut -d: -f1)
        lnum=$(echo "$line" | cut -d: -f2)
        prev=$((lnum - 1))
        prev_line=$(sed -n "${prev}p" "$file")
        echo "$prev_line" | grep -qE 'LOG_ERR|LOG_FAIL|LOG_RETURN|log_json.*error' || echo "$line"
    done || true)
if [ -n "$HITS" ]; then
    echo "$HITS"
    echo "FAIL: silent error returns found in $FAIL_LABEL ($HINT)"
    exit 1
fi
echo "  OK: all $OK_LABEL error returns logged"
