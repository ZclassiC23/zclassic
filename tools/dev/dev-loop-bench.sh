#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Controlled developer-loop latency benchmark. Safe cases run build/check
# paths only. The hot-swap and process-reload cases are skipped unless the
# operator explicitly sets ZCL_DEV_BENCH_ACTIVATE=1.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ZCL_DEV_BENCH_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
OUTPUT="${ZCL_DEV_BENCH_OUTPUT:-$ROOT/.cache/zcl-dev-loop/bench-latest.json}"
ITERATIONS="${ZCL_DEV_BENCH_ITERATIONS:-5}"
WARMUP="${ZCL_DEV_BENCH_WARMUP:-1}"
ACTIVATE="${ZCL_DEV_BENCH_ACTIVATE:-0}"
HOTSWAP_TARGET_MS="${ZCL_DEV_BENCH_HOTSWAP_TARGET_MS:-1000}"
RELOAD_TARGET_MS="${ZCL_DEV_BENCH_RELOAD_TARGET_MS:-8000}"

TMP_DIR=""
OUTPUT_TMP=""
ANY_FAILURE=0

declare -A CASE_COMMAND CASE_STATUS CASE_CONFIGURED CASE_REQUIRES_ACTIVATION
declare -A CASE_P50 CASE_P95 CASE_SUCCESSES CASE_FAILURES CASE_LAST_RC
declare -A CASE_LOG CASE_SAMPLES

CASES=(no_op one_controller one_service one_header hot_swap process_reload)

log()
{
    printf '[dev-loop-bench] %s\n' "$*"
}

fail()
{
    printf '[dev-loop-bench] FATAL: %s\n' "$*" >&2
    exit 2
}

usage()
{
    printf '%s\n' \
        'Usage: tools/dev/dev-loop-bench.sh [--status|--self-test|--help]' \
        '' \
        'Default: benchmark no-op/controller/service/header check+link paths;' \
        'skip hot-swap and process reload. No service is activated by default.' \
        '' \
        'Activation opt-in:' \
        '  ZCL_DEV_BENCH_ACTIVATE=1' \
        '  ZCL_DEV_BENCH_CMD_HOTSWAP=<persistent-process swap command>' \
        '  ZCL_DEV_BENCH_CMD_RELOAD=<transactional reload command>' \
        '' \
        'Other controls: ZCL_DEV_BENCH_ITERATIONS, ZCL_DEV_BENCH_WARMUP,' \
        'ZCL_DEV_BENCH_OUTPUT, and ZCL_DEV_BENCH_CMD_<case> overrides.'
}

is_uint()
{
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

is_true()
{
    case "${1:-}" in
        1|true|yes|on) return 0 ;;
        0|false|no|off|"") return 1 ;;
        *) fail "expected boolean value, got: $1" ;;
    esac
}

json_escape()
{
    printf '%s' "$1" | sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e ':a;N;$!ba;s/\n/\\n/g'
}

cleanup()
{
    [ -n "$OUTPUT_TMP" ] && rm -f "$OUTPUT_TMP"
    [ -n "$TMP_DIR" ] && rm -rf "$TMP_DIR"
}

clock_ns()
{
    local now
    now="$(date +%s%N 2>/dev/null || true)"
    if [[ "$now" =~ ^[0-9]+$ ]]; then
        printf '%s' "$now"
    else
        printf '%s000000000' "$(date +%s)"
    fi
}

default_safe_watch_command()
{
    local path="$1"
    printf '%s' \
        "ZCL_DEV_WATCH_ONCE=1 ZCL_DEV_WATCH_MODE=check ZCL_DEV_WATCH_ONCE_FILES='$path' tools/dev/watch-dev-lane.sh --once"
}

configure_cases()
{
    CASE_COMMAND[no_op]="${ZCL_DEV_BENCH_CMD_NOOP:-:}"
    CASE_COMMAND[one_controller]="${ZCL_DEV_BENCH_CMD_CONTROLLER:-$(
        default_safe_watch_command app/controllers/src/agent_controller.c)}"
    CASE_COMMAND[one_service]="${ZCL_DEV_BENCH_CMD_SERVICE:-$(
        default_safe_watch_command app/services/src/block_source_policy.c)}"
    CASE_COMMAND[one_header]="${ZCL_DEV_BENCH_CMD_HEADER:-$(
        default_safe_watch_command lib/json/include/json/json.h)}"
    CASE_COMMAND[hot_swap]="${ZCL_DEV_BENCH_CMD_HOTSWAP:-make --no-print-directory hotswap FILES=tools/mcp/controllers/app_controller.c PROBE=zcl_name_list}"
    CASE_COMMAND[process_reload]="${ZCL_DEV_BENCH_CMD_RELOAD:-$(
        default_safe_watch_command app/controllers/src/agent_controller.c |
            sed 's/ZCL_DEV_WATCH_MODE=check/ZCL_DEV_WATCH_MODE=reload/')}"

    CASE_CONFIGURED[no_op]=true
    CASE_CONFIGURED[one_controller]=true
    CASE_CONFIGURED[one_service]=true
    CASE_CONFIGURED[one_header]=true
    CASE_CONFIGURED[hot_swap]=true
    CASE_CONFIGURED[process_reload]=true

    CASE_REQUIRES_ACTIVATION[no_op]=false
    CASE_REQUIRES_ACTIVATION[one_controller]=false
    CASE_REQUIRES_ACTIVATION[one_service]=false
    CASE_REQUIRES_ACTIVATION[one_header]=false
    CASE_REQUIRES_ACTIVATION[hot_swap]=true
    CASE_REQUIRES_ACTIVATION[process_reload]=true
}

percentile_nearest_rank()
{
    local samples="$1" percent="$2" count rank
    count="$(sed '/^$/d' "$samples" | wc -l | tr -d ' ')"
    [ "$count" -gt 0 ] || { printf 'null'; return; }
    rank=$(( (count * percent + 99) / 100 ))
    [ "$rank" -gt 0 ] || rank=1
    LC_ALL=C sort -n "$samples" | sed -n "${rank}p"
}

run_case()
{
    local name="$1" command="${CASE_COMMAND[$1]}" total run start end elapsed rc
    local samples="$TMP_DIR/$name.samples" case_log
    local successes=0 failures=0 last_rc=0
    case_log="$(dirname "$OUTPUT")/logs/$name.log"
    mkdir -p "$(dirname "$case_log")"
    : > "$case_log"
    : > "$samples"
    CASE_LOG[$name]="$case_log"
    CASE_SAMPLES[$name]="$samples"

    if [ "${CASE_CONFIGURED[$name]}" != true ]; then
        CASE_STATUS[$name]="not_configured"
        CASE_P50[$name]=null
        CASE_P95[$name]=null
        CASE_SUCCESSES[$name]=0
        CASE_FAILURES[$name]=0
        CASE_LAST_RC[$name]=0
        return
    fi
    if [ "${CASE_REQUIRES_ACTIVATION[$name]}" = true ] &&
       ! is_true "$ACTIVATE"; then
        CASE_STATUS[$name]="skipped_activation_not_opted_in"
        CASE_P50[$name]=null
        CASE_P95[$name]=null
        CASE_SUCCESSES[$name]=0
        CASE_FAILURES[$name]=0
        CASE_LAST_RC[$name]=0
        return
    fi

    total=$((WARMUP + ITERATIONS))
    log "case=$name warmup=$WARMUP measured=$ITERATIONS"
    for ((run = 1; run <= total; run++)); do
        start="$(clock_ns)"
        (cd "$ROOT" && /bin/sh -c "$command") >> "$case_log" 2>&1
        rc=$?
        end="$(clock_ns)"
        elapsed=$(( (end - start + 999999) / 1000000 ))
        last_rc="$rc"
        if [ "$run" -le "$WARMUP" ]; then
            [ "$rc" -eq 0 ] || failures=$((failures + 1))
            continue
        fi
        if [ "$rc" -eq 0 ]; then
            printf '%s\n' "$elapsed" >> "$samples"
            successes=$((successes + 1))
        else
            failures=$((failures + 1))
        fi
    done

    CASE_SUCCESSES[$name]="$successes"
    CASE_FAILURES[$name]="$failures"
    CASE_LAST_RC[$name]="$last_rc"
    CASE_P50[$name]="$(percentile_nearest_rank "$samples" 50)"
    CASE_P95[$name]="$(percentile_nearest_rank "$samples" 95)"
    if [ "$failures" -gt 0 ] || [ "$successes" -ne "$ITERATIONS" ]; then
        CASE_STATUS[$name]="failed"
        ANY_FAILURE=1
    else
        CASE_STATUS[$name]="passed"
    fi
}

write_samples_json()
{
    local file="$1" sep="" value
    printf '['
    while IFS= read -r value; do
        [ -n "$value" ] || continue
        printf '%s%s' "$sep" "$value"
        sep=','
    done < "$file"
    printf ']'
}

slo_case_status()
{
    local name="$1" target="$2" status="${CASE_STATUS[$1]}"
    local p95="${CASE_P95[$1]}"
    if [ "$status" != passed ] || [ "$p95" = null ]; then
        case "$status" in
            failed) printf 'error' ;;
            *) printf 'not_measured' ;;
        esac
    elif [ "$p95" -le "$target" ]; then
        printf 'pass'
    else
        printf 'miss'
    fi
}

emit_artifact()
{
    local destination="$1" tmp generated_at epoch hostname kernel cpu_count
    local commit dirty=false source_fingerprint hot_status reload_status
    local overall_evaluable=false next_action sep="" name
    mkdir -p "$(dirname "$destination")"
    tmp="$(mktemp "$(dirname "$destination")/.bench-latest.json.XXXXXX")" ||
        fail 'could not create atomic benchmark staging file'
    OUTPUT_TMP="$tmp"
    generated_at="$(date -u +%FT%TZ)"
    epoch="$(date +%s)"
    hostname="$(hostname 2>/dev/null || printf unknown)"
    kernel="$(uname -sr 2>/dev/null || printf unknown)"
    cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 0)"
    commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || printf unknown)"
    if ! git -C "$ROOT" diff-index --quiet HEAD -- 2>/dev/null; then
        dirty=true
    fi
    source_fingerprint="$(
        git -C "$ROOT" status --porcelain=v1 --untracked-files=normal 2>/dev/null |
        sha256sum | awk '{print $1}' || printf unavailable)"
    hot_status="$(slo_case_status hot_swap "$HOTSWAP_TARGET_MS")"
    reload_status="$(slo_case_status process_reload "$RELOAD_TARGET_MS")"
    if [ "$hot_status" != not_measured ] &&
       [ "$reload_status" != not_measured ]; then
        overall_evaluable=true
    fi
    if ! is_true "$ACTIVATE"; then
        next_action="rerun with ZCL_DEV_BENCH_ACTIVATE=1 and explicit persistent hot-swap/reload commands before making activation SLO claims"
    elif [ "$hot_status" = miss ] || [ "$reload_status" = miss ]; then
        next_action="inspect the per-case logs and optimize the measured miss"
    elif [ "$hot_status" = pass ] && [ "$reload_status" = pass ]; then
        next_action="retain this artifact as the declared reference-host SLO proof"
    else
        next_action="configure every activation case before making SLO claims"
    fi

    {
        printf '{\n'
        printf '  "schema":"zcl.dev_loop_bench.v1",\n'
        printf '  "status":"%s",\n' \
            "$([ "$ANY_FAILURE" -eq 0 ] && printf ok || printf failed)"
        printf '  "generated_at_utc":"%s",\n' "$(json_escape "$generated_at")"
        printf '  "generated_at_epoch":%s,\n' "$epoch"
        printf '  "artifact":"%s",\n' "$(json_escape "$destination")"
        printf '  "host":{"hostname":"%s","kernel":"%s","cpu_count":%s},\n' \
            "$(json_escape "$hostname")" "$(json_escape "$kernel")" "$cpu_count"
        printf '  "source":{"build_commit":"%s","dirty":%s,"worktree_status_sha256":"%s"},\n' \
            "$(json_escape "$commit")" "$dirty" "$source_fingerprint"
        printf '  "configuration":{"iterations":%s,"warmup":%s,"activation_opt_in":%s},\n' \
            "$ITERATIONS" "$WARMUP" \
            "$(is_true "$ACTIVATE" && printf true || printf false)"
        printf '  "cases":[\n'
        for name in "${CASES[@]}"; do
            printf '%s    {' "$sep"
            printf '"name":"%s",' "$name"
            printf '"requires_activation":%s,' "${CASE_REQUIRES_ACTIVATION[$name]}"
            printf '"configured":%s,' "${CASE_CONFIGURED[$name]}"
            printf '"status":"%s",' "${CASE_STATUS[$name]}"
            printf '"command":"%s",' "$(json_escape "${CASE_COMMAND[$name]}")"
            printf '"successful_samples":%s,' "${CASE_SUCCESSES[$name]}"
            printf '"failures":%s,' "${CASE_FAILURES[$name]}"
            printf '"last_exit_code":%s,' "${CASE_LAST_RC[$name]}"
            printf '"samples_ms":'
            write_samples_json "${CASE_SAMPLES[$name]}"
            printf ',"p50_ms":%s,"p95_ms":%s,' \
                "${CASE_P50[$name]}" "${CASE_P95[$name]}"
            printf '"log":"%s"}' "$(json_escape "${CASE_LOG[$name]}")"
            sep=",\n"
        done
        printf '\n  ],\n'
        printf '  "slo":{"evaluable":%s,' "$overall_evaluable"
        printf '"hot_swap":{"target_p95_ms":%s,"p95_ms":%s,"status":"%s"},' \
            "$HOTSWAP_TARGET_MS" "${CASE_P95[hot_swap]}" "$hot_status"
        printf '"process_reload":{"target_p95_ms":%s,"p95_ms":%s,"status":"%s"}},\n' \
            "$RELOAD_TARGET_MS" "${CASE_P95[process_reload]}" "$reload_status"
        printf '  "agent_next_action":"%s"\n' "$(json_escape "$next_action")"
        printf '}\n'
    } > "$tmp"
    mv -f "$tmp" "$destination"
    OUTPUT_TMP=""
}

run_benchmark()
{
    local name
    [ -d "$ROOT" ] || fail "repository root does not exist: $ROOT"
    is_uint "$ITERATIONS" && [ "$ITERATIONS" -gt 0 ] ||
        fail 'ZCL_DEV_BENCH_ITERATIONS must be a positive integer'
    is_uint "$WARMUP" || fail 'ZCL_DEV_BENCH_WARMUP must be non-negative'
    is_uint "$HOTSWAP_TARGET_MS" && is_uint "$RELOAD_TARGET_MS" ||
        fail 'SLO targets must be non-negative integers'
    is_true "$ACTIVATE" >/dev/null || true

    TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zcl-dev-loop-bench.XXXXXX")" ||
        fail 'could not create temporary workspace'
    trap cleanup EXIT
    mkdir -p "$(dirname "$OUTPUT")/logs"
    configure_cases
    for name in "${CASES[@]}"; do
        run_case "$name"
    done
    emit_artifact "$OUTPUT"
    log "artifact=$OUTPUT status=$([ "$ANY_FAILURE" -eq 0 ] && printf ok || printf failed)"
    return "$ANY_FAILURE"
}

self_test()
{
    local sandbox first second
    sandbox="$(mktemp -d "${TMPDIR:-/tmp}/zcl-dev-loop-selftest.XXXXXX")" ||
        return 1
    ROOT="$sandbox/repo"
    mkdir -p "$ROOT"
    ITERATIONS=3
    WARMUP=1
    ACTIVATE=0
    ZCL_DEV_BENCH_CMD_NOOP=':'
    ZCL_DEV_BENCH_CMD_CONTROLLER=':'
    ZCL_DEV_BENCH_CMD_SERVICE=':'
    ZCL_DEV_BENCH_CMD_HEADER=':'
    ZCL_DEV_BENCH_CMD_HOTSWAP=':'
    ZCL_DEV_BENCH_CMD_RELOAD=':'
    export ZCL_DEV_BENCH_CMD_NOOP ZCL_DEV_BENCH_CMD_CONTROLLER
    export ZCL_DEV_BENCH_CMD_SERVICE ZCL_DEV_BENCH_CMD_HEADER
    export ZCL_DEV_BENCH_CMD_HOTSWAP ZCL_DEV_BENCH_CMD_RELOAD

    first="$sandbox/no-activation.json"
    OUTPUT="$first"
    ANY_FAILURE=0
    run_benchmark >/dev/null || { rm -rf "$sandbox"; return 1; }
    grep -q '"schema":"zcl.dev_loop_bench.v1"' "$first" &&
    grep -q '"name":"hot_swap".*"status":"skipped_activation_not_opted_in"' "$first" &&
    grep -q '"evaluable":false' "$first" || {
        printf '[dev-loop-bench-selftest] FAIL: safe artifact contract\n' >&2
        rm -rf "$sandbox"
        return 1
    }

    cleanup
    TMP_DIR=""
    ACTIVATE=1
    second="$sandbox/activation-opt-in.json"
    OUTPUT="$second"
    ANY_FAILURE=0
    run_benchmark >/dev/null || { rm -rf "$sandbox"; return 1; }
    grep -q '"name":"hot_swap".*"status":"passed"' "$second" &&
    grep -q '"name":"process_reload".*"status":"passed"' "$second" &&
    grep -q '"evaluable":true' "$second" || {
        printf '[dev-loop-bench-selftest] FAIL: opt-in artifact contract\n' >&2
        rm -rf "$sandbox"
        return 1
    }
    rm -rf "$sandbox"
    TMP_DIR=""
    printf '[dev-loop-bench-selftest] PASS: safe skips, opt-in activation, samples, percentiles, JSON artifact\n'
}

emit_status()
{
    if [ -r "$OUTPUT" ]; then
        sed -n '1,$p' "$OUTPUT"
        return 0
    fi
    printf '{"schema":"zcl.dev_loop_bench.v1","status":"missing",'
    printf '"artifact":"%s","slo":{"evaluable":false,' \
        "$(json_escape "$OUTPUT")"
    printf '"hot_swap":{"target_p95_ms":%s,"p95_ms":null,"status":"not_measured"},' \
        "$HOTSWAP_TARGET_MS"
    printf '"process_reload":{"target_p95_ms":%s,"p95_ms":null,"status":"not_measured"}},' \
        "$RELOAD_TARGET_MS"
    printf '"agent_next_action":"run make dev-loop-bench; opt into activation only for declared reference-host SLO proof"}\n'
}

case "${1:-}" in
    --help|-h) usage ;;
    --status) emit_status ;;
    --self-test|--selftest) self_test ;;
    "") run_benchmark ;;
    *) usage >&2; exit 2 ;;
esac
