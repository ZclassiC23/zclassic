#!/usr/bin/env bash
# Correct gate wrapper: lint then full suite, success keyed on ALL-TESTS-PASSED
# (never on grep matching a summary line — SOME-TESTS-FAILED matches too).
# Usage: gate-and-report.sh <lintlog> <testlog>
#
# Manual operator/agent tool — intentionally no in-repo caller: it is run by
# hand (or by an agent) as the self-gate before reporting a lane done. Owning
# runbook: docs/work/agent-protocol.md (the "self-gates (build + focused
# tests + make lint)" step) — the PASS-TOKEN, never a grep-match, is the
# acceptance bar this wraps.
set -u
LINTLOG="${1:?lintlog}"; TESTLOG="${2:?testlog}"
cd "$(git rev-parse --show-toplevel)" || exit 3
if ! make lint >"$LINTLOG" 2>&1; then
  echo "GATE: LINT FAILED"; grep -iE "FAIL —|grew to|Error 1|violation" "$LINTLOG" | tail -8; exit 1
fi
echo "GATE: LINT OK"
# Full binary link — build-only compiles library objects but NOT src/main.c or
# the final binaries, so it cannot catch a broken entry point or a link gap.
if ! make -j"$(nproc)" >>"$LINTLOG" 2>&1; then
  echo "GATE: FULL BUILD FAILED"; grep -iE "error:|undefined reference|Error 1" "$LINTLOG" | tail -8; exit 1
fi
echo "GATE: FULL BUILD OK"
make test-parallel >"$TESTLOG" 2>&1
if grep -q "ALL TESTS PASSED" "$TESTLOG" && ! grep -q "SOME TESTS FAILED" "$TESTLOG"; then
  echo "GATE: SUITE OK — $(grep -E 'ALL TESTS PASSED' "$TESTLOG" | tail -1)"; exit 0
fi
echo "GATE: SUITE FAILED — $(grep -E 'SOME TESTS FAILED' "$TESTLOG" | tail -1)"
grep -B1 "FAIL," "$TESTLOG" | grep "====" | head -12
exit 2
