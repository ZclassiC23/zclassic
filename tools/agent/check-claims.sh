#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check-claims.sh — judge a lane's gate claims from its receipts, without
# re-running a single gate.
#
# ── WHY THIS EXISTS ────────────────────────────────────────────────────────
# "make lint passed" costs the orchestrator a full re-run to check. This reads
# the receipts tools/agent/gate-receipt.sh left behind and answers three
# questions the prose cannot:
#
#   1. Did the gate run AT ALL for the gate the lane claims? (a claimed gate
#      with no receipt is the whole point — it exits non-zero.)
#   2. Does the receipt describe THIS tree? (head_sha, worktree, working-tree
#      digest — a receipt from three commits ago is not evidence about now.)
#   3. Does the STORED OUTPUT still say what the receipt says it says? The
#      verdict is re-derived here from the log's own success/failure tokens
#      and from the log's SHA3, never taken on the receipt's word.
#
# Check 3 is what makes the receipt awkward to forge: a `verdict=PASS` line is
# one edit, but it has to be backed by a log whose bytes hash to the recorded
# digest AND which literally contains the gate's success banner. See the
# forgery-model section of gate-receipt.sh for what this does NOT defend
# against — chiefly, a lane that writes both files by hand. This is evidence,
# not proof.
#
# ── USAGE ──────────────────────────────────────────────────────────────────
#   make check-claims CLAIMS="lint t-fast"
#   tools/agent/check-claims.sh --require lint,t-fast --dir .cache/agent-receipts
#   tools/agent/check-claims.sh                    # judge whatever is there
#
#   --require <a,b>     gates that MUST have a passing receipt. Missing one is
#                       a non-zero exit; that is the F7 answer.
#   --dir <path>        receipt directory (default $ZCL_RECEIPT_DIR or
#                       .cache/agent-receipts).
#   --max-age-hours <N> a receipt older than this is STALE_AGE (default: off).
#   --strict            also fail on a working-tree digest that has moved
#                       since the receipt (default: reported, not fatal —
#                       editing a file after a green lint is normal and the
#                       orchestrator should see it, not be blocked by it).
#   --json              emit machine-readable rows instead of the table.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=tools/scripts/sh_str.sh
. "$REPO/tools/scripts/sh_str.sh"
cd "$REPO"

DIR="${ZCL_RECEIPT_DIR:-$REPO/.cache/agent-receipts}"
REQUIRE_RAW="${CLAIMS:-}"
MAX_AGE_HOURS=0
STRICT=0
JSON=0

die() { echo "check-claims: $*" >&2; exit 2; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --require) [ "$#" -ge 2 ] || die "--require needs a value"
                   REQUIRE_RAW="$REQUIRE_RAW,$2"; shift 2 ;;
        --dir)     [ "$#" -ge 2 ] || die "--dir needs a value"; DIR="$2"; shift 2 ;;
        --max-age-hours) [ "$#" -ge 2 ] || die "--max-age-hours needs a value"
                   MAX_AGE_HOURS="$2"; shift 2 ;;
        --strict)  STRICT=1; shift ;;
        --json)    JSON=1; shift ;;
        -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
        *)         die "unknown option '$1'" ;;
    esac
done

# Deduped on purpose: `make check-claims CLAIMS="lint t-fast"` sets CLAIMS in
# the recipe ENVIRONMENT (make exports command-line variables) *and* passes it
# as --require, so every required gate arrived twice and a single missing gate
# was reported — and counted — twice.
REQUIRED=()
declare -A _seen=()
IFS=', ' read -r -a _req <<< "$REQUIRE_RAW"
for g in ${_req[@]+"${_req[@]}"}; do
    [ -n "$g" ] || continue
    [ -n "${_seen[$g]:-}" ] && continue
    _seen["$g"]=1
    REQUIRED+=("$g")
done

SHA3_BIN="$REPO/build/bin/agent_sha3"
[ -x "$SHA3_BIN" ] || die "missing $SHA3_BIN — run a gate through tools/agent/gate-receipt.sh first (it builds it), or: make agent-sha3"
sha3_of_file() { "$SHA3_BIN" "$1" | cut -d' ' -f1; }
sha3_of_stdin() { "$SHA3_BIN" -; }

if [ ! -d "$DIR" ]; then
    echo "check-claims: no receipt directory at $DIR"
    if [ "${#REQUIRED[@]}" -gt 0 ]; then
        printf 'check-claims: NO RECEIPT for required gate: %s\n' "${REQUIRED[@]}"
        echo "check-claims: FAIL — ${#REQUIRED[@]} claimed gate(s) have no evidence at all."
        exit 1
    fi
    echo "check-claims: nothing claimed and nothing to check."
    exit 0
fi
DIR="$(cd "$DIR" && pwd)"

# ── current ground truth, derived here and not read from any receipt ───────
now_head="$(git rev-parse HEAD 2>/dev/null || echo UNKNOWN)"
now_diff="$(git diff HEAD 2>/dev/null | sha3_of_stdin)"
now_epoch="$(date +%s)"

mapfile -t receipts < <(ls -1 "$DIR"/*.receipt 2>/dev/null | sort || true)
if [ "${#receipts[@]}" -eq 0 ]; then
    echo "check-claims: $DIR holds no *.receipt files"
    if [ "${#REQUIRED[@]}" -gt 0 ]; then
        printf 'check-claims: NO RECEIPT for required gate: %s\n' "${REQUIRED[@]}"
        exit 1
    fi
    exit 0
fi

# field <file> <key> — last value wins; keys are unique in a v1 receipt.
field() { sed -n "s/^$2=//p" "$1" | tail -n1; }

# ── SHAPE FLOOR (the anti-two-line-log prong) ─────────────────────────────
# Token presence alone is weak: a hand-written two-line log containing only
# the success banner satisfies it. So for gates whose output has a KNOWN
# STRUCTURE, also require the log to have that structure at the scale the
# tree currently demands.
#
# `make lint` (parallel path, tools/lint/run_lint.sh) prints exactly one
# `PASS check-<name>` / `FAIL check-<name>` line per gate. The expected count
# is DERIVED from the Makefile's own LINT_GATES list — never hardcoded — so
# this floor rises by itself the day a gate is added and can never be a stale
# number.
#
# ZERO per-gate lines is a FAILURE, not an unknown. That case is precisely the
# cheap forgery — a hand-written log holding the banner and nothing else —
# and it was reported as SHAPE_UNKNOWN in the first cut of this function,
# which let the two-line fake pass. Serial mode (ZCL_LINT_SERIAL=1) is the one
# legitimate way to print no per-gate lines, and it is NOT indistinguishable:
# the serial recipe prints `all checks passed (serial)` where the parallel one
# prints `all checks passed`. So serial is recognised from the log's own bytes
# and exempted by evidence, rather than the floor being softened for everyone.
lint_gate_count() {
    awk '
        /^LINT_GATES[[:space:]]*:?=/ { inside = 1 }
        inside {
            n = split($0, a, /[[:space:]]+/)
            for (i = 1; i <= n; i++) if (a[i] ~ /^check-/) c++
            if ($0 !~ /\\[[:space:]]*$/) exit
        }
        END { print c + 0 }
    ' "$REPO/Makefile"
}

# shape_note <gate-slug> <logpath> -> prints "<OK|BAD>|<note-or-empty>".
#
# It returns the verdict IN ITS STDOUT rather than setting a variable: the
# caller reads it through `$( )`, which forks, and an assignment made inside a
# command substitution is lost. The first cut of this function set a SHAPE_OK
# global and the caller read it back unchanged — so the note printed
# "SHAPE_TOO_SMALL" while the verdict stayed OK, which is the exact hollow-gate
# shape this whole file exists to catch. Caught by running the fabricated
# receipt through it.
shape_note() {
    local slug="$1" log="$2" want got
    case "$slug" in
        lint|lint-cached|lint-cold-audit) ;;
        *) echo "OK|"; return 0 ;;
    esac
    want="$(lint_gate_count)"
    if [ "$want" -le 0 ]; then echo "OK|"; return 0; fi
    got="$(grep -cE '^(PASS|FAIL|CACHED) check-' "$log" 2>/dev/null || true)"
    if [ "$got" -eq 0 ]; then
        # Serial mode, proven from the log's own banner — not assumed.
        if [ "$(grep -cF 'all checks passed (serial)' "$log" 2>/dev/null || true)" -gt 0 ]; then
            echo "OK|SHAPE_SERIAL"
        else
            echo "BAD|SHAPE_TOO_SMALL(0/$want gate lines)"
        fi
        return 0
    fi
    if [ "$got" -lt "$want" ]; then
        echo "BAD|SHAPE_TOO_SMALL($got/$want gate lines)"
    else
        echo "OK|"
    fi
}

declare -A LATEST=()      # gate -> receipt path with the highest chain_index
declare -A LATEST_IDX=()
declare -A RUNS=()        # gate -> how many receipts exist
declare -A FAILED_RUNS=() # gate -> how many of them are FAIL
declare -A WALL_MS=()     # gate -> wall_ms of the latest
declare -A ROWVERDICT=()
declare -A ROWNOTE=()

problems=0
chain_expected_idx=-1
chain_prev_sha3="GENESIS"
chain_break=""

for r in "${receipts[@]}"; do
    schema="$(field "$r" receipt_schema)"
    gate="$(field "$r" gate)"
    idx="$(field "$r" chain_index)"
    case "$idx" in ''|*[!0-9]*) idx=-1 ;; esac

    notes=()
    verdict_ok=1

    if [ "$schema" != "zcl.gate_receipt.v1" ]; then
        notes+=("BAD_SCHEMA($schema)"); verdict_ok=0
    fi
    [ -n "$gate" ] || { gate="(unnamed)"; notes+=("NO_GATE_FIELD"); verdict_ok=0; }

    # 1) the receipt's own integrity: recompute receipt_sha3 over everything
    #    above it. Catches a field edited in place by anything that did not
    #    also recompute the digest.
    claimed_self="$(field "$r" receipt_sha3)"
    actual_self="$(sed '$d' "$r" | sha3_of_stdin)"
    if [ "$claimed_self" != "$actual_self" ]; then
        notes+=("RECEIPT_ALTERED"); verdict_ok=0
    fi

    # 2) the log must exist and still hash to the recorded digest.
    logname="$(field "$r" output_path)"
    logpath="$DIR/$logname"
    claimed_out="$(field "$r" output_sha3)"
    if [ -z "$logname" ] || [ ! -f "$logpath" ]; then
        notes+=("OUTPUT_MISSING"); verdict_ok=0
    else
        actual_out="$(sha3_of_file "$logpath")"
        if [ "$actual_out" != "$claimed_out" ]; then
            notes+=("OUTPUT_ALTERED"); verdict_ok=0
        else
            # 3) RE-DERIVE the verdict from the log itself. The receipt's
            #    `verdict=` line is never trusted on its own.
            miss=0; forb=0
            while IFS= read -r tok; do
                [ -n "$tok" ] || continue
                if [ "$(grep -cF -- "$tok" "$logpath" 2>/dev/null || true)" -eq 0 ]; then
                    miss=$(( miss + 1 ))
                fi
            done < <(sed -n 's/^expect_token=//p' "$r")
            while IFS= read -r tok; do
                [ -n "$tok" ] || continue
                if [ "$(grep -cF -- "$tok" "$logpath" 2>/dev/null || true)" -gt 0 ]; then
                    forb=$(( forb + 1 ))
                fi
            done < <(sed -n 's/^forbid_token=//p' "$r")
            [ "$miss" -eq 0 ] || { notes+=("SUCCESS_TOKEN_ABSENT"); verdict_ok=0; }
            [ "$forb" -eq 0 ] || { notes+=("FAILURE_TOKEN_PRESENT"); verdict_ok=0; }

            sn="$(shape_note "$gate" "$logpath")"
            [ -z "${sn#*|}" ] || notes+=("${sn#*|}")
            [ "${sn%%|*}" = "OK" ] || verdict_ok=0
        fi
    fi

    # 4) does it describe THIS tree, in THIS worktree?
    r_head="$(field "$r" head_sha)"
    if [ "$r_head" != "$now_head" ]; then
        notes+=("STALE_HEAD(${r_head:0:9})"); verdict_ok=0
    fi
    r_wt="$(field "$r" worktree_path)"
    if [ "$r_wt" != "$REPO" ]; then
        notes+=("FOREIGN_WORKTREE"); verdict_ok=0
    fi
    r_diff="$(field "$r" tree_diff_sha3_after)"
    if [ "$r_diff" != "$now_diff" ]; then
        notes+=("TREE_MOVED")
        [ "$STRICT" -eq 1 ] && verdict_ok=0
    fi

    # 5) the gate's own status.
    r_exit="$(field "$r" exit_status)"
    if [ "$r_exit" != "0" ]; then
        notes+=("EXIT_$r_exit"); verdict_ok=0
    fi
    r_verdict="$(field "$r" verdict)"
    if [ "$r_verdict" != "PASS" ]; then
        notes+=("RECORDED_$r_verdict"); verdict_ok=0
    fi
    if [ "$(field "$r" expect_tokens)" = "0" ] && [ "$(field "$r" forbid_tokens)" = "0" ]; then
        notes+=("EXIT_STATUS_ONLY")
    fi

    # 6) age.
    if [ "$MAX_AGE_HOURS" != "0" ]; then
        r_start="$(field "$r" started_at_utc)"
        r_epoch="$(date -u -d "$r_start" +%s 2>/dev/null || echo 0)"
        if [ "$r_epoch" -gt 0 ]; then
            age_h=$(( (now_epoch - r_epoch) / 3600 ))
            if [ "$age_h" -gt "$MAX_AGE_HOURS" ]; then
                notes+=("STALE_AGE(${age_h}h)"); verdict_ok=0
            fi
        fi
    fi

    # 7) the chain. A receipt inserted after the fact does not link, unless
    #    every later receipt was rewritten too — which is worth saying out
    #    loud rather than pretending it is impossible.
    if [ "$idx" -ne $(( chain_expected_idx + 1 )) ]; then
        chain_break="${chain_break}index $idx follows $chain_expected_idx; "
    fi
    r_prev="$(field "$r" prev_receipt_sha3)"
    if [ "$r_prev" != "$chain_prev_sha3" ]; then
        chain_break="${chain_break}$(basename "$r") does not link to its predecessor; "
        notes+=("CHAIN_BREAK")
    fi
    chain_expected_idx="$idx"
    chain_prev_sha3="$(sha3_of_file "$r")"

    RUNS["$gate"]=$(( ${RUNS[$gate]:-0} + 1 ))
    [ "$verdict_ok" -eq 1 ] || FAILED_RUNS["$gate"]=$(( ${FAILED_RUNS[$gate]:-0} + 1 ))

    if [ "$idx" -ge "${LATEST_IDX[$gate]:--1}" ]; then
        LATEST_IDX["$gate"]="$idx"
        LATEST["$gate"]="$r"
        WALL_MS["$gate"]="$(field "$r" wall_ms)"
        if [ "$verdict_ok" -eq 1 ]; then ROWVERDICT["$gate"]="OK"; else ROWVERDICT["$gate"]="BAD"; fi
        if [ "${#notes[@]}" -eq 0 ]; then
            ROWNOTE["$gate"]="-"
        else
            ROWNOTE["$gate"]="$(IFS=,; echo "${notes[*]}")"
        fi
    fi
done

# ── report ────────────────────────────────────────────────────────────────
declare -A IS_REQUIRED=()
for g in ${REQUIRED[@]+"${REQUIRED[@]}"}; do IS_REQUIRED["$g"]=1; done

missing=()
for g in ${REQUIRED[@]+"${REQUIRED[@]}"}; do
    [ -n "${LATEST[$g]:-}" ] || missing+=("$g")
done

if [ "$JSON" -eq 1 ]; then
    echo "{"
    echo "  \"head\": \"$now_head\","
    echo "  \"receipt_dir\": \"$DIR\","
    echo "  \"gates\": ["
    first=1
    for g in $(printf '%s\n' "${!LATEST[@]}" | sort); do
        [ "$first" -eq 1 ] || echo "  ,"
        first=0
        printf '    {"gate":"%s","verdict":"%s","runs":%d,"failed_runs":%d,"wall_ms":%s,"receipt":"%s","notes":"%s","required":%s}\n' \
            "$g" "${ROWVERDICT[$g]}" "${RUNS[$g]}" "${FAILED_RUNS[$g]:-0}" \
            "${WALL_MS[$g]:-0}" "$(basename "${LATEST[$g]}")" "${ROWNOTE[$g]}" \
            "$([ -n "${IS_REQUIRED[$g]:-}" ] && echo true || echo false)"
    done
    echo "  ],"
    printf '  "missing_required": ['
    first=1
    for g in ${missing[@]+"${missing[@]}"}; do
        [ "$first" -eq 1 ] || printf ', '
        first=0; printf '"%s"' "$g"
    done
    echo "]"
    echo "}"
else
    echo "check-claims: $DIR"
    echo "check-claims: HEAD $now_head  (${#receipts[@]} receipt(s), ${#LATEST[@]} distinct gate(s))"
    echo ""
    printf '%-3s %-22s %-7s %5s %5s %10s  %s\n' "REQ" "GATE" "VERDICT" "RUNS" "FAIL" "WALL_MS" "NOTES"
    printf '%-3s %-22s %-7s %5s %5s %10s  %s\n' "---" "----------------------" "-------" "-----" "-----" "----------" "-----"
    for g in $(printf '%s\n' "${!LATEST[@]}" | sort); do
        printf '%-3s %-22s %-7s %5s %5s %10s  %s\n' \
            "$([ -n "${IS_REQUIRED[$g]:-}" ] && echo "*" || echo " ")" \
            "$g" "${ROWVERDICT[$g]}" "${RUNS[$g]}" "${FAILED_RUNS[$g]:-0}" \
            "${WALL_MS[$g]:-?}" "${ROWNOTE[$g]}"
    done
    echo ""
    for g in ${missing[@]+"${missing[@]}"}; do
        echo "  MISSING  $g — claimed, but no receipt exists. Run it as:"
        echo "             tools/agent/gate-receipt.sh --gate $g -- make $g"
    done
    if [ -n "$chain_break" ]; then
        echo "  CHAIN    receipt chain does not link: ${chain_break%; }"
        echo "           (a receipt written or removed after the fact; the chain"
        echo "            is a tell, not a proof — see gate-receipt.sh)"
    fi
fi

for g in $(printf '%s\n' "${!LATEST[@]}" | sort); do
    [ "${ROWVERDICT[$g]}" = "OK" ] || problems=$(( problems + 1 ))
done
[ -z "$chain_break" ] || problems=$(( problems + 1 ))
problems=$(( problems + ${#missing[@]} ))

if [ "$problems" -gt 0 ]; then
    [ "$JSON" -eq 1 ] || echo "check-claims: FAIL — $problems problem(s); ${#missing[@]} claimed gate(s) without a receipt."
    exit 1
fi
[ "$JSON" -eq 1 ] || echo "check-claims: PASS — every receipt is well-formed, describes HEAD $now_head, and its stored output still says so."
exit 0
