#!/usr/bin/env bash
# Lint gate — test-registration drift guard (HARD).
#
# THE BUG THIS PREVENTS (lane-3, 2026-06-22): three test entry points
# (test_refold_from_anchor_fatal, test_refold_auto_arm, test_anchor_selfmint)
# lived in dedicated lib/test/src/test_<name>.c files, COMPILED and linked
# into the test binaries, yet were ABSENT from the TEST_LIST X()-macro list in
# lib/test/src/test_parallel.c (and not dispatched by the legacy serial runner
# lib/test/src/test.c either). They therefore proved NOTHING — green forever,
# never executed. This gate makes that drift FAIL CI.
#
# ── CONVENTION (verified against the source) ──────────────────────────────
# A test "entry point" is the function that bears the SAME name as its
# dedicated file: lib/test/src/test_<name>.c defining
#     int test_<name>(void)
# (with the body opener on its own line — the project style). Multi-test files
# (e.g. test_coins_amount_codec.c, test_models.c) and group files define helper
# / sub-test functions whose names do NOT match the host filename; those are
# deliberately NOT treated as entry points (no false positives on helpers).
#
# An entry point is "dispatched" (i.e. actually runs) iff its <name> is either
#   1. registered in the TEST_LIST X(<name>) macro in test_parallel.c
#      (the `make test` parallel runner — the doctrine runner of record), OR
#   2. invoked as `test_<name>()` from the legacy serial runner test.c.
# Both runners link the same TEST_SRCS_NO_MAIN (Makefile:149), so a function
# dispatched by EITHER does run somewhere. A filename-matching entry point
# dispatched by NEITHER is an orphan: compiled but never executed.
#
# The X() macro maps to the symbol via (test_parallel.c):
#     #define DECL_TEST(name) extern int test_##name(void);   // forward decl
#     #define ROW_TEST(name)  {"test_" #name, test_##name},   // dispatch row
# i.e. X(foo) <=> the function test_foo().
#
# Fail-loud: grep exit >=2 (real error) aborts; an empty entry-point scan
# (convention drift) aborts — we never report "clean" off a broken scan.
set -euo pipefail

validate_unique_registrations() {
    local registered_raw="$1"
    local source_label="$2"
    local duplicates

    duplicates=$(printf '%s\n' "$registered_raw" | sort | uniq -d)
    if [ -z "$duplicates" ]; then
        return 0
    fi

    echo "FAIL: duplicate TEST_LIST registration(s) in $source_label:" >&2
    while IFS= read -r name; do
        [ -n "$name" ] && echo "    X($name)" >&2
    done <<< "$duplicates"
    echo "  Each parallel test group must have one canonical row." >&2
    return 1
}

# Keep the duplicate detector from becoming a decorative check: prove its
# positive and negative controls on every gate run before trusting it on the
# real registry. This is pure string processing and never mutates the tree.
if ! validate_unique_registrations $'alpha\nbeta' '<selftest-clean>' \
        >/dev/null 2>&1; then
    echo "check_test_registration: FATAL — uniqueness selftest rejected clean input" >&2
    exit 2
fi
set +e
uniqueness_selftest_out=$(validate_unique_registrations \
    $'alpha\nbeta\nalpha' '<selftest-duplicate>' 2>&1)
uniqueness_selftest_rc=$?
set -e
if [ "$uniqueness_selftest_rc" -ne 1 ] || \
   ! grep -qF 'X(alpha)' <<< "$uniqueness_selftest_out"; then
    echo "check_test_registration: FATAL — uniqueness negative control failed" >&2
    exit 2
fi

cd "$(dirname "$0")/../.."

# Impact plans are part of test registration: every plan token must have an
# exact primary group, and a rule naming a registered test source must select
# that source's own group. Keep this in the canonical lint gate so a broken
# map cannot evade the focused group intended to audit the map itself.
if ! tools/dev/test-group-list.sh --check-impact-rules; then
    echo "check_test_registration: FAIL — impact proof registration drift" >&2
    exit 1
fi

TEST_DIR="lib/test/src"
PARALLEL="$TEST_DIR/test_parallel.c"
SERIAL="$TEST_DIR/test.c"

for f in "$PARALLEL" "$SERIAL"; do
    if [ ! -f "$f" ]; then
        echo "check_test_registration: FATAL — expected runner file missing: $f" >&2
        echo "  The test-runner layout drifted; refusing to report 'clean'." >&2
        exit 2
    fi
done

# ── Registered names (TEST_LIST X(...) in test_parallel.c) ──
# Scan only the TEST_LIST macro region (from its #define up to SPEC_LIST's).
set +e
reg_block=$(awk '/#define[ \t]+TEST_LIST\(/{f=1} f{print} /#define[ \t]+SPEC_LIST\(/{exit}' "$PARALLEL")
awkrc=$?
set -e
if [ "$awkrc" -ne 0 ]; then
    echo "check_test_registration: FATAL — awk failed slicing TEST_LIST from $PARALLEL" >&2
    exit 2
fi
registered_raw=$(printf '%s\n' "$reg_block" | grep -oE 'X\([a-zA-Z0-9_]+\)' | sed -E 's/^X\((.*)\)$/\1/')

# A duplicate row runs the same group twice, inflates the advertised group
# count, and can hide the absence of a genuinely distinct test behind a green
# total. Check the raw list before de-duplicating it for membership lookups.
if ! validate_unique_registrations "$registered_raw" "$PARALLEL"; then
    exit 1
fi

registered=$(printf '%s\n' "$registered_raw" | sort -u)

# FAIL-LOUD floor: TEST_LIST must yield a non-trivial set, else the slice or
# macro shape drifted and a real orphan could slip through a tiny scan.
reg_count=$(printf '%s\n' "$registered" | grep -c . || true)
if [ "$reg_count" -lt 100 ]; then
    echo "check_test_registration: FATAL — only $reg_count TEST_LIST X() entries parsed" >&2
    echo "  from $PARALLEL (expected >=100). The macro shape or slice drifted;" >&2
    echo "  refusing to validate against a near-empty registration set." >&2
    exit 2
fi

# ── Names dispatched by the legacy serial runner (test_<name>() calls) ──
set +e
serial_calls=$(grep -oE 'test_[a-zA-Z0-9_]+\(\)' "$SERIAL")
grc=$?
set -e
if [ "$grc" -ge 2 ]; then
    echo "check_test_registration: FATAL — grep over $SERIAL failed (exit $grc)" >&2
    exit 2
fi
dispatched=$(printf '%s\n' "$serial_calls" | sed -E 's/^test_(.*)\(\)$/\1/' | sort -u)

# Union: a name that runs in EITHER runner.
runs=$(printf '%s\n%s\n' "$registered" "$dispatched" | grep -v '^$' | sort -u)

# ── Enumerate filename-matching entry points and check each ──
orphans=""
entry_count=0
for f in "$TEST_DIR"/test_*.c; do
    [ -e "$f" ] || continue
    base=$(basename "$f" .c)        # test_<name>
    name=${base#test_}              # <name>
    # The runner files own main(), not entry points.
    if [ "$base" = "test_parallel" ] || [ "$base" = "test" ]; then
        continue
    fi
    # Entry point := defines `int test_<name>(void)` body opener (own line).
    set +e
    grep -qE "^int[[:space:]]+test_${name}\(void\)[[:space:]]*\$" "$f"
    grc=$?
    set -e
    if [ "$grc" -ge 2 ]; then
        echo "check_test_registration: FATAL — grep over $f failed (exit $grc)" >&2
        exit 2
    fi
    [ "$grc" -eq 0 ] || continue    # no filename-matching entry point in this file
    entry_count=$((entry_count + 1))
    # rc 1 = genuinely unregistered; rc >=2 (fork failure under load, etc.)
    # must FATAL, not masquerade as an orphan — seen misreporting a
    # registered test right after a 32-worker suite run (2026-07-10).
    # Feed grep with a here-string: `printf | grep -q` under pipefail can make
    # printf receive SIGPIPE after grep's early match and falsely report 141.
    set +e
    grep -qxF "$name" <<< "$runs"
    grc=$?
    set -e
    if [ "$grc" -ge 2 ]; then
        echo "check_test_registration: FATAL — membership grep for $name failed (exit $grc)" >&2
        exit 2
    fi
    if [ "$grc" -eq 1 ]; then
        orphans="${orphans}${name}\t${f}\n"
    fi
done

# FAIL-LOUD floor: we must have found a healthy number of entry points, else
# the convention/path drifted and the gate is hollow.
if [ "$entry_count" -lt 100 ]; then
    echo "check_test_registration: FATAL — only $entry_count filename-matching test" >&2
    echo "  entry points found under $TEST_DIR (expected >=100). The naming" >&2
    echo "  convention or path drifted; refusing to report 'clean'." >&2
    exit 2
fi

# ── PRONG B: canonical-registry drift ────────────────────────────────────
# Prong A above accepts a test dispatched by EITHER runner. That union is too
# weak in one direction, and the weak direction is the one that matters:
# test_parallel.c is the CANONICAL registry and `make test-parallel` is the
# doctrine runner (and the acceptance gate). test.c is the legacy sequential
# shape, and `build/bin/test_zcl` is explicitly never run. So a test dispatched
# ONLY by test.c runs in NO gate — it is dead coverage that reports nothing,
# exactly the failure prong A was written to prevent, just one runner over.
#
# Found on 2026-07-25 by this prong: 5 such names. One of them
# (test_lcc_write_rules) did not merely fail to run — it FAILED when finally
# executed, having been wrong since it was written, because no runner had ever
# reached it.
#
# A serial-dispatched name is canonical-clean iff EITHER
#   (1) it is itself in TEST_LIST/SPEC_LIST, OR
#   (2) the filename-matching entry point of the FILE that defines it is in
#       TEST_LIST/SPEC_LIST — i.e. it is a sub-test reached through a
#       registered parent group (test_simnet_wire.c's per-scenario functions
#       are dispatched by the registered `simnet_wire` group).
# Index every entry-point definition ONCE (name -> defining file). Grepping
# per dispatched name re-scanned the whole test tree ~600 times and cost ~13 s
# of the lint wall on its own.
set +e
defs=$(grep -HE "^int[[:space:]]+test_[a-zA-Z0-9_]+\(void\)[[:space:]]*\$" \
       "$TEST_DIR"/*.c 2>/dev/null)
grc=$?
set -e
if [ "$grc" -ge 2 ]; then
    echo "check_test_registration: FATAL — grep indexing entry points failed (exit $grc)" >&2
    exit 2
fi
declare -A DEF_FILE=()
while IFS= read -r line; do
    [ -n "$line" ] || continue
    f=${line%%:*}
    fn=${line#*:}
    fn=${fn#int}
    fn=${fn##*[[:space:]]}          # test_<name>(void)
    fn=${fn%%(*}                    # test_<name>
    [ -n "${DEF_FILE[${fn#test_}]:-}" ] || DEF_FILE[${fn#test_}]="$f"
done <<< "$defs"

if [ "${#DEF_FILE[@]}" -lt 100 ]; then
    echo "check_test_registration: FATAL — indexed only ${#DEF_FILE[@]} entry-point" >&2
    echo "  definitions under $TEST_DIR (expected >=100). Refusing to report 'clean'." >&2
    exit 2
fi

drift=""
serial_checked=0
for name in $dispatched; do
    [ -n "$name" ] || continue
    # (1) directly registered?
    if grep -qxF "$name" <<< "$registered"; then
        serial_checked=$((serial_checked + 1))
        continue
    fi
    # Not defined in a dedicated lib/test/src file: a helper or an out-of-tree
    # symbol, not a registrable group. Out of scope.
    def_file="${DEF_FILE[$name]:-}"
    [ -n "$def_file" ] || continue
    serial_checked=$((serial_checked + 1))
    # (2) is the defining file's own group registered?
    parent=$(basename "$def_file" .c); parent=${parent#test_}
    if grep -qxF "$parent" <<< "$registered"; then
        continue
    fi
    drift="${drift}${name}\t${def_file}\n"
done

# FAIL-LOUD floor: the serial runner must have yielded a real population, else
# the scan silently emptied and this prong is decorative.
if [ "$serial_checked" -lt 100 ]; then
    echo "check_test_registration: FATAL — only $serial_checked serial-dispatched" >&2
    echo "  names resolved against the canonical registry (expected >=100)." >&2
    echo "  The dispatch or definition convention drifted; refusing to report 'clean'." >&2
    exit 2
fi

if [ -n "$drift" ]; then
    echo "FAIL: test(s) dispatched ONLY by the legacy serial runner (test.c) and"
    echo "  ABSENT from the canonical TEST_LIST/SPEC_LIST registry in"
    echo "  test_parallel.c. \`make test-parallel\` is the doctrine runner and the"
    echo "  acceptance gate; build/bin/test_zcl is never run. These therefore"
    echo "  execute in NO gate and prove NOTHING:"
    echo ""
    printf '%b' "$drift" | while IFS=$'\t' read -r n path; do
        [ -n "$n" ] && echo "    test_$n   ($path)"
    done
    echo ""
    echo "  Fix: add X(<name>) to TEST_LIST in lib/test/src/test_parallel.c — and"
    echo "  RUN it (make t-fast ONLY=test_<name>) before assuming it passes."
    echo "  Do NOT delete the test, and do NOT silence this by removing the"
    echo "  test.c dispatch."
    exit 1
fi

if [ -n "$orphans" ]; then
    echo "FAIL: test entry point(s) DEFINED + COMPILED but dispatched by NEITHER"
    echo "  the TEST_LIST X() macro (test_parallel.c) nor the serial runner"
    echo "  (test.c). They prove NOTHING — green forever, never executed:"
    echo ""
    printf '%b' "$orphans" | while IFS=$'\t' read -r n path; do
        [ -n "$n" ] && echo "    test_$n   ($path)"
    done
    echo ""
    echo "  Fix: register the name in the TEST_LIST X(...) list in"
    echo "  lib/test/src/test_parallel.c (the doctrine \`make test\` runner), or"
    echo "  dispatch it from lib/test/src/test.c. Do NOT delete the test to"
    echo "  silence this gate."
    exit 1
fi

echo "check_test_registration: clean — all $entry_count test entry points are dispatched"
echo "  ($reg_count registered in TEST_LIST; the rest covered by the serial runner)"
echo "  canonical-registry drift: 0 of $serial_checked serial-dispatched name(s)"
echo "  run only under the legacy test.c runner"
exit 0
