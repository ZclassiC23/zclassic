#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Background quality lanes for proof work that is valuable but too expensive
# for the synchronous pre-push path. systemd user timers run this script and
# the latest verdict is always available as JSON under ~/.local/state.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

STATE_ROOT="${ZCL_QUALITY_STATE_DIR:-${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/zclassic23-quality}"
STATUS_DIR="$STATE_ROOT/status"
LOG_DIR="$STATE_ROOT/logs"
ARTIFACT_DIR="$STATE_ROOT/artifacts"

mkdir -p "$STATUS_DIR" "$LOG_DIR" "$ARTIFACT_DIR"

utc_now() { date -u '+%Y-%m-%dT%H:%M:%SZ'; }
epoch_now() { date -u '+%s'; }

json_escape() {
    printf '%s' "$1" \
        | sed 's/\\/\\\\/g; s/"/\\"/g; s/	/\\t/g; s/\r/\\r/g' \
        | tr '\n' ' '
}

git_commit() {
    git rev-parse --short=12 HEAD 2>/dev/null || printf 'unknown'
}

write_status() {
    local lane="$1" status="$2" started="$3" finished="$4" elapsed="$5" rc="$6" log="$7" detail="$8"
    local tmp="$STATUS_DIR/${lane}.json.tmp"
    local out="$STATUS_DIR/${lane}.json"
    {
        printf '{'
        printf '"schema":"zcl.background_quality_lane.v1"'
        printf ',"lane":"%s"' "$(json_escape "$lane")"
        printf ',"status":"%s"' "$(json_escape "$status")"
        printf ',"started_at":"%s"' "$(json_escape "$started")"
        printf ',"finished_at":"%s"' "$(json_escape "$finished")"
        printf ',"elapsed_seconds":%s' "$elapsed"
        printf ',"exit_code":%s' "$rc"
        printf ',"commit":"%s"' "$(json_escape "$(git_commit)")"
        printf ',"log":"%s"' "$(json_escape "$log")"
        printf ',"artifacts":"%s"' "$(json_escape "$ARTIFACT_DIR")"
        printf ',"detail":"%s"' "$(json_escape "$detail")"
        printf '}\n'
    } > "$tmp"
    mv "$tmp" "$out"
}

run_logged() {
    local log="$1"
    shift
    set +e
    "$@" 2>&1 | tee -a "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    return "$rc"
}

run_fuzz() {
    local started started_epoch finished finished_epoch elapsed rc log
    local duration timeout leak_detect
    started="$(utc_now)"
    started_epoch="$(epoch_now)"
    log="$LOG_DIR/fuzz-${started//[:]/}.log"
    duration="${ZCL_FUZZ_DURATION:-900}"
    timeout="${ZCL_FUZZ_TIMEOUT:-2}"
    leak_detect="${ZCL_FUZZ_DETECT_LEAKS:-0}"

    write_status "fuzz" "running" "$started" "" 0 0 "$log" "building fuzz targets"
    {
        printf 'background_quality: lane=fuzz started=%s duration_per_target=%s timeout=%s commit=%s\n' \
            "$started" "$duration" "$timeout" "$(git_commit)"
    } | tee -a "$log"

    if ! command -v "${FUZZ_CC:-clang}" >/dev/null 2>&1; then
        finished="$(utc_now)"
        finished_epoch="$(epoch_now)"
        elapsed=$((finished_epoch - started_epoch))
        write_status "fuzz" "skipped" "$started" "$finished" "$elapsed" 0 "$log" "clang not available"
        echo "background_quality: fuzz skipped (clang not available)" | tee -a "$log"
        return 0
    fi

    if run_logged "$log" make fuzz; then
        :
    else
        rc=$?
        finished="$(utc_now)"
        finished_epoch="$(epoch_now)"
        elapsed=$((finished_epoch - started_epoch))
        write_status "fuzz" "failed" "$started" "$finished" "$elapsed" "$rc" "$log" "make fuzz failed"
        return "$rc"
    fi

    rc=0
    for kind in block script p2p http; do
        local bin="build/bin/fuzz_$kind"
        local seed_dir="lib/test/fuzz_seeds/$kind"
        local work_dir
        work_dir="$(mktemp -d "${TMPDIR:-/tmp}/zcl_fuzz_${kind}.XXXXXX")"
        echo "=== $bin (${duration}s background lane) ===" | tee -a "$log"
        set +e
        ASAN_OPTIONS="detect_leaks=$leak_detect" "$bin" \
            "-max_total_time=$duration" \
            "-timeout=$timeout" \
            "-artifact_prefix=$ARTIFACT_DIR/${kind}-" \
            -print_final_stats=1 \
            "$work_dir" "$seed_dir" 2>&1 | tee -a "$log"
        local one_rc=${PIPESTATUS[0]}
        set -e
        rm -rf "$work_dir"
        if [ "$one_rc" -ne 0 ]; then
            rc="$one_rc"
            break
        fi
    done

    finished="$(utc_now)"
    finished_epoch="$(epoch_now)"
    elapsed=$((finished_epoch - started_epoch))
    if [ "$rc" -eq 0 ]; then
        write_status "fuzz" "passed" "$started" "$finished" "$elapsed" 0 "$log" "all fuzz targets completed"
    else
        write_status "fuzz" "failed" "$started" "$finished" "$elapsed" "$rc" "$log" "fuzzer failure or crash artifact emitted"
    fi
    return "$rc"
}

run_coverage() {
    local started started_epoch finished finished_epoch elapsed rc log
    started="$(utc_now)"
    started_epoch="$(epoch_now)"
    log="$LOG_DIR/coverage-${started//[:]/}.log"

    write_status "coverage" "running" "$started" "" 0 0 "$log" "running make coverage"
    printf 'background_quality: lane=coverage started=%s commit=%s\n' \
        "$started" "$(git_commit)" | tee -a "$log"

    if run_logged "$log" make coverage; then
        rc=0
    else
        rc=$?
    fi

    finished="$(utc_now)"
    finished_epoch="$(epoch_now)"
    elapsed=$((finished_epoch - started_epoch))
    if [ "$rc" -eq 0 ]; then
        write_status "coverage" "passed" "$started" "$finished" "$elapsed" 0 "$log" "coverage completed"
    else
        write_status "coverage" "failed" "$started" "$finished" "$elapsed" "$rc" "$log" "make coverage failed"
    fi
    return "$rc"
}

run_tests() {
    local started started_epoch finished finished_epoch elapsed rc log
    started="$(utc_now)"
    started_epoch="$(epoch_now)"
    log="$LOG_DIR/tests-${started//[:]/}.log"

    write_status "tests" "running" "$started" "" 0 0 "$log" "running make test"
    printf 'background_quality: lane=tests started=%s commit=%s\n' \
        "$started" "$(git_commit)" | tee -a "$log"

    if run_logged "$log" make test; then
        rc=0
    else
        rc=$?
    fi

    finished="$(utc_now)"
    finished_epoch="$(epoch_now)"
    elapsed=$((finished_epoch - started_epoch))
    if [ "$rc" -eq 0 ]; then
        write_status "tests" "passed" "$started" "$finished" "$elapsed" 0 "$log" "full test suite completed"
    else
        write_status "tests" "failed" "$started" "$finished" "$elapsed" "$rc" "$log" "make test failed"
    fi
    return "$rc"
}

print_status() {
    local fuzz coverage tests
    fuzz="null"
    coverage="null"
    tests="null"
    [ -f "$STATUS_DIR/fuzz.json" ] && fuzz="$(cat "$STATUS_DIR/fuzz.json")"
    [ -f "$STATUS_DIR/coverage.json" ] && coverage="$(cat "$STATUS_DIR/coverage.json")"
    [ -f "$STATUS_DIR/tests.json" ] && tests="$(cat "$STATUS_DIR/tests.json")"
    printf '{'
    printf '"schema":"zcl.background_quality_status.v1"'
    printf ',"state_dir":"%s"' "$(json_escape "$STATE_ROOT")"
    printf ',"lanes":{"fuzz":%s,"coverage":%s,"tests":%s}' "$fuzz" "$coverage" "$tests"
    printf '}\n'
}

usage() {
    cat <<'EOF'
usage: tools/scripts/background_quality_lane.sh <fuzz|coverage|tests|status>

Environment:
  ZCL_QUALITY_STATE_DIR   status/log root (default ~/.local/state/zclassic23-quality)
  ZCL_FUZZ_DURATION       seconds per fuzz target in background mode (default 900)
  ZCL_FUZZ_TIMEOUT        libFuzzer per-input timeout (default 2)
  ZCL_FUZZ_DETECT_LEAKS   ASAN leak detection for fuzz lane (default 0)
EOF
}

case "${1:-status}" in
    fuzz) run_fuzz ;;
    coverage) run_coverage ;;
    tests) run_tests ;;
    status) print_status ;;
    -h|--help|help) usage ;;
    *)
        usage >&2
        exit 2
        ;;
esac
