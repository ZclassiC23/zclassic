#!/usr/bin/env bash
# Lint gate #14 — lib/ layer purity (regression ratchet).
#
# Files under lib/ should not #include from app/controllers/, app/models/,
# app/services/, or app/views/. lib/ is the foundation; app/ is the
# upstream consumer. A backward include typically means the lib/ file is
# doing something that belongs in app/, or relying on a struct/function
# that should live in lib/.
#
# Deployment strategy: this codebase has many pre-existing violations
# (architectural debt from earlier development). Rather than block CI
# until every one is cleaned, the gate uses a baseline file that lists
# accepted pre-existing violations. Any NEW violation not in the
# baseline fails the build, ratcheting the count downward over time.
#
# Baseline file: tools/scripts/lib_layering_baseline.txt
#   Format: one "<file>:<include-line>" entry per line.
#   Blank lines and # comments are ignored.
#
# Override on a specific line (preferred when you understand the trade-off
# and want to keep the include): add `// lib-layer-ok:<tag>` after the
# include directive. Use sparingly — every override is a debt marker.
#
# To clean up debt: remove the include from lib/ code (via forward decl
# or moving the symbol down to lib/), then delete the matching baseline
# entry. CI will then enforce that the file stays clean.
set -euo pipefail

BASELINE=tools/scripts/lib_layering_baseline.txt
[ -f "$BASELINE" ] || touch "$BASELINE"

# Read accepted violations into a hash set. Lines that start with # or
# that are blank are ignored.
declare -A baseline
baseline_count=0
while IFS= read -r line; do
    [[ -z "$line" || "$line" =~ ^[[:space:]]*# ]] && continue
    baseline["$line"]=1
    baseline_count=$((baseline_count + 1))
done < "$BASELINE"

fail=0
new_violations=()
while IFS= read -r f; do
    while IFS= read -r match; do
        line_content="${match#*:}"
        # Per-line override marker: skip immediately.
        if echo "$line_content" | grep -qE '//[[:space:]]*lib-layer-ok:[A-Za-z][A-Za-z0-9_-]*'; then
            continue
        fi
        # Extract just the bare `#include "..."` token (drop any trailing
        # comments) so the baseline key stays stable across cosmetic edits.
        include_token=$(echo "$line_content" \
            | sed -E 's@//.*$@@' \
            | sed -E 's@/\*.*\*/@@g' \
            | sed -E 's@^[[:space:]]+@@; s@[[:space:]]+$@@')
        key="${f}:${include_token}"
        if [ -n "${baseline[$key]+x}" ]; then
            # Pre-existing violation, accepted by the baseline. Continue.
            continue
        fi
        new_violations+=("$key")
        fail=1
    done < <(grep -nE '^[[:space:]]*#include[[:space:]]+"(controllers|models|services|views)/' "$f" || true)
done < <(find lib -type f \( -name '*.c' -o -name '*.h' \) ! -path '*/test/*')

if [ "$fail" = "0" ]; then
    echo "check_lib_layering: clean — ${baseline_count} grandfathered violation(s), no new ones"
    exit 0
fi

echo ""
echo "check_lib_layering: ${#new_violations[@]} NEW violation(s) not in $BASELINE"
echo ""
for v in "${new_violations[@]}"; do
    echo "  $v"
done
echo ""
echo "Fix options (preferred → fallback):"
echo "  1. Delete the include if it's unused (the symbol may already come from elsewhere)."
echo "  2. Replace with a forward declaration (struct fwd + extern fn decl)."
echo "  3. Move the symbol down into lib/ where it can be referenced cleanly."
echo "  4. Add an override marker '// lib-layer-ok:<tag>' to the include line."
echo "  5. As last resort, add the file:include entry to $BASELINE (with a comment explaining)."
exit 1
