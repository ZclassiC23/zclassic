#!/usr/bin/env bash
# Lint gate E1 — file-size ceiling.
#
# Mega-modules cannot hide behind a wall of <500-LOC functions: even if
# every function passes check-long-functions, a 1,900-line file is still
# doing too many things. This gate caps a file's line count and ratchets
# existing oversized files down via a baseline.
#
# Two tiers, same mechanics, different consequence:
#
#   ENFORCED (fails the build) — app/**/*.c (the eight shapes) AND
#   config/src/*.c (the composition root). Per docs/FRAMEWORK.md §5 and
#   docs/DEFENSIVE_CODING.md, this is a RATCHET-mode architecture gate: the
#   baseline in tools/scripts/file_size_ceiling_baseline.txt can only
#   shrink, never grow, and growing it costs an ADR.
#
#   WARN (prints, never fails) — lib/**/*.c (excluding lib/test/, which is
#   fixtures/registrations and legitimately huge) AND domain/**/*.c. lib/
#   and domain/ are primitives and pure-domain code, not the eight
#   app-shape surfaces E1 was written to police, so a violation here is a
#   heads-up, not a build break. Baseline:
#   tools/scripts/file_size_ceiling_lib_baseline.txt.
#
# Baseline format (both tiers): '<relative path> <max-loc>' per line,
# lines starting with # are comments. The gate flags a tier when:
#   - a file NOT in that tier's baseline exceeds CEILING lines, OR
#   - a baselined file grows ABOVE its recorded LOC.
# Shrinking a baselined file below CEILING lets you delete its baseline
# line; shrinking it while still over CEILING earns an auto-suggestion to
# tighten (lower) the recorded number so the ratchet keeps closing.
#
# Override: there is no per-file inline override. To carry a deliberate
# large file, add it to the tier's baseline (a visible, reviewable line —
# for the WARN tier this documents intent, it does not gate anything) or,
# for straight-line generated/tabular code where LOC is not a complexity
# signal (e.g. crypto field-arithmetic tables), add it to LIB_ALLOWLIST
# below to skip it entirely.
set -euo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh
# shellcheck source=tools/lint/scan_exclusions.sh
source tools/lint/scan_exclusions.sh

CEILING=800

# Straight-line generated/tabular crypto & math code: long by nature, not
# by neglect. Skipped entirely by the WARN tier (not even baselined).
LIB_ALLOWLIST=(
    "lib/chain/src/sha3_windows.c"
    "lib/sapling/src/bn254.c"
    "lib/sapling/src/bls12_381.c"
    "domain/wallet/src/mnemonic.c"
)
lib_is_allowlisted() {
    local f="$1" a
    for a in "${LIB_ALLOWLIST[@]}"; do
        [ "$f" = "$a" ] && return 0
    done
    return 1
}

# Load a baseline file into the nameref'd associative array: path -> max-loc.
load_baseline() {
    local -n out="$1"
    local file="$2" line path loc
    out=()
    [ -f "$file" ] || return 0
    while IFS= read -r line; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -z "$line" ] && continue
        path="${line%% *}"
        loc="${line##* }"
        out["$path"]="$loc"
    done < "$file"
}

# Check one tier's scan set against its baseline. Populates the nameref'd
# `new_out` / `grown_out` / `shrink_out` arrays (all cleared first); returns
# nothing — callers decide whether a non-empty new/grown set fails the tier.
check_tier() {
    local -n files="$1" base="$2" new_out="$3" grown_out="$4" shrink_out="$5"
    local ceiling="$6"
    local f loc recorded
    new_out=()
    grown_out=()
    shrink_out=()
    for f in "${files[@]}"; do
        loc=$(wc -l < "$f")
        if [ -n "${base[$f]+x}" ]; then
            recorded="${base[$f]}"
            if [ "$loc" -gt "$recorded" ]; then
                grown_out+=("$f grew to $loc lines (baseline $recorded)")
            elif [ "$loc" -lt "$recorded" ] && [ "$loc" -gt "$ceiling" ]; then
                shrink_out+=("$f is now $loc lines (baseline $recorded, ceiling $ceiling) — tighten the baseline entry to $loc")
            fi
            continue
        fi
        if [ "$loc" -gt "$ceiling" ]; then
            new_out+=("$f is $loc lines (ceiling $ceiling)")
        fi
    done
}

overall_fail=0

# ── ENFORCED tier: app/ + config/src/ ────────────────────────────────────

ENFORCED_BASELINE="${ZCL_FILE_SIZE_CEILING_BASELINE:-tools/scripts/file_size_ceiling_baseline.txt}"
[ -f "$ENFORCED_BASELINE" ] || touch "$ENFORCED_BASELINE"
declare -A enforced_baseline=()
load_baseline enforced_baseline "$ENFORCED_BASELINE"
enforced_baseline_count="${#enforced_baseline[@]}"

# Fail-loud preflight: the scan set MUST be non-empty. A `find` over a
# renamed/moved/emptied app/ or config/src/ would otherwise silently
# iterate zero files and this gate would report "clean" while blind.
mapfile -t enforced_files < <( { find app -type f -name '*.c' "${LINT_FIND_PRUNE_ARGS[@]}"; \
    find config/src -type f -name '*.c' "${LINT_FIND_PRUNE_ARGS[@]}" 2>/dev/null; } | sort )
gate_require_scanned "${#enforced_files[@]}" 1 check_file_size_ceiling \
    "no *.c under app/ or config/src/ — was a shape dir renamed/moved?"

check_tier enforced_files enforced_baseline \
    enforced_new enforced_grown enforced_shrink "$CEILING"

if [ "${#enforced_new[@]}" -gt 0 ] || [ "${#enforced_grown[@]}" -gt 0 ]; then
    overall_fail=1
    echo ""
    echo "check_file_size_ceiling: FAIL — file-size ceiling violations (E1, ratchet, app/+config/src/)"
    echo ""
    for v in "${enforced_new[@]}"; do
        echo "  NEW oversized file (not in baseline): $v"
    done
    for v in "${enforced_grown[@]}"; do
        echo "  REGRESSION (grew past its baselined LOC): $v"
    done
    echo ""
    echo "Fix options (preferred -> fallback):"
    echo "  1. Split the file along its seams (per-section helpers, separate"
    echo "     shape files) so each piece stays under $CEILING lines."
    echo "  2. For a baselined file that grew, shrink it back at or below its"
    echo "     recorded baseline LOC in $ENFORCED_BASELINE."
    echo "  3. As last resort, record a NEW file in $ENFORCED_BASELINE at its"
    echo "     current LOC (a reviewable line; the baseline must only shrink"
    echo "     over time — raising an existing entry needs an ADR, not this"
    echo "     gate)."
else
    echo "check_file_size_ceiling: clean — ${enforced_baseline_count} baselined, no new/grown oversized files (ceiling $CEILING, app/+config/src/)"
fi
if [ "${#enforced_shrink[@]}" -gt 0 ]; then
    echo ""
    echo "  Baseline can tighten (files shrank but are still over ceiling):"
    for s in "${enforced_shrink[@]}"; do
        echo "    $s"
    done
fi

# ── WARN tier: lib/ (excluding lib/test/) + domain/ ──────────────────────

LIB_BASELINE="${ZCL_FILE_SIZE_CEILING_LIB_BASELINE:-tools/scripts/file_size_ceiling_lib_baseline.txt}"
[ -f "$LIB_BASELINE" ] || touch "$LIB_BASELINE"
declare -A lib_baseline=()
load_baseline lib_baseline "$LIB_BASELINE"
lib_baseline_count="${#lib_baseline[@]}"

mapfile -t lib_files_all < <( { find lib -type f -name '*.c' -not -path 'lib/test/*' "${LINT_FIND_PRUNE_ARGS[@]}"; \
    find domain -type f -name '*.c' -not -path '*/test/*' "${LINT_FIND_PRUNE_ARGS[@]}" 2>/dev/null; } | sort )
gate_require_scanned "${#lib_files_all[@]}" 1 check_file_size_ceiling \
    "no *.c under lib/ (excl. lib/test/) or domain/ — was a dir renamed/moved?"

lib_files=()
for f in "${lib_files_all[@]}"; do
    lib_is_allowlisted "$f" && continue
    lib_files+=("$f")
done

check_tier lib_files lib_baseline lib_new lib_grown lib_shrink "$CEILING"

if [ "${#lib_new[@]}" -gt 0 ] || [ "${#lib_grown[@]}" -gt 0 ]; then
    echo ""
    echo "check_file_size_ceiling: WARN — file-size ceiling watch (E1, lib/+domain/, non-blocking)"
    echo ""
    for v in "${lib_new[@]}"; do
        echo "  NEW oversized file (not in $LIB_BASELINE): $v"
    done
    for v in "${lib_grown[@]}"; do
        echo "  grew past its baselined LOC in $LIB_BASELINE: $v"
    done
    echo ""
    echo "  This tier is WARN-only — it does not fail the build. Consider"
    echo "  splitting the file, or if it's baselined intentionally, add/adjust"
    echo "  its line in $LIB_BASELINE. Straight-line generated/tabular code"
    echo "  belongs in LIB_ALLOWLIST in this script instead."
else
    echo "check_file_size_ceiling: clean — ${lib_baseline_count} baselined, no new/grown oversized files (ceiling $CEILING, lib/+domain/, WARN tier)"
fi
if [ "${#lib_shrink[@]}" -gt 0 ]; then
    echo ""
    echo "  lib/domain baseline can tighten (files shrank but are still over ceiling):"
    for s in "${lib_shrink[@]}"; do
        echo "    $s"
    done
fi

exit "$overall_fail"
