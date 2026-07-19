#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Fixture harness for deploy/zclassic23-launch.sh. Drives the launcher in its
# ZCL_LAUNCH_TEST_ECHO seam (prints the decision instead of exec'ing a node)
# against fake binary slots, asserting slot selection, streak counting, and the
# fallback-at-threshold + marker behavior. Invoked by the test_binary_ab_fallback
# test group; also runnable standalone. Prints "LAUNCHER-HARNESS OK" on success.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCH="$here/../../deploy/zclassic23-launch.sh"
[ -x "$LAUNCH" ] || { echo "FAIL: launcher not found/exec at $LAUNCH"; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
slots="$work/slots"
cur="$work/current"; printf '#!/bin/sh\nexit 0\n' > "$cur"; chmod +x "$cur"

fail=0
run() { ZCL_BINARY_SLOTS_DIR="$slots" ZCL_LAUNCH_TEST_ECHO=1 "$LAUNCH" "$cur" -x 2>/dev/null; }
check() { if [ "$1" = "$2" ]; then echo "OK $3"; else echo "FAIL $3: '$1' != '$2'"; fail=1; fi; }
exec_of() { printf '%s\n' "$1" | sed -n 's/^EXEC //p'; }
field_of() { printf '%s\n' "$1" | sed -n "s/^$2=//p"; }

# 1. Fresh: runs current, increments streak 0 -> 1, not fallback.
out=$(run)
check "$(exec_of "$out")"        "$cur" "fresh runs current"
check "$(field_of "$out" STREAK_WRITTEN)" "1"   "fresh increments streak to 1"
check "$(field_of "$out" FALLBACK_ACTIVE)" ""   "fresh is not fallback"

# 2. Streak at threshold but NO last-good yet: nothing to fall back to.
echo 3 > "$slots/boot-fail-streak"
check "$(exec_of "$(run)")" "$cur" "streak>=3 without last-good stays current"

# 3. last-good present + streak at threshold: falls back, marker set.
printf '#!/bin/sh\nexit 0\n' > "$slots/last-good"; chmod +x "$slots/last-good"
echo 3 > "$slots/boot-fail-streak"
out=$(run)
check "$(exec_of "$out")"          "$slots/last-good" "streak>=3 with last-good falls back"
check "$(field_of "$out" FALLBACK_ACTIVE)" "1"        "fallback marker set"

# 4. Below threshold: current even with last-good present.
echo 2 > "$slots/boot-fail-streak"
out=$(run)
check "$(exec_of "$out")"          "$cur" "streak=2 below threshold stays current"
check "$(field_of "$out" CURRENT)" "$cur" "current env exported in normal path"

if [ "$fail" = 0 ]; then echo "LAUNCHER-HARNESS OK"; else echo "LAUNCHER-HARNESS FAIL"; exit 1; fi
