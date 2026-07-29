#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# zcl_intervene.sh — the front door for "I am about to touch a node".
# Install as `zcl-intervene` (see the bottom of this comment).
#
# Every human and every agent that restarts a unit, edits a drop-in,
# replaces a binary, or otherwise reaches into a running lane declares it
# here FIRST. tools/scripts/intervention_ledger.sh independently detects the
# change; the two are joined by time. A detected change with no declaration
# inside the attribution window is recorded as `unattributed`, and that is
# the whole mechanism: it is what turns "the node ran N days untouched"
# from a story into a claim that the record can contradict.
#
# Declaring does NOT do the thing. This wrapper can optionally run the
# command for you (`-- <cmd>`), which is the recommended form because it
# makes the declaration and the action the same keystroke and records the
# action's exit status; but a bare declaration is always valid, including
# after the fact ("that was me"), because the window is symmetric.
#
# Usage:
#   zcl-intervene "<reason>"                 declare only
#   zcl-intervene "<reason>" -- <cmd> [args] declare, run, record the result
#   zcl-intervene --list [since_epoch]       what has been declared
#   zcl-intervene --selftest
#
# Env:
#   ZCL_INTERVENE_DIR    ledger dir (shared with intervention_ledger.sh)
#   ZCL_INTERVENE_ACTOR  override the recorded actor (agents should set
#                        this to their lane/agent id — "rhett" on every
#                        line is a worse record than "wt3-executor")
#   ZCL_INTERVENE_ORIGIN override the recorded origin (local/ssh/agent/ci)
#
# Install (owner action, NOT performed by this script):
#   ln -s ~/github/zclassic23/tools/scripts/zcl_intervene.sh ~/bin/zcl-intervene
#
# This script never touches a unit or a datadir itself. Anything it runs
# under `--` is the operator's own command, run as the operator.

set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LEDGER_SH="$SCRIPT_DIR/intervention_ledger.sh"

if [ ! -x "$LEDGER_SH" ] && [ ! -r "$LEDGER_SH" ]; then
    echo "zcl-intervene: FATAL missing $LEDGER_SH — cannot record a declaration" >&2
    exit 3
fi

usage() {
    sed -n '/^# Usage:/,/^#   zcl-intervene --selftest/p' "${BASH_SOURCE[0]}" |
        sed 's/^# \{0,1\}//'
}

cmd_list() {
    bash "$LEDGER_SH" summary "${1:-0}"
}

main() {
    case "${1:-}" in
        '' | -h | --help)
            usage
            # No reason given is not a declaration. Exiting 2 keeps a
            # mistyped `zcl-intervene` from reading as "declared".
            [ -z "${1:-}" ] && exit 2
            exit 0
            ;;
        --list) shift; cmd_list "${1:-0}"; exit 0 ;;
        --selftest) shift; cmd_selftest; exit 0 ;;
    esac

    local reason="$1"; shift
    local -a runcmd=()
    if [ "${1:-}" = "--" ]; then
        shift
        runcmd=("$@")
        if [ "${#runcmd[@]}" -eq 0 ]; then
            echo "zcl-intervene: FAIL '--' given with no command" >&2
            exit 2
        fi
    elif [ "$#" -gt 0 ]; then
        echo "zcl-intervene: FAIL unexpected argument '$1' — put a command after '--'" >&2
        exit 2
    fi

    # Enrich the reason with provenance the detector cannot infer: which
    # checkout the declarer was standing in, and what it was built from.
    # An attribution that cannot be traced back to a commit is only half
    # an attribution.
    local head_commit="" repo=""
    repo="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || true)"
    head_commit="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || true)"
    local full_reason="$reason"
    [ -n "$repo" ] && full_reason="$full_reason [repo=$repo head=${head_commit:-unknown}]"
    if [ "${#runcmd[@]}" -gt 0 ]; then
        full_reason="$full_reason [cmd=${runcmd[*]}]"
    fi

    bash "$LEDGER_SH" declare "$full_reason" >/dev/null || {
        echo "zcl-intervene: FAIL could not record the declaration — NOT running the command" >&2
        exit 1
    }
    echo "zcl-intervene: declared: $full_reason"

    [ "${#runcmd[@]}" -gt 0 ] || exit 0

    # Run it. The exit status is recorded as a second declaration rather
    # than swallowed: "I declared a restart and it failed" and "I declared
    # a restart and it worked" are different facts about the same window.
    local rc=0
    "${runcmd[@]}" || rc=$?
    bash "$LEDGER_SH" declare "result rc=$rc for: $full_reason" >/dev/null || true
    echo "zcl-intervene: command exited rc=$rc"
    exit "$rc"
}

# ── selftest ───────────────────────────────────────────────────────────

st_fail() { echo "selftest: FAIL $*" >&2; exit 1; }

cmd_selftest() {
    ST_TMP="$(mktemp -d /tmp/zcl-intervene-selftest.XXXXXX)"
    local T="$ST_TMP"
    trap 'rm -rf "$ST_TMP"' EXIT
    local L="$T/intervention-ledger.jsonl"

    # A) a bare declaration lands in the shared ledger with actor+origin.
    ZCL_INTERVENE_DIR="$T" ZCL_INTERVENE_ACTOR=wt13-agent ZCL_INTERVENE_ORIGIN=agent \
        bash "${BASH_SOURCE[0]}" "restarting canonical to pick up build X" >/dev/null \
        || st_fail "case=declare bare declaration must exit 0"
    grep -q '"kind":"declared"' "$L" || { cat "$L" >&2; st_fail "case=declare nothing written"; }
    grep -q '"actor":"wt13-agent","origin":"agent"' "$L" \
        || { cat "$L" >&2; st_fail "case=declare actor/origin not recorded"; }
    grep -q 'restarting canonical to pick up build X' "$L" \
        || { cat "$L" >&2; st_fail "case=declare reason not recorded"; }
    echo "selftest: ok case=declare"

    # B) `-- cmd` runs the command, records its exit status, and PROPAGATES
    #    it. A wrapper that swallows a failed restart is worse than none.
    local rc=0
    ZCL_INTERVENE_DIR="$T" bash "${BASH_SOURCE[0]}" "run a thing" -- false >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || st_fail "case=run-cmd must propagate the command's exit status (got $rc)"
    grep -q '"reason":"result rc=1 for: run a thing' "$L" \
        || { cat "$L" >&2; st_fail "case=run-cmd failure result not recorded"; }
    rc=0
    ZCL_INTERVENE_DIR="$T" bash "${BASH_SOURCE[0]}" "run a good thing" -- true >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 0 ] || st_fail "case=run-cmd success must exit 0"
    grep -q '"reason":"result rc=0 for: run a good thing' "$L" \
        || { cat "$L" >&2; st_fail "case=run-cmd success result not recorded"; }
    echo "selftest: ok case=run-cmd"

    # C) no reason is refused with a non-zero status, and writes nothing.
    local before; before="$(wc -l < "$L")"
    if ZCL_INTERVENE_DIR="$T" bash "${BASH_SOURCE[0]}" >/dev/null 2>&1; then
        st_fail "case=no-reason a bare invocation must not read as a declaration"
    fi
    [ "$(wc -l < "$L")" -eq "$before" ] \
        || st_fail "case=no-reason must not append anything"
    echo "selftest: ok case=no-reason"

    # D) a stray argument without `--` is refused rather than silently
    #    dropped — `zcl-intervene "reason" systemctl restart x` must not
    #    look like it ran anything.
    if ZCL_INTERVENE_DIR="$T" bash "${BASH_SOURCE[0]}" "reason" systemctl >/dev/null 2>&1; then
        st_fail "case=stray-arg must be refused"
    fi
    echo "selftest: ok case=stray-arg"

    # E) the declaration is visible to the DETECTOR's window logic, which
    #    is the only thing that makes it an attribution rather than a note.
    local out
    out="$(ZCL_INTERVENE_DIR="$T" bash "$LEDGER_SH" summary 2>&1)"
    # 5 = one bare declaration + two `-- cmd` runs, each of which files a
    # second line carrying the command's exit status.
    printf '%s' "$out" | grep -q 'declared=5' \
        || { printf '%s\n' "$out" >&2; st_fail "case=visible-to-detector summary must count the declarations"; }
    # And declarations ALONE must not read as a clean window: with no
    # detector cycles there is nothing to have been attributed.
    printf '%s' "$out" | grep -q 'VERDICT: UNPROVEN' \
        || { printf '%s\n' "$out" >&2; st_fail "case=visible-to-detector declarations without detection must be UNPROVEN"; }
    echo "selftest: ok case=visible-to-detector"

    echo "selftest: PASS"
}

main "$@"
