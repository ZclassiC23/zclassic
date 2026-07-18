#!/usr/bin/env bash
# Lint gate E11 — DEFENSIVE_CODING.md gate list matches the Makefile (HARD).
#
# Doc rot is silent: the Makefile LINT_GATES target gains/loses a check-* gate
# and DEFENSIVE_CODING.md still claims the old count and names. This gate
# cross-checks the two sources of truth so the doc can never drift from
# the build.
#
# Source of truth A: the LINT_GATES variable in Makefile (the umbrella gate
#   list; historically the literal check-* prerequisites of the `lint:`
#   target, which remains the fallback when LINT_GATES is absent).
# Source of truth B: a machine-readable canonical block in
#   DEFENSIVE_CODING.md delimited by the markers
#     <!-- LINT-GATES-BEGIN --> ... <!-- LINT-GATES-END -->
#   containing one `check-*` token per documented gate (any surrounding
#   markdown is ignored; we extract the check-* tokens).
#
# The gate fails if the COUNT differs or the NAME SET differs. To fix a
# real drift: update the doc block (markers) to match the Makefile — never
# the other way around (the Makefile is authoritative for what runs).
set -euo pipefail

cd "$(dirname "$0")/../.."

MAKEFILE=Makefile
DOC=docs/DEFENSIVE_CODING.md

[ -f "$MAKEFILE" ] || { echo "FAIL: $MAKEFILE not found"; exit 1; }
[ -f "$DOC" ]      || { echo "FAIL: $DOC not found"; exit 1; }

# A) Extract the umbrella gate list. Since the loop-speed lane, the single
#    source of truth is the LINT_GATES variable (a backslash-continued list):
#    the serial fallback runs it as lint: prerequisites, the default path
#    passes it to tools/lint/run_lint.sh. Fall back to the historical
#    literal `lint: dep dep ...` line if LINT_GATES is absent.
lint_gates_block=$(awk '
    /^LINT_GATES[[:space:]]*:=/ { inb=1 }
    inb { print; if ($0 !~ /\\[[:space:]]*$/) exit }
' "$MAKEFILE")
if [ -n "$lint_gates_block" ]; then
    make_gates=$(printf '%s\n' "$lint_gates_block" | grep -oE 'check-[a-z0-9-]+' | sort -u)
else
    lint_line=$(grep -E '^lint:[[:space:]]' "$MAKEFILE" | head -1)
    if [ -z "$lint_line" ]; then
        echo "FAIL: could not find LINT_GATES or a literal 'lint:' target line in $MAKEFILE"
        exit 1
    fi
    make_gates=$(echo "$lint_line" | grep -oE 'check-[a-z0-9-]+' | sort -u)
fi

# B) Extract the check-* tokens from the canonical doc block.
doc_block=$(awk '/<!-- LINT-GATES-BEGIN -->/{f=1;next} /<!-- LINT-GATES-END -->/{f=0} f' "$DOC")
if [ -z "${doc_block//[[:space:]]/}" ]; then
    echo "FAIL: missing or empty <!-- LINT-GATES-BEGIN/END --> block in $DOC"
    echo "      Add the canonical gate list so E11 can verify doc/Makefile parity."
    exit 1
fi
doc_gates=$(echo "$doc_block" | grep -oE 'check-[a-z0-9-]+' | sort -u)

make_count=$(echo "$make_gates" | grep -c . || true)
doc_count=$(echo "$doc_gates" | grep -c . || true)

fail=0

if [ "$make_count" != "$doc_count" ]; then
    echo "FAIL: gate COUNT mismatch — Makefile LINT_GATES has $make_count check-* gates, $DOC documents $doc_count"
    fail=1
fi

only_make=$(comm -23 <(echo "$make_gates") <(echo "$doc_gates") || true)
only_doc=$(comm -13 <(echo "$make_gates") <(echo "$doc_gates") || true)

if [ -n "${only_make//[[:space:]]/}" ]; then
    echo "FAIL: gates in Makefile LINT_GATES but NOT documented in $DOC:"
    echo "$only_make" | sed 's/^/    /'
    fail=1
fi
if [ -n "${only_doc//[[:space:]]/}" ]; then
    echo "FAIL: gates documented in $DOC but NOT in Makefile LINT_GATES:"
    echo "$only_doc" | sed 's/^/    /'
    fail=1
fi

if [ "$fail" != "0" ]; then
    echo ""
    echo "Fix: update the <!-- LINT-GATES-BEGIN/END --> block in $DOC to"
    echo "     match the LINT_GATES list in Makefile. The Makefile is"
    echo "     authoritative for what actually runs."
    exit 1
fi

echo "check_doc_accuracy: clean — $make_count gates, doc and Makefile agree"
exit 0
