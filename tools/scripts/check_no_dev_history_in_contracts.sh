#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Lint gate — no dev-history phrasing in production contract files.
#
# A header under any **/include/** dir, or a command .def table, IS the
# contract an operator or an LLM agent reads to learn what a module
# currently does. A stale "STEP-0 STATUS: contract + stub bodies; lane 2A
# lands the real thing" comment left behind after the real body landed is
# INCORRECT MODEL CONTEXT, not a style nit — an agent (or a human) reading
# the header trusts it over the .c body and reasonably concludes the
# feature is still unimplemented, re-proposing already-shipped work or
# distrusting a real, working call site. This gate rejects a narrow,
# high-signal set of dev-history phrases from those two production-contract
# surfaces so the pattern cannot creep back in once cleaned up.
#
# Scope: every tracked-shape *.h file under any **/include/** directory,
# and every *.def table, anywhere in the tree. Allowlisted OUT: docs/
# (narrative is its whole point), vendor/ (third-party), and anything under
# a "test"/"tests" path component or named *_test.* (fixtures/tests narrate
# lane history and stub scaffolding on purpose).
#
# Phrase set is deliberately NARROW (high-signal only): generic phrases
# like "in flight" / "not done" false-positive on legitimate present-tense
# state descriptions elsewhere in the tree and are intentionally NOT
# included.
#
# Hollow-gate guard: gate_require_scanned aborts (exit 2) if the scan set
# is empty (a renamed/moved include/ dir or emptied .def population would
# otherwise silently report "clean" while blind).
set -euo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh
# shellcheck source=tools/lint/scan_exclusions.sh
source tools/lint/scan_exclusions.sh

GATE_NAME=check-no-dev-history-in-contracts

# ── Allowlist predicate ──────────────────────────────────────────────────
is_allowlisted_path() {
    local p="$1" part base
    case "$p" in
        docs/*|*/docs/*) return 0 ;;
        vendor/*|*/vendor/*) return 0 ;;
    esac
    IFS='/' read -ra _nda_parts <<< "$p"
    for part in "${_nda_parts[@]}"; do
        if [[ "$part" == "test" || "$part" == "tests" ]]; then
            return 0
        fi
    done
    base="$(basename "$p")"
    [[ "$base" == *_test.* ]] && return 0
    return 1
}

# ── Scan set ──────────────────────────────────────────────────────────────
mapfile -t h_candidates < <(find . -type f -name '*.h' -path '*/include/*' \
    "${LINT_FIND_PRUNE_ARGS[@]}" 2>/dev/null | sed 's|^\./||' | sort)
mapfile -t def_candidates < <(find . -type f -name '*.def' \
    "${LINT_FIND_PRUNE_ARGS[@]}" 2>/dev/null | sed 's|^\./||' | sort)

files=()
for f in "${h_candidates[@]}" "${def_candidates[@]}"; do
    is_allowlisted_path "$f" && continue
    files+=("$f")
done

gate_require_scanned "${#files[@]}" 1 "$GATE_NAME" \
    "no in-scope *.h (**/include/**) or *.def file found -- was a dir renamed/moved?"

# ── Phrase set (high-signal only). Each entry is "grep-flag|pattern". ────
PHRASES=(
    '-F|STEP-0 STATUS'
    '-F|stub bodies'
    '-F|stub body'
    '-E|lane [0-9][A-Z]?'
    '-F|future slice'
)

violations=()
for f in "${files[@]}"; do
    [[ -f "$f" ]] || continue
    for spec in "${PHRASES[@]}"; do
        flag="${spec%%|*}"
        pattern="${spec#*|}"
        hit=$(gate_grep -n -I "$flag" "$pattern" "$f") && {
            while IFS= read -r hitline; do
                [ -n "$hitline" ] && violations+=("$f:$hitline  [/$pattern/]")
            done <<< "$hit"
        }
    done
done

if [ "${#violations[@]}" -gt 0 ]; then
    echo "$GATE_NAME: FAIL — dev-history phrasing in ${#violations[@]} production contract line(s)"
    echo ""
    for v in "${violations[@]}"; do
        echo "  $v"
    done
    echo ""
    echo "Dev-history phrasing (\"STEP-0 STATUS\", \"stub bod(y|ies)\", \"lane <N><letter>\","
    echo "\"future slice\") is INCORRECT MODEL CONTEXT once the real work has landed —"
    echo "an agent or operator reading the header trusts it over the .c body. Rewrite"
    echo "the comment to describe the CURRENT contract plus any remaining invariant,"
    echo "without lane numbers / STEP-N status / 'future slice' phrasing. Dated"
    echo "narrative belongs in git history or docs/work/*, never in a production"
    echo "header or .def table."
    exit 1
fi

echo "$GATE_NAME: clean — ${#files[@]} in-scope file(s) (*.h under **/include/**, *.def), no dev-history phrasing"
exit 0
