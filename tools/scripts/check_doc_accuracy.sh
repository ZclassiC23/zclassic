#!/usr/bin/env bash
# Lint gate E11 — DEFENSIVE_CODING.md gate list matches the Makefile (HARD).
#
# Doc rot is silent: the Makefile lint: target gains/loses a check-* gate
# and DEFENSIVE_CODING.md still claims the old count and names. This gate
# cross-checks the two sources of truth so the doc can never drift from
# the build.
#
# Source of truth A: the check-* dependencies of the `lint:` target in
#   Makefile.
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

# A) Extract the check-* deps of the lint: target. The recipe is a single
#    `lint: dep dep dep ...` line (the prerequisites), not the command body.
lint_line=$(grep -E '^lint:[[:space:]]' "$MAKEFILE" | head -1)
if [ -z "$lint_line" ]; then
    echo "FAIL: could not find the 'lint:' target line in $MAKEFILE"
    exit 1
fi
make_gates=$(echo "$lint_line" | grep -oE 'check-[a-z0-9-]+' | sort -u)

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
    echo "FAIL: gate COUNT mismatch — Makefile lint: has $make_count check-* gates, $DOC documents $doc_count"
    fail=1
fi

only_make=$(comm -23 <(echo "$make_gates") <(echo "$doc_gates") || true)
only_doc=$(comm -13 <(echo "$make_gates") <(echo "$doc_gates") || true)

if [ -n "${only_make//[[:space:]]/}" ]; then
    echo "FAIL: gates in Makefile lint: but NOT documented in $DOC:"
    echo "$only_make" | sed 's/^/    /'
    fail=1
fi
if [ -n "${only_doc//[[:space:]]/}" ]; then
    echo "FAIL: gates documented in $DOC but NOT in Makefile lint::"
    echo "$only_doc" | sed 's/^/    /'
    fail=1
fi

if [ "$fail" != "0" ]; then
    echo ""
    echo "Fix: update the <!-- LINT-GATES-BEGIN/END --> block in $DOC to"
    echo "     match the check-* prerequisites of the lint: target. The"
    echo "     Makefile is authoritative for what actually runs."
    exit 1
fi

echo "check_doc_accuracy: clean — $make_count gates, doc and Makefile agree"
exit 0
