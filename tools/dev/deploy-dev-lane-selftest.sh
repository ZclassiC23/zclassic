#!/usr/bin/env bash
# Hermetic behavioral proof for transactional dev-generation activation.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPLOY="$REPO/tools/dev/deploy-dev-lane.sh"
SANDBOX="$(mktemp -d /tmp/zcl-dev-activation-selftest.XXXXXX)"
chmod 700 "$SANDBOX"
HOME_DIR="$SANDBOX/home"
GEN_ROOT="$HOME_DIR/.local/lib/zclassic23-dev"
ARTIFACT="$SANDBOX/candidate-zclassic23-dev"
RUNNING="$SANDBOX/running-exe"
COMMAND_LOG="$SANDBOX/commands.log"
OUTPUT="$SANDBOX/output.log"
OLD_GENERATION="legacy-aaaa"
SOURCE_ID="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
CAPABILITY_FILE="$SANDBOX/.deploy-dev-lane-selftest-capability"
CAPABILITY_TOKEN="$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')"
[[ "$CAPABILITY_TOKEN" =~ ^[0-9a-f]{64}$ ]] || {
    printf '[dev-activation-selftest] FAIL: could not mint fixture capability\n' >&2
    exit 1
}
printf '%s\n' "$CAPABILITY_TOKEN" > "$CAPABILITY_FILE"
chmod 600 "$CAPABILITY_FILE"

cleanup() {
    [ ! -d "$SANDBOX" ] || chmod -R u+w "$SANDBOX" 2>/dev/null || true
    rm -rf "$SANDBOX"
}
trap cleanup EXIT

fail() {
    printf '[dev-activation-selftest] FAIL: %s\n' "$*" >&2
    [ -r "$OUTPUT" ] && sed 's/^/[selftest-output] /' "$OUTPUT" >&2
    exit 1
}

assert_eq() {
    [ "$1" = "$2" ] || fail "$3 (expected '$2', got '$1')"
}

assert_file_contains() {
    grep -q -- "$2" "$1" || fail "$3 (missing '$2' in $1)"
}

prepare_case() {
    [ ! -d "$HOME_DIR" ] || chmod -R u+w "$HOME_DIR" 2>/dev/null || true
    rm -rf "$HOME_DIR" "$RUNNING" "$COMMAND_LOG" "$OUTPUT"
    mkdir -p "$GEN_ROOT/$OLD_GENERATION" "$GEN_ROOT/rejected" \
        "$HOME_DIR/.zclassic-c23-dev" "$HOME_DIR/.local/bin"
    printf '#!/bin/sh\nprintf "old\\n"\n' > "$GEN_ROOT/$OLD_GENERATION/zclassic23-dev"
    chmod 755 "$GEN_ROOT/$OLD_GENERATION/zclassic23-dev"
    printf '{"source_id_sha256":"%s","build_commit":"test-old"}\n' \
        "$SOURCE_ID" > "$GEN_ROOT/$OLD_GENERATION/manifest.json"
    ln -s "$OLD_GENERATION" "$GEN_ROOT/current"
    ln -s "$OLD_GENERATION" "$GEN_ROOT/last-good"
    printf '#!/bin/sh\nprintf "candidate\\n"\n' > "$ARTIFACT"
    chmod 755 "$ARTIFACT"
    : > "$HOME_DIR/.zclassic-c23-dev/node.db"
    : > "$COMMAND_LOG"
}

current_generation() {
    readlink "$GEN_ROOT/current"
}

last_good_generation() {
    readlink "$GEN_ROOT/last-good"
}

staged_generation() {
    readlink "$GEN_ROOT/staged"
}

candidate_generation_from_state() {
    sed -n 's/.*"candidate_generation"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" | head -1
}

run_deploy() {
    local preflight="$1" probe="$2" running_exe_command="$3"
    local stop_command="${SELFTEST_STOP_COMMAND_OVERRIDE:-printf \"stop\\n\" >> \"\$ZCL_TEST_LOG\"; rm -f \"\$ZCL_TEST_RUNNING\"}"
    shift 3
    env \
        HOME="$HOME_DIR" \
        ZCL_DEV_GENERATION_ROOT="$GEN_ROOT" \
        ZCL_DEV_SKIP_BUILD=1 \
        ZCL_DEV_BUILD_ARTIFACT="$ARTIFACT" \
        ZCL_DEV_BUILD_COMMIT_OVERRIDE=test-build \
        ZCL_DEV_SOURCE_ID="$SOURCE_ID" \
        ZCL_DEV_PREFLIGHT_COMMAND="$preflight" \
        ZCL_DEV_STOP_COMMAND="$stop_command" \
        ZCL_DEV_START_COMMAND='readlink -f "$ZCL_DEV_GENERATION_ROOT/current/zclassic23-dev" > "$ZCL_TEST_RUNNING"; printf "start\n" >> "$ZCL_TEST_LOG"' \
        ZCL_DEV_RESET_FAILED_COMMAND='printf "reset\n" >> "$ZCL_TEST_LOG"' \
        ZCL_DEV_DAEMON_RELOAD_COMMAND='printf "reload\n" >> "$ZCL_TEST_LOG"' \
        ZCL_DEV_ACTIVE_COMMAND='test -s "$ZCL_TEST_RUNNING"' \
        ZCL_DEV_PID_COMMAND='printf "4242\n"' \
        ZCL_DEV_RUNNING_EXE_COMMAND="$running_exe_command" \
        ZCL_DEV_ACTIVATION_PROBE_COMMAND="$probe" \
        ZCL_DEV_ACTIVATION_TIMEOUT=1 \
        ZCL_DEV_PROBE_INTERVAL_MS=20 \
        ZCL_TEST_RUNNING="$RUNNING" \
        ZCL_TEST_LOG="$COMMAND_LOG" \
        bash "$DEPLOY" --internal-self-test "$SANDBOX" 8 \
        "$@" 8< "$CAPABILITY_FILE" > "$OUTPUT" 2>&1
}

test_public_environment_cannot_authorize() {
    local rc signal="$SANDBOX/public-env-reached"
    prepare_case
    rm -f "$signal"
    set +e
    env HOME="$HOME_DIR" \
        ZCL_DEV_GENERATION_ROOT="$GEN_ROOT" \
        ZCL_DEV_ACTIVATION_TEST_MODE=1 \
        ZCL_DEV_STOP_COMMAND="touch '$signal'" \
        bash "$DEPLOY" > "$OUTPUT" 2>&1
    rc=$?
    set -e
    assert_eq "$rc" 3 "public test-mode environment did not refuse"
    [ ! -e "$signal" ] || fail "public environment reached an injected command"
    assert_eq "$(current_generation)" "$OLD_GENERATION" \
        "public environment changed current"
    assert_file_contains "$OUTPUT" 'runtime generation publication is contained' \
        "public environment refusal missing"
    printf '[dev-activation-selftest] PASS: public environment cannot mint activation authority\n'
}

test_internal_capability_is_fixture_bound() {
    local rc
    prepare_case
    set +e
    env HOME="$HOME_DIR" bash "$DEPLOY" \
        --internal-self-test "$SANDBOX" 8 > "$OUTPUT" 2>&1
    rc=$?
    set -e
    assert_eq "$rc" 3 "closed capability fd did not refuse"
    assert_file_contains "$OUTPUT" 'capability fd is not bound' \
        "closed capability fd refusal missing"

    set +e
    env HOME="$SANDBOX/not-the-fixture-home" bash "$DEPLOY" \
        --internal-self-test "$SANDBOX" 8 \
        8< "$CAPABILITY_FILE" > "$OUTPUT" 2>&1
    rc=$?
    set -e
    assert_eq "$rc" 3 "fixture capability accepted a different HOME"
    assert_file_contains "$OUTPUT" 'paths escaped the isolated fixture' \
        "wrong-HOME fixture refusal missing"
    assert_eq "$(current_generation)" "$OLD_GENERATION" \
        "invalid internal capability changed current"
    [ ! -s "$COMMAND_LOG" ] || fail "invalid internal capability reached service commands"
    printf '[dev-activation-selftest] PASS: internal capability is fd- and fixture-bound\n'
}

test_internal_capability_rejects_command_injection() {
    local rc signal="$SANDBOX/injected-command-reached"
    prepare_case
    rm -f "$signal"
    set +e
    SELFTEST_STOP_COMMAND_OVERRIDE="touch '$signal'" \
        run_deploy true true 'cat "$ZCL_TEST_RUNNING"'
    rc=$?
    set -e
    assert_eq "$rc" 3 "internal capability accepted a command override"
    [ ! -e "$signal" ] || fail "internal capability executed a command override"
    assert_file_contains "$OUTPUT" 'invalid internal self-test environment' \
        "command-override refusal missing"
    assert_eq "$(current_generation)" "$OLD_GENERATION" \
        "command override changed current"
    printf '[dev-activation-selftest] PASS: internal capability rejects command injection\n'
}

test_preflight_untouched() {
    local rc
    prepare_case
    set +e
    run_deploy false true 'cat "$ZCL_TEST_RUNNING"'
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || fail "preflight failure unexpectedly succeeded"
    assert_eq "$(current_generation)" "$OLD_GENERATION" \
        "preflight failure changed current"
    [ ! -s "$COMMAND_LOG" ] || fail "preflight failure stopped or started service"
    assert_file_contains "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" \
        '"activation_status": "preflight_failed"' \
        "preflight failure state missing"
    printf '[dev-activation-selftest] PASS: preflight failure leaves prior process/generation untouched\n'
}

test_stage_untouched() {
    local staged
    prepare_case
    run_deploy true true 'cat "$ZCL_TEST_RUNNING"' --stage || fail "stage failed"
    staged="$(staged_generation)"
    assert_eq "$(current_generation)" "$OLD_GENERATION" "stage changed current"
    case "$staged" in gen-*) ;; *) fail "stage did not publish a candidate generation" ;; esac
    [ ! -s "$COMMAND_LOG" ] || fail "stage stopped or started service"
    assert_file_contains "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" \
        '"activation_status": "staged"' "stage state missing"
    printf '[dev-activation-selftest] PASS: --stage preflights without activation\n'
}

test_success_promotes_candidate() {
    local candidate
    prepare_case
    run_deploy true true 'cat "$ZCL_TEST_RUNNING"' || fail "successful activation rejected"
    candidate="$(candidate_generation_from_state)"
    case "$candidate" in gen-*) ;; *) fail "candidate generation missing" ;; esac
    assert_eq "$(current_generation)" "$candidate" "success did not promote current"
    assert_eq "$(last_good_generation)" "$candidate" "success did not promote last-good"
    assert_eq "$(cat "$RUNNING")" "$GEN_ROOT/$candidate/zclassic23-dev" \
        "success did not run exact immutable executable"
    assert_file_contains "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" \
        '"activation_status": "active"' "success state missing"
    assert_file_contains "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" \
        "\"source_id_sha256\": \"$SOURCE_ID\"" \
        "deploy state omitted authoritative source identity"
    assert_file_contains \
        "$HOME_DIR/.config/systemd/user/zcl23-dev.service.d/90-build-identity.conf" \
        "ZCL_AGENT_EXPECT_SOURCE_ID=$SOURCE_ID" \
        "runtime drop-in omitted authoritative source identity"
    printf '[dev-activation-selftest] PASS: success promotes exact candidate\n'
}

test_probe_failure_rolls_back() {
    local rc candidate starts
    prepare_case
    set +e
    run_deploy true '[ "$EXPECTED_GENERATION" = "legacy-aaaa" ]' \
        'cat "$ZCL_TEST_RUNNING"'
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || fail "failing candidate probe unexpectedly succeeded"
    candidate="$(candidate_generation_from_state)"
    assert_eq "$(current_generation)" "$OLD_GENERATION" "rollback did not restore current"
    assert_eq "$(last_good_generation)" "$OLD_GENERATION" "rollback changed last-good"
    assert_eq "$(cat "$RUNNING")" "$GEN_ROOT/$OLD_GENERATION/zclassic23-dev" \
        "rollback did not restart exact last-good executable"
    [ -r "$GEN_ROOT/rejected/$candidate.json" ] || fail "rejected generation not quarantined"
    starts="$(grep -c '^start$' "$COMMAND_LOG" || true)"
    assert_eq "$starts" 2 "rollback did not perform one candidate start plus one recovery start"
    assert_file_contains "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" \
        '"rollback_status": "verified"' "verified rollback state missing"
    printf '[dev-activation-selftest] PASS: failed activation restores/verifies last-good once\n'
}

test_lock_rejects_concurrent_activation() {
    local rc holder signal="$SANDBOX/lock-held"
    prepare_case
    rm -f "$signal"
    (
        exec 8>"$GEN_ROOT/activation.lock"
        flock 8
        : > "$signal"
        sleep 5
    ) &
    holder=$!
    for _ in $(seq 1 100); do [ -e "$signal" ] && break; sleep 0.01; done
    [ -e "$signal" ] || fail "could not acquire fixture activation lock"
    set +e
    run_deploy true true 'cat "$ZCL_TEST_RUNNING"'
    rc=$?
    set -e
    kill "$holder" 2>/dev/null || true
    wait "$holder" 2>/dev/null || true
    assert_eq "$rc" 75 "concurrent activation did not return lock-busy status"
    assert_eq "$(current_generation)" "$OLD_GENERATION" "lock failure changed current"
    [ ! -s "$COMMAND_LOG" ] || fail "lock failure reached service commands"
    printf '[dev-activation-selftest] PASS: nonblocking lock rejects concurrent activation\n'
}

test_running_identity_mismatch_fails_closed() {
    local rc candidate
    prepare_case
    set +e
    run_deploy true true \
        'printf "%s\n" "$ZCL_DEV_GENERATION_ROOT/legacy-aaaa/zclassic23-dev"'
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || fail "running-executable mismatch unexpectedly succeeded"
    candidate="$(candidate_generation_from_state)"
    assert_eq "$(current_generation)" "$OLD_GENERATION" \
        "identity mismatch did not restore current"
    [ -r "$GEN_ROOT/rejected/$candidate.json" ] || fail "identity mismatch not quarantined"
    assert_file_contains "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" \
        '"rollback_status": "verified"' "identity mismatch rollback not verified"
    printf '[dev-activation-selftest] PASS: /proc generation mismatch fails closed\n'
}

test_no_prior_generation_removes_candidate_links() {
    local rc candidate
    prepare_case
    rm -f "$GEN_ROOT/current" "$GEN_ROOT/last-good"
    rm -rf "$GEN_ROOT/$OLD_GENERATION"
    set +e
    run_deploy true false 'cat "$ZCL_TEST_RUNNING"'
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || fail "first-generation failure unexpectedly succeeded"
    candidate="$(candidate_generation_from_state)"
    [ ! -e "$GEN_ROOT/current" ] && [ ! -L "$GEN_ROOT/current" ] ||
        fail "no-prior rollback left current pointing at rejected candidate"
    [ ! -e "$HOME_DIR/.local/bin/zclassic23-dev" ] &&
        [ ! -L "$HOME_DIR/.local/bin/zclassic23-dev" ] ||
        fail "no-prior rollback left compatibility link to rejected candidate"
    [ -r "$GEN_ROOT/rejected/$candidate.json" ] ||
        fail "first rejected generation was not quarantined"
    assert_file_contains "$HOME_DIR/.zclassic-c23-dev/agent-deploy.json" \
        '"rollback_status": "unavailable"' "no-prior rollback status missing"
    printf '[dev-activation-selftest] PASS: no-prior failure removes rejected current links\n'
}

test_default_stop_window_outlives_node_backstop() {
    grep -q 'DEV_SYSTEMCTL_TIMEOUT="${ZCL_DEV_SYSTEMCTL_TIMEOUT:-120}"' "$DEPLOY" ||
        fail "default systemctl window no longer outlives the node's 90-second shutdown backstop"
    printf '[dev-activation-selftest] PASS: default stop window outlives node backstop\n'
}

test_public_environment_cannot_authorize
test_internal_capability_is_fixture_bound
test_internal_capability_rejects_command_injection
test_preflight_untouched
test_stage_untouched
test_success_promotes_candidate
test_probe_failure_rolls_back
test_lock_rejects_concurrent_activation
test_running_identity_mismatch_fails_closed
test_no_prior_generation_removes_candidate_links
test_default_stop_window_outlives_node_backstop
printf '[dev-activation-selftest] PASS: all transactional activation proofs\n'
