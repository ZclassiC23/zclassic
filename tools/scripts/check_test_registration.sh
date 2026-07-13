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

cd "$(dirname "$0")/../.."

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
registered=$(printf '%s\n' "$reg_block" | grep -oE 'X\([a-zA-Z0-9_]+\)' | sed -E 's/^X\((.*)\)$/\1/' | sort -u)

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
exit 0
