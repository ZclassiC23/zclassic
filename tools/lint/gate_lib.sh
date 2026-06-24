# shellcheck shell=bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# gate_lib.sh — shared helpers that make lint gates FAIL-LOUD instead of
# fail-silent. See docs/work/lint-gate-hollowness-audit.md.
#
# A hollow gate reports "clean" exit 0 while a real violation is present:
# its scan set silently emptied (a renamed/moved dir, a `for f in glob`
# that found nothing, a non-GNU grep that exited >=2) and the violation
# loop ran zero times. This file provides two primitives every gate should
# reach for so that "scanned nothing" becomes a LOUD exit 2, never a quiet
# pass:
#
#   gate_require_scanned <count> <floor> <gate-name> [hint]
#       Abort (exit 2) when the realized scan count is below a known floor.
#       Use floor=1 for "must scan at least one file" or a higher integer
#       for a known-population gate.
#
#   gate_grep <args...>
#       A `grep` wrapper that treats exit >=2 (a real grep error: bad flag,
#       unreadable file, broken regex on a non-GNU grep) as FATAL (exit 2)
#       instead of masking it as "no match" the way `grep ... || true` does.
#       Exit 0 (match) and 1 (no match) pass through unchanged. Captures
#       and re-emits grep's stdout so callers can `gate_grep ... < <(...)`.
#
# Sourcing contract: a gate sources this AFTER `set -euo pipefail` and its
# own `cd` to the repo root. These helpers do not change cwd.

# Abort LOUD if the scan set is smaller than a known floor. This is the
# single most important anti-hollow primitive: a gate that derives its scan
# set from find/glob/grep -rl and never asserts non-emptiness will silently
# pass when the producer empties.
gate_require_scanned() {
    local count="$1" floor="$2" name="$3" hint="${4:-}"
    if ! [ "$count" -ge "$floor" ] 2>/dev/null; then
        echo "$name: FATAL — scan set is '${count}' (< floor ${floor})." >&2
        echo "  The scan producer (find/glob/grep) returned too little; a" >&2
        echo "  scanned dir/file was likely renamed, moved, or deleted." >&2
        echo "  Refusing to report 'clean' off a hollow (empty) scan." >&2
        [ -n "$hint" ] && echo "  $hint" >&2
        exit 2
    fi
}

# grep wrapper: exit 0/1 pass through; exit >=2 (real error) is FATAL.
# Echoes grep's stdout so it composes in pipelines / process substitution.
gate_grep() {
    local out rc
    set +e
    out=$(grep "$@")
    rc=$?
    set -e
    if [ "$rc" -ge 2 ]; then
        echo "gate_grep: FATAL — grep failed (exit $rc) for: grep $*" >&2
        echo "  A grep error (bad flag, unreadable file, regex unsupported by" >&2
        echo "  this grep) was about to be masked as 'no match'. Refusing to" >&2
        echo "  report 'clean' off a broken scan." >&2
        exit 2
    fi
    [ -n "$out" ] && printf '%s\n' "$out"
    return "$rc"
}
