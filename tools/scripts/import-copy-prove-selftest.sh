#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# import-copy-prove-selftest.sh — hermetic proof that
# tools/scripts/import-copy-prove.sh's own gating logic is correct, with NO
# real node binary, NO real zclassicd, NO real chainstate, and NO network
# ports opened. It fakes $NODE_BIN and $RPC_BIN (both env-overridable on the
# real script) with tiny fixture scripts that emit exactly the RPC response
# substrings import-copy-prove.sh greps/seds for, and drives the real script
# end-to-end against those fakes.
#
# This exists because the real importer (docs/work/fast-sync-to-tip-plan-
# 2026-07-16.md section 4) is not merged yet — there is no live wedge to
# reproduce and no zclassicd chainstate copy to import in CI/dev. What CAN
# be proven hermetically, and what this proves, is that the DRIVER correctly
# computes GATE (a)/(b)/(c) and the overall verdict from a given set of node
# responses — i.e. that the harness will gate correctly the moment the real
# importer lands. It does not (and cannot) prove anything about the importer
# itself.
#
# All fixture paths live under a throwaway sandbox; nothing under $HOME or
# any real datadir is read or written.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$REPO/tools/scripts/import-copy-prove.sh"
SANDBOX="$(mktemp -d /tmp/zcl-import-copy-prove-selftest.XXXXXX)"
chmod 700 "$SANDBOX"

cleanup() { rm -rf "$SANDBOX"; }
trap cleanup EXIT

FAKE_NODE="$SANDBOX/fake-zclassic23"
FAKE_RPC="$SANDBOX/fake-zcl-rpc"
SRC_FIXTURE="$SANDBOX/fixture-datadir"
CHAINSTATE_FIXTURE="$SANDBOX/fixture-chainstate"
ZD_FIXTURE="$SANDBOX/fixture-zclassicd-datadir"
IMPORT_BEHAVIOR_FILE="$SANDBOX/import-behavior"
HEADER_REFRESH_BEHAVIOR_FILE="$SANDBOX/header-refresh-behavior"

WEDGE=3176325
WEDGE_PLUS1=3176326

fail() {
    printf '[import-copy-prove-selftest] FAIL: %s\n' "$*" >&2
    [ -r "$OUTPUT" ] && sed 's/^/[selftest-output] /' "$OUTPUT" >&2
    exit 1
}

assert_rc() {
    local got="$1" want="$2" msg="$3"
    [ "$got" = "$want" ] || fail "$msg (expected rc=$want, got rc=$got)"
}

assert_contains() {
    grep -q -- "$2" "$1" || fail "$3 (missing '$2' in $1)"
}

# ── build the fixtures (once; each test overrides behavior via files) ──

cat > "$FAKE_NODE" <<'NODE_EOF'
#!/bin/sh
# Fake zclassic23 for the import-copy-prove hermetic selftest ONLY.
if [ "${1:-}" = "--importblockindex" ]; then
    # Header-refresh step (step 1b): zclassic23 --importblockindex <src> [<target>]
    behavior="ok"
    [ -r "${FAKE_HEADER_REFRESH_BEHAVIOR_FILE:-}" ] && behavior="$(cat "$FAKE_HEADER_REFRESH_BEHAVIOR_FILE")"
    case "$behavior" in
        ok)
            target="${3:-}"
            if [ -n "$target" ]; then
                mkdir -p "$(dirname "$target")" 2>/dev/null || true
                : > "$target"
            fi
            echo "=== ZClassic Block-Index (header) Import ==="
            exit 0 ;;
        fail)
            echo "header-refresh fixture: forced failure" >&2
            exit 1 ;;
    esac
fi
datadir=""
import_arg=""
for a in "$@"; do
    case "$a" in
        -datadir=*)                 datadir="${a#-datadir=}" ;;
        -import-complete-shielded=*) import_arg="${a#-import-complete-shielded=}" ;;
    esac
done
if [ -n "$import_arg" ]; then
    behavior="ok"
    [ -r "$FAKE_IMPORT_BEHAVIOR_FILE" ] && behavior="$(cat "$FAKE_IMPORT_BEHAVIOR_FILE")"
    case "$behavior" in
        ok)
            echo "IMPORT COMPLETE (committed=5): selftest fixture ok"
            exit 0 ;;
        fail)
            echo "IMPORT REFUSED — selftest fixture forced failure (wedge intact)" >&2
            exit 1 ;;
        missing)
            # Simulates an unmerged flag: the real argv loop ignores unknown
            # flags and just no-ops this one call (this fixture short-circuits
            # rather than falling through to a real long-running boot, purely
            # for selftest speed — the harness-under-test behavior being
            # proven is identical either way: no banner => FAIL).
            exit 0 ;;
    esac
fi
# Normal boot mode.
mkdir -p "$datadir"
: > "$datadir/.cookie"
trap 'exit 0' TERM INT
while true; do sleep 1; done
NODE_EOF
chmod +x "$FAKE_NODE"

cat > "$FAKE_RPC" <<'RPC_EOF'
#!/bin/sh
# Fake zcl-rpc for the import-copy-prove hermetic selftest ONLY. Emits raw
# substrings the real driver's sed/grep patterns look for -- does not need
# to be a fully faithful JSON-RPC envelope.
method="$1"
params="${2:-}"
role="copy"
[ "$ZCL_DATADIR" = "${FAKE_ZD_DIR:-}" ] && role="zd"

case "$method" in
    getblockcount)
        if [ "$role" = "zd" ]; then
            printf '{"result":%s,"error":null,"id":1}\n' "${FAKE_ZD_TIP:-0}"
        else
            printf '{"result":%s,"error":null,"id":1}\n' "${FAKE_COPY_TIP:--1}"
        fi
        ;;
    getblockhash)
        table="${FAKE_COPY_HASHES:-}"
        [ "$role" = "zd" ] && table="${FAKE_ZD_HASHES:-}"
        hash=""
        [ -r "$table" ] && hash="$(awk -F: -v h="$params" '$1==h{print $2}' "$table")"
        printf '{"result":"%s","error":null,"id":1}\n' "$hash"
        ;;
    dumpstate)
        case "$params" in
            *blocker*)          cat "${FAKE_BLOCKER_RESPONSE:-/dev/null}" ;;
            *reducer_frontier*) cat "${FAKE_FRONTIER_RESPONSE:-/dev/null}" ;;
            *sovereignty*)
                if [ -r "${FAKE_SOVEREIGNTY_RESPONSE:-}" ]; then
                    cat "$FAKE_SOVEREIGNTY_RESPONSE"
                else
                    echo '{"error":"unknown subsystem: sovereignty"}'
                fi
                ;;
            *) echo '{}' ;;
        esac
        ;;
    *) echo '{}' ;;
esac
RPC_EOF
chmod +x "$FAKE_RPC"

mkdir -p "$SRC_FIXTURE" "$CHAINSTATE_FIXTURE" "$ZD_FIXTURE"
: > "$SRC_FIXTURE/node.db"
: > "$CHAINSTATE_FIXTURE/CURRENT"

BLOCKER_CLEAR="$SANDBOX/blocker-clear.json"
printf '{"active_count":0,"blockers":[]}' > "$BLOCKER_CLEAR"
BLOCKER_ANCHOR_GAP="$SANDBOX/blocker-anchor-gap.json"
printf '{"active_count":1,"blockers":[{"id":"utxo_apply.anchor_backfill_gap","class":"permanent"}]}' \
    > "$BLOCKER_ANCHOR_GAP"

FRONTIER_CONTINUOUS="$SANDBOX/frontier-continuous.json"
printf '{"hstar":%d,"coins_applied_height":%d}' "$((WEDGE + 5))" "$((WEDGE + 6))" \
    > "$FRONTIER_CONTINUOUS"
FRONTIER_GAP="$SANDBOX/frontier-gap.json"
printf '{"hstar":%d,"coins_applied_height":%d}' "$((WEDGE + 5))" "$((WEDGE + 9))" \
    > "$FRONTIER_GAP"

TIP=$((WEDGE + 5))
COPY_HASHES_MATCH="$SANDBOX/copy-hashes-match.txt"
{
    printf '%d:aaaa1111\n' "$WEDGE"
    printf '%d:bbbb2222\n' "$WEDGE_PLUS1"
    printf '%d:cccc3333\n' "$TIP"
} > "$COPY_HASHES_MATCH"
ZD_HASHES_MATCH="$SANDBOX/zd-hashes-match.txt"
cp "$COPY_HASHES_MATCH" "$ZD_HASHES_MATCH"

COPY_HASHES_MISMATCH="$SANDBOX/copy-hashes-mismatch.txt"
{
    printf '%d:aaaa1111\n' "$WEDGE"
    printf '%d:WRONGHASH\n' "$WEDGE_PLUS1"
    printf '%d:cccc3333\n' "$TIP"
} > "$COPY_HASHES_MISMATCH"

run_script() {
    local rc
    set +e
    env \
        ZCL_NODE_BIN="$FAKE_NODE" \
        ZCL_RPC_BIN="$FAKE_RPC" \
        FAKE_IMPORT_BEHAVIOR_FILE="$IMPORT_BEHAVIOR_FILE" \
        FAKE_HEADER_REFRESH_BEHAVIOR_FILE="$HEADER_REFRESH_BEHAVIOR_FILE" \
        FAKE_ZD_DIR="$ZD_FIXTURE" \
        FAKE_COPY_TIP="$TIP" \
        FAKE_ZD_TIP="$((TIP + 100))" \
        FAKE_BLOCKER_RESPONSE="${TEST_BLOCKER_RESPONSE:-$BLOCKER_CLEAR}" \
        FAKE_FRONTIER_RESPONSE="${TEST_FRONTIER_RESPONSE:-$FRONTIER_CONTINUOUS}" \
        FAKE_COPY_HASHES="${TEST_COPY_HASHES:-$COPY_HASHES_MATCH}" \
        FAKE_ZD_HASHES="$ZD_HASHES_MATCH" \
        "$SCRIPT" --deadline=15 \
        --src="$SRC_FIXTURE" \
        --chainstate-src="$CHAINSTATE_FIXTURE" \
        --copy-dir="$COPY_DIR" \
        --zclassicd-datadir="$ZD_FIXTURE" \
        "${EXTRA_ARGS[@]}" \
        > "$OUTPUT" 2>&1
    rc=$?
    set -e
    echo "$rc"
}

fresh_copy_dir() {
    printf '%s/.zclassic-c23-COPY-%s-import' "$SANDBOX" "$(date +%s%N)"
}

# ── test 1: --dry-run never touches the filesystem or asserts a verdict ──

test_dry_run_prints_plan_only() {
    OUTPUT="$SANDBOX/out-dryrun"
    COPY_DIR="$(fresh_copy_dir)"
    EXTRA_ARGS=(--dry-run)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 0 "dry-run did not exit 0"
    assert_contains "$OUTPUT" "import-copy-prove plan" "dry-run missing plan banner"
    assert_contains "$OUTPUT" "no filesystem or process action taken" "dry-run missing no-op notice"
    [ ! -e "$COPY_DIR" ] || fail "dry-run created a copy-dir on disk"
    printf '[import-copy-prove-selftest] PASS: --dry-run prints the plan and touches nothing\n'
}

# ── test 2: usage/safety refusals (exit 2, no side effects) ─────────────

test_refuses_non_copy_path() {
    OUTPUT="$SANDBOX/out-noncopy"
    COPY_DIR="$SANDBOX/not-marked-path"
    EXTRA_ARGS=(--dry-run)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 2 "non-COPY --copy-dir was not refused"
    assert_contains "$OUTPUT" "must contain the '/.zclassic-c23-COPY-' marker" \
        "non-COPY refusal message missing"
    printf '[import-copy-prove-selftest] PASS: refuses a --copy-dir without the -COPY- marker\n'
}

test_refuses_preexisting_copy_dir() {
    OUTPUT="$SANDBOX/out-preexist"
    COPY_DIR="$(fresh_copy_dir)"
    mkdir -p "$COPY_DIR"
    EXTRA_ARGS=(--dry-run)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 2 "pre-existing --copy-dir was not refused"
    assert_contains "$OUTPUT" "already exists, refusing to overwrite" \
        "pre-existing refusal message missing"
    printf '[import-copy-prove-selftest] PASS: refuses a --copy-dir that already exists\n'
}

# ── test 3: phase 1 dependency gate — no IMPORTED banner => FAIL, not a hang ──

test_missing_importer_fails_fast_with_dependency_note() {
    OUTPUT="$SANDBOX/out-missing"
    COPY_DIR="$(fresh_copy_dir)"
    echo missing > "$IMPORT_BEHAVIOR_FILE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    assert_rc "$rc" 1 "an unimplemented importer flag did not FAIL"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "missing-importer run did not report FAIL"
    assert_contains "$OUTPUT" "did not report IMPORT COMPLETE" "missing-importer FAIL reason absent"
    assert_contains "$OUTPUT" "is not yet merged on this build" \
        "missing-importer FAIL did not point at the dependency notice"
    printf '[import-copy-prove-selftest] PASS: an unmerged importer fails fast, not silently\n'
}

test_importer_explicit_refusal_fails() {
    OUTPUT="$SANDBOX/out-refused"
    COPY_DIR="$(fresh_copy_dir)"
    echo fail > "$IMPORT_BEHAVIOR_FILE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    assert_rc "$rc" 1 "an explicit importer REFUSED did not FAIL"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "importer refusal did not report FAIL"
    printf '[import-copy-prove-selftest] PASS: an explicit importer REFUSED fails the harness\n'
}

# ── test 4: the full green path — GATE (a)+(b)+(c) all satisfied ────────

test_full_pass_all_gates_green() {
    OUTPUT="$SANDBOX/out-pass"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_BLOCKER_RESPONSE="$BLOCKER_CLEAR"
    TEST_FRONTIER_RESPONSE="$FRONTIER_CONTINUOUS"
    TEST_COPY_HASHES="$COPY_HASHES_MATCH"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE")
    local rc; rc="$(run_script)"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE TEST_COPY_HASHES
    assert_rc "$rc" 0 "the all-green fixture did not PASS"
    assert_contains "$OUTPUT" "VERDICT: PASS" "all-green fixture missing PASS verdict"
    assert_contains "$OUTPUT" "GATE (a) H\* climb:.*ok=1" "GATE (a) not reported ok"
    assert_contains "$OUTPUT" "GATE (b) continuity:.*ok=1" "GATE (b) not reported ok"
    assert_contains "$OUTPUT" "GATE (c) hash parity:.*ok=1" "GATE (c) not reported ok"
    printf '[import-copy-prove-selftest] PASS: all three gates green yields overall PASS\n'
}

# ── test 5: GATE (a) catches a blocker still present despite H* climbing ──

test_blocker_still_present_fails_gate_a() {
    OUTPUT="$SANDBOX/out-blockerpresent"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_BLOCKER_RESPONSE="$BLOCKER_ANCHOR_GAP"
    TEST_FRONTIER_RESPONSE="$FRONTIER_CONTINUOUS"
    TEST_COPY_HASHES="$COPY_HASHES_MATCH"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE")
    local rc; rc="$(run_script)"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE TEST_COPY_HASHES
    assert_rc "$rc" 1 "a still-present anchor_backfill_gap blocker did not FAIL"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "blocker-present fixture missing FAIL verdict"
    assert_contains "$OUTPUT" "GATE (a) H\* climb:.*ok=0" "GATE (a) did not report not-ok"
    printf '[import-copy-prove-selftest] PASS: a lingering blocker fails GATE (a) even though H* climbed\n'
}

# ── test 6: GATE (b) catches a continuity break (coins_applied != hstar+1) ──

test_continuity_gap_fails_gate_b() {
    OUTPUT="$SANDBOX/out-continuitygap"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_BLOCKER_RESPONSE="$BLOCKER_CLEAR"
    TEST_FRONTIER_RESPONSE="$FRONTIER_GAP"
    TEST_COPY_HASHES="$COPY_HASHES_MATCH"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE")
    local rc; rc="$(run_script)"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE TEST_COPY_HASHES
    assert_rc "$rc" 1 "a coins_applied/hstar continuity gap did not FAIL"
    assert_contains "$OUTPUT" "GATE (b) continuity:.*ok=0" "GATE (b) did not report not-ok"
    printf '[import-copy-prove-selftest] PASS: a continuity gap fails GATE (b)\n'
}

# ── test 7: GATE (c) is the one that catches a missing anchor/nullifier ──

test_hash_mismatch_fails_gate_c() {
    OUTPUT="$SANDBOX/out-hashmismatch"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_BLOCKER_RESPONSE="$BLOCKER_CLEAR"
    TEST_FRONTIER_RESPONSE="$FRONTIER_CONTINUOUS"
    TEST_COPY_HASHES="$COPY_HASHES_MISMATCH"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE")
    local rc; rc="$(run_script)"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE TEST_COPY_HASHES
    assert_rc "$rc" 1 "a same-height hash mismatch vs zclassicd did not FAIL"
    assert_contains "$OUTPUT" "GATE (c) hash parity:.*ok=0" "GATE (c) did not report not-ok"
    assert_contains "$OUTPUT" "MISMATCH" "hash mismatch was not surfaced as evidence"
    printf '[import-copy-prove-selftest] PASS: a same-height hash mismatch fails GATE (c) (the missing-anchor signature)\n'
}

# ── test 8: header-refresh (step 1b) failure FAILs before phase 1 ever runs ──

test_header_refresh_failure_fails_before_phase1() {
    OUTPUT="$SANDBOX/out-headerrefresh-fail"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    echo fail > "$HEADER_REFRESH_BEHAVIOR_FILE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    echo ok > "$HEADER_REFRESH_BEHAVIOR_FILE"
    assert_rc "$rc" 1 "a failed header-refresh (step 1b) did not FAIL the harness"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "header-refresh failure did not report FAIL"
    assert_contains "$OUTPUT" "header-refresh (--importblockindex) failed" \
        "header-refresh FAIL reason absent"
    printf '[import-copy-prove-selftest] PASS: a failed header refresh fails fast, before phase 1 runs\n'
}

# ── test 9: --skip-header-refresh bypasses step 1b ──────────────────────

test_skip_header_refresh_bypasses_step() {
    OUTPUT="$SANDBOX/out-skip-headerrefresh"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    echo fail > "$HEADER_REFRESH_BEHAVIOR_FILE"   # would FAIL the run if NOT skipped
    TEST_BLOCKER_RESPONSE="$BLOCKER_CLEAR"
    TEST_FRONTIER_RESPONSE="$FRONTIER_CONTINUOUS"
    TEST_COPY_HASHES="$COPY_HASHES_MATCH"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE" --skip-header-refresh)
    local rc; rc="$(run_script)"
    echo ok > "$HEADER_REFRESH_BEHAVIOR_FILE"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE TEST_COPY_HASHES
    assert_rc "$rc" 0 "--skip-header-refresh did not bypass a forced header-refresh failure"
    assert_contains "$OUTPUT" "step 1b SKIPPED" "skip flag did not note the step was skipped"
    printf '[import-copy-prove-selftest] PASS: --skip-header-refresh bypasses step 1b\n'
}

# ── test 10: a stale auto_reindex_request in the source is cleared before boot ──

test_stale_auto_reindex_request_is_cleared() {
    OUTPUT="$SANDBOX/out-staleautoreindex"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_BLOCKER_RESPONSE="$BLOCKER_CLEAR"
    TEST_FRONTIER_RESPONSE="$FRONTIER_CONTINUOUS"
    TEST_COPY_HASHES="$COPY_HASHES_MATCH"
    printf '%d 2\n' "$WEDGE" > "$SRC_FIXTURE/auto_reindex_request"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE")
    local rc; rc="$(run_script)"
    rm -f "$SRC_FIXTURE/auto_reindex_request"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE TEST_COPY_HASHES
    assert_rc "$rc" 0 "a run with a stale auto_reindex_request in the source did not still PASS"
    assert_contains "$OUTPUT" "stale auto_reindex_request found in the copy — clearing before boot" \
        "the stale auto_reindex_request was not reported as cleared"
    [ ! -e "$COPY_DIR/auto_reindex_request" ] || \
        fail "auto_reindex_request still present in \$COPY_DIR after the run — it was not actually cleared"
    printf '[import-copy-prove-selftest] PASS: a stale auto_reindex_request carried in cp -a is cleared before the proving boot\n'
}

# ── test 11: no stale request present — step 3b is a documented no-op ───

test_no_auto_reindex_request_is_a_documented_noop() {
    OUTPUT="$SANDBOX/out-noautoreindex"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_BLOCKER_RESPONSE="$BLOCKER_CLEAR"
    TEST_FRONTIER_RESPONSE="$FRONTIER_CONTINUOUS"
    TEST_COPY_HASHES="$COPY_HASHES_MATCH"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE")
    local rc; rc="$(run_script)"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE TEST_COPY_HASHES
    assert_rc "$rc" 0 "a clean run with no stale auto_reindex_request did not PASS"
    assert_contains "$OUTPUT" "no stale auto_reindex_request present — nothing to clear" \
        "the no-op case was not reported"
    printf '[import-copy-prove-selftest] PASS: no stale auto_reindex_request is a documented no-op\n'
}

# ── test 12: --skip-zclassicd-check yields PASS-INCOMPLETE, never a bare PASS ──

test_skip_zclassicd_check_is_incomplete_not_pass() {
    OUTPUT="$SANDBOX/out-skipzd"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_BLOCKER_RESPONSE="$BLOCKER_CLEAR"
    TEST_FRONTIER_RESPONSE="$FRONTIER_CONTINUOUS"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE" --skip-zclassicd-check)
    local rc; rc="$(run_script)"
    unset TEST_BLOCKER_RESPONSE TEST_FRONTIER_RESPONSE
    assert_rc "$rc" 0 "--skip-zclassicd-check did not exit 0 with the other gates green"
    assert_contains "$OUTPUT" "PASS-INCOMPLETE" \
        "--skip-zclassicd-check did not label the verdict incomplete"
    printf '[import-copy-prove-selftest] PASS: --skip-zclassicd-check is labelled incomplete, never a bare PASS\n'
}

test_dry_run_prints_plan_only
test_refuses_non_copy_path
test_refuses_preexisting_copy_dir
test_missing_importer_fails_fast_with_dependency_note
test_importer_explicit_refusal_fails
test_full_pass_all_gates_green
test_blocker_still_present_fails_gate_a
test_continuity_gap_fails_gate_b
test_hash_mismatch_fails_gate_c
test_header_refresh_failure_fails_before_phase1
test_skip_header_refresh_bypasses_step
test_stale_auto_reindex_request_is_cleared
test_no_auto_reindex_request_is_a_documented_noop
test_skip_zclassicd_check_is_incomplete_not_pass
printf '[import-copy-prove-selftest] PASS: all import-copy-prove.sh driver-logic proofs\n'
