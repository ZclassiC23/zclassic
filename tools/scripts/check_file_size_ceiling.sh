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
#   WARN (prints, never fails on an individual file) — lib/**/*.c
#   (excluding lib/test/, which is fixtures/registrations and legitimately
#   huge), domain/**/*.c, AND src/*.c (the binary's composition entrypoint,
#   e.g. main.c). None of these are the eight app-shape surfaces E1 was
#   written to police, so a single violation here is a heads-up, not a
#   build break. Baseline: tools/scripts/file_size_ceiling_lib_baseline.txt.
#   The tier's total (new + grown) violation COUNT is separately ratcheted
#   (see "Drift-count ratchet" below) so silent accumulation still fails.
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
#
# `--fix`: a file SHRINKING below its recorded baseline is progress, never a
# failure (see "shrink" below) — but it also must never be a silent no-op
# forever, or the ratchet stops closing. `check_file_size_ceiling.sh --fix`
# rewrites every shrink-flagged baseline entry (both tiers) to the file's
# current line count and exits 0. Growth/new-violation handling is
# unaffected: --fix never touches a NEW or GROWN entry, and the normal
# FAIL/WARN reporting below still runs against the (now-tightened) baseline.
set -euo pipefail

FIX_MODE=0
if [ "${1:-}" = "--fix" ]; then
    FIX_MODE=1
fi

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
                # Structured "path|current-loc|recorded|ceiling" — printers
                # format the human message; --fix parses the same fields to
                # rewrite the baseline entry without re-deriving anything.
                shrink_out+=("$f|$loc|$recorded|$ceiling")
            fi
            continue
        fi
        if [ "$loc" -gt "$ceiling" ]; then
            new_out+=("$f is $loc lines (ceiling $ceiling)")
        fi
    done
}

# Print a tier's shrink advisory from its structured shrink_out array (see
# check_tier above), and — in --fix mode — rewrite each flagged entry's
# recorded LOC to its current line count directly in the baseline file.
# Shrink NEVER affects the exit code; this only tightens the ratchet.
print_and_maybe_fix_shrink() {
    local -n pairs="$1"
    local baseline_file="$2"
    local heading="$3"
    [ "${#pairs[@]}" -gt 0 ] || return 0

    echo ""
    echo "  $heading"
    local pair sf sloc srecorded sceiling
    for pair in "${pairs[@]}"; do
        IFS='|' read -r sf sloc srecorded sceiling <<< "$pair"
        echo "    $sf is now $sloc lines (baseline $srecorded, ceiling $sceiling) — tighten the baseline entry to $sloc"
    done

    [ "$FIX_MODE" -eq 1 ] || return 0

    local mapfile_tmp
    mapfile_tmp="$(mktemp)"
    for pair in "${pairs[@]}"; do
        IFS='|' read -r sf sloc srecorded sceiling <<< "$pair"
        printf '%s\t%s\n' "$sf" "$sloc" >> "$mapfile_tmp"
    done
    awk -v mapfile="$mapfile_tmp" '
        BEGIN {
            while ((getline line < mapfile) > 0) {
                split(line, a, "\t")
                newloc[a[1]] = a[2]
            }
        }
        {
            stripped = $0
            sub(/#.*/, "", stripped)
            gsub(/^[ \t]+/, "", stripped)
            gsub(/[ \t]+$/, "", stripped)
            if (stripped != "") {
                n = split(stripped, parts, /[ \t]+/)
                path = parts[1]
                if (path in newloc) {
                    print path " " newloc[path]
                    next
                }
            }
            print $0
        }
    ' "$baseline_file" > "$baseline_file.zcl_fix_tmp" && mv "$baseline_file.zcl_fix_tmp" "$baseline_file"
    rm -f "$mapfile_tmp"
    echo "  --fix: tightened ${#pairs[@]} baseline entr$( [ "${#pairs[@]}" -eq 1 ] && echo y || echo ies ) in $baseline_file"
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
print_and_maybe_fix_shrink enforced_shrink "$ENFORCED_BASELINE" \
    "Baseline can tighten (files shrank but are still over ceiling):"

# ── WARN tier: lib/ (excluding lib/test/) + domain/ + src/ ───────────────
# src/ (main.c, cli.c — the binary's composition entrypoint) shares this
# tier and baseline file: it's not one of the eight app/ shapes the
# ENFORCED tier polices, same rationale as lib/+domain/.

LIB_BASELINE="${ZCL_FILE_SIZE_CEILING_LIB_BASELINE:-tools/scripts/file_size_ceiling_lib_baseline.txt}"
[ -f "$LIB_BASELINE" ] || touch "$LIB_BASELINE"
declare -A lib_baseline=()
load_baseline lib_baseline "$LIB_BASELINE"
lib_baseline_count="${#lib_baseline[@]}"

mapfile -t lib_files_all < <( { find lib -type f -name '*.c' -not -path 'lib/test/*' "${LINT_FIND_PRUNE_ARGS[@]}"; \
    find domain -type f -name '*.c' -not -path '*/test/*' "${LINT_FIND_PRUNE_ARGS[@]}" 2>/dev/null; \
    find src -maxdepth 1 -type f -name '*.c' "${LINT_FIND_PRUNE_ARGS[@]}" 2>/dev/null; } | sort )
gate_require_scanned "${#lib_files_all[@]}" 1 check_file_size_ceiling \
    "no *.c under lib/ (excl. lib/test/), domain/, or src/ — was a dir renamed/moved?"

lib_files=()
for f in "${lib_files_all[@]}"; do
    lib_is_allowlisted "$f" && continue
    lib_files+=("$f")
done

check_tier lib_files lib_baseline lib_new lib_grown lib_shrink "$CEILING"

# Drift-count ratchet: no single WARN-tier file failing the build let this
# tier accumulate 5 new unbaselined files + 13 baselined files silently
# growing past their recorded LOC with nothing ever failing `make lint`.
# tools/scripts/file_size_ceiling_lib_drift_count.txt records the highest
# (new + grown) violation COUNT ever reviewed and accepted for this tier.
# Existing drift at or below that count is grandfathered (still WARN-only,
# per-file); the count itself is what ratchets — growing PAST the recorded
# value fails `make lint`, same enforcement shape as the ENFORCED tier's
# per-file LOC baseline, just applied to the violation count instead of
# individual LOC.
lib_drift_count=$(( ${#lib_new[@]} + ${#lib_grown[@]} ))
DRIFT_RATCHET="${ZCL_FILE_SIZE_CEILING_LIB_DRIFT_RATCHET:-tools/scripts/file_size_ceiling_lib_drift_count.txt}"
[ -f "$DRIFT_RATCHET" ] || echo 0 > "$DRIFT_RATCHET"
lib_drift_recorded=$(grep -vE '^[[:space:]]*(#|$)' "$DRIFT_RATCHET" | head -1 | tr -d '[:space:]')
[ -n "$lib_drift_recorded" ] || lib_drift_recorded=0

if [ "$lib_drift_count" -gt 0 ]; then
    echo ""
    if [ "$lib_drift_count" -gt "$lib_drift_recorded" ]; then
        overall_fail=1
        echo "check_file_size_ceiling: FAIL — WARN-tier drift count grew (E1 drift ratchet, lib/+domain/+src/)"
        echo "  drift count = $lib_drift_count (new+grown), recorded ceiling = $lib_drift_recorded in $DRIFT_RATCHET"
    else
        echo "check_file_size_ceiling: WARN — file-size ceiling watch (E1, lib/+domain/+src/, non-blocking; drift $lib_drift_count/$lib_drift_recorded grandfathered)"
    fi
    echo ""
    for v in "${lib_new[@]}"; do
        echo "  NEW oversized file (not in $LIB_BASELINE): $v"
    done
    for v in "${lib_grown[@]}"; do
        echo "  grew past its baselined LOC in $LIB_BASELINE: $v"
    done
    echo ""
    if [ "$lib_drift_count" -gt "$lib_drift_recorded" ]; then
        echo "  Fix: baseline the new file(s) in $LIB_BASELINE at their current LOC (or"
        echo "  shrink the grown file(s) back to their recorded baseline). If this drift is"
        echo "  reviewed and deliberately accepted, raise the count in $DRIFT_RATCHET to"
        echo "  $lib_drift_count instead."
    else
        echo "  This tier is WARN-only — it does not fail the build unless the drift COUNT"
        echo "  exceeds $DRIFT_RATCHET's recorded value ($lib_drift_recorded). Consider"
        echo "  splitting the file, or if it's baselined intentionally, add/adjust its line"
        echo "  in $LIB_BASELINE. Straight-line generated/tabular code belongs in"
        echo "  LIB_ALLOWLIST in this script instead."
    fi
else
    echo "check_file_size_ceiling: clean — ${lib_baseline_count} baselined, no new/grown oversized files (ceiling $CEILING, lib/+domain/+src/, WARN tier)"
fi
if [ "$lib_drift_count" -lt "$lib_drift_recorded" ]; then
    echo ""
    echo "  Drift ratchet can tighten: $DRIFT_RATCHET records $lib_drift_recorded but current"
    echo "  drift is only $lib_drift_count — lower it to close the ratchet."
fi
print_and_maybe_fix_shrink lib_shrink "$LIB_BASELINE" \
    "lib/domain baseline can tighten (files shrank but are still over ceiling):"

exit "$overall_fail"
