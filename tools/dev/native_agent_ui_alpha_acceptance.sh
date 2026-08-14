#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# purpose: physical, browser-free acceptance for the resident native C23 UI.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NODE_BIN="$REPO_ROOT/build/bin/zclassic23"
DRIVER_BIN="$REPO_ROOT/build/bin/native_ui_driver"
RUN_ROOT="$(mktemp -d /tmp/zcl23-native-ui-alpha.XXXXXX)"
RUNTIME_DIR="$RUN_ROOT/runtime"
LATENCIES="$RUN_ROOT/warm-total-us"
HANDOFFS="$RUN_ROOT/warm-handoff-us"
DISPLAY_NAME="${DISPLAY:-}"

fail() {
    echo "native-agent-ui-alpha: $*" >&2
    exit 1
}

host_pids() {
    local proc cmd environment
    for proc in /proc/[0-9]*; do
        [ -r "$proc/cmdline" ] && [ -r "$proc/environ" ] || continue
        cmd="$(tr '\0' ' ' < "$proc/cmdline" 2>/dev/null || true)"
        [ "$cmd" = "$NODE_BIN --ui-present-host " ] || continue
        environment="$(tr '\0' '\n' < "$proc/environ" 2>/dev/null || true)"
        case "$environment" in
            *"XDG_RUNTIME_DIR=$RUNTIME_DIR"*) printf '%s\n' "${proc##*/}" ;;
        esac
    done
}

cleanup() {
    local rc="$?" pid
    trap - EXIT INT TERM
    while IFS= read -r pid; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done < <(host_pids)
    case "$RUN_ROOT" in
        /tmp/zcl23-native-ui-alpha.*) rm -rf "$RUN_ROOT" ;;
        *) echo "native-agent-ui-alpha: refusing unsafe cleanup: $RUN_ROOT" >&2 ;;
    esac
    exit "$rc"
}
trap cleanup EXIT INT TERM

[ -n "$DISPLAY_NAME" ] || fail "DISPLAY is unset; physical native-window proof cannot run"
[ -x "$NODE_BIN" ] || fail "missing node binary: $NODE_BIN"
[ -x "$DRIVER_BIN" ] || fail "missing physical driver: $DRIVER_BIN"
mkdir -m 0700 "$RUNTIME_DIR"
export XDG_RUNTIME_DIR="$RUNTIME_DIR"

browser_snapshot() {
    ps -eo pid=,comm= | awk '
        { name=tolower($2) }
        name ~ /^(firefox|chrome|chromium|brave|webkit)/ {
            print $1 ":" $2
        }' | sort
}

json_has() {
    case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac
}

json_int() {
    printf '%s\n' "$1" | sed -n \
        "s/.*\"$2\":[[:space:]]*\([-0-9][0-9]*\).*/\1/p" | tail -1
}

timed_command() {
    local output_name="$1" elapsed_name="$2"
    shift 2
    local started finished command_output
    started="$(date +%s%N)"
    command_output="$("$@")"
    finished="$(date +%s%N)"
    printf -v "$output_name" '%s' "$command_output"
    printf -v "$elapsed_name" '%s' "$(( (finished - started) / 1000 ))"
}

assert_display_reply() {
    local reply="$1"
    json_has "$reply" '"launched":true' ||
        fail "command did not launch a window: $reply"
    json_has "$reply" '"resident_host":true' ||
        fail "resident host was not used: $reply"
    json_has "$reply" '"authority":"display-only"' ||
        fail "visual authority boundary missing: $reply"
}

BEFORE_BROWSERS="$(browser_snapshot)"

# The first request owns an isolated runtime endpoint, so this measures a real
# cold same-binary host start plus the first native software blit.
timed_command QR_REPLY COLD_TOTAL_US \
    "$NODE_BIN" app qr show \
    --input='{"payload":"native-ui-alpha-qr","title":"Alpha QR"}'
assert_display_reply "$QR_REPLY"
[ "$COLD_TOTAL_US" -le 500000 ] ||
    fail "cold native-window latency ${COLD_TOTAL_US}us exceeds 500000us"
"$DRIVER_BIN" --title='Alpha QR' --key=escape --timeout-ms=3000 >/dev/null

STATUS_MODEL='{"kind":"status","request_id":"alpha-status","title":"Alpha node status","summary":"NODE FACT - bounded native status instrument","items":[{"kind":"key-value","status":"green","label":"NODE FACT - presentation host","value":"ready"}]}'
timed_command STATUS_REPLY STATUS_TOTAL_US \
    "$NODE_BIN" app presentation show --input="$STATUS_MODEL"
assert_display_reply "$STATUS_REPLY"
json_has "$STATUS_REPLY" '"presentation_kind":"status"' ||
    fail "status card kind was not returned"
"$DRIVER_BIN" --title='Alpha node status' --key=escape --timeout-ms=3000 >/dev/null

DIFF_MODEL='{"kind":"code-diff","request_id":"alpha-diff","title":"Alpha exact code diff","summary":"AGENT SUMMARY - requested behavior; exact lines remain local observations","exact_root":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","items":[{"kind":"diff-remove","label":"LOCAL OBSERVATION - before","value":"return 0;"},{"kind":"diff-add","status":"green","label":"LOCAL OBSERVATION - candidate","value":"return verified;"}]}'
timed_command DIFF_REPLY DIFF_TOTAL_US \
    "$NODE_BIN" app presentation show --input="$DIFF_MODEL"
assert_display_reply "$DIFF_REPLY"
json_has "$DIFF_REPLY" '"presentation_kind":"code-diff"' ||
    fail "code-diff kind was not returned"
"$DRIVER_BIN" --title='Alpha exact code diff' --key=escape --timeout-ms=3000 >/dev/null

# Reuse one action-bound request identity. Every new inert frame must replace
# its predecessor; the final p95 is edit-to-command-return, not render-only.
for step in $(seq 1 20); do
    PROGRESS_MODEL="{\"kind\":\"progress\",\"request_id\":\"alpha-reproduction\",\"title\":\"Alpha reproduction progress\",\"summary\":\"LOCAL OBSERVATION - independent reproduction\",\"exact_root\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"items\":[{\"kind\":\"progress\",\"status\":\"info\",\"label\":\"LOCAL OBSERVATION - verified stages\",\"value\":\"${step}/20\",\"numerator\":${step},\"denominator\":20}]}"
    timed_command PROGRESS_REPLY PROGRESS_TOTAL_US \
        "$NODE_BIN" app presentation show --input="$PROGRESS_MODEL"
    assert_display_reply "$PROGRESS_REPLY"
    json_has "$PROGRESS_REPLY" '"host_reused":true' ||
        fail "warm presentation request did not reuse the resident host"
    if [ "$step" -gt 1 ]; then
        json_has "$PROGRESS_REPLY" '"view_replaced":true' ||
            fail "live progress update did not replace its exact prior view"
    fi
    printf '%s\n' "$PROGRESS_TOTAL_US" >> "$LATENCIES"
    PROGRESS_HANDOFF_US="$(json_int "$PROGRESS_REPLY" launch_handoff_us)"
    [ -n "$PROGRESS_HANDOFF_US" ] ||
        fail "progress reply omitted launch_handoff_us"
    printf '%s\n' "$PROGRESS_HANDOFF_US" >> "$HANDOFFS"
done
"$DRIVER_BIN" --title='Alpha reproduction progress' --key=escape \
    --timeout-ms=3000 >/dev/null

WARM_P50_US="$(sort -n "$LATENCIES" | awk 'NR==10 {print; exit}')"
WARM_P95_US="$(sort -n "$LATENCIES" | awk 'NR==19 {print; exit}')"
HANDOFF_P50_US="$(sort -n "$HANDOFFS" | awk 'NR==10 {print; exit}')"
HANDOFF_P95_US="$(sort -n "$HANDOFFS" | awk 'NR==19 {print; exit}')"
[ -n "$WARM_P50_US" ] && [ -n "$WARM_P95_US" ] &&
    [ -n "$HANDOFF_P50_US" ] && [ -n "$HANDOFF_P95_US" ] ||
    fail "warm latency sample was incomplete"
[ "$HANDOFF_P95_US" -le 100000 ] ||
    fail "warm resident-host p95 ${HANDOFF_P95_US}us exceeds 100000us"
UPDATE_TOTAL_US="$(tail -1 "$LATENCIES")"
UPDATE_HANDOFF_US="$(tail -1 "$HANDOFFS")"
[ "$UPDATE_HANDOFF_US" -le 50000 ] ||
    fail "resident progress update ${UPDATE_HANDOFF_US}us exceeds 50000us"

# An interactive model blocks only for one bounded action. The physical driver
# clicks inside action zero; the command must return its ID and exact root, but
# performs no publication or other software effect.
CONFIRM_MODEL='{"kind":"confirmation","request_id":"alpha-publish","title":"Alpha publication confirmation","summary":"HUMAN DECISION - confirm the exact local publication plan","exact_root":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","items":[{"kind":"key-value","label":"LOCAL OBSERVATION - plan root","value":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}],"actions":[{"kind":"confirm","id":"confirm-exact-local-publication","label":"Confirm exact local publication"},{"kind":"cancel","id":"cancel","label":"Cancel - make no change"}]}'
CONFIRM_REPLY_FILE="$RUN_ROOT/confirmation.json"
"$NODE_BIN" app presentation show --input="$CONFIRM_MODEL" \
    >"$CONFIRM_REPLY_FILE" 2>&1 &
CONFIRM_PID="$!"
"$DRIVER_BIN" --title='Alpha publication confirmation' --key=1 \
    --timeout-ms=3000 >/dev/null || {
        kill "$CONFIRM_PID" 2>/dev/null || true
        wait "$CONFIRM_PID" 2>/dev/null || true
        fail "physical confirmation action was not delivered"
    }
wait "$CONFIRM_PID" || fail "interactive presentation command failed"
CONFIRM_REPLY="$(<"$CONFIRM_REPLY_FILE")"
assert_display_reply "$CONFIRM_REPLY"
json_has "$CONFIRM_REPLY" '"event_return":true' ||
    fail "bounded confirmation event did not return"
json_has "$CONFIRM_REPLY" '"action_id":"confirm-exact-local-publication"' ||
    fail "confirmation returned the wrong action identity"
json_has "$CONFIRM_REPLY" '"exact_root":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"' ||
    fail "confirmation event lost its exact plan root"

set +e
HEADLESS_REPLY="$(env -u DISPLAY "$NODE_BIN" app qr show \
    --input='{"payload":"headless-refusal"}' 2>&1)"
HEADLESS_RC="$?"
set -e
[ "$HEADLESS_RC" -ne 0 ] || fail "headless QR unexpectedly succeeded"
json_has "$HEADLESS_REPLY" 'HEADLESS_DISPLAY_UNAVAILABLE' ||
    fail "headless refusal was not named"

DEPENDENCIES="$(ldd "$NODE_BIN" 2>&1 | tr '[:upper:]' '[:lower:]')"
case "$DEPENDENCIES" in
    *gtk*|*webkit*|*javascriptcore*|*libnode*)
        fail "release node acquired a browser/UI-framework dependency" ;;
esac
AFTER_BROWSERS="$(browser_snapshot)"
[ "$BEFORE_BROWSERS" = "$AFTER_BROWSERS" ] ||
    fail "browser process set changed during the native journey"

READY_P95_US="$(printf '%s\n' "$PROGRESS_REPLY" | sed -n \
    's/.*"window_ready_us":[[:space:]]*\([-0-9][0-9]*\).*/\1/p' | tail -1)"
printf '%s\n' \
    "{\"schema\":\"zcl.native_agent_ui_physical.v1\",\"verdict\":\"PASS\",\"qr_window\":true,\"status_card\":true,\"code_diff\":true,\"live_reproduction_progress\":true,\"exact_confirmation_event\":true,\"display_only_authority\":true,\"headless_named_refusal\":true,\"browser_process_delta\":0,\"release_browser_dependency\":false,\"cold_total_us\":$COLD_TOTAL_US,\"warm_total_p50_us\":$WARM_P50_US,\"warm_total_p95_us\":$WARM_P95_US,\"warm_handoff_p50_us\":$HANDOFF_P50_US,\"warm_handoff_p95_us\":$HANDOFF_P95_US,\"update_total_us\":$UPDATE_TOTAL_US,\"update_handoff_us\":$UPDATE_HANDOFF_US,\"last_worker_ready_us\":${READY_P95_US:--1}}"
