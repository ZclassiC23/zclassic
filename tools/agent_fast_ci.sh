#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Cache-aware fast lane for agent/operator edit loops.
# This is deliberately not the release gate; pre-push/full CI remains authority.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

SCHEMA="zcl.agent_fast_ci.v1"
PLAN_SCHEMA="zcl.agent_fast_plan.v1"
CACHE_SCHEMA="zcl.agent_fast_ci.cache.v1"
FAST_CC="${ZCL_FAST_CC:-}"
FAST_COMPILE="${ZCL_FAST_COMPILE:-changed}"
FAST_CHANGED_COMPILE_LIMIT="${ZCL_FAST_CHANGED_COMPILE_LIMIT:-24}"
CACHE_ROOT="${ZCL_FAST_CACHE_DIR:-$ROOT/.cache/zcl-agent-fast-ci}"
CACHE_KEY=""
CACHE_RECORD=""
CACHE_AVAILABLE=0
CACHE_TOOL="none"
TEST_GROUPS=""
UNMAPPED_CODE_CHANGES=""
DIRECT_DEV_OBJECTS=""
DIRECT_DEV_OBJECT_COUNT=0
COMPILE_PLAN_KIND=""
COMPILE_PLAN_TARGET=""
COMPILE_PLAN_DETAIL=""
COMPILE_PLAN_FALLBACK_REASON=""
NODE_BIN="${ZCL_FAST_NODE_BIN:-build/bin/zclassic23}"
DEV_NODE_BIN="${ZCL_FAST_DEV_NODE_BIN:-build/bin/zclassic23-dev}"
FAST_JOBS="${ZCL_FAST_JOBS:-}"
IMPACT_RULES_FILE="${ZCL_FAST_IMPACT_RULES_FILE:-app/controllers/include/controllers/agent_impact_rules.def}"

log() {
    printf '[agent-fast-ci] %s\n' "$*"
}

fail() {
    log "FAIL: $*"
    exit 1
}

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

json_array_words() {
    local sep="" item
    printf '['
    for item in "$@"; do
        [ -n "$item" ] || continue
        printf '%s"%s"' "$sep" "$(json_escape "$item")"
        sep=","
    done
    printf ']'
}

json_array_stdin() {
    local sep="" item
    printf '['
    while IFS= read -r item; do
        [ -n "$item" ] || continue
        printf '%s"%s"' "$sep" "$(json_escape "$item")"
        sep=","
    done
    printf ']'
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

fast_changed_files_only() {
    case "${ZCL_FAST_CHANGED_FILES_ONLY:-0}" in
        1|true|yes|only) return 0 ;;
        0|false|no|"") return 1 ;;
        *) fail "unknown ZCL_FAST_CHANGED_FILES_ONLY=${ZCL_FAST_CHANGED_FILES_ONLY}" ;;
    esac
}

validate_changed_files_only() {
    if fast_changed_files_only &&
       [ -z "${ZCL_FAST_CHANGED_FILES_FILE:-}" ] &&
       [ -z "${ZCL_FAST_CHANGED_FILES:-}" ]; then
        fail "ZCL_FAST_CHANGED_FILES_ONLY requires ZCL_FAST_CHANGED_FILES_FILE or ZCL_FAST_CHANGED_FILES"
    fi
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
    if fast_changed_files_only; then
        return
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

# Transient lint/shape-gate fixture naming contract: test_make_lint_gates.c
# plants `_*fixture*.c` files under app/, lib/, domain/, etc. to exercise
# the gate scripts (E1-E13, raw-malloc, observability, ...), then deletes
# them before the test returns. A changed-file scan that samples the tree
# mid-test (this script's own `-changed`/`compile-changed` gates, or a
# concurrently running `dev-watch`/`pre-push-ci`) can observe one of these
# as a transient "changed" .c file with no impact-rules mapping and fail
# with "no focused test mapping for code changes" even though the actual
# push is clean. Treat it as never a real source change everywhere the
# changed-file set feeds a classification decision. Kept in sync with
# zcl_devloop_path_is_relevant() in tools/dev/devloop_plan.c (real, tracked
# fixture sources live under lib/test/fixtures/ and have no leading '_').
is_transient_lint_fixture() {
    case "$1" in
        */_*fixture*.c|_*fixture*.c)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_code_like_change() {
    local file="$1"
    is_transient_lint_fixture "$file" && return 1
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

is_graph_wide_compile_change() {
    local file="$1"
    case "$file" in
        *.h|Makefile|*.mk|app/views/templates/*|app/views/css/*|tools/gen_templates.c|vendor/include/*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_node_c_source() {
    local file="$1"
    is_transient_lint_fixture "$file" && return 1
    case "$file" in
        src/main.c|tools/mcp_server.c|app/*/src/*.c|config/src/*.c|\
        lib/*/src/*.c|domain/*/src/*.c|application/*/src/*.c|\
        adapters/outbound/persistence/src/*.c|tools/mcp/*.c|\
        tools/mcp/controllers/*.c|tools/mcp/views/*.c)
            case "$file" in
                lib/test/*|tools/sim/*)
                    return 1
                    ;;
                *)
                    return 0
                    ;;
            esac
            ;;
        *)
            return 1
            ;;
    esac
}

add_dev_object_path() {
    local obj="$1"
    case " $DIRECT_DEV_OBJECTS " in
        *" $obj "*) ;;
        *)
            DIRECT_DEV_OBJECTS="${DIRECT_DEV_OBJECTS:+$DIRECT_DEV_OBJECTS }$obj"
            DIRECT_DEV_OBJECT_COUNT=$((DIRECT_DEV_OBJECT_COUNT + 1))
            ;;
    esac
}

add_direct_dev_object() {
    local file="$1"
    add_dev_object_path "build/dev-obj/${file%.c}.o"
}

is_direct_dependency_compile_change() {
    local file="$1"
    case "$file" in
        *.h|*.def|*.inc)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

dev_depfiles_available() {
    [ -f build/dev-obj/.complete ] || return 1
    find build/dev-obj -type f -name '*.d' -print -quit 2>/dev/null |
        grep -q .
}

add_dependent_dev_objects() {
    local file="$1" dep obj

    dev_depfiles_available || return 1

    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        obj="${dep%.d}.o"
        add_dev_object_path "$obj"
    done < <(find build/dev-obj -type f -name '*.d' -exec grep -F -l -- "$file" {} + 2>/dev/null || true)

    return 0
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
    printf 'fast_changed_compile_limit\t%s\n' "$FAST_CHANGED_COMPILE_LIMIT"
    printf 'fast_tests_env\t%s\n' "${ZCL_FAST_TESTS:-}"
    printf 'fast_changed_files_only\t%s\n' "${ZCL_FAST_CHANGED_FILES_ONLY:-0}"
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
        tools/dev/deploy-dev-lane.sh tools/dev/agent-dev-status.sh \
        tools/dev/agent-doctor.sh \
        tools/scripts/remote_node_update.sh \
        tools/scripts/lane_recover.sh \
        tools/scripts/check_stable_publish_containment.sh \
        tools/scripts/build_vendor.sh \
        tools/scripts/background_quality_lane.sh \
        tools/scripts/check_agentdeployguard_cli_exit.sh \
        deploy/examples/zclassic23-remote-test-node.service \
        deploy/examples/zclassic23-remote-test.env.example \
        deploy/examples/zclassic23-self-update.service \
        deploy/examples/zclassic23-self-update.timer \
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

PLAN_CACHE_ENABLED="true"
PLAN_CACHE_AVAILABLE="false"
PLAN_CACHE_HIT="false"
PLAN_CACHE_REASON=""

compute_plan_cache_status() {
    local manifest key record
    PLAN_CACHE_ENABLED="true"
    PLAN_CACHE_AVAILABLE="false"
    PLAN_CACHE_HIT="false"
    PLAN_CACHE_REASON=""
    CACHE_KEY=""
    CACHE_RECORD=""

    case "${ZCL_FAST_CACHE:-1}" in
        1|true|yes|on|"") ;;
        0|false|no|off|skip)
            PLAN_CACHE_ENABLED="false"
            PLAN_CACHE_REASON="disabled_by_ZCL_FAST_CACHE"
            return
            ;;
        *)
            PLAN_CACHE_REASON="invalid_ZCL_FAST_CACHE"
            return
            ;;
    esac

    if ! command -v sha256sum >/dev/null 2>&1; then
        PLAN_CACHE_REASON="sha256sum_not_found"
        return
    fi
    if ! mkdir -p "$CACHE_ROOT" 2>/dev/null; then
        PLAN_CACHE_REASON="cache_root_unwritable"
        return
    fi
    manifest="$(mktemp "$CACHE_ROOT/manifest.XXXXXX" 2>/dev/null || true)"
    if [ -z "$manifest" ]; then
        PLAN_CACHE_REASON="manifest_create_failed"
        return
    fi
    if ! cache_manifest >"$manifest"; then
        rm -f "$manifest"
        PLAN_CACHE_REASON="manifest_write_failed"
        return
    fi
    key="$(sha256sum "$manifest" | awk '{print $1}')"
    rm -f "$manifest"
    if [ -z "$key" ]; then
        PLAN_CACHE_REASON="cache_key_empty"
        return
    fi

    record="$CACHE_ROOT/$key.ok"
    CACHE_KEY="$key"
    CACHE_RECORD="$record"
    PLAN_CACHE_AVAILABLE="true"
    if [ -f "$record" ] && grep -q "^schema=$CACHE_SCHEMA$" "$record"; then
        PLAN_CACHE_HIT="true"
        PLAN_CACHE_REASON="green_input_cache_hit"
    else
        PLAN_CACHE_REASON="cache_miss"
    fi
}

changed_file_count() {
    changed_files | sort -u | sed '/^$/d' | wc -l | tr -d ' '
}

recommended_plan_command() {
    local changed_count="$1"
    if [ -n "$UNMAPPED_CODE_CHANGES" ]; then
        printf 'set ZCL_FAST_TESTS=<group[,group]> or extend %s' "$IMPACT_RULES_FILE"
    elif [ "$changed_count" = "0" ]; then
        printf 'make agent-dev-status'
    elif [ "$PLAN_CACHE_HIT" = "true" ]; then
        printf 'make fast-ci'
    else
        printf 'make agent-loop'
    fi
}

emit_plan_json() {
    local changed_count command

    select_compiler
    resolve_fast_jobs
    validate_changed_files_only
    select_test_groups
    compute_changed_compile_plan
    compute_plan_cache_status

    changed_count="$(changed_file_count)"
    command="$(recommended_plan_command "$changed_count")"

    printf '{\n'
    printf '  "schema": "%s",\n' "$PLAN_SCHEMA"
    printf '  "status": "ok",\n'
    printf '  "compiler": "%s",\n' "$(json_escape "$FAST_CC")"
    printf '  "cache_tool": "%s",\n' "$(json_escape "$CACHE_TOOL")"
    printf '  "jobs": "%s",\n' "$(json_escape "$FAST_JOBS")"
    printf '  "fast_compile_mode": "%s",\n' "$(json_escape "$FAST_COMPILE")"
    printf '  "changed_file_count": %s,\n' "$changed_count"
    printf '  "changed_files": '
    changed_files | sort -u | json_array_stdin
    printf ',\n'
    printf '  "test_groups": '
    json_array_words $TEST_GROUPS
    printf ',\n'
    printf '  "unmapped_code_changes": '
    json_array_words $UNMAPPED_CODE_CHANGES
    printf ',\n'
    printf '  "compile_plan": {\n'
    printf '    "schema": "zcl.agent_changed_compile_plan.v1",\n'
    printf '    "kind": "%s",\n' "$(json_escape "$COMPILE_PLAN_KIND")"
    printf '    "target": "%s",\n' "$(json_escape "$COMPILE_PLAN_TARGET")"
    printf '    "detail": "%s",\n' "$(json_escape "$COMPILE_PLAN_DETAIL")"
    printf '    "fallback_reason": "%s",\n' "$(json_escape "$COMPILE_PLAN_FALLBACK_REASON")"
    printf '    "direct_object_count": %s,\n' "$DIRECT_DEV_OBJECT_COUNT"
    printf '    "direct_objects": '
    json_array_words $DIRECT_DEV_OBJECTS
    printf '\n'
    printf '  },\n'
    printf '  "green_input_cache": {\n'
    printf '    "schema": "%s",\n' "$CACHE_SCHEMA"
    printf '    "enabled": %s,\n' "$PLAN_CACHE_ENABLED"
    printf '    "available": %s,\n' "$PLAN_CACHE_AVAILABLE"
    printf '    "hit": %s,\n' "$PLAN_CACHE_HIT"
    printf '    "key": "%s",\n' "$(json_escape "$CACHE_KEY")"
    printf '    "record": "%s",\n' "$(json_escape "$CACHE_RECORD")"
    printf '    "reason": "%s",\n' "$(json_escape "$PLAN_CACHE_REASON")"
    printf '    "root": "%s"\n' "$(json_escape "$CACHE_ROOT")"
    printf '  },\n'
    printf '  "native_shortcuts": {\n'
    printf '    "fresh_source_tree": "zclassic23 <leaf> [--input=json]",\n'
    printf '    "dev_linger_lane": "zclassic23-dev <leaf> [--input=json]",\n'
    printf '    "discover": "zclassic23 discover help | zclassic23 discover search <q>",\n'
    printf '    "dev_hotswap_probe": "contained_before_dlopen_use_build_test_sim"\n'
    printf '  },\n'
    printf '  "mcp_shortcuts": {\n'
    printf '    "status": "legacy, removed in W3 -- prefer native_shortcuts above",\n'
    printf '    "fresh_source_tree": "make agent-mcp-call TOOL=<tool> [ARGS='
    printf "'{}'"
    printf ']",\n'
    printf '    "hot_source_tree": "make agent-mcp-call-hot TOOL=<tool> [ARGS='
    printf "'{}'"
    printf ']",\n'
    printf '    "dev_linger_lane": "make agent-mcp-call-dev TOOL=<tool> [ARGS='
    printf "'{}'"
    printf ']"\n'
    printf '  },\n'
    printf '  "dev_lane": {\n'
    printf '    "runtime_publication": false,\n'
    printf '    "publication_blocker": "immutable epoch/proof/resident-CAS/rollback transaction incomplete",\n'
    printf '    "status": "make agent-dev-status",\n'
    printf '    "stage_without_restart": "make agent-stage-dev",\n'
    printf '    "hot_swap_restart": "make agent-deploy-fast",\n'
    printf '    "loop_stage": "ZCL_AGENT_LOOP_DEPLOY=stage make agent-loop",\n'
    printf '    "loop_deploy": "ZCL_AGENT_LOOP_DEPLOY=dev make agent-loop"\n'
    printf '  },\n'
    printf '  "live_probe_mode": "%s",\n' "$(json_escape "${ZCL_FAST_LIVE:-auto}")"
    printf '  "recommended_command": "%s"\n' "$(json_escape "$command")"
    printf '}\n'
}

run_shell_checks() {
    local script
    log "shell checks"
    git diff --check
    for script in tools/agent_fast_ci.sh tools/z tools/githooks/pre-push \
        tools/deploy_guard.sh tools/deploy_verify.sh \
        tools/dev/deploy-dev-lane.sh tools/dev/agent-dev-status.sh \
        tools/dev/agent-doctor.sh \
        tools/scripts/remote_node_update.sh \
        tools/scripts/lane_recover.sh \
        tools/scripts/check_stable_publish_containment.sh \
        tools/scripts/build_vendor.sh \
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

compute_changed_compile_plan() {
    local file fallback_reason
    DIRECT_DEV_OBJECTS=""
    DIRECT_DEV_OBJECT_COUNT=0
    COMPILE_PLAN_KIND=""
    COMPILE_PLAN_TARGET=""
    COMPILE_PLAN_DETAIL=""
    COMPILE_PLAN_FALLBACK_REASON=""
    fallback_reason=""

    case "$FAST_CHANGED_COMPILE_LIMIT" in
        ''|*[!0-9]*)
            fail "ZCL_FAST_CHANGED_COMPILE_LIMIT must be a non-negative integer (got ${FAST_CHANGED_COMPILE_LIMIT:-empty})"
            ;;
    esac

    while IFS= read -r file; do
        [ -n "$file" ] || continue

        if is_direct_dependency_compile_change "$file"; then
            if [ ! -f "$file" ]; then
                fallback_reason="removed or moved dependency: $file"
                break
            fi
            if ! add_dependent_dev_objects "$file"; then
                fallback_reason="dependency depfiles unavailable for: $file"
                break
            fi
            continue
        fi

        if is_graph_wide_compile_change "$file"; then
            fallback_reason="graph-wide change: $file"
            break
        fi

        case "$file" in
            *.c)
                if [ ! -f "$file" ]; then
                    fallback_reason="removed or moved C source: $file"
                    break
                fi
                if is_node_c_source "$file"; then
                    add_direct_dev_object "$file"
                fi
                ;;
            *)
                if is_code_like_change "$file"; then
                    fallback_reason="non-C code/config change: $file"
                    break
                fi
                ;;
        esac
    done <<EOF
$(changed_files | sort -u)
EOF

    if [ -n "$fallback_reason" ]; then
        COMPILE_PLAN_KIND="fast_compile_fallback"
        COMPILE_PLAN_TARGET="fast-compile"
        COMPILE_PLAN_DETAIL="fallback to fast-compile"
        COMPILE_PLAN_FALLBACK_REASON="$fallback_reason"
        return
    fi

    if [ "$FAST_CHANGED_COMPILE_LIMIT" -gt 0 ] &&
       [ "$DIRECT_DEV_OBJECT_COUNT" -gt "$FAST_CHANGED_COMPILE_LIMIT" ]; then
        COMPILE_PLAN_KIND="fast_compile_fallback"
        COMPILE_PLAN_TARGET="fast-compile"
        COMPILE_PLAN_DETAIL="fallback to fast-compile"
        COMPILE_PLAN_FALLBACK_REASON="$DIRECT_DEV_OBJECT_COUNT direct dev objects > limit $FAST_CHANGED_COMPILE_LIMIT"
        return
    fi

    if [ -z "$DIRECT_DEV_OBJECTS" ]; then
        COMPILE_PLAN_KIND="already_satisfied"
        COMPILE_PLAN_TARGET="none"
        COMPILE_PLAN_DETAIL="no changed node C sources"
        return
    fi

    COMPILE_PLAN_KIND="direct_dev_objects"
    COMPILE_PLAN_TARGET="$DIRECT_DEV_OBJECTS"
    COMPILE_PLAN_DETAIL="direct dev object compile"
}

compile_changed_gate() {
    compute_changed_compile_plan

    case "$COMPILE_PLAN_KIND" in
        fast_compile_fallback)
            log "fast-changed-compile: fallback to fast-compile ($COMPILE_PLAN_FALLBACK_REASON)"
            make_fast fast-compile
            ;;
        already_satisfied)
            log "fast-changed-compile: no changed node C sources; compile gate is already satisfied"
            ;;
        direct_dev_objects)
            log "fast-changed-compile: direct dev object compile count=$DIRECT_DEV_OBJECT_COUNT"
            make_fast $DIRECT_DEV_OBJECTS
            ;;
        *)
            fail "internal error: unknown changed compile plan ${COMPILE_PLAN_KIND:-empty}"
            ;;
    esac
}

run_compile_gate() {
    local target
    case "$FAST_COMPILE" in
        changed|changed-dev|auto)
            compile_changed_gate
            return
            ;;
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

run_dev_rebuild() {
    local start end size

    start="$(date +%s)"
    compile_changed_gate
    log "dev-bin link target=$DEV_NODE_BIN"
    make_fast "$DEV_NODE_BIN"
    [ -x "$DEV_NODE_BIN" ] ||
        fail "dev rebuild did not produce executable $DEV_NODE_BIN"

    end="$(date +%s)"
    size="$(stat -c '%s' "$DEV_NODE_BIN" 2>/dev/null || echo unknown)"
    log "PASS: dev rebuild complete bin=$DEV_NODE_BIN size=$size elapsed_s=$((end - start))"
    log "Use $DEV_NODE_BIN for local iteration; run make zclassic23 or make deploy for release/live."
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

# Dense one-line failure diagnosis for a rung. Priority:
#  (1) first compiler error   file:line:col: error:
#  (2) else, if a "Failed groups:" block is present, the first log=<path>
#      token's first failing assertion line (inline it — the harness only
#      prints the path, so the reader would otherwise have to open the file)
#  (3) else the first line matching error|FAIL
# Always prints exactly one line, prefixed FIRST-ERROR[<label>]: .
first_error_line() {
    local label="$1" output="$2" line log_path
    line="$(printf '%s\n' "$output" | grep -m1 -E ':[0-9]+:[0-9]+: error:' || true)"
    if [ -n "$line" ]; then
        log "FIRST-ERROR[$label]: $line"
        return
    fi
    if printf '%s\n' "$output" | grep -q 'Failed groups:'; then
        log_path="$(printf '%s\n' "$output" |
            grep -m1 -oE 'log=[^[:space:]]+' | sed 's/^log=//' || true)"
        if [ -n "$log_path" ] && [ -f "$log_path" ]; then
            line="$(grep -m1 -E 'FAIL|Assertion|assert|EXPECT' "$log_path" || true)"
            if [ -n "$line" ]; then
                log "FIRST-ERROR[$label]: $line"
                return
            fi
        fi
    fi
    line="$(printf '%s\n' "$output" | grep -m1 -iE 'error|FAIL' || true)"
    log "FIRST-ERROR[$label]: ${line:-<no matching error line>}"
}

# Run one ladder rung, capturing combined output. Always prints the output;
# on non-zero exit prints a dense FIRST-ERROR line then fails (short-circuits
# the ladder). set +e/-e brackets the capture so pipefail does not abort us
# before we can diagnose.
run_rung() {
    local label="$1"
    shift
    local output rc
    set +e
    output="$("$@" 2>&1)"
    rc=$?
    set -e
    printf '%s\n' "$output"
    if [ "$rc" -ne 0 ]; then
        first_error_line "$label" "$output"
        fail "rung $label failed (exit $rc)"
    fi
}

main() {
    local mode="${1:-run}"
    case "$mode" in
        plan|plan-json|doctor-json)
            emit_plan_json
            return
            ;;
    esac

    log "schema=$SCHEMA"
    select_compiler
    resolve_fast_jobs
    log "compiler=$FAST_CC cache=$CACHE_TOOL jobs=$FAST_JOBS compile=$FAST_COMPILE"
    validate_changed_files_only

    case "$mode" in
        run|"") ;;
        compile-changed|changed-compile|fast-changed-compile)
            compile_changed_gate
            log "PASS: changed compile gate complete"
            return
            ;;
        test-changed|t-changed|focused-tests)
            # Leanest changed-aware loop: resolve the focused group(s) for the
            # working-tree diff via the impact rules and run ONLY those through
            # the cached non-LTO harness (t-fast). No lint/compile/probe layer,
            # no group name to remember. The compile still happens — t-fast
            # rebuilds the changed TUs before running — so a broken change
            # fails here too.
            select_test_groups
            fail_on_unmapped_code_changes
            if [ -z "$TEST_GROUPS" ]; then
                log "no focused test groups for current diff (docs/tooling only)"
                return
            fi
            run_focused_tests
            log "PASS: focused tests for changed files ($TEST_GROUPS)"
            return
            ;;
        ff)
            # Fail-fast ladder for the edit loop: cost-ordered, short-circuiting,
            # no live probe, no full/LTO build. Order is load-bearing — compile is
            # the cheapest rung and a broken compile poisons test + lint output, so
            # it runs first; focused tests before lint keeps the failure that most
            # often matters at the front.
            log "ff ladder: compile -> focused-tests -> lint-fast (fail-fast; not release CI)"

            # rung 1: compile ONLY the changed TUs (non-LTO, no link).
            run_rung compile compile_changed_gate

            # rung 2: focused test group(s) for the working-tree diff.
            select_test_groups
            fail_on_unmapped_code_changes
            if [ -n "$TEST_GROUPS" ]; then
                run_rung focused-tests run_focused_tests
            else
                log "no focused test groups (docs/tooling only)"
            fi

            # rung 3: fast lint gates.
            run_rung lint-fast make_fast lint-fast

            log "PASS: ff ladder green (compile -> focused-tests -> lint-fast); not release CI"
            return
            ;;
        rebuild-dev|dev-rebuild|fast-rebuild|hot-rebuild)
            run_dev_rebuild
            return
            ;;
        *)
            fail "unknown mode: $mode"
            ;;
    esac

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
    # Compile before lint: a broken compile must surface the compiler error
    # first, not be buried under lint noise. Both still run on green.
    run_compile_gate
    log "lint-fast"
    make_fast lint-fast
    run_focused_tests
    maybe_live_probe

    record_fast_cache_pass
    log "PASS: fast lane complete; not full release CI"
    log "Before pushing main, keep the strict gate: make lint && make build-only && relevant tests; default pre-push runs make pre-push-ci. Full-suite/fuzz/coverage run through make install-quality-linger."
}

main "$@"
