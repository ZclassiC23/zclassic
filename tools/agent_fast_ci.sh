#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Cache-aware fast lane for agent/operator edit loops.
# This is deliberately not the release gate; pre-push/full CI remains authority.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

SCHEMA="zcl.agent_fast_ci.v1"
FAST_CC="${ZCL_FAST_CC:-}"
CACHE_TOOL="none"
TEST_GROUPS=""
UNMAPPED_CODE_CHANGES=""
NODE_BIN="${ZCL_FAST_NODE_BIN:-build/bin/zclassic23}"
FAST_JOBS="${ZCL_FAST_JOBS:-}"

log() {
    printf '[agent-fast-ci] %s\n' "$*"
}

fail() {
    log "FAIL: $*"
    exit 1
}

select_compiler() {
    if [ -n "$FAST_CC" ]; then
        case "$FAST_CC" in
            *sccache*) CACHE_TOOL="sccache" ;;
            *ccache*) CACHE_TOOL="ccache" ;;
            *) CACHE_TOOL="custom" ;;
        esac
        return
    fi

    if command -v sccache >/dev/null 2>&1; then
        FAST_CC="sccache cc"
        CACHE_TOOL="sccache"
    elif command -v ccache >/dev/null 2>&1; then
        FAST_CC="ccache cc"
        CACHE_TOOL="ccache"
    else
        FAST_CC="cc"
        CACHE_TOOL="none"
    fi
}

show_cache_stats() {
    case "$CACHE_TOOL" in
        sccache)
            command -v sccache >/dev/null 2>&1 && sccache --show-stats || true
            ;;
        ccache)
            command -v ccache >/dev/null 2>&1 && ccache -s || true
            ;;
        *)
            ;;
    esac
}

make_fast() {
    if [ -z "$FAST_JOBS" ]; then
        FAST_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"
        if [ "$FAST_JOBS" -gt 16 ] 2>/dev/null; then
            FAST_JOBS=16
        fi
    fi
    make -j"$FAST_JOBS" CC="$FAST_CC" "$@"
}

changed_files() {
    if [ -n "${ZCL_FAST_BASE:-}" ]; then
        git diff --name-only "$ZCL_FAST_BASE"...HEAD -- || true
    fi
    git diff --name-only HEAD --
    git diff --cached --name-only --
}

add_group() {
    local group="$1"
    case " $TEST_GROUPS " in
        *" $group "*) ;;
        *) TEST_GROUPS="${TEST_GROUPS:+$TEST_GROUPS }$group" ;;
    esac
}

add_unmapped_code_change() {
    local file="$1"
    case " $UNMAPPED_CODE_CHANGES " in
        *" $file "*) ;;
        *) UNMAPPED_CODE_CHANGES="${UNMAPPED_CODE_CHANGES:+$UNMAPPED_CODE_CHANGES }$file" ;;
    esac
}

is_code_like_change() {
    local file="$1"
    case "$file" in
        *.c|*.h|Makefile|*.mk|tools/*.sh|tools/githooks/*)
            return 0
            ;;
        app/*|application/*|adapters/*|config/*|domain/*|lib/*|ports/*|tools/mcp/*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

select_test_groups() {
    local file matched
    if [ -n "${ZCL_FAST_TESTS:-}" ]; then
        for file in $(printf '%s\n' "$ZCL_FAST_TESTS" | tr ',:' '  '); do
            [ -n "$file" ] && add_group "$file"
        done
        return
    fi

    while IFS= read -r file; do
        [ -n "$file" ] || continue
        matched=0
        case "$file" in
            Makefile|src/main.c|tools/*|docs/AGENT_API.md|docs/CODEBASE_MAP.md|docs/HOW_THE_NODE_WORKS.md|docs/work/fast-path.md|docs/RUNBOOK.md|lib/test/src/test_make_lint_gates.c)
                add_group "make_lint_gates"
                matched=1
                ;;
        esac
        case "$file" in
            tools/mcp/*|tools/mcp_server.c|app/controllers/src/*mcp*|lib/test/src/test_mcp_controllers.c)
                add_group "mcp_controllers"
                matched=1
                ;;
            lib/mcp/*|lib/util/src/mcp_*|lib/test/src/test_mcp_*|app/controllers/src/diagnostics_registry.c)
                add_group "mcp_controllers"
                matched=1
                ;;
            lib/net/src/peer_lifecycle.c|lib/net/src/msg_version.c|lib/net/src/protocol.c|app/controllers/src/network_controller.c|lib/test/src/test_peer_lifecycle.c)
                add_group "peer_lifecycle"
                matched=1
                ;;
            app/controllers/src/network_controller.c|app/controllers/src/sync_controller.c|lib/test/src/test_syncdiag_rpc.c)
                add_group "syncdiag_rpc"
                matched=1
                ;;
            app/services/src/node_health_service.c|app/services/include/services/node_health_service.h|lib/test/src/test_node_health_service.c)
                add_group "node_health_service"
                matched=1
                ;;
            app/controllers/src/api_controller_status.c|app/controllers/src/api_controller_index.c|app/controllers/src/api_controller_routes.c|lib/test/src/test_api.c)
                add_group "api"
                matched=1
                ;;
            app/controllers/src/agent_controller.c|app/controllers/include/controllers/agent_controller.h|app/controllers/src/event_controller.c)
                add_group "make_lint_gates"
                add_group "node_health_service"
                add_group "mcp_controllers"
                add_group "syncdiag_rpc"
                matched=1
                ;;
        esac
        if [ "$matched" -eq 0 ] && is_code_like_change "$file"; then
            add_unmapped_code_change "$file"
        fi
    done <<EOF
$(changed_files | sort -u)
EOF
}

run_shell_checks() {
    local script
    log "shell checks"
    git diff --check
    for script in tools/agent_fast_ci.sh tools/z tools/githooks/pre-push tools/deploy_verify.sh; do
        bash -n "$script"
    done
}

run_focused_tests() {
    local group target
    select_test_groups
    if [ -n "$UNMAPPED_CODE_CHANGES" ]; then
        fail "no focused test mapping for code changes: $UNMAPPED_CODE_CHANGES; set ZCL_FAST_TESTS=<group[,group]> or extend tools/agent_fast_ci.sh"
    fi
    if [ -z "$TEST_GROUPS" ]; then
        log "focused tests: none selected from changed files; set ZCL_FAST_TESTS=<group[,group]> to force"
        return
    fi

    target="t-fast"
    case "${ZCL_FAST_STRICT_TESTS:-0}" in
        1|true|yes|strict) target="t" ;;
        0|false|no|"") ;;
        *) fail "unknown ZCL_FAST_STRICT_TESTS=${ZCL_FAST_STRICT_TESTS}" ;;
    esac
    log "focused test target=$target jobs=${FAST_JOBS:-auto}"

    for group in $TEST_GROUPS; do
        log "focused test: $group"
        make_fast "$target" ONLY="$group"
    done
}

live_service_detected() {
    if command -v systemctl >/dev/null 2>&1 &&
       systemctl --user is-active --quiet zclassic23; then
        return 0
    fi
    [ -f "$HOME/.zclassic-c23/.cookie" ]
}

validate_agent_json() {
    local json="$1"
    if command -v jq >/dev/null 2>&1; then
        printf '%s\n' "$json" |
            jq -e '.schema == "zcl.public_status.v1" and
                   .status == "healthy" and
                   .healthy == true and
                   .serving == true and
                   (.operator_needed == false) and
                   ((.gap // 0) <= 1)' >/dev/null
    else
        printf '%s\n' "$json" |
            grep -q '"schema"[[:space:]]*:[[:space:]]*"zcl.public_status.v1"'
        printf '%s\n' "$json" |
            grep -q '"status"[[:space:]]*:[[:space:]]*"healthy"'
        printf '%s\n' "$json" |
            grep -q '"healthy"[[:space:]]*:[[:space:]]*true'
        printf '%s\n' "$json" |
            grep -q '"serving"[[:space:]]*:[[:space:]]*true'
        printf '%s\n' "$json" |
            grep -q '"operator_needed"[[:space:]]*:[[:space:]]*false'
    fi
}

validate_health_json() {
    local json="$1"
    if command -v jq >/dev/null 2>&1; then
        printf '%s\n' "$json" |
            jq -e '.healthy == true and
                   .serving == true and
                   .checks.has_peers == true and
                   .checks.peer_count > 0 and
                   ((.checks.blocking_reason // "") == "")' >/dev/null
    else
        printf '%s\n' "$json" |
            grep -q '"healthy"[[:space:]]*:[[:space:]]*true'
        printf '%s\n' "$json" |
            grep -q '"serving"[[:space:]]*:[[:space:]]*true'
        printf '%s\n' "$json" |
            grep -q '"has_peers"[[:space:]]*:[[:space:]]*true'
        printf '%s\n' "$json" |
            grep -q '"peer_count"[[:space:]]*:[[:space:]]*[1-9][0-9]*'
    fi
}

run_native_service_probe() {
    local agent health
    log "live service probe via $NODE_BIN agent"
    agent="$("$NODE_BIN" agent)"
    validate_agent_json "$agent"

    log "live service probe via $NODE_BIN healthcheck"
    health="$("$NODE_BIN" healthcheck)"
    validate_health_json "$health"
}

run_tools_z_fallback_probe() {
    local json
    [ -x ./tools/z ] || return 1
    log "fallback live topology probe via tools/z topology --json"
    json="$(./tools/z topology --json)"
    if command -v jq >/dev/null 2>&1; then
        printf '%s\n' "$json" |
            jq -e '.schema == "zcl.operator_topology.v1" and .ok == true' >/dev/null
    else
        printf '%s\n' "$json" |
            grep -q '"schema"[[:space:]]*:[[:space:]]*"zcl.operator_topology.v1"'
        printf '%s\n' "$json" | grep -q '"ok"[[:space:]]*:[[:space:]]*true'
    fi
}

run_live_probe() {
    if [ -x "$NODE_BIN" ]; then
        run_native_service_probe
        return
    fi
    log "native service binary $NODE_BIN unavailable; trying shell fallback"
    run_tools_z_fallback_probe
}

maybe_live_probe() {
    case "${ZCL_FAST_LIVE:-auto}" in
        0|false|no|skip)
            log "live topology probe skipped by ZCL_FAST_LIVE=${ZCL_FAST_LIVE}"
            ;;
        1|true|yes|require)
            run_live_probe
            ;;
        auto|"")
            if live_service_detected; then
                run_live_probe
            else
                log "live topology probe skipped; zclassic23 service was not detected"
            fi
            ;;
        *)
            fail "unknown ZCL_FAST_LIVE=${ZCL_FAST_LIVE}"
            ;;
    esac
}

main() {
    log "schema=$SCHEMA"
    select_compiler
    log "compiler=$FAST_CC cache=$CACHE_TOOL"
    show_cache_stats

    run_shell_checks
    log "lint-fast"
    make_fast lint-fast
    log "build-only"
    make_fast build-only
    run_focused_tests
    maybe_live_probe

    log "PASS: fast lane complete; not full release CI"
    log "Before pushing main, keep the full gate: make lint && make build-only && relevant tests; default pre-push remains full make ci."
}

main "$@"
