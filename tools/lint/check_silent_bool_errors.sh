#!/usr/bin/env bash
# check_silent_bool_errors — RATCHET gate (shrink-only).
#
# The check-silent-errors-{,services,controllers,jobs,conditions} gates enforce
# "every error return logs context" — but only for the int/`return -1;` error
# convention. The bool/`return false;` error idiom slips through. This gate
# closes that blind spot for the highest-signal form: a SWALLOWED CALL FAILURE —
#
#     if (!some_call(...))
#         return false;          // no LOG_*, no // raw-return-ok: marker
#
# where a fallible call's failure is propagated as a bare `false` with no
# diagnostic context. (Plain predicate returns — `if (!ok) return false;` on a
# local bool — are NOT flagged; they are legitimate negative results, and
# gating them would be noise.)
#
# RATCHET: today's population is grandfathered in the baseline; the gate fails
# only when a NEW (previously-unseen) swallowed call-failure appears. Debt can
# only shrink. Stable key = "<relpath>::<guarded_call_name>" so editing lines
# above a hit does not churn the baseline (unlike a file:line key).
#
# Known limitation (documented, accepted for a ratchet): two silent guards of
# the SAME call in the SAME file collapse to one key, so re-introducing a guard
# of an already-listed call in that file is not caught. New DISTINCT swallowed
# calls — the common regression — are caught.
#
# Escape hatch: `// raw-return-ok:<reason>` (or `/* raw-return-ok:<reason> */`)
# on the guard line or the return line, same as the int-convention gates.
#
# Usage:
#   ./tools/lint/check_silent_bool_errors.sh              # FAIL mode (CI/lint)
#   ZCL_LINT_MODE=UPDATE ./tools/lint/check_silent_bool_errors.sh   # shrink/regen baseline
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
# shellcheck source=tools/lint/scan_exclusions.sh
source "$SCRIPT_DIR/scan_exclusions.sh"
BASELINE="tools/lint/silent_bool_errors_baseline.txt"
DIRS="${ZCL_SILENT_BOOL_SCAN_DIRS_FOR_TEST:-app/controllers/src app/services/src app/jobs/src app/conditions/src app/models/src app/views/src app/supervisors/src app/events/src}"
MODE="${ZCL_LINT_MODE:-FAIL}"

scan() {
  for d in $DIRS; do
    [ -d "$d" ] || continue
    grep -rn 'return false;' "$d" --include='*.c' "${LINT_GREP_EXCLUDE_ARGS[@]}" 2>/dev/null \
      | grep -vE 'LOG_ERR|LOG_FAIL|LOG_RETURN|LOG_WARN|LOG_NULL|log_json' \
      | grep -vE '(//|/\*) raw-return-ok:' \
      | while IFS= read -r hit; do
          file=$(printf '%s' "$hit" | cut -d: -f1)
          lnum=$(printf '%s' "$hit" | cut -d: -f2)
          [ -n "$file" ] && [ -n "$lnum" ] || continue
          prev=$((lnum - 1))
          pl=$(sed -n "${prev}p" "$file")
          # A fallible call-guard on the previous line: if (!ident(  ...
          call=$(printf '%s' "$pl" \
            | grep -oE 'if \(![a-zA-Z_][a-zA-Z0-9_]*\(' \
            | grep -oE '[a-zA-Z_][a-zA-Z0-9_]*\(' | head -1 | tr -d '(')
          [ -n "$call" ] || continue
          # prev line must not itself log or be marked
          printf '%s' "$pl" | grep -qE 'LOG_|log_json|raw-return-ok:' && continue
          rel=${file#./}
          printf '%s::%s\n' "$rel" "$call"
        done
  done | sort -u
}

CUR=$(scan)

if [ "$MODE" = "UPDATE" ]; then
  {
    echo "# check_silent_bool_errors RATCHET baseline (shrink-only)."
    echo "# Stable key = <relpath>::<guarded_call>. A swallowed call failure:"
    echo "#   if (!call(...)) return false;   with no LOG_* and no // raw-return-ok:"
    echo "# Regenerate after fixing some: ZCL_LINT_MODE=UPDATE ./tools/lint/check_silent_bool_errors.sh"
    printf '%s\n' "$CUR"
  } > "$BASELINE"
  echo "check_silent_bool_errors: baseline updated ($(printf '%s' "$CUR" | grep -c '::') entries)"
  exit 0
fi

BASE=$(grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$BASELINE" 2>/dev/null | sort -u)
NEW=$(comm -23 <(printf '%s\n' "$CUR") <(printf '%s\n' "$BASE") | grep -E '::' || true)
if [ -n "$NEW" ]; then
  echo "FAIL: new silent call-guard 'return false' (log the failure via LOG_WARN/LOG_FAIL, or mark // raw-return-ok:<reason>):"
  printf '%s\n' "$NEW"
  exit 1
fi

n_cur=$(printf '%s' "$CUR" | grep -c '::')
n_base=$(printf '%s' "$BASE" | grep -c '::')
GONE=$(comm -13 <(printf '%s\n' "$CUR") <(printf '%s\n' "$BASE") | grep -c '::' || true)
echo "  OK: no new silent call-guard return-false ($n_cur tracked; baseline $n_base)"
[ "${GONE:-0}" -gt 0 ] && echo "  (ratchet: $GONE fixed since baseline — run ZCL_LINT_MODE=UPDATE to shrink)"
exit 0
