#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# zclassic23-cutover-selftest.sh — hermetic proof of zclassic23-cutover.sh's
# preflight comparison + auto-rollback logic. NO live nodes, NO real systemd:
#   * SYSTEMCTL=echo turns unit stop/start into visible no-ops,
#   * CUTOVER_HSTAR_READER points at a mock that returns injected H* per role,
#   * two throwaway fixture datadirs stand in for canonical + candidate.
# Every case asserts the exit code, the verdict line, AND the on-disk datadir
# layout (promotion actually swaps; rollback actually restores).
#
# Emits a final "cutover-selftest: PASS" line iff every case passed.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CUTOVER="$SCRIPT_DIR/zclassic23-cutover.sh"
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/zcl-cutover-selftest.XXXXXX")"
trap 'rm -rf "$SANDBOX"' EXIT

fails=0
fail() { printf '[cutover-selftest] FAIL: %s\n' "$*" >&2; fails=$((fails + 1)); }
ok()   { printf '[cutover-selftest] PASS: %s\n' "$*"; }

# ── mock H* reader: returns $MOCK_H_<ROLE> or -1 ────────────────────────────
MOCK_READER="$SANDBOX/mock_hstar.sh"
cat > "$MOCK_READER" <<'EOF'
#!/bin/sh
role="$1"
var="MOCK_H_$(printf '%s' "$role" | tr 'a-z-' 'A-Z_')"
eval "val=\${$var:-}"
[ -n "$val" ] && printf '%s\n' "$val" || printf '%s\n' "-1"
EOF
chmod +x "$MOCK_READER"

# Fresh fixture datadirs: canonical/ (marker CANON), candidate/ (marker CAND).
setup_fixtures() {
    local root="$1"
    rm -rf "$root"
    mkdir -p "$root/canonical" "$root/candidate"
    echo CANON > "$root/canonical/marker"
    echo CAND  > "$root/candidate/marker"
}

marker_of() { cat "$1/marker" 2>/dev/null || echo MISSING; }

# run_cutover <case-root> <extra cutover args...> — invokes the script with the
# mock wired in; per-case MOCK_H_* + knobs come from the caller's environment.
run_cutover() {
    local root="$1"; shift
    SYSTEMCTL="${SYSTEMCTL_MOCK:-echo}" \
    CANONICAL_DATADIR="$root/canonical" \
    CANONICAL_RPCPORT=19998 \
    CANDIDATE_RPCPORT=19999 \
    CUTOVER_HSTAR_READER="$MOCK_READER" \
    POLL_INTERVAL=1 STOP_GRACE=0 \
    sh "$CUTOVER" --candidate="$root/candidate" "$@"
}

# ── case 1: preflight REFUSE — candidate behind canonical ───────────────────
c1() {
    local r="$SANDBOX/c1"; setup_fixtures "$r"
    local out rc
    out="$(MOCK_H_CANONICAL_PRE=100 MOCK_H_CANDIDATE_PRE=90 \
           run_cutover "$r" --yes 2>&1)"; rc=$?
    [ "$rc" = 2 ] || fail "c1: expected exit 2 (behind), got $rc"
    printf '%s' "$out" | grep -q "BEHIND canonical" || fail "c1: no BEHIND message"
    [ "$(marker_of "$r/canonical")" = CANON ] || fail "c1: canonical datadir touched"
    [ "$(marker_of "$r/candidate")" = CAND ] || fail "c1: candidate datadir touched"
    [ "$rc" = 2 ] && [ "$(marker_of "$r/canonical")" = CANON ] && ok "case1 refuses a candidate behind canonical, touches nothing"
}

# ── case 2: preflight REFUSE — no --yes (owner gate) ────────────────────────
c2() {
    local r="$SANDBOX/c2"; setup_fixtures "$r"
    local out rc
    out="$(MOCK_H_CANONICAL_PRE=100 MOCK_H_CANDIDATE_PRE=105 \
           run_cutover "$r" 2>&1)"; rc=$?
    [ "$rc" = 2 ] || fail "c2: expected exit 2 (no --yes), got $rc"
    printf '%s' "$out" | grep -q "owner action" || fail "c2: no owner-gate message"
    [ "$(marker_of "$r/canonical")" = CANON ] || fail "c2: canonical datadir touched without --yes"
    ls -d "$r"/canonical.pre-cutover-* >/dev/null 2>&1 && fail "c2: a swap happened without --yes"
    [ "$rc" = 2 ] && ok "case2 refuses without --yes (owner gate), touches nothing"
}

# ── case 3: PROMOTE — candidate ahead, reaches the bar ──────────────────────
c3() {
    local r="$SANDBOX/c3"; setup_fixtures "$r"
    local out rc
    out="$(MOCK_H_CANONICAL_PRE=100 MOCK_H_CANDIDATE_PRE=105 \
           MOCK_H_CANDIDATE_POSTSTOP=-1 MOCK_H_CANONICAL_POST=105 \
           run_cutover "$r" --yes --timeout=5 2>&1)"; rc=$?
    [ "$rc" = 0 ] || fail "c3: expected exit 0 (promoted), got $rc"
    printf '%s' "$out" | grep -q "CUTOVER: PROMOTED" || fail "c3: no PROMOTED verdict"
    [ "$(marker_of "$r/canonical")" = CAND ] || fail "c3: candidate not promoted into canonical path"
    [ -e "$r/candidate" ] && fail "c3: candidate dir still present after promote"
    local dem; dem="$(ls -d "$r"/canonical.pre-cutover-* 2>/dev/null | head -1)"
    [ -n "$dem" ] && [ "$(marker_of "$dem")" = CANON ] || fail "c3: old canonical not preserved as pre-cutover backup"
    [ "$rc" = 0 ] && [ "$(marker_of "$r/canonical")" = CAND ] && ok "case3 promotes candidate, preserves old canonical, verdict PROMOTED"
}

# ── case 4: ROLLBACK — promoted node never reaches the bar ──────────────────
c4() {
    local r="$SANDBOX/c4"; setup_fixtures "$r"
    local out rc
    out="$(MOCK_H_CANONICAL_PRE=100 MOCK_H_CANDIDATE_PRE=105 \
           MOCK_H_CANDIDATE_POSTSTOP=-1 MOCK_H_CANONICAL_POST=42 \
           run_cutover "$r" --yes --timeout=1 2>&1)"; rc=$?
    [ "$rc" = 1 ] || fail "c4: expected exit 1 (rolled back), got $rc"
    printf '%s' "$out" | grep -q "CUTOVER: ROLLED-BACK" || fail "c4: no ROLLED-BACK verdict"
    # Datadir layout must be fully restored.
    [ "$(marker_of "$r/canonical")" = CANON ] || fail "c4: old canonical NOT restored (got $(marker_of "$r/canonical"))"
    [ "$(marker_of "$r/candidate")" = CAND ] || fail "c4: candidate NOT restored (got $(marker_of "$r/candidate"))"
    ls -d "$r"/canonical.pre-cutover-* >/dev/null 2>&1 && fail "c4: pre-cutover backup left behind after rollback"
    [ "$rc" = 1 ] && [ "$(marker_of "$r/canonical")" = CANON ] && [ "$(marker_of "$r/candidate")" = CAND ] \
        && ok "case4 rolls back cleanly, restores BOTH datadirs, verdict ROLLED-BACK"
}

# ── case 5: REFUSE — candidate unhealthy/unreadable (H*=-1) ──────────────────
c5() {
    local r="$SANDBOX/c5"; setup_fixtures "$r"
    local out rc
    out="$(MOCK_H_CANONICAL_PRE=100 \
           run_cutover "$r" --yes 2>&1)"; rc=$?   # candidate-pre unset -> -1
    [ "$rc" = 2 ] || fail "c5: expected exit 2 (unhealthy candidate), got $rc"
    printf '%s' "$out" | grep -q "not healthy/readable" || fail "c5: no unhealthy-candidate message"
    [ "$(marker_of "$r/canonical")" = CANON ] || fail "c5: canonical touched on unhealthy candidate"
    [ "$rc" = 2 ] && ok "case5 refuses an unreadable/unhealthy candidate"
}

# ── case 6: FAILOVER — canonical DOWN, candidate healthy, promotes ──────────
c6() {
    local r="$SANDBOX/c6"; setup_fixtures "$r"
    local out rc
    # canonical-pre unset -> -1 (down). Bar becomes candidate H*.
    out="$(MOCK_H_CANDIDATE_PRE=200 \
           MOCK_H_CANDIDATE_POSTSTOP=-1 MOCK_H_CANONICAL_POST=200 \
           run_cutover "$r" --yes --timeout=5 2>&1)"; rc=$?
    [ "$rc" = 0 ] || fail "c6: expected exit 0 (failover promote), got $rc"
    printf '%s' "$out" | grep -q "FAILOVER" || fail "c6: no FAILOVER warning"
    printf '%s' "$out" | grep -q "CUTOVER: PROMOTED" || fail "c6: no PROMOTED verdict"
    [ "$(marker_of "$r/canonical")" = CAND ] || fail "c6: candidate not promoted in failover"
    [ "$rc" = 0 ] && ok "case6 promotes on failover when canonical is down"
}

# ── case 7: REFUSE pre-swap — candidate still running after stop ─────────────
c7() {
    local r="$SANDBOX/c7"; setup_fixtures "$r"
    local out rc
    # poststop still answers (105) -> refuse to rename a live datadir.
    out="$(MOCK_H_CANONICAL_PRE=100 MOCK_H_CANDIDATE_PRE=105 \
           MOCK_H_CANDIDATE_POSTSTOP=105 MOCK_H_CANONICAL_POST=105 \
           run_cutover "$r" --yes --timeout=5 2>&1)"; rc=$?
    [ "$rc" = 2 ] || fail "c7: expected exit 2 (candidate not stopped), got $rc"
    printf '%s' "$out" | grep -q "refusing to rename a live datadir" || fail "c7: no live-datadir refusal"
    [ "$(marker_of "$r/canonical")" = CANON ] || fail "c7: canonical touched despite live candidate"
    [ "$(marker_of "$r/candidate")" = CAND ] || fail "c7: candidate touched despite being live"
    [ "$rc" = 2 ] && ok "case7 refuses to rename a candidate that is still running"
}

# ── case 8: ROLLBACK on start failure — new canonical won't start ────────────
c8() {
    local r="$SANDBOX/c8"; setup_fixtures "$r"
    # Mock systemctl that FAILS `start zclassic23` (the canonical unit) but
    # succeeds everything else, forcing the start-failure rollback branch.
    local mc="$SANDBOX/mock_systemctl_c8.sh"
    cat > "$mc" <<'EOF'
#!/bin/sh
if [ "$1" = start ] && [ "$2" = zclassic23 ]; then
    echo "mock systemctl: refusing to start $2" >&2
    exit 1
fi
echo "mock systemctl: $*"
exit 0
EOF
    chmod +x "$mc"
    local out rc
    out="$(SYSTEMCTL_MOCK="$mc" \
           MOCK_H_CANONICAL_PRE=100 MOCK_H_CANDIDATE_PRE=105 \
           MOCK_H_CANDIDATE_POSTSTOP=-1 MOCK_H_CANONICAL_POST=105 \
           run_cutover "$r" --yes --timeout=5 2>&1)"; rc=$?
    [ "$rc" = 1 ] || fail "c8: expected exit 1 (start-failure rollback), got $rc"
    printf '%s' "$out" | grep -q "CUTOVER: ROLLED-BACK" || fail "c8: no ROLLED-BACK verdict on start failure"
    [ "$(marker_of "$r/canonical")" = CANON ] || fail "c8: old canonical NOT restored after start-failure rollback"
    [ "$(marker_of "$r/candidate")" = CAND ] || fail "c8: candidate NOT restored after start-failure rollback"
    [ "$rc" = 1 ] && [ "$(marker_of "$r/canonical")" = CANON ] \
        && ok "case8 rolls back and restores datadirs when the promoted unit won't start"
}

c1; c2; c3; c4; c5; c6; c7; c8

if [ "$fails" -eq 0 ]; then
    echo "cutover-selftest: PASS"
    exit 0
fi
echo "cutover-selftest: FAIL ($fails case(s))"
exit 1
