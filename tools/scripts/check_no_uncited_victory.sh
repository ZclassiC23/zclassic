#!/usr/bin/env bash
# Lint gate — no UNCITED victory claim in the one live-state page (HARD).
#
# WHY THIS EXISTS: this repo shipped 9+ "cured / at tip / fully synced" claims
# in six weeks, every one later false, and the owner counted ~103 "wedge FIXED"
# -> re-wedge cycles. A victory sentence with no machine-checkable evidence next
# to it is how a stale narrative outlives the node it describes. This gate makes
# an uncited victory claim in docs/HANDOFF.md (the SINGLE live-state page) fail
# `make lint`, so a "cured" claim cannot land without an evidence token or an
# explicit historical override.
#
# RULE: split docs/HANDOFF.md into blank-line-delimited paragraphs. A paragraph
# that contains a VICTORY PHRASE (case-insensitive, word-bounded) —
#   "at tip", "at-tip", "reaches tip", "holds tip", "fully synced", "cured",
#   "unwedged", "wedge cleared", "wedge closed", "wedge fixed",
#   "soak window open", "soak window running", "proven live", "live-proven",
#   "stable at tip"
# FAILS unless the SAME paragraph also carries a CITATION TOKEN:
#   "uptime-ledger", "slo-summary:", "VERDICT=PASS", "WALL_CLOCK_SECONDS",
#   "gap_vs_oracle", a `ts=<digits>` / `"ts": <digits>` stamp, or the explicit
#   per-paragraph override  <!-- victory-ok: <reason> -->  (for narrating a
#   HISTORICAL event only; never a current-state claim).
#
# HOLLOW-GATE RULE: docs/HANDOFF.md missing or < 10 lines = FAIL (the scan set
# must be non-empty, or an uncited claim could hide in a stubbed-out page).
#
# Standalone-runnable. Hermetic --selftest below. Fast: file read + grep only.
# Test hook: ZCL_LINT_MODE, when it points at an existing file, overrides the
# scanned doc (the C harness's run_gate_script passes its 2nd arg as
# ZCL_LINT_MODE); production `make lint` never sets it, so the real
# docs/HANDOFF.md is scanned.
set -euo pipefail

cd "$(dirname "$0")/../.."

# Victory phrases: matched case-insensitively and WORD-BOUNDED (grep -iw), so
# "secured" never trips "cured" and "sync=at_tip" (underscore) never trips
# "at-tip". Multi-word phrases carry their own internal spaces/hyphens.
VICTORY_RE='at tip|at-tip|reaches tip|holds tip|fully synced|cured|unwedged|wedge cleared|wedge closed|wedge fixed|soak window open|soak window running|proven live|live-proven|stable at tip'

# Citation tokens: machine-checkable evidence of a real, dated, ledgered proof.
# NOT word-bounded (tokens carry '=', ':', '_'). The victory-ok override is a
# citation of last resort for HISTORICAL narration.
CITATION_RE='uptime-ledger|slo-summary:|VERDICT=PASS|WALL_CLOCK_SECONDS|gap_vs_oracle|(ts=|"ts"[[:space:]]*:[[:space:]]*)[0-9]|victory-ok:'

g_found=0

# check_para <doc> <start-line> <end-line> <text>: emit a FAIL line if the
# paragraph carries a victory phrase but no citation token. Runs in the caller's
# shell (no subshell), so it updates g_found directly.
check_para() {
    local doc="$1" s="$2" e="$3" text="$4"
    local vmatch=""
    vmatch=$(printf '%s\n' "$text" | grep -iwoE "$VICTORY_RE" 2>/dev/null \
        | sort -u | paste -sd',' - || true)
    [ -n "$vmatch" ] || return 0
    if printf '%s\n' "$text" | grep -iqE "$CITATION_RE" 2>/dev/null; then
        return 0
    fi
    g_found=$((g_found + 1))
    echo "  ${doc##*/}:${s}-${e}: uncited victory phrase(s): ${vmatch}"
}

# scan_doc <path>: 0 = clean, 1 = uncited victory found, 2 = hollow scan set.
scan_doc() {
    local doc="$1"
    if [ ! -f "$doc" ]; then
        echo "FAIL: $doc is missing — the victory-claim scan set is empty."
        echo "      This is the one live-state page; it must exist and carry"
        echo "      current node state, or an uncited claim could hide here."
        return 2
    fi
    local nlines
    nlines=$(wc -l < "$doc")
    if [ "$nlines" -lt 10 ]; then
        echo "FAIL: $doc has $nlines lines (< 10) — hollow scan set."
        echo "      The live-state page must carry real current state, not a"
        echo "      stub; a near-empty file would pass this gate vacuously."
        return 2
    fi

    g_found=0
    local lineno=0 pstart=0 ptext="" active=0 line
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        if [ -z "${line//[[:space:]]/}" ]; then
            if [ "$active" = 1 ]; then
                check_para "$doc" "$pstart" "$((lineno - 1))" "$ptext"
                active=0; ptext=""
            fi
        else
            if [ "$active" = 0 ]; then
                pstart=$lineno; ptext="$line"; active=1
            else
                ptext="$ptext"$'\n'"$line"
            fi
        fi
    done < "$doc"
    if [ "$active" = 1 ]; then
        check_para "$doc" "$pstart" "$lineno" "$ptext"
    fi

    if [ "$g_found" != 0 ]; then
        echo ""
        echo "FAIL: $g_found uncited victory claim(s) in $doc."
        echo "  This repo shipped 9+ false 'cured / at tip' claims (~103 wedge-"
        echo "  FIXED -> re-wedge cycles). Cite a real proof in the SAME paragraph"
        echo "  (VERDICT=PASS, gap_vs_oracle, uptime-ledger, a ts= stamp, slo-summary:,"
        echo "  WALL_CLOCK_SECONDS) or, for HISTORICAL narration only, add"
        echo "  <!-- victory-ok: <reason> -->. Never override a current-state claim."
        return 1
    fi
    return 0
}

# ── Hermetic selftest ────────────────────────────────────────────────────────
if [ "${1:-}" = "--selftest" ]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    st_fail=0

    # Helper: run scan_doc, capture rc without tripping set -e.
    run_scan() { local f="$1"; if out=$(scan_doc "$f" 2>&1); then return 0; else return $?; fi; }

    cat > "$tmp/clean.md" <<'EOF'
# HANDOFF — current state

The canonical node is blocked. H* has not advanced for 13 days.
Primary blocker: catchup_stalled, a downstream symptom of the fold blocker.

The wedge root cause is closed in code; the live apply is pending the owner
gate. The tip_finalize rate bug is still open and gates any catch-up.

Read the live node before trusting this file.
Verify with typed status commands.
Do not deploy on unit-test green alone.
EOF

    cat > "$tmp/uncited.md" <<'EOF'
# HANDOFF — current state

The node is at tip and holding. Everything is fine now.
No further action is required and we can move on.

Filler line one to clear the hollow-gate line floor.
Filler line two to clear the hollow-gate line floor.
Filler line three to clear the hollow-gate line floor.
Filler line four to clear the hollow-gate line floor.
Filler line five to clear the hollow-gate line floor.
EOF

    cat > "$tmp/cited.md" <<'EOF'
# HANDOFF — current state

The soak run reached tip and held: VERDICT=PASS, gap_vs_oracle=0,
WALL_CLOCK_SECONDS=54, ts=1690000000. This is a ledgered proof.

Filler line one to clear the hollow-gate line floor.
Filler line two to clear the hollow-gate line floor.
Filler line three to clear the hollow-gate line floor.
Filler line four to clear the hollow-gate line floor.
Filler line five to clear the hollow-gate line floor.
EOF

    cat > "$tmp/override.md" <<'EOF'
# HANDOFF — current state

The old July claim that the wedge was cured is historical, not current.
<!-- victory-ok: narrating a superseded false-victory, not a live claim -->

Filler line one to clear the hollow-gate line floor.
Filler line two to clear the hollow-gate line floor.
Filler line three to clear the hollow-gate line floor.
Filler line four to clear the hollow-gate line floor.
Filler line five to clear the hollow-gate line floor.
EOF

    printf 'too short\n' > "$tmp/short.md"

    expect() { # <label> <path> <want-rc>
        local label="$1" f="$2" want="$3" got=0
        run_scan "$f" && got=0 || got=$?
        if [ "$got" = "$want" ]; then
            echo "  ok: $label (rc=$got)"
        else
            echo "  FAIL: $label expected rc=$want got rc=$got"
            st_fail=1
        fi
    }

    expect "clean doc passes"                 "$tmp/clean.md"        0
    expect "uncited victory trips"            "$tmp/uncited.md"     1
    expect "cited victory passes"             "$tmp/cited.md"       0
    expect "victory-ok override passes"       "$tmp/override.md"    0
    expect "hollow (short) doc fails"         "$tmp/short.md"       2
    expect "missing doc fails"                "$tmp/does-not-exist" 2

    if [ "$st_fail" != 0 ]; then
        echo "selftest: FAIL"
        exit 1
    fi
    echo "selftest: PASS"
    exit 0
fi

# ── Production scan ───────────────────────────────────────────────────────────
DOC="docs/HANDOFF.md"
if [ -n "${ZCL_LINT_MODE:-}" ] && [ -f "${ZCL_LINT_MODE}" ]; then
    DOC="${ZCL_LINT_MODE}"   # test-only override (see header)
fi

if scan_doc "$DOC"; then
    echo "  OK: no uncited victory claim in $DOC"
    exit 0
fi
exit 1
