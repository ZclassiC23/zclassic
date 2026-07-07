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
FAST_COMPILE="${ZCL_FAST_COMPILE:-dev}"
CACHE_ROOT="${ZCL_FAST_CACHE_DIR:-$ROOT/.cache/zcl-agent-fast-ci}"
CACHE_KEY=""
CACHE_RECORD=""
CACHE_AVAILABLE=0
CACHE_TOOL="none"
TEST_GROUPS=""
UNMAPPED_CODE_CHANGES=""
NODE_BIN="${ZCL_FAST_NODE_BIN:-build/bin/zclassic23}"
FAST_JOBS="${ZCL_FAST_JOBS:-}"
IMPACT_RULES_FILE="${ZCL_FAST_IMPACT_RULES_FILE:-app/controllers/include/controllers/agent_impact_rules.def}"

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

match_shared_impact_rules() {
    local file="$1" line patterns groups pattern group matched rule_re
    [ -f "$IMPACT_RULES_FILE" ] ||
        fail "agent impact rule file missing: $IMPACT_RULES_FILE"

    matched=1
    rule_re='^[[:space:]]*AGENT_IMPACT_RULE\("([^"]*)",[[:space:]]*"([^"]*)"\)'
    while IFS= read -r line; do
        [[ "$line" =~ $rule_re ]] || continue
        patterns="${BASH_REMATCH[1]}"
        groups="${BASH_REMATCH[2]}"
        IFS='|' read -r -a rule_patterns <<< "$patterns"
        for pattern in "${rule_patterns[@]}"; do
            case "$file" in
                $pattern)
                    for group in $groups; do
                        add_group "$group"
                    done
                    matched=0
                    break
                    ;;
            esac
        done
    done < "$IMPACT_RULES_FILE"
    return "$matched"
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
        if match_shared_impact_rules "$file"; then
            matched=1
        fi
        if [ "$matched" -eq 0 ] && is_code_like_change "$file"; then
            add_unmapped_code_change "$file"
        fi
    done <<EOF
$(changed_files | sort -u)
EOF
}

fail_on_unmapped_code_changes() {
    if [ -n "$UNMAPPED_CODE_CHANGES" ]; then
        fail "no focused test mapping for code changes: $UNMAPPED_CODE_CHANGES; set ZCL_FAST_TESTS=<group[,group]> or extend $IMPACT_RULES_FILE"
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
    printf 'fast_compile\t%s\n' "$FAST_COMPILE"
    printf 'fast_tests_env\t%s\n' "${ZCL_FAST_TESTS:-}"
    printf 'fast_changed_files_file\t%s\n' "${ZCL_FAST_CHANGED_FILES_FILE:-}"
    printf 'fast_changed_files_env\t%s\n' "${ZCL_FAST_CHANGED_FILES:-}"
    printf 'fast_strict_tests\t%s\n' "${ZCL_FAST_STRICT_TESTS:-0}"
    printf 'fast_live\t%s\n' "${ZCL_FAST_LIVE:-auto}"
    printf 'node_bin\t%s\n' "$NODE_BIN"
    printf 'impact_rules_file\t%s\n' "$IMPACT_RULES_FILE"
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

    for file in Makefile "$IMPACT_RULES_FILE" tools/agent_fast_ci.sh tools/z \
        tools/githooks/pre-push tools/deploy_guard.sh tools/deploy_verify.sh \
        tools/scripts/background_quality_lane.sh \
        tools/scripts/check_agentdeployguard_cli_exit.sh \
        deploy/zclassic23-fuzz.service deploy/zclassic23-fuzz.timer \
        deploy/zclassic23-coverage.service deploy/zclassic23-coverage.timer \
        deploy/zclassic23-test-suite.service deploy/zclassic23-test-suite.timer \
        lib/test/src/test_make_lint_gates.c docs/work/fast-path.md \
        docs/AGENT_API.md app/controllers/src/agent_controller.c \
        app/controllers/src/agent_lane_runtime.c \
        app/controllers/src/agent_runtime_controller.c; do
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
    log "fast result cache hit key=$CACHE_KEY; skipping lint-fast/compile-gate/focused tests"
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
    for script in tools/agent_fast_ci.sh tools/z tools/githooks/pre-push \
        tools/deploy_guard.sh tools/deploy_verify.sh \
        tools/scripts/background_quality_lane.sh \
        tools/scripts/check_agentdeployguard_cli_exit.sh; do
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

run_compile_gate() {
    local target
    case "$FAST_COMPILE" in
        dev|fast|quick|"")
            target="fast-compile"
            ;;
        strict|release|build-only)
            target="build-only"
            ;;
        *)
            fail "unknown ZCL_FAST_COMPILE=${FAST_COMPILE}"
            ;;
    esac
    log "$target"
    make_fast "$target"
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
        if ! printf '%s\n' "$json" |
            jq -e '.schema == "zcl.public_status.v1" and
                   .status == "healthy" and
                   .healthy == true and
                   .serving == true and
                   (.operator_needed == false)' >/dev/null; then
            log "agent probe summary: $(printf '%s\n' "$json" |
                jq -c '{schema,status,healthy,serving,operator_needed,gap,primary_blocker,next}' 2>/dev/null ||
                printf '%s' "$json")"
            fail "agent live probe did not report healthy serving status"
        fi
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
        if ! printf '%s\n' "$json" |
            jq -e '.healthy == true and
                   .serving == true and
                   .checks.has_peers == true and
                   .checks.peer_count > 0 and
                   ((.checks.blocking_reason // "") == "")' >/dev/null; then
            log "health probe summary: $(printf '%s\n' "$json" |
                jq -c '{healthy,serving,peer_count:.checks.peer_count,blocking_reason:.checks.blocking_reason,warning:.checks.warning,warning_reasons:.checks.warning_reasons}' 2>/dev/null ||
                printf '%s' "$json")"
            fail "health live probe did not report healthy serving status"
        fi
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

run_live_probe() {
    [ -x "$NODE_BIN" ] ||
        fail "native service binary $NODE_BIN unavailable; run make build-only or set ZCL_FAST_LIVE=0"
    run_native_service_probe
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
    log "compiler=$FAST_CC cache=$CACHE_TOOL jobs=$FAST_JOBS compile=$FAST_COMPILE"
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
    run_compile_gate
    run_focused_tests
    maybe_live_probe

    record_fast_cache_pass
    log "PASS: fast lane complete; not full release CI"
    log "Before pushing main, keep the strict gate: make lint && make build-only && relevant tests; default pre-push runs make pre-push-ci. Full-suite/fuzz/coverage run through make install-quality-linger."
}

main "$@"
