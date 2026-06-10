#!/usr/bin/env bash
# Lint gate E1 — file-size ceiling for app/ and config/ .c files (RATCHET).
#
# Mega-modules cannot hide behind a wall of <500-LOC functions: even if
# every function passes check-long-functions, a 1,900-line file is still
# doing too many things. This gate caps each app/**/*.c and config/src/*.c
# file at CEILING lines and ratchets the existing oversized files down via
# a baseline.
#
# Scope: app/**/*.c (the eight shapes) AND config/src/*.c (the composition
# root). Both obey the same ceiling — config/ is not exempt: the boot
# mega-files are grandfathered in the baseline so they can only shrink.
#
# Baseline: tools/scripts/file_size_ceiling_baseline.txt lists each
# pre-existing oversized file with its recorded LOC. The gate fails when:
#   - a file NOT in the baseline exceeds CEILING lines (a new mega-module), OR
#   - a baselined file grows ABOVE its recorded LOC (regression).
# Shrinking a baselined file below CEILING lets you delete its line. The
# baseline can only shrink — never grow. That is the ratchet.
#
# Override: there is no per-file inline override. To carry a deliberate
# large file, add it to the baseline (a visible, reviewable line) — the
# baseline IS the escape hatch, and review keeps it shrinking.
set -euo pipefail

cd "$(dirname "$0")/../.."

CEILING=800
BASELINE=tools/scripts/file_size_ceiling_baseline.txt
[ -f "$BASELINE" ] || touch "$BASELINE"

# Load baseline into an associative array: path -> max-loc.
declare -A baseline
baseline_count=0
while IFS= read -r line; do
    line="${line%%#*}"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [ -z "$line" ] && continue
    path="${line%% *}"
    loc="${line##* }"
    baseline["$path"]="$loc"
    baseline_count=$((baseline_count + 1))
done < "$BASELINE"

fail=0
new_violations=()
grown_violations=()

# Scan app/**/*.c and config/src/*.c against the same ceiling.
while IFS= read -r f; do
    loc=$(wc -l < "$f")
    base="${baseline[$f]+x}"
    if [ -n "$base" ]; then
        # Baselined file: must not grow above its recorded LOC.
        recorded="${baseline[$f]}"
        if [ "$loc" -gt "$recorded" ]; then
            grown_violations+=("$f grew to $loc lines (baseline $recorded)")
            fail=1
        fi
        continue
    fi
    # Not baselined: must stay under the ceiling.
    if [ "$loc" -gt "$CEILING" ]; then
        new_violations+=("$f is $loc lines (ceiling $CEILING)")
        fail=1
    fi
done < <( { find app -type f -name '*.c'; find config/src -type f -name '*.c' 2>/dev/null; } | sort )

if [ "$fail" = "0" ]; then
    echo "check_file_size_ceiling: clean — ${baseline_count} baselined, no new/grown oversized files (ceiling $CEILING)"
    exit 0
fi

echo ""
echo "check_file_size_ceiling: file-size ceiling violations"
echo ""
for v in "${new_violations[@]}"; do
    echo "  NEW oversized file: $v"
done
for v in "${grown_violations[@]}"; do
    echo "  REGRESSION: $v"
done
echo ""
echo "Fix options (preferred → fallback):"
echo "  1. Split the file along its seams (per-section helpers, separate"
echo "     shape files) so each piece stays under $CEILING lines."
echo "  2. For a baselined file that grew, shrink it back at or below its"
echo "     recorded baseline LOC."
echo "  3. As last resort, record the file in $BASELINE"
echo "     (a reviewable line; the baseline must only shrink over time)."
exit 1
