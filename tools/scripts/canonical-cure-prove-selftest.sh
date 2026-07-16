#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# canonical-cure-prove-selftest.sh — hermetic proof that
# tools/scripts/canonical-cure-prove.sh's own gating/plumbing logic is
# correct, with NO real node binary, NO real zclassicd, NO real chainstate,
# and NO network ports opened. It fakes $NODE_BIN and $RPC_BIN (both
# env-overridable on the real script) with tiny fixture scripts that print
# exactly the banners/RPC substrings the real driver greps/seds for, and
# drives the real script end-to-end against those fakes.
#
# What this proves: the driver correctly (a) refuses unsafe --copy-dir
# values, (b) requires the exact 'IMPORT COMPLETE (committed=' banner before
# proceeding, (c) auto-parses "boundary=<height>" out of the combined
# header-refresh/import log and gates H* CLIMB against it, (d) lets an
# explicit --expect-climb-past override the parsed boundary, (e) clears a
# stale auto_reindex_request carried in by cp -a, and (f) always deletes the
# copy on exit unless --keep-copy is passed. It does not (and cannot) prove
# anything about the real importer/boot binary.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$REPO/tools/scripts/canonical-cure-prove.sh"
SANDBOX="$(mktemp -d /tmp/zcl-canonical-cure-prove-selftest.XXXXXX)"
chmod 700 "$SANDBOX"

cleanup() { rm -rf "$SANDBOX"; }
trap cleanup EXIT

FAKE_NODE="$SANDBOX/fake-zclassic23"
FAKE_RPC="$SANDBOX/fake-zcl-rpc"
SRC_FIXTURE="$SANDBOX/fixture-datadir"
ZD_FIXTURE="$SANDBOX/fixture-zclassicd-datadir"
IMPORT_BEHAVIOR_FILE="$SANDBOX/import-behavior"
HEADER_REFRESH_BEHAVIOR_FILE="$SANDBOX/header-refresh-behavior"

WEDGE=3176325

fail() {
    printf '[canonical-cure-prove-selftest] FAIL: %s\n' "$*" >&2
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
# Fake zclassic23 for the canonical-cure-prove hermetic selftest ONLY.
if [ "${1:-}" = "--importblockindex" ]; then
    # Header-refresh step: zclassic23 --importblockindex <src> [<target>]
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
            # stdout: the exact terminal banner the driver greps for.
            echo "IMPORT COMPLETE (committed=5): sapling_anchors=1 sprout_anchors=1 sapling_nf=1 sprout_nf=1"
            # stderr: mirrors the real LOG_INFO line carrying boundary=.
            echo "[shielded_import] INFO IMPORT COMPLETE: sapling_anchors=1 sprout_anchors=1 sapling_nf=1 sprout_nf=1 boundary=${FAKE_IMPORT_BOUNDARY:-3176325} — both cursors flipped to 0, shielded history published" >&2
            exit 0 ;;
        ok_no_boundary)
            echo "IMPORT COMPLETE (committed=5): sapling_anchors=1 sprout_anchors=1 sapling_nf=1 sprout_nf=1"
            exit 0 ;;
        fail)
            echo "IMPORT REFUSED — selftest fixture forced failure (wedge intact)" >&2
            exit 1 ;;
        missing)
            # Simulates an unmerged flag: the real argv loop ignores unknown
            # flags and just no-ops this one call.
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
# Fake zcl-rpc for the canonical-cure-prove hermetic selftest ONLY. Emits
# raw substrings the real driver's sed/grep patterns look for.
method="$1"
case "$method" in
    getblockcount)
        printf '{"result":%s,"error":null,"id":1}\n' "${FAKE_COPY_TIP:--1}"
        ;;
    dumpstate)
        cat "${FAKE_FRONTIER_RESPONSE:-/dev/null}"
        ;;
    *) echo '{}' ;;
esac
RPC_EOF
chmod +x "$FAKE_RPC"

mkdir -p "$SRC_FIXTURE" "$ZD_FIXTURE"
: > "$SRC_FIXTURE/node.db"

FRONTIER_PAST_WEDGE="$SANDBOX/frontier-past-wedge.json"
printf '{"hstar":%d,"coins_applied_height":%d}' "$((WEDGE + 6))" "$((WEDGE + 6))" \
    > "$FRONTIER_PAST_WEDGE"
FRONTIER_STILL_WEDGED="$SANDBOX/frontier-still-wedged.json"
printf '{"hstar":%d,"coins_applied_height":%d}' "$WEDGE" "$WEDGE" \
    > "$FRONTIER_STILL_WEDGED"

run_script() {
    local rc
    set +e
    env \
        ZCL_NODE_BIN="$FAKE_NODE" \
        ZCL_RPC_BIN="$FAKE_RPC" \
        FAKE_IMPORT_BEHAVIOR_FILE="$IMPORT_BEHAVIOR_FILE" \
        FAKE_HEADER_REFRESH_BEHAVIOR_FILE="$HEADER_REFRESH_BEHAVIOR_FILE" \
        FAKE_IMPORT_BOUNDARY="${TEST_IMPORT_BOUNDARY:-$WEDGE}" \
        FAKE_COPY_TIP="$((WEDGE + 6))" \
        FAKE_FRONTIER_RESPONSE="${TEST_FRONTIER_RESPONSE:-$FRONTIER_PAST_WEDGE}" \
        "$SCRIPT" --deadline=15 \
        --src="$SRC_FIXTURE" \
        --copy-dir="$COPY_DIR" \
        --zclassicd-datadir="$ZD_FIXTURE" \
        "${EXTRA_ARGS[@]}" \
        > "$OUTPUT" 2>&1
    rc=$?
    set -e
    echo "$rc"
}

fresh_copy_dir() {
    printf '%s/.zclassic-c23-COPY-%s-canonical' "$SANDBOX" "$(date +%s%N)"
}

# ── test 1: --dry-run never touches the filesystem or asserts a verdict ──

test_dry_run_prints_plan_only() {
    OUTPUT="$SANDBOX/out-dryrun"
    COPY_DIR="$(fresh_copy_dir)"
    EXTRA_ARGS=(--dry-run)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 0 "dry-run did not exit 0"
    assert_contains "$OUTPUT" "canonical-cure-prove plan" "dry-run missing plan banner"
    assert_contains "$OUTPUT" "no filesystem or process action taken" "dry-run missing no-op notice"
    [ ! -e "$COPY_DIR" ] || fail "dry-run created a copy-dir on disk"
    printf '[canonical-cure-prove-selftest] PASS: --dry-run prints the plan and touches nothing\n'
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
    printf '[canonical-cure-prove-selftest] PASS: refuses a --copy-dir without the -COPY- marker\n'
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
    rm -rf "$COPY_DIR"
    printf '[canonical-cure-prove-selftest] PASS: refuses a --copy-dir that already exists\n'
}

# ── test 3: header-refresh failure FAILs before phase 1 ever runs ───────

test_header_refresh_failure_fails_before_import() {
    OUTPUT="$SANDBOX/out-headerrefresh-fail"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    echo fail > "$HEADER_REFRESH_BEHAVIOR_FILE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    echo ok > "$HEADER_REFRESH_BEHAVIOR_FILE"
    assert_rc "$rc" 1 "a failed header-refresh did not FAIL the harness"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "header-refresh failure did not report FAIL"
    assert_contains "$OUTPUT" "header-refresh (--importblockindex) failed" \
        "header-refresh FAIL reason absent"
    [ ! -e "$COPY_DIR" ] || fail "the copy was not cleaned up after a header-refresh FAIL"
    printf '[canonical-cure-prove-selftest] PASS: a failed header refresh fails fast and the copy is still cleaned up\n'
}

# ── test 4: no IMPORT COMPLETE banner => FAIL, not a hang ───────────────

test_missing_importer_fails_fast() {
    OUTPUT="$SANDBOX/out-missing"
    COPY_DIR="$(fresh_copy_dir)"
    echo missing > "$IMPORT_BEHAVIOR_FILE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    assert_rc "$rc" 1 "an unimplemented importer flag did not FAIL"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "missing-importer run did not report FAIL"
    assert_contains "$OUTPUT" "did not report IMPORT COMPLETE" "missing-importer FAIL reason absent"
    printf '[canonical-cure-prove-selftest] PASS: an unmerged importer fails fast, not silently\n'
}

test_importer_explicit_refusal_fails() {
    OUTPUT="$SANDBOX/out-refused"
    COPY_DIR="$(fresh_copy_dir)"
    echo fail > "$IMPORT_BEHAVIOR_FILE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    assert_rc "$rc" 1 "an explicit importer REFUSED did not FAIL"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "importer refusal did not report FAIL"
    printf '[canonical-cure-prove-selftest] PASS: an explicit importer REFUSED fails the harness\n'
}

# ── test 5: IMPORT COMPLETE but no boundary= and no override => FAIL blind-gate ──

test_no_boundary_and_no_override_fails() {
    OUTPUT="$SANDBOX/out-noboundary"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok_no_boundary > "$IMPORT_BEHAVIOR_FILE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    assert_rc "$rc" 1 "a missing boundary= with no override did not FAIL"
    assert_contains "$OUTPUT" "no 'boundary=<height>' could" "the blind-gate refusal reason is missing"
    printf '[canonical-cure-prove-selftest] PASS: a missing boundary= with no override refuses to gate blind\n'
}

test_no_boundary_with_explicit_override_passes() {
    OUTPUT="$SANDBOX/out-noboundary-override"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok_no_boundary > "$IMPORT_BEHAVIOR_FILE"
    EXTRA_ARGS=(--expect-climb-past="$WEDGE")
    local rc; rc="$(run_script)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    assert_rc "$rc" 0 "an explicit --expect-climb-past override did not compensate for a missing boundary="
    assert_contains "$OUTPUT" "operator-supplied --expect-climb-past=$WEDGE (override)" \
        "the override was not reported as used"
    assert_contains "$OUTPUT" "VERDICT: PASS" "the override run did not PASS"
    printf '[canonical-cure-prove-selftest] PASS: an explicit --expect-climb-past overrides a missing boundary=\n'
}

# ── test 6: the full green path — boundary auto-parsed, H* climbs past it ──

test_full_pass_auto_parsed_boundary() {
    OUTPUT="$SANDBOX/out-pass"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_IMPORT_BOUNDARY="$WEDGE"
    TEST_FRONTIER_RESPONSE="$FRONTIER_PAST_WEDGE"
    EXTRA_ARGS=()
    local rc; rc="$(run_script)"
    unset TEST_IMPORT_BOUNDARY TEST_FRONTIER_RESPONSE
    assert_rc "$rc" 0 "the all-green fixture did not PASS"
    assert_contains "$OUTPUT" "auto-parsed pre-import wedge height: boundary=$WEDGE" \
        "the boundary was not auto-parsed"
    assert_contains "$OUTPUT" "VERDICT: PASS" "all-green fixture missing PASS verdict"
    assert_contains "$OUTPUT" "GATE H\* CLIMB:.*ok=1" "the H* CLIMB gate was not reported ok"
    [ ! -e "$COPY_DIR" ] || fail "the copy was not cleaned up after a PASS"
    printf '[canonical-cure-prove-selftest] PASS: auto-parsed boundary + H* climb yields PASS, copy cleaned up\n'
}

# ── test 7: H* still at the wedge (no climb) => FAIL, copy still cleaned up ──

test_still_wedged_fails_gate() {
    OUTPUT="$SANDBOX/out-stillwedged"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_IMPORT_BOUNDARY="$WEDGE"
    TEST_FRONTIER_RESPONSE="$FRONTIER_STILL_WEDGED"
    EXTRA_ARGS=(--deadline=6)
    local rc; rc="$(run_script)"
    unset TEST_IMPORT_BOUNDARY TEST_FRONTIER_RESPONSE
    assert_rc "$rc" 1 "a frontier that never climbs past the wedge did not FAIL"
    assert_contains "$OUTPUT" "VERDICT: FAIL" "still-wedged fixture missing FAIL verdict"
    assert_contains "$OUTPUT" "GATE H\* CLIMB:.*ok=0" "the H* CLIMB gate was not reported not-ok"
    [ ! -e "$COPY_DIR" ] || fail "the copy was not cleaned up after a FAIL"
    printf '[canonical-cure-prove-selftest] PASS: a frontier stuck at the wedge fails the gate, copy still cleaned up on FAIL\n'
}

# ── test 8: --keep-copy retains the copy after a PASS ────────────────────

test_keep_copy_retains_directory() {
    OUTPUT="$SANDBOX/out-keepcopy"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_IMPORT_BOUNDARY="$WEDGE"
    TEST_FRONTIER_RESPONSE="$FRONTIER_PAST_WEDGE"
    EXTRA_ARGS=(--keep-copy)
    local rc; rc="$(run_script)"
    unset TEST_IMPORT_BOUNDARY TEST_FRONTIER_RESPONSE
    assert_rc "$rc" 0 "--keep-copy run did not PASS"
    [ -e "$COPY_DIR" ] || fail "--keep-copy did not retain the copy directory"
    assert_contains "$OUTPUT" "keep-copy: copy left on disk for inspection" \
        "--keep-copy was not reported in the plan"
    rm -rf "$COPY_DIR"
    printf '[canonical-cure-prove-selftest] PASS: --keep-copy retains the copy after a PASS\n'
}

# ── test 9: a stale auto_reindex_request carried in cp -a is cleared ─────

test_stale_auto_reindex_request_is_cleared() {
    OUTPUT="$SANDBOX/out-staleautoreindex"
    COPY_DIR="$(fresh_copy_dir)"
    echo ok > "$IMPORT_BEHAVIOR_FILE"
    TEST_IMPORT_BOUNDARY="$WEDGE"
    TEST_FRONTIER_RESPONSE="$FRONTIER_PAST_WEDGE"
    printf '%d 2\n' "$WEDGE" > "$SRC_FIXTURE/auto_reindex_request"
    EXTRA_ARGS=(--keep-copy)
    local rc; rc="$(run_script)"
    rm -f "$SRC_FIXTURE/auto_reindex_request"
    unset TEST_IMPORT_BOUNDARY TEST_FRONTIER_RESPONSE
    assert_rc "$rc" 0 "a run with a stale auto_reindex_request in the source did not still PASS"
    assert_contains "$OUTPUT" "stale auto_reindex_request found in the copy — clearing before boot" \
        "the stale auto_reindex_request was not reported as cleared"
    [ ! -e "$COPY_DIR/auto_reindex_request" ] || \
        fail "auto_reindex_request still present in \$COPY_DIR after the run"
    rm -rf "$COPY_DIR"
    printf '[canonical-cure-prove-selftest] PASS: a stale auto_reindex_request carried in cp -a is cleared before the proving boot\n'
}

test_dry_run_prints_plan_only
test_refuses_non_copy_path
test_refuses_preexisting_copy_dir
test_header_refresh_failure_fails_before_import
test_missing_importer_fails_fast
test_importer_explicit_refusal_fails
test_no_boundary_and_no_override_fails
test_no_boundary_with_explicit_override_passes
test_full_pass_auto_parsed_boundary
test_still_wedged_fails_gate
test_keep_copy_retains_directory
test_stale_auto_reindex_request_is_cleared
printf '[canonical-cure-prove-selftest] PASS: all canonical-cure-prove.sh driver-logic proofs\n'
