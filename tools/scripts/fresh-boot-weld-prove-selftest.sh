#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# fresh-boot-weld-prove-selftest.sh — hermetic proof that
# tools/scripts/fresh-boot-weld-prove.sh's DRIVER computes the right verdict
# from a given set of boot outcomes, with NO real node binary, NO real chain
# state, and NO network ports opened for anything but a loopback fake. It
# fakes $ZCL_NODE_BIN with a tiny fixture script that emits exactly the log
# lines / marker files / dumpstate JSON the real driver greps and parses, and
# drives the real driver end-to-end against each fixture scenario.
#
# This exists for the same reason tools/scripts/import-copy-prove-selftest.sh
# does: the real weld (a zero-flag cold-boot bundle autodetect + install +
# fold-forward) cannot be exercised in CI/dev on demand (no from-scratch
# chain-binding relaxation merged yet at the time this was written — see the
# real driver's header). What CAN be proven hermetically, and what this
# proves, is that the DRIVER classifies each boot outcome (installed+climbed,
# tamper-refused, chain-binding-blocked, installed-but-frozen, a denylisted
# work dir) into the correct verdict and exit code — i.e. that the harness
# will gate correctly the moment a real weld runs. It does not (and cannot)
# prove anything about the weld/installer itself.
#
# All fixture paths live under a throwaway mktemp sandbox; nothing under
# $HOME or any real datadir is written (the one deliberate exception —
# proving the denylist refusal — only ever passes a --work-base string that
# is compared, never created or booted against).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$REPO/tools/scripts/fresh-boot-weld-prove.sh"
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/zcl-fresh-boot-weld-selftest.XXXXXX")"
chmod 700 "$SANDBOX"

cleanup() { rm -rf "$SANDBOX"; }
trap cleanup EXIT

FAKE_NODE="$SANDBOX/fake-zclassic23"
GOOD_BUNDLE="$SANDBOX/fixture-bundle.sqlite"
CHECKPOINT=3056758

fail() {
    printf '[fresh-boot-weld-prove-selftest] FAIL: %s\n' "$*" >&2
    [ -r "${OUTPUT:-}" ] && sed 's/^/[selftest-output] /' "$OUTPUT" >&2
    exit 1
}
assert_rc() {
    [ "$1" = "$2" ] || fail "$3 (expected rc=$2, got rc=$1)"
}
assert_contains() {
    grep -q -- "$2" "$1" || fail "$3 (missing '$2' in $1)"
}
assert_not_contains() {
    grep -q -- "$2" "$1" && fail "$3 (unexpectedly found '$2' in $1)" || true
}

# ── the one fake $ZCL_NODE_BIN, scenario-selected via FAKE_WELD_SCENARIO ────
cat > "$FAKE_NODE" <<'NODE_EOF'
#!/bin/sh
# Fake zclassic23 for the fresh-boot-weld-prove hermetic selftest ONLY.
datadir=""
query=0
for a in "$@"; do
    case "$a" in
        -datadir=*) datadir="${a#-datadir=}" ;;
        dumpstate)  query=1 ;;
    esac
done

if [ "$query" = "1" ]; then
    if [ "${FAKE_WELD_SCENARIO:-}" = "never_rpc" ]; then
        # Simulates an RPC that never binds: no output, non-zero exit.
        exit 1
    fi
    hstar=-1
    [ -r "$datadir/fake_hstar" ] && hstar="$(cat "$datadir/fake_hstar")"
    printf '{"hstar":%s,"network_tip":-1,"coins_applied_height":-1}\n' "$hstar"
    exit 0
fi

# Boot mode: find the staged bundle (if any) under <datadir>/bundles/.
mkdir -p "$datadir"
bundle_path=""
for f in "$datadir"/bundles/*.sqlite; do
    [ -f "$f" ] && bundle_path="$f"
done
scenario="${FAKE_WELD_SCENARIO:-install_and_climb}"
cp_height="${FAKE_CHECKPOINT:-3056758}"

trap 'exit 0' TERM INT

case "$scenario" in
    install_and_climb)
        echo "$cp_height" > "$datadir/fake_hstar"
        : > "$datadir/consensus-bundle-installed.marker"
        echo "[install_consensus_bundle] autodetected consensus bundle installed $bundle_path (H*=$cp_height)"
        h="$cp_height"
        i=0
        while [ "$i" -lt 60 ]; do
            sleep 1
            h=$((h + 1))
            echo "$h" > "$datadir/fake_hstar"
            i=$((i + 1))
        done
        while true; do sleep 1; done
        ;;
    refused_tamper)
        echo "-1" > "$datadir/fake_hstar"
        echo "[install_consensus_bundle] autodetected bundle $bundle_path did not install (marked .failed -> normal boot next time): bundle admission/validation failed: artifact digest mismatch"
        [ -n "$bundle_path" ] && : > "${bundle_path}.failed"
        while true; do sleep 1; done
        ;;
    chain_binding_blocked)
        echo "-1" > "$datadir/fake_hstar"
        echo "[install_consensus_bundle] autodetected bundle $bundle_path did not install (marked .failed -> normal boot next time): selected-chain binding failed (the bundle's height/hash is not on this node's validated header chain, or the node is not the open singleton): chain binding: selected frontier changed or is not durable"
        [ -n "$bundle_path" ] && : > "${bundle_path}.failed"
        while true; do sleep 1; done
        ;;
    frozen_at_checkpoint)
        echo "$cp_height" > "$datadir/fake_hstar"
        : > "$datadir/consensus-bundle-installed.marker"
        echo "[install_consensus_bundle] autodetected consensus bundle installed $bundle_path (H*=$cp_height)"
        while true; do sleep 1; done
        ;;
    tamper_falsely_installed)
        # A deliberate REGRESSION fixture: despite tamper, the fake installs
        # anyway. Proves the driver's PASS predicate genuinely checks the
        # marker + .failed absence, not merely "H* moved".
        echo "$cp_height" > "$datadir/fake_hstar"
        : > "$datadir/consensus-bundle-installed.marker"
        echo "[install_consensus_bundle] autodetected consensus bundle installed $bundle_path (H*=$cp_height) [SELFTEST REGRESSION FIXTURE]"
        while true; do sleep 1; done
        ;;
    never_rpc)
        # Simulates a node that never binds RPC in time.
        while true; do sleep 1; done
        ;;
esac
NODE_EOF
chmod +x "$FAKE_NODE"

# ── a fixture bundle that passes is_sqlite_bundle()'s magic-header check ───
printf 'SQLite format 3\000' > "$GOOD_BUNDLE"
dd if=/dev/zero bs=1 count=284 >> "$GOOD_BUNDLE" 2>/dev/null

run_script() {
    local rc
    set +e
    env ZCL_NODE_BIN="$FAKE_NODE" FAKE_CHECKPOINT="$CHECKPOINT" \
        FAKE_WELD_SCENARIO="${SCENARIO:-install_and_climb}" \
        "$SCRIPT" --checkpoint="$CHECKPOINT" --work-base="$SANDBOX" \
        "${EXTRA_ARGS[@]}" > "$OUTPUT" 2>&1
    rc=$?
    set -e
    echo "$rc"
}

# ============================================================================
test_no_bundle_found_skips() {
    OUTPUT="$SANDBOX/out-nobundle"
    EMPTY_HOME="$SANDBOX/empty-home-$$"
    mkdir -p "$EMPTY_HOME"
    local rc
    set +e
    env ZCL_NODE_BIN="$FAKE_NODE" HOME="$EMPTY_HOME" \
        "$SCRIPT" --work-base="$SANDBOX" > "$OUTPUT" 2>&1
    rc=$?
    set -e
    assert_rc "$rc" 0 "no-bundle-found did not exit 0 (SKIP)"
    assert_contains "$OUTPUT" "VERDICT: SKIP" "no-bundle-found missing SKIP verdict"
    printf '[fresh-boot-weld-prove-selftest] PASS: no bundle anywhere -> SKIP, exit 0\n'
}

test_explicit_bundle_discovery() {
    OUTPUT="$SANDBOX/out-explicit-negonly"
    SCENARIO=refused_tamper
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --negative-only --negative-deadline=15)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 0 "--bundle=explicit path did not resolve/PASS"
    assert_contains "$OUTPUT" "bundle: $GOOD_BUNDLE" "explicit --bundle was not used"
    printf '[fresh-boot-weld-prove-selftest] PASS: --bundle=PATH is used verbatim\n'
}

test_negative_pass_tamper_refused() {
    OUTPUT="$SANDBOX/out-neg-pass"
    SCENARIO=refused_tamper
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --negative-only --negative-deadline=15)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 0 "a cleanly-refused tamper did not PASS the negative leg"
    assert_contains "$OUTPUT" "NEGATIVE PASS" "negative leg missing PASS"
    assert_contains "$OUTPUT" "flipped 1 byte at offset" "byte-flip evidence missing (tamper may be a no-op)"
    assert_contains "$OUTPUT" "VERDICT: PASS" "negative-only run missing overall PASS verdict"
    printf '[fresh-boot-weld-prove-selftest] PASS: a cleanly-refused tamper -> NEGATIVE PASS\n'
}

test_negative_fails_if_marker_present() {
    OUTPUT="$SANDBOX/out-neg-fail-marker"
    SCENARIO=tamper_falsely_installed
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --negative-only --negative-deadline=15)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 1 "a falsely-installed tamper did not FAIL the negative leg"
    assert_contains "$OUTPUT" "NEGATIVE FAIL" "negative leg missing FAIL for a falsely-installed tamper"
    printf '[fresh-boot-weld-prove-selftest] PASS: a falsely-installed tamper (regression fixture) -> NEGATIVE FAIL\n'
}

test_positive_pass_install_and_climb() {
    OUTPUT="$SANDBOX/out-pos-pass"
    SCENARIO=install_and_climb
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --positive-only --deadline=20)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 0 "install-and-climb fixture did not PASS the positive leg"
    assert_contains "$OUTPUT" "first H\*: $CHECKPOINT" "did not observe H* land exactly at the checkpoint"
    assert_contains "$OUTPUT" "POSITIVE PASS" "positive leg missing PASS"
    assert_contains "$OUTPUT" "VERDICT: PASS" "positive-only run missing overall PASS verdict"
    printf '[fresh-boot-weld-prove-selftest] PASS: zero-flag install-then-climb fixture -> POSITIVE PASS\n'
}

test_positive_blocked_chain_binding() {
    OUTPUT="$SANDBOX/out-pos-blocked"
    SCENARIO=chain_binding_blocked
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --positive-only --deadline=8)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 3 "a chain-binding refusal did not report BLOCKED (rc=3)"
    assert_contains "$OUTPUT" "BLOCKED-CHAIN-BINDING" "chain-binding refusal not distinguished from a generic FAIL"
    printf '[fresh-boot-weld-prove-selftest] PASS: chain-binding refusal -> honest BLOCKED, not FAIL or a false PASS\n'
}

test_positive_fails_frozen_at_checkpoint() {
    OUTPUT="$SANDBOX/out-pos-frozen"
    SCENARIO=frozen_at_checkpoint
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --positive-only --deadline=8)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 1 "landed-but-never-climbed did not FAIL the positive leg"
    assert_contains "$OUTPUT" "did not climb past" "frozen-at-checkpoint FAIL reason missing"
    printf '[fresh-boot-weld-prove-selftest] PASS: installed but frozen at the checkpoint -> POSITIVE FAIL (gate is CLIMB, not booted)\n'
}

test_positive_inconclusive_when_rpc_never_answers() {
    OUTPUT="$SANDBOX/out-pos-norpc"
    SCENARIO=never_rpc
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --positive-only --deadline=6)
    local rc; rc="$(run_script)"
    assert_rc "$rc" 1 "a node that never answers RPC did not FAIL"
    assert_contains "$OUTPUT" "INCONCLUSIVE" "never-answers-RPC case missing INCONCLUSIVE diagnosis"
    printf '[fresh-boot-weld-prove-selftest] PASS: RPC never answering -> INCONCLUSIVE, not a false PASS\n'
}

test_denylist_refuses_live_datadir() {
    OUTPUT="$SANDBOX/out-denylist"
    SCENARIO=install_and_climb
    EXTRA_ARGS=(--bundle="$GOOD_BUNDLE" --negative-only --negative-deadline=5)
    local rc
    set +e
    env ZCL_NODE_BIN="$FAKE_NODE" FAKE_CHECKPOINT="$CHECKPOINT" \
        FAKE_WELD_SCENARIO="$SCENARIO" \
        "$SCRIPT" --checkpoint="$CHECKPOINT" --bundle="$GOOD_BUNDLE" \
        --negative-only --negative-deadline=5 \
        --work-base="$HOME/.zclassic" > "$OUTPUT" 2>&1
    rc=$?
    set -e
    assert_rc "$rc" 1 "a denylisted --work-base was not refused"
    assert_contains "$OUTPUT" "denylisted" "denylist refusal message missing"
    printf '[fresh-boot-weld-prove-selftest] PASS: a --work-base under a live datadir is refused, never booted against\n'
}

test_no_bundle_found_skips
test_explicit_bundle_discovery
test_negative_pass_tamper_refused
test_negative_fails_if_marker_present
test_positive_pass_install_and_climb
test_positive_blocked_chain_binding
test_positive_fails_frozen_at_checkpoint
test_positive_inconclusive_when_rpc_never_answers
test_denylist_refuses_live_datadir

printf '[fresh-boot-weld-prove-selftest] ALL 9 HERMETIC ASSERTIONS PASSED\n'
