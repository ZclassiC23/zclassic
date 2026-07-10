#!/usr/bin/env bash
# Lint gate E1 — file-size ceiling for app/ and config/ .c files (ENFORCED
# RATCHET — fails the build).
#
# Mega-modules cannot hide behind a wall of <500-LOC functions: even if
# every function passes check-long-functions, a 1,900-line file is still
# doing too many things. This gate caps each app/**/*.c and config/src/*.c
# file at CEILING lines and ratchets the existing oversized files down via
# a baseline. Per docs/FRAMEWORK.md §5 and docs/DEFENSIVE_CODING.md, E1 is
# one of the RATCHET-mode architecture gates: the baseline can only shrink,
# never grow, and growing it costs an ADR — not a WARN print.
#
# Scope: app/**/*.c (the eight shapes) AND config/src/*.c (the composition
# root). Both obey the same ceiling — config/ is not exempt: the boot
# mega-files are grandfathered in the baseline so they can only shrink.
#
# Baseline: tools/scripts/file_size_ceiling_baseline.txt lists each
# pre-existing oversized file with its recorded LOC. The gate FAILS when:
#   - a file NOT in the baseline exceeds CEILING lines (a new mega-module), OR
#   - a baselined file grows ABOVE its recorded LOC (regression).
# Shrinking a baselined file below CEILING lets you delete its baseline
# line; shrinking it while still over CEILING earns an auto-suggestion to
# tighten (lower) the recorded number so the ratchet keeps closing.
#
# Override: there is no per-file inline override. To carry a deliberate
# large file, add it to the baseline (a visible, reviewable line) — the
# baseline IS the escape hatch, and review keeps it shrinking.
set -euo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

CEILING=800
BASELINE="${ZCL_FILE_SIZE_CEILING_BASELINE:-tools/scripts/file_size_ceiling_baseline.txt}"
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

# Fail-loud preflight: the scan set MUST be non-empty. A `find` over a
# renamed/moved/emptied app/ or config/src/ would otherwise silently
# iterate zero files and this gate would report "clean" while blind.
mapfile -t scan_files < <( { find app -type f -name '*.c'; \
    find config/src -type f -name '*.c' 2>/dev/null; } | sort )
gate_require_scanned "${#scan_files[@]}" 1 check_file_size_ceiling \
    "no *.c under app/ or config/src/ — was a shape dir renamed/moved?"

fail=0
new_violations=()
grown_violations=()
shrink_suggestions=()

for f in "${scan_files[@]}"; do
    loc=$(wc -l < "$f")
    base="${baseline[$f]+x}"
    if [ -n "$base" ]; then
        # Baselined file: must not grow above its recorded LOC.
        recorded="${baseline[$f]}"
        if [ "$loc" -gt "$recorded" ]; then
            grown_violations+=("$f grew to $loc lines (baseline $recorded)")
            fail=1
        elif [ "$loc" -lt "$recorded" ] && [ "$loc" -gt "$CEILING" ]; then
            # Shrank but still over ceiling — nudge the baseline tighter.
            shrink_suggestions+=("$f is now $loc lines (baseline $recorded, ceiling $CEILING) — tighten the baseline entry to $loc")
        fi
        continue
    fi
    # Not baselined: must stay under the ceiling.
    if [ "$loc" -gt "$CEILING" ]; then
        new_violations+=("$f is $loc lines (ceiling $CEILING)")
        fail=1
    fi
done

if [ "$fail" != "0" ]; then
    echo ""
    echo "check_file_size_ceiling: FAIL — file-size ceiling violations (E1, ratchet)"
    echo ""
    for v in "${new_violations[@]}"; do
        echo "  NEW oversized file (not in baseline): $v"
    done
    for v in "${grown_violations[@]}"; do
        echo "  REGRESSION (grew past its baselined LOC): $v"
    done
    echo ""
    echo "Fix options (preferred -> fallback):"
    echo "  1. Split the file along its seams (per-section helpers, separate"
    echo "     shape files) so each piece stays under $CEILING lines."
    echo "  2. For a baselined file that grew, shrink it back at or below its"
    echo "     recorded baseline LOC in $BASELINE."
    echo "  3. As last resort, record a NEW file in $BASELINE at its current"
    echo "     LOC (a reviewable line; the baseline must only shrink over"
    echo "     time — raising an existing entry needs an ADR, not this gate)."
    exit 1
fi

echo "check_file_size_ceiling: clean — ${baseline_count} baselined, no new/grown oversized files (ceiling $CEILING)"
if [ "${#shrink_suggestions[@]}" -gt 0 ]; then
    echo ""
    echo "  Baseline can tighten (files shrank but are still over ceiling):"
    for s in "${shrink_suggestions[@]}"; do
        echo "    $s"
    done
fi
exit 0
