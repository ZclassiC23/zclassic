#!/usr/bin/env bash
# Lint gate E5 — Job stages advance OR block; they never silently spin (HARD).
#
# A Job (app/jobs/src/*_stage.c) is the single-purpose, idempotent step
# function the supervisor ticks. The Job contract (FRAMEWORK.md §3) is that
# every step is honest about non-progress: when it cannot move the chain it
# must surface the blocked/idle outcome AND it must reason about a cursor (the
# stage's position in the log). A step that can ONLY return JOB_ADVANCED, or
# that never references a cursor, is the silent-halt anti-pattern — it spins
# forward with no way to say "I am stuck and here is the position I am stuck
# at."
#
# Scope: every app/jobs/src/*.c that is a Job step — i.e. it either calls
# stage_create(...) (registers a stage with the kernel) or defines a
# `job_result_t <name>_step*(...)` entry point. Each such file MUST contain:
#
#   1. at least one JOB_BLOCKED or JOB_IDLE return — the honest non-progress
#      outcome; and
#   2. at least one cursor reference — one of `cursor_out`, `c->cursor_in`,
#      or `stage_cursor` — proving the step reasons about its log position.
#
# The current 8 stages all satisfy both, so this gate runs HARD: any Job step
# file missing either property fails immediately.
#
# Override: a Job step that legitimately cannot block (none exist today) may
# carry a file-level `// stage-advance-ok:<tag>` marker (no space after the
# colon, non-empty tag) on any line.
set -euo pipefail

cd "$(dirname "$0")/../.."

JOBS_DIR=app/jobs/src

fail=0
violations=()

file_overridden() {
    grep -qE '//[[:space:]]*stage-advance-ok:[A-Za-z][A-Za-z0-9_-]*' "$1"
}

is_job_step_file() {
    grep -qE 'stage_create[[:space:]]*\(|job_result_t[[:space:]]+[a-z0-9_]+_step' "$1"
}

[ -d "$JOBS_DIR" ] || { echo "check_stage_advances_or_blocks: $JOBS_DIR not found"; exit 1; }

checked=0
while IFS= read -r f; do
    is_job_step_file "$f" || continue
    file_overridden "$f" && continue
    checked=$((checked + 1))

    if ! grep -qE 'JOB_BLOCKED|JOB_IDLE' "$f"; then
        violations+=("$f: Job step never returns JOB_BLOCKED/JOB_IDLE — it can only advance, so it has no way to surface a stall")
        fail=1
    fi
    if ! grep -qE 'cursor_out|c->cursor_in|stage_cursor' "$f"; then
        violations+=("$f: Job step references no cursor (cursor_out / c->cursor_in / stage_cursor) — it does not reason about its log position")
        fail=1
    fi
done < <(find "$JOBS_DIR" -type f -name '*.c' | sort)

if [ "$fail" = "0" ]; then
    echo "check_stage_advances_or_blocks: clean — all $checked Job step file(s) advance-or-block with a cursor reference"
    exit 0
fi

echo ""
echo "check_stage_advances_or_blocks: ${#violations[@]} Job-contract violation(s)"
echo ""
for v in "${violations[@]}"; do
    echo "  $v"
done
echo ""
echo "A Job step (app/jobs/src/*_stage.c) must be honest about non-progress:"
echo "  1. Return JOB_BLOCKED or JOB_IDLE on any non-advancing path (never"
echo "     spin forward silently)."
echo "  2. Reference a cursor (cursor_out / c->cursor_in / stage_cursor) so"
echo "     the stall is anchored to a log position."
echo "  3. For a Job that genuinely cannot block, add a file-level"
echo "     '// stage-advance-ok:<tag>' marker (no exemptions exist today)."
exit 1
