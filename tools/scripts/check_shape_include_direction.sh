#!/usr/bin/env bash
# Lint gate #49 — inter-shape include direction (RATCHET).
#
# The eight app/ shapes include DOWNWARD only:
#   controllers -> services -> models -> (lib -> core)
# This gate forbids the two upward edges that can occur in practice:
#   (a) app/models/**   #include-ing "services/..." or "controllers/..."
#   (b) app/services/** #include-ing "controllers/..."
# A models/ file reaching UP into services/ (or controllers/), or a
# services/ file reaching UP into controllers/, typically means the symbol
# belongs lower in the stack, or the dependency should be inverted (pass the
# value down from the upstream caller, or register a callback/port — see
# node_db_set_quick_check_skip_probe in models/database.h for the pattern).
#
# This gate is the sibling of check_lib_layering.sh (#14, lib/ -> app/) and
# check_domain_purity.sh (#45, domain/ purity) — those two guard the OUTER
# boundary of the layer stack; this one guards the inter-shape ordering
# WITHIN app/.
#
# Baseline file: tools/scripts/shape_include_direction_baseline.txt
#   Format: one "<file>:<include-line>" entry per line.
#   Blank lines and # comments are ignored.
#
# Ratchet, not HARD-empty: at gate-introduction time the models/ -> services/
# edge was already at zero (its two known violations were fixed in the same
# change that added this gate — see the baseline file's header), but a scan
# of the services/ -> controllers/ edge turned up pre-existing debt nobody
# had tracked before (there was no gate on this direction until now). That
# debt is grandfathered in the baseline exactly like check_lib_layering.sh's
# baseline once grandfathered its own pre-existing debt before being paid
# down to zero and promoted to HARD (see that script's history). A NEW
# violation on EITHER edge — not already in the baseline — fails immediately.
#
# Override on a specific line (preferred when you understand the trade-off
# and want to keep the include): add `// shape-layer-ok:<tag>` after the
# include directive. Use sparingly — every override is a debt marker.
#
# To clean up debt: remove the upward include (forward decl, move the
# symbol down into models/lib, or invert the dependency via a registered
# callback/port seam), then delete the matching baseline entry. CI then
# enforces that the file stays clean.
set -euo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/scan_exclusions.sh
source tools/lint/scan_exclusions.sh

BASELINE=tools/scripts/shape_include_direction_baseline.txt
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

# $1 = directory to scan, $2 = "|"-separated forbidden top-level include
# prefixes for files under that directory.
scan_dir() {
    local dir="$1" forbidden="$2"
    while IFS= read -r f; do
        while IFS= read -r match; do
            line_content="${match#*:}"
            # Per-line override marker: skip immediately.
            if echo "$line_content" | grep -qE '//[[:space:]]*shape-layer-ok:[A-Za-z][A-Za-z0-9_-]*'; then
                continue
            fi
            # Extract just the bare `#include "..."` token (drop any trailing
            # comments) so the baseline key stays stable across cosmetic
            # edits.
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
        done < <(grep -nE "^[[:space:]]*#include[[:space:]]+\"(${forbidden})/" "$f" || true)
    done < <(find "$dir" -type f \( -name '*.c' -o -name '*.h' \) ! -path '*/test/*' "${LINT_FIND_PRUNE_ARGS[@]}")
}

scan_dir app/models   "services|controllers"
scan_dir app/services "controllers"

if [ "$fail" = "0" ]; then
    echo "check_shape_include_direction: clean — ${baseline_count} baselined (pre-existing), no NEW upward shape includes"
    exit 0
fi

echo ""
echo "check_shape_include_direction: ${#new_violations[@]} NEW violation(s) not in $BASELINE"
echo ""
for v in "${new_violations[@]}"; do
    echo "  $v"
done
echo ""
echo "Fix options (RATCHET gate — new debt is never accepted):"
echo "  1. Delete the include if it's unused (the symbol may already come from elsewhere)."
echo "  2. Move the needed symbol DOWN into models/ (or lib/) where both sides"
echo "     can reference it cleanly (e.g. a pure policy table)."
echo "  3. Invert the dependency: pass the value in from the upstream caller,"
echo "     or register a callback/port seam (see node_db_set_quick_check_skip_probe"
echo "     in app/models/include/models/database.h for the pattern)."
echo "  4. As a deliberate, reviewed exception only, add an override marker"
echo "     '// shape-layer-ok:<tag>' to the include line, or add a reviewed"
echo "     baseline entry (grandfathering pre-existing debt only — never a"
echo "     way to introduce a brand-new upward include)."
exit 1
