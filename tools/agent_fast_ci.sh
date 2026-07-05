#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Cache-aware fast lane for agent/operator edit loops.
# This is deliberately not the release gate; pre-push/full CI remains authority.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

SCHEMA="zcl.agent_fast_ci.v1"
CACHE_SCHEMA="zcl.agent_fast_ci.cache.v1"
FAST_CC="${ZCL_FAST_CC:-}"
CACHE_ROOT="${ZCL_FAST_CACHE_DIR:-$ROOT/.cache/zcl-agent-fast-ci}"
CACHE_KEY=""
CACHE_RECORD=""
CACHE_AVAILABLE=0
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

resolve_fast_jobs() {
    if [ -z "$FAST_JOBS" ]; then
        FAST_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"
        case "$FAST_JOBS" in
            ''|*[!0-9]*) FAST_JOBS=8 ;;
        esac
        if [ "$FAST_JOBS" -gt 16 ] 2>/dev/null; then
            FAST_JOBS=16
        fi
    fi
    case "$FAST_JOBS" in
        ''|*[!0-9]*|0)
            fail "ZCL_FAST_JOBS must be a positive integer (got ${FAST_JOBS:-empty})"
            ;;
    esac
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
    resolve_fast_jobs
    make -j"$FAST_JOBS" CC="$FAST_CC" "$@"
}

changed_files() {
    if [ -n "${ZCL_FAST_CHANGED_FILES_FILE:-}" ]; then
        [ -f "$ZCL_FAST_CHANGED_FILES_FILE" ] ||
            fail "ZCL_FAST_CHANGED_FILES_FILE does not exist: $ZCL_FAST_CHANGED_FILES_FILE"
        cat "$ZCL_FAST_CHANGED_FILES_FILE"
    fi
    if [ -n "${ZCL_FAST_CHANGED_FILES:-}" ]; then
        printf '%s\n' "$ZCL_FAST_CHANGED_FILES" | tr ' ,' '\n'
    fi
    if [ -n "${ZCL_FAST_BASE:-}" ]; then
        git diff --name-only "$ZCL_FAST_BASE"...HEAD -- || true
    fi
    git diff --name-only HEAD --
    git diff --cached --name-only --
    git ls-files --others --exclude-standard
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
    TEST_GROUPS=""
    UNMAPPED_CODE_CHANGES=""
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
            Makefile|src/main.c|tools/*|tools/scripts/*|deploy/*.service|deploy/*.timer|docs/AGENT_API.md|docs/CODEBASE_MAP.md|docs/HOW_THE_NODE_WORKS.md|docs/work/fast-path.md|docs/RUNBOOK.md|lib/test/src/test_make_lint_gates.c)
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
            lib/net/src/connman.c)
                add_group "connman_addnode_fallback"
                add_group "net"
                matched=1
                ;;
            lib/net/include/net/msg_internal.h|lib/net/include/net/peer_lifecycle.h|lib/net/include/net/port_policy.h|lib/net/include/net/version.h)
                add_group "peer_lifecycle"
                matched=1
                ;;
            lib/net/include/net/protocol.h)
                add_group "net"
                matched=1
                ;;
            lib/test/src/test_net.c)
                add_group "net"
                matched=1
                ;;
            app/models/src/peer.c|app/models/include/models/peer.h|lib/test/src/test_models_app.c)
                add_group "models"
                matched=1
                ;;
            app/controllers/src/network_controller.c|app/controllers/include/controllers/network_controller.h|app/controllers/src/sync_controller.c|lib/test/src/test_syncdiag_rpc.c)
                add_group "syncdiag_rpc"
                matched=1
                ;;
            config/src/boot_services.c)
                add_group "make_lint_gates"
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

fail_on_unmapped_code_changes() {
    if [ -n "$UNMAPPED_CODE_CHANGES" ]; then
        fail "no focused test mapping for code changes: $UNMAPPED_CODE_CHANGES; set ZCL_FAST_TESTS=<group[,group]> or extend tools/agent_fast_ci.sh"
    fi
}

fast_cache_enabled() {
    case "${ZCL_FAST_CACHE:-1}" in
        1|true|yes|on|"") return 0 ;;
        0|false|no|off|skip) return 1 ;;
        *) fail "unknown ZCL_FAST_CACHE=${ZCL_FAST_CACHE}" ;;
    esac
}

maybe_reset_fast_cache() {
    case "${ZCL_FAST_CACHE_RESET:-0}" in
        1|true|yes|reset)
            rm -rf "$CACHE_ROOT"
            log "fast result cache reset at $CACHE_ROOT"
            ;;
        0|false|no|"") ;;
        *) fail "unknown ZCL_FAST_CACHE_RESET=${ZCL_FAST_CACHE_RESET}" ;;
    esac
}

hash_file() {
    sha256sum "$1" | awk '{print $1}'
}

cache_manifest_file() {
    local label="$1" path="$2"
    if [ -f "$path" ]; then
        printf 'file\t%s\t%s\t%s\n' "$label" "$path" "$(hash_file "$path")"
    elif [ -e "$path" ]; then
        printf 'file\t%s\t%s\tpresent-nonregular\n' "$label" "$path"
    else
        printf 'file\t%s\t%s\tabsent\n' "$label" "$path"
    fi
}

cache_manifest() {
    local file status_line node_stat cc_version
    printf 'cache_schema\t%s\n' "$CACHE_SCHEMA"
    printf 'fast_schema\t%s\n' "$SCHEMA"
    printf 'git_head\t%s\n' "$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    printf 'fast_base\t%s\n' "${ZCL_FAST_BASE:-}"
    printf 'fast_cc\t%s\n' "$FAST_CC"
    printf 'cache_tool\t%s\n' "$CACHE_TOOL"
    printf 'fast_jobs\t%s\n' "$FAST_JOBS"
    printf 'fast_tests_env\t%s\n' "${ZCL_FAST_TESTS:-}"
    printf 'fast_changed_files_file\t%s\n' "${ZCL_FAST_CHANGED_FILES_FILE:-}"
    printf 'fast_changed_files_env\t%s\n' "${ZCL_FAST_CHANGED_FILES:-}"
    printf 'fast_strict_tests\t%s\n' "${ZCL_FAST_STRICT_TESTS:-0}"
    printf 'fast_live\t%s\n' "${ZCL_FAST_LIVE:-auto}"
    printf 'node_bin\t%s\n' "$NODE_BIN"
    printf 'test_groups\t%s\n' "$TEST_GROUPS"
    printf 'make_version\t%s\n' "$(make --version 2>/dev/null | sed -n '1p' || echo unknown)"
    cc_version="$($FAST_CC --version 2>/dev/null | sed -n '1p' || true)"
    printf 'cc_version\t%s\n' "${cc_version:-unknown}"

    if [ -e "$NODE_BIN" ]; then
        node_stat="$(stat -c '%s:%Y' "$NODE_BIN" 2>/dev/null || echo unknown)"
    else
        node_stat="absent"
    fi
    printf 'node_bin_stat\t%s\n' "$node_stat"

    git status --porcelain=v1 --untracked-files=normal |
        while IFS= read -r status_line; do
            printf 'git_status\t%s\n' "$status_line"
        done

    for file in Makefile tools/agent_fast_ci.sh tools/z \
        tools/githooks/pre-push tools/deploy_verify.sh \
        tools/scripts/background_quality_lane.sh \
        deploy/zclassic23-fuzz.service deploy/zclassic23-fuzz.timer \
        deploy/zclassic23-coverage.service deploy/zclassic23-coverage.timer \
        deploy/zclassic23-test-suite.service deploy/zclassic23-test-suite.timer \
        lib/test/src/test_make_lint_gates.c docs/work/fast-path.md \
        docs/AGENT_API.md app/controllers/src/agent_controller.c; do
        cache_manifest_file "$file" "$file"
    done

    changed_files | sort -u |
        while IFS= read -r file; do
            [ -n "$file" ] || continue
            cache_manifest_file "changed:$file" "$file"
        done
}

compute_cache_key() {
    local manifest key
    CACHE_AVAILABLE=0
    CACHE_KEY=""
    CACHE_RECORD=""

    if ! fast_cache_enabled; then
        log "fast result cache disabled by ZCL_FAST_CACHE=${ZCL_FAST_CACHE:-0}"
        return 1
    fi
    if ! command -v sha256sum >/dev/null 2>&1; then
        log "fast result cache unavailable: sha256sum not found"
        return 1
    fi
    mkdir -p "$CACHE_ROOT"
    manifest="$(mktemp "$CACHE_ROOT/manifest.XXXXXX")" || return 1
    if ! cache_manifest >"$manifest"; then
        rm -f "$manifest"
        log "fast result cache unavailable: could not write manifest"
        return 1
    fi
    key="$(sha256sum "$manifest" | awk '{print $1}')"
    rm -f "$manifest"
    [ -n "$key" ] || return 1
    CACHE_KEY="$key"
    CACHE_RECORD="$CACHE_ROOT/$CACHE_KEY.ok"
    CACHE_AVAILABLE=1
    return 0
}

maybe_fast_cache_hit() {
    compute_cache_key || return 1
    [ -f "$CACHE_RECORD" ] || return 1
    if ! grep -q "^schema=$CACHE_SCHEMA$" "$CACHE_RECORD"; then
        rm -f "$CACHE_RECORD"
        return 1
    fi
    log "fast result cache hit key=$CACHE_KEY; skipping lint-fast/build-only/focused tests"
    return 0
}

record_fast_cache_pass() {
    local old_key tmp
    [ "$CACHE_AVAILABLE" = "1" ] || return 0
    old_key="$CACHE_KEY"
    compute_cache_key || return 0
    if [ "$CACHE_KEY" != "$old_key" ]; then
        log "fast result cache not stored; inputs changed during run"
        return 0
    fi

    tmp="$(mktemp "$CACHE_ROOT/pass.XXXXXX")" || return 0
    {
        printf 'schema=%s\n' "$CACHE_SCHEMA"
        printf 'key=%s\n' "$CACHE_KEY"
        printf 'stored_at=%s\n' "$(date -u +%FT%TZ)"
        printf 'groups=%s\n' "$TEST_GROUPS"
        printf 'node_bin=%s\n' "$NODE_BIN"
    } >"$tmp"
    mv "$tmp" "$CACHE_RECORD"
    log "fast result cache stored key=$CACHE_KEY"
}

run_shell_checks() {
    local script
    log "shell checks"
    git diff --check
    for script in tools/agent_fast_ci.sh tools/z tools/githooks/pre-push tools/deploy_verify.sh tools/scripts/background_quality_lane.sh; do
        bash -n "$script"
    done
}

run_focused_tests() {
    local group target
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
    resolve_fast_jobs
    log "compiler=$FAST_CC cache=$CACHE_TOOL jobs=$FAST_JOBS"
    maybe_reset_fast_cache
    select_test_groups
    fail_on_unmapped_code_changes

    if maybe_fast_cache_hit; then
        maybe_live_probe
        log "PASS: fast lane cache hit; not full release CI"
        log "Before pushing main, keep the strict gate: make lint && make build-only && relevant tests; default pre-push runs make pre-push-ci. Full-suite/fuzz/coverage run through make install-quality-linger."
        return
    fi

    show_cache_stats

    run_shell_checks
    log "lint-fast"
    make_fast lint-fast
    log "build-only"
    make_fast build-only
    run_focused_tests
    maybe_live_probe

    record_fast_cache_pass
    log "PASS: fast lane complete; not full release CI"
    log "Before pushing main, keep the strict gate: make lint && make build-only && relevant tests; default pre-push runs make pre-push-ci. Full-suite/fuzz/coverage run through make install-quality-linger."
}

main "$@"
