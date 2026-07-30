#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_proof_server_pin.sh — the proof-server promotion binding must stay
# self-recording, not just prose.
#
# WHY THIS EXISTS: tools/ship.sh's proof-server guard used to instruct the
# operator to "re-tag the candidate afterwards so the tag still names what
# runs" — and nothing ever performed that re-tag. The prose described a
# binding no code produced, and the running box ended up unpinnable
# (`agentbuild` reports `"build_commit":"external"`, and the source_id cannot
# be reverse-mapped to a commit). tools/scripts/proof_server_pin.sh and its
# wiring into tools/ship.sh fix that; this gate is the anti-rot check so the
# same failure — an instruction in prose that nothing performs — cannot
# silently return.
#
# Two things checked:
#   1. tools/scripts/proof_server_pin.sh --self-test passes (hermetic: a
#      throwaway git repo under /tmp, no network, no proof-server contact).
#   2. tools/ship.sh's promotion path still actually CALLS
#      `proof_server_pin.sh record` — grepped, not inferred. This is the
#      literal shape of the original defect: catch the day the call is
#      deleted (refactor, revert, merge conflict) while the surrounding prose
#      still claims recording happens.
#
# What this gate deliberately does NOT check: that a proof-server/* tag
# exists. A fresh clone has none — nobody has shipped a promotion yet in that
# checkout — and that is an operational fact about deploy history, not a
# defect in the source tree. `tools/scripts/proof_server_pin.sh check` is the
# live operational check for "is a pin recorded and does the box match it";
# this gate only proves the recording MACHINERY exists and stays wired.

set -uo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1

fail=0

# 1. Hermetic self-test of the pin recorder/checker.
PIN_SCRIPT=tools/scripts/proof_server_pin.sh
if [ ! -r "$PIN_SCRIPT" ]; then
    echo "FAIL: $PIN_SCRIPT is missing — the proof-server promotion has no recorder"
    fail=1
else
    out=""
    rc=0
    out="$(bash "$PIN_SCRIPT" --self-test 2>&1)" || rc=$?
    # Decide on the extracted PASS line, not on the pipeline's exit status:
    # under pipefail a matching `printf | grep -q` can surface printf's SIGPIPE
    # 141 instead of grep's 0, which reads a genuine PASS as a missing PASS
    # line. MEASURED 2026-07-30: a passing transcript is 32 bytes, so the
    # inversion is NOT reachable at this size — a shape fix, not a live-bug
    # fix, kept because a failing transcript is unbounded and nobody
    # re-measures. Same regex; without -q grep drains stdin.
    pass_line="$(printf '%s\n' "$out" | grep '^PROOF SERVER PIN SELF-TEST: PASS$' || true)"
    if [ "$rc" != "0" ] || [ -z "$pass_line" ]; then
        echo "FAIL: $PIN_SCRIPT --self-test (rc=$rc; no 'PROOF SERVER PIN SELF-TEST: PASS' line)"
        printf '%s\n' "$out"
        fail=1
    else
        echo "  ok: $PIN_SCRIPT --self-test"
    fi
fi

# 2. Anti-rot: ship.sh's promotion path must actually invoke the recorder,
#    not just describe recording in a comment. Scoped to the real call
#    (script name + subcommand) so a mention in a comment or die-message
#    alone does not satisfy this — it must be code that runs.
SHIP_SCRIPT=tools/ship.sh
if [ ! -r "$SHIP_SCRIPT" ]; then
    echo "FAIL: $SHIP_SCRIPT is missing"
    fail=1
elif ! grep -qE 'proof_server_pin\.sh[[:space:]]+record\b' "$SHIP_SCRIPT"; then
    echo "FAIL: $SHIP_SCRIPT no longer calls 'proof_server_pin.sh record' — the"
    echo "      promotion path would go back to describing a binding it does"
    echo "      not record. Wire the call back in after the remote health"
    echo "      check confirms the running daemon reports the candidate's"
    echo "      source id."
    fail=1
else
    echo "  ok: $SHIP_SCRIPT calls 'proof_server_pin.sh record'"
fi

# Note (not a failure): a fresh clone / CI checkout has zero proof-server/*
# tags. That is expected — see header — and is not scanned for here.

if [ "$fail" != 0 ]; then
    exit 1
fi
echo "check_proof_server_pin: clean — recorder self-test passes and ship.sh still wires it into the promotion path"
