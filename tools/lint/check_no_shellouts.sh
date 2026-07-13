#!/usr/bin/env bash
# check-no-shellouts: no system()/popen()/execlp() in the always-running node
# binary's own code (os-substrate Rung 0, docs/work/os-substrate-plan.md §1).
#
# Every shell-out site in the node has been migrated onto the two in-tree
# primitives — lib/util file_tree_ops (fd-based cp/rm) and lib/util spawn
# (zcl_spawn_detached / zcl_spawn_capture, no /bin/sh). A shell-out inside the
# resident process is also what blocks the Rung-2 seccomp `execve` deny-list,
# so this gate is HARD: a new one fails the build.
#
# Out of scope (allowlisted): standalone dev/bench/CLI binaries under tools/
# (a human invokes them directly; they are never wrapped by the node sandbox)
# and the test tree under lib/test/ (fixtures spin up real processes). The
# spawn primitive itself lives under lib/util and calls execvp()/fork() — those
# are not in the forbidden pattern set, so it needs no exception here.
#
# Mode: WARN | FAIL (controlled by ZCL_LINT_MODE; default FAIL).
set -euo pipefail

MODE="${ZCL_LINT_MODE:-FAIL}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$ROOT"

roots=()
for root in app lib src config; do
    [[ -d "$root" ]] && roots+=("$root")
done

matches=$(
    grep -rn --include='*.c' --include='*.h' \
        -E '\b(system|popen|execlp)\s*\(' \
        "${roots[@]}" 2>/dev/null \
    | grep -v '^lib/test/' \
    | grep -v '// shellout-ok' \
    | grep -vE ':[0-9]+:[[:space:]]*(\*|//|/\*)' \
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

echo "[check_no_shellouts] $violations violation(s) found (mode: $MODE)"
echo "[check_no_shellouts] the node must not shell out — use lib/util spawn"
echo "[check_no_shellouts] (zcl_spawn_detached/zcl_spawn_capture) or"
echo "[check_no_shellouts] lib/util file_tree_ops (zcl_tree_copy/zcl_tree_remove);"
echo "[check_no_shellouts] add // shellout-ok for a documented, reviewed exception"

if (( violations > 0 )) && [[ "$MODE" == "FAIL" ]]; then
    exit 1
fi
exit 0
