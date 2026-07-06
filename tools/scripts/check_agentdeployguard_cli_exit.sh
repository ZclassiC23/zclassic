#!/usr/bin/env bash
# Verify the native agentdeployguard CLI contract without jq or live datadirs.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

BIN="${ZCL_NODE_BIN:-build/bin/zclassic23}"
TIMEOUT_SECS="${ZCL_AGENTDEPLOYGUARD_CLI_TIMEOUT:-10}"

fail() {
    printf 'check_agentdeployguard_cli_exit: FAIL: %s\n' "$*" >&2
    exit 1
}

require_field() {
    local file="$1"
    local pattern="$2"

    grep -Fq -- "$pattern" "$file" ||
        fail "missing pattern $pattern in $file"
}

run_guard() {
    local home_dir="$1"
    local out="$2"
    shift 2

    if command -v timeout >/dev/null 2>&1; then
        HOME="$home_dir" timeout "${TIMEOUT_SECS}s" "$BIN" "$@" >"$out"
    else
        HOME="$home_dir" "$BIN" "$@" >"$out"
    fi
}

[ -x "$BIN" ] || fail "node binary is not executable: $BIN"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

allow_home="$tmp/allow-home"
allow_json="$tmp/allow.json"
mkdir -p "$allow_home/.zclassic-c23"

set +e
run_guard "$allow_home" "$allow_json" \
    "-datadir=$allow_home/.zclassic-c23" agentdeployguard deploy-dev
allow_rc=$?
set -e

[ "$allow_rc" -eq 0 ] ||
    fail "allow case exited $allow_rc, expected 0"
require_field "$allow_json" '"schema":"zcl.agent_deploy_guard.v1"'
require_field "$allow_json" '"decision":"allow"'
require_field "$allow_json" '"exit_code":0'

refuse_home="$tmp/refuse-home"
refuse_json="$tmp/refuse.json"
mkdir -p "$refuse_home/.zclassic-c23-dev"
printf '3172354 1\n' >"$refuse_home/.zclassic-c23-dev/auto_reindex_request"

set +e
run_guard "$refuse_home" "$refuse_json" agentdeployguard deploy-dev
refuse_rc=$?
set -e

[ "$refuse_rc" -eq 1 ] ||
    fail "refuse case exited $refuse_rc, expected 1"
require_field "$refuse_json" '"schema":"zcl.agent_deploy_guard.v1"'
require_field "$refuse_json" '"decision":"refuse"'
require_field "$refuse_json" '"exit_code":1'
require_field "$refuse_json" \
    '"reason":"pending_auto_reindex_requires_explicit_recovery_boot"'

printf 'check_agentdeployguard_cli_exit: OK\n'
