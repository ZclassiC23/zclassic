#!/usr/bin/env bash
# Doc-count lint gate — machine-checks the numeric claims docs make about code.
#
# Doc count rot is silent: code grows a new test group / port / adapter and the
# docs still cite the old number (or, worse, the prose drifts to a number that
# was never true, e.g. "15 ports + 10 sqlite impls"). This gate has two prongs:
#
#   (A) CANONICAL BLOCK (HARD): a machine-readable declaration in
#       docs/CODEBASE_MAP.md delimited by
#         <!-- DOC-COUNTS-BEGIN --> ... <!-- DOC-COUNTS-END -->
#       containing `key: value` lines for test_groups / port_interfaces /
#       persistence_adapters / condition_registrations. This gate measures the
#       real counts from the code and FAILS if the declared counts disagree. To
#       fix a real drift: update the canonical block (and any prose) to match the
#       code — never the other way around; the code is authoritative for what
#       exists.
#
#   (B) STALE-PHRASE DENYLIST (best-effort regression guard): scans the prose of
#       CLAUDE.md + docs/**/*.md for a small set of compound number-phrases that
#       are UNAMBIGUOUSLY stale at the current counts (e.g. "15 ports",
#       "10 sqlite impls", "460 parallel groups", "1500+ tests"). Compound
#       phrases only (number+unit together) => no false positives on bare
#       numbers. This catches the exact historical drifts this gate was created
#       to prevent from creeping back.
#
# Standalone-runnable. NOT yet wired into the Makefile `lint:` target (a separate
# worker owns Makefile targets this round) — invoke directly, or wire with a
# `check-doc-counts:` target that calls this script. Fast: filesystem + grep only,
# no build, no test run.
#
# Source of truth: the CODE.
set -euo pipefail

cd "$(dirname "$0")/../.."

fail=0
fail_lines=()

add_fail() { fail_lines+=("$1"); fail=1; }

# --------------------------------------------------------------------------
# Measure the real counts from the code. These mirror the commands in the task
# definition; keep them in sync with how the counts are defined.
# --------------------------------------------------------------------------
test_groups_file=lib/test/src/test_parallel.c
ports_glob='ports/include/ports/*.h'
adapters_glob='adapters/outbound/persistence/src/*.c'
conditions_glob='app/conditions/src/*.c'

if [ ! -f "$test_groups_file" ]; then
    echo "FAIL: $test_groups_file not found (run from repo root)" >&2
    exit 1
fi

# Count parallel test groups: one per `X(<name>)` registration macro.
code_test_groups=$(grep -oE 'X\([a-z_0-9]+\)' "$test_groups_file" | wc -l)
code_ports=$(ls $ports_glob 2>/dev/null | wc -l)
code_adapters=$(ls $adapters_glob 2>/dev/null | wc -l)
code_conditions=$(grep -RhoE 'condition_register[[:space:]]*\(' \
    $conditions_glob 2>/dev/null | wc -l)

echo "code-measured: test_groups=$code_test_groups port_interfaces=$code_ports persistence_adapters=$code_adapters condition_registrations=$code_conditions"

# --------------------------------------------------------------------------
# (A) Canonical block in docs/CODEBASE_MAP.md must agree with the code.
# --------------------------------------------------------------------------
DOC=docs/CODEBASE_MAP.md
[ -f "$DOC" ] || { echo "FAIL: $DOC not found" >&2; exit 1; }

block=$(awk '/<!-- DOC-COUNTS-BEGIN -->/{f=1;next} /<!-- DOC-COUNTS-END -->/{f=0} f' "$DOC")
if [ -z "${block//[[:space:]]/}" ]; then
    echo "FAIL: missing or empty <!-- DOC-COUNTS-BEGIN/END --> block in $DOC"
    echo "      Add test_groups / port_interfaces / persistence_adapters /"
    echo "      condition_registrations declarations"
    echo "      (see the block format in $DOC) so the counts can be machine-checked."
    exit 1
fi

get_declared() { # $1=key
    local line
    line=$(echo "$block" | grep -E "^[[:space:]]*${1}[[:space:]]*:" | head -1) || true
    echo "${line##*:}" | tr -d '[:space:]'
}

dec_test_groups=$(get_declared test_groups)
dec_ports=$(get_declared port_interfaces)
dec_adapters=$(get_declared persistence_adapters)
dec_conditions=$(get_declared condition_registrations)

[ -n "$dec_test_groups" ] || add_fail "test_groups not declared in $DOC DOC-COUNTS block"
[ -n "$dec_ports" ]       || add_fail "port_interfaces not declared in $DOC DOC-COUNTS block"
[ -n "$dec_adapters" ]    || add_fail "persistence_adapters not declared in $DOC DOC-COUNTS block"
[ -n "$dec_conditions" ]  || add_fail "condition_registrations not declared in $DOC DOC-COUNTS block"

[ "${dec_test_groups:-_}" = "$code_test_groups" ] || \
    add_fail "test_groups MISMATCH — code=$code_test_groups doc-says=${dec_test_groups:-<blank>} (update the DOC-COUNTS block in $DOC)"
[ "${dec_ports:-_}" = "$code_ports" ] || \
    add_fail "port_interfaces MISMATCH — code=$code_ports doc-says=${dec_ports:-<blank>} (update the DOC-COUNTS block in $DOC)"
[ "${dec_adapters:-_}" = "$code_adapters" ] || \
    add_fail "persistence_adapters MISMATCH — code=$code_adapters doc-says=${dec_adapters:-<blank>} (update the DOC-COUNTS block in $DOC)"
[ "${dec_conditions:-_}" = "$code_conditions" ] || \
    add_fail "condition_registrations MISMATCH — code=$code_conditions doc-says=${dec_conditions:-<blank>} (update the DOC-COUNTS block in $DOC)"

# --------------------------------------------------------------------------
# (B) Stale-phrase denylist. Compound number+unit phrases only — these were
# wrong when this gate was authored and must never return. Adding a phrase here
# is safe as long as it cannot appear in a correct context.
# --------------------------------------------------------------------------
# Build the denylist dynamically from the current counts so it stays correct as
# the code grows: any "N parallel groups" / "N port" / "N sqlite impls" that
# does NOT match the current count is suspect. But to avoid false positives on
# unrelated numbers, we only flag a closed set of historically-wrong phrasings.

denylist=(
    # test-group drifts (current: 487)
    '1500+ tests' '1500 tests' '460 parallel groups' '460 test groups' '486 groups' '486 parallel groups'
    # ports drifts (current: 12)
    '15 ports'
    # adapter drifts (current: 13 persistence adapters; "10 sqlite impls" was wrong)
    '10 sqlite impls'
    # condition drifts (current: 30 registered conditions)
    '28 conditions live'
)

# Docs to scan: CLAUDE.md at repo root + everything under docs/.
scan_files=(CLAUDE.md)
while IFS= read -r f; do scan_files+=("$f"); done < <(find docs -type f -name '*.md' 2>/dev/null)

for phrase in "${denylist[@]}"; do
    # -F fixed-string (phrases contain regex metachars like '+'), -I skip binary,
    # -n line numbers. One grep per phrase over all scan files.
    while IFS= read -r match_line; do
        [ -n "$match_line" ] || continue
        add_fail "stale phrase \"$phrase\" → $match_line  (remove or correct the prose)"
    done < <(grep -rnIF -- "$phrase" "${scan_files[@]}" 2>/dev/null || true)
done

# --------------------------------------------------------------------------
# Report.
# --------------------------------------------------------------------------
if [ "$fail" != "0" ]; then
    echo ""
    echo "FAIL: doc-count drift detected."
    printf '    %s\n' "${fail_lines[@]}"
    echo ""
    echo "Fix: the CODE is authoritative."
    echo "  - For a MISMATCH: update the <!-- DOC-COUNTS --> block in $DOC"
    echo "    AND any prose (FRAMEWORK.md, CLAUDE.md, docs/BUILD.md, docs/HANDOFF.md) to match."
    echo "  - For a stale phrase: delete or correct the prose (the number is wrong)."
    exit 1
fi

echo "check_doc_counts: clean — test_groups=$code_test_groups port_interfaces=$code_ports persistence_adapters=$code_adapters condition_registrations=$code_conditions, docs agree and no stale phrasings found"
exit 0
