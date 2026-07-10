#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Save-driven development loop for the isolated zcl23-dev lane.
#
# The loop classifies every save before activation.  A manifest-eligible,
# stateless app-layer change may use a caller-supplied persistent hot-swap
# transport; every other code change crosses the safe process-reload boundary.
# Canonical and soak targets never appear in an activation command.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROOT="${ZCL_DEV_WATCH_ROOT:-$DEFAULT_ROOT}"
MODE="${ZCL_DEV_WATCH_MODE:-auto}"
POLL_MS="${ZCL_DEV_WATCH_POLL_MS:-500}"
DEBOUNCE_MS="${ZCL_DEV_WATCH_DEBOUNCE_MS:-500}"
BACKEND="${ZCL_DEV_WATCH_BACKEND:-auto}"
RUN_ONCE="${ZCL_DEV_WATCH_ONCE:-0}"
RUN_INITIAL="${ZCL_DEV_WATCH_INITIAL:-0}"

# Test/automation seams.  Empty means use the real in-tree command.
CHECK_COMMAND="${ZCL_DEV_WATCH_CHECK_COMMAND:-}"
REBUILD_COMMAND="${ZCL_DEV_WATCH_REBUILD_COMMAND:-}"
DEPLOY_COMMAND="${ZCL_DEV_WATCH_DEPLOY_COMMAND:-}"
STAGE_COMMAND="${ZCL_DEV_WATCH_STAGE_COMMAND:-}"
HOTSWAP_COMMAND="${ZCL_DEV_WATCH_HOTSWAP_COMMAND-$ROOT/tools/dev/hotswap-running-dev.sh}"
HOTSWAP_MANIFEST="${ZCL_DEV_WATCH_HOTSWAP_MANIFEST:-$ROOT/config/hotswap_eligible.def}"
STATE_DIR="${ZCL_DEV_WATCH_STATE_DIR:-$HOME/.local/state/zclassic23-dev}"

STOP_REQUESTED=0
CYCLE=0
WORK=""
FALLBACK_LOCK_DIR=""
NEXT_MANIFEST=""
SETTLED_MANIFEST=""
SELECTED_PATH=""
SELECTION_REASON=""
IMPACT_PLAN=""
HEARTBEAT="$STATE_DIR/watcher-heartbeat.json"
HOTSWAP_RESULT_FILE="${ZCL_DEV_HOTSWAP_RESULT:-$STATE_DIR/hotswap-latest.json}"
LAST_CYCLE_RECORD=""
LAST_OUTCOME="starting"

# Per-cycle evidence populated by run_cycle and serialized once on every
# verdict.  Millisecond timings are wall-clock feedback metrics, never
# consensus time.
CYCLE_ID=""
CYCLE_STARTED_MS=0
CLASSIFY_MS=0
CHECK_MS=0
LINK_MS=0
ACTIVATE_MS=0
CANDIDATE_GENERATION=""
RUNNING_GENERATION=""
LAST_GOOD_GENERATION=""
ROLLBACK_STATUS="not_needed"
FAILURE_PHASE=""
FAILURE_DETAIL=""
AGENT_NEXT_ACTION=""
CHECK_RESULT="not_run"
LINK_RESULT="not_run"
ACTIVATION_RESULT="not_run"

log()
{
    printf '[dev-watch] %s\n' "$*"
}

fail()
{
    printf '[dev-watch] FATAL: %s\n' "$*" >&2
    exit 2
}

usage()
{
    cat <<'EOF'
Usage: tools/dev/watch-dev-lane.sh [--once|--self-test|--help]

Watches C/header/build/tool inputs, coalesces editor saves, runs the existing
fast changed-file checks plus the non-LTO dev rebuild, then optionally updates
ONLY the isolated zcl23-dev lane.

Environment:
  ZCL_DEV_WATCH_MODE=auto|hotswap|reload|stage|check
                                      smallest safe path after green checks
  ZCL_DEV_WATCH_POLL_MS=500           polling interval without inotifywait
  ZCL_DEV_WATCH_DEBOUNCE_MS=500       quiet window used to coalesce saves
  ZCL_DEV_WATCH_BACKEND=auto|poll|inotify
  ZCL_DEV_WATCH_ONCE=1                run one deterministic cycle and exit
  ZCL_DEV_WATCH_ONCE_FILES='a.c b.h'  exact once-mode paths
  ZCL_DEV_WATCH_ONCE_FILES_FILE=path  newline-delimited once-mode paths
  ZCL_DEV_WATCH_INITIAL=1             check current dirty relevant paths first
  ZCL_DEV_WATCH_HOTSWAP_COMMAND=cmd   persistent dev-process swap transport;
                                      defaults to the dev-only RPC bridge;
                                      set empty to force reload fallback
  ZCL_DEV_WATCH_STATE_DIR=path        durable cycles + watcher heartbeat

Examples:
  make dev-watch
  ZCL_DEV_WATCH_MODE=stage make dev-watch
  ZCL_DEV_WATCH_MODE=check make dev-watch
  ZCL_DEV_WATCH_ONCE_FILES=lib/net/src/msg_tx.c make dev-watch-once
EOF
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

clock_ms()
{
    local value
    value="$(date +%s%3N 2>/dev/null || true)"
    if [[ "$value" =~ ^[0-9]+$ ]]; then
        printf '%s' "$value"
    else
        printf '%s000' "$(date +%s)"
    fi
}

json_escape()
{
    # Paths and diagnostics are single-line by construction.  Keep this
    # stock-toolchain implementation independent of jq/python.
    printf '%s' "${1:-}" | sed \
        -e 's/\\/\\\\/g' -e 's/"/\\"/g' \
        -e ':a;N;$!ba;s/\n/\\n/g' -e 's/\r/\\r/g' -e 's/\t/\\t/g'
}

json_array_from_file()
{
    local input="$1" first=1 item
    printf '['
    if [ -r "$input" ]; then
        while IFS= read -r item; do
            [ -n "$item" ] || continue
            [ "$first" -eq 1 ] || printf ','
            printf '"%s"' "$(json_escape "$item")"
            first=0
        done < "$input"
    fi
    printf ']'
}

json_string_field_from_file()
{
    local input="$1" key="$2" token
    [ -r "$input" ] || return 0
    token="$(grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" \
        "$input" 2>/dev/null | head -1 || true)"
    [ -n "$token" ] || return 0
    printf '%s\n' "$token" |
        sed -n "s/^\"${key}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"$/\1/p"
}

write_heartbeat()
{
    local state="${1:-running}" tmp now
    mkdir -p "$STATE_DIR"
    tmp="$(mktemp "$STATE_DIR/.watcher-heartbeat.XXXXXX")" || return 0
    now="$(date -u +%FT%TZ)"
    {
        printf '{"schema":"zcl.dev_watch_heartbeat.v1"'
        printf ',"pid":%s' "$$"
        printf ',"state":"%s"' "$(json_escape "$state")"
        printf ',"updated_at_utc":"%s"' "$(json_escape "$now")"
        printf ',"cycle":%s' "$CYCLE"
        printf ',"current_cycle_id":"%s"' "$(json_escape "$CYCLE_ID")"
        printf ',"last_outcome":"%s"' "$(json_escape "$LAST_OUTCOME")"
        printf ',"last_cycle_record":"%s"' "$(json_escape "$LAST_CYCLE_RECORD")"
        printf '}\n'
    } > "$tmp"
    mv -f "$tmp" "$HEARTBEAT"
}

impact_plan_is_json()
{
    [ -s "$IMPACT_PLAN" ] && grep -q '^[[:space:]]*{' "$IMPACT_PLAN"
}

write_cycle_record()
{
    local changed_file="$1" outcome="$2" total_ms now cycle_dir record tmp latest_tmp
    local persisted_files next_action
    local impact='{"schema":"zcl.agent_fast_plan.v1","status":"unavailable"}'
    local hotswap='{"schema":"zcl.dev_hotswap_result.v1","status":"not_applicable"}'
    local activation_state="${ZCL_DEV_ACTIVATION_RESULT:-$HOME/.zclassic-c23-dev/agent-deploy.json}"

    mkdir -p "$STATE_DIR/cycles" || fail "cannot create cycle state dir: $STATE_DIR/cycles"
    cycle_dir="$STATE_DIR/cycles"
    record="$cycle_dir/$CYCLE_ID.json"
    persisted_files="$cycle_dir/$CYCLE_ID.files"
    cp "$changed_file" "$persisted_files" || fail "cannot persist changed-file list"
    next_action="${AGENT_NEXT_ACTION//$changed_file/$persisted_files}"
    tmp="$(mktemp "$cycle_dir/.cycle.XXXXXX")" || fail "cannot allocate cycle record"
    now="$(date -u +%FT%TZ)"
    total_ms=$(( $(clock_ms) - CYCLE_STARTED_MS ))
    impact_plan_is_json && impact="$(cat "$IMPACT_PLAN")"
    if [ -r "$HOTSWAP_RESULT_FILE" ] &&
       grep -q '"schema"[[:space:]]*:[[:space:]]*"zcl.dev_hotswap_result.v1"' "$HOTSWAP_RESULT_FILE" &&
       [ "$(json_string_field_from_file "$HOTSWAP_RESULT_FILE" cycle_id)" = "$CYCLE_ID" ]; then
        hotswap="$(cat "$HOTSWAP_RESULT_FILE")"
    fi

    CANDIDATE_GENERATION="$(json_string_field_from_file "$activation_state" candidate_generation)"
    RUNNING_GENERATION="$(json_string_field_from_file "$activation_state" running_generation)"
    LAST_GOOD_GENERATION="$(json_string_field_from_file "$activation_state" last_good_generation)"
    ROLLBACK_STATUS="$(json_string_field_from_file "$activation_state" rollback_status)"
    [ -n "$ROLLBACK_STATUS" ] || ROLLBACK_STATUS="not_needed"

    {
        printf '{\n'
        printf '  "schema":"zcl.dev_cycle.v1",\n'
        printf '  "schema_version":1,\n'
        printf '  "cycle_id":"%s",\n' "$(json_escape "$CYCLE_ID")"
        printf '  "captured_at_utc":"%s",\n' "$(json_escape "$now")"
        printf '  "watcher_pid":%s,\n' "$$"
        printf '  "requested_mode":"%s",\n' "$(json_escape "$MODE")"
        printf '  "selected_path":"%s",\n' "$(json_escape "$SELECTED_PATH")"
        printf '  "selection_reason":"%s",\n' "$(json_escape "$SELECTION_REASON")"
        printf '  "outcome":"%s",\n' "$(json_escape "$outcome")"
        printf '  "changed_files":'; json_array_from_file "$changed_file"; printf ',\n'
        printf '  "impact_plan":%s,\n' "$impact"
        printf '  "timings_ms":{"classification":%s,"check":%s,"link":%s,"activation":%s,"total":%s},\n' \
            "$CLASSIFY_MS" "$CHECK_MS" "$LINK_MS" "$ACTIVATE_MS" "$total_ms"
        printf '  "candidate_generation":"%s",\n' "$(json_escape "$CANDIDATE_GENERATION")"
        printf '  "running_generation":"%s",\n' "$(json_escape "$RUNNING_GENERATION")"
        printf '  "last_good_generation":"%s",\n' "$(json_escape "$LAST_GOOD_GENERATION")"
        printf '  "tests":{"mapped_source":"impact_plan.test_groups","focused_status":"%s"},\n' \
            "$(json_escape "$CHECK_RESULT")"
        printf '  "probes":{"check":{"status":"%s","elapsed_ms":%s},' \
            "$(json_escape "$CHECK_RESULT")" "$CHECK_MS"
        printf '"link":{"status":"%s","elapsed_ms":%s},' \
            "$(json_escape "$LINK_RESULT")" "$LINK_MS"
        printf '"activation":{"status":"%s","elapsed_ms":%s}},\n' \
            "$(json_escape "$ACTIVATION_RESULT")" "$ACTIVATE_MS"
        printf '  "hotswap":%s,\n' "$hotswap"
        printf '  "rollback":{"status":"%s"},\n' "$(json_escape "$ROLLBACK_STATUS")"
        printf '  "failure_capsule":{"phase":"%s","detail":"%s","replay_command":"%s"},\n' \
            "$(json_escape "$FAILURE_PHASE")" "$(json_escape "$FAILURE_DETAIL")" \
            "$(json_escape "ZCL_DEV_WATCH_ONCE_FILES_FILE=$persisted_files MODE=$MODE make dev-watch-once")"
        printf '  "agent_next_action":"%s"\n' "$(json_escape "$next_action")"
        printf '}\n'
    } > "$tmp"
    mv -f "$tmp" "$record" || fail "cannot publish cycle record"
    latest_tmp="$(mktemp "$STATE_DIR/.latest-cycle.XXXXXX")" || fail "cannot allocate latest-cycle record"
    cp "$record" "$latest_tmp" || fail "cannot copy latest-cycle record"
    mv -f "$latest_tmp" "$STATE_DIR/latest-cycle.json" || fail "cannot publish latest-cycle record"
    LAST_CYCLE_RECORD="$record"
    LAST_OUTCOME="$outcome"
    write_heartbeat running
}

validate_options()
{
    [ -d "$ROOT" ] || fail "repository root does not exist: $ROOT"
    case "$MODE" in
        auto|hotswap|reload|stage|check) ;;
        deploy) MODE="reload" ;; # compatibility with the bounded watcher MVP
        off) MODE="check" ;;     # compatibility with the bounded watcher MVP
        *) fail "ZCL_DEV_WATCH_MODE must be auto, hotswap, reload, stage, or check (got $MODE)" ;;
    esac
    case "$BACKEND" in
        auto|poll|inotify) ;;
        *) fail "ZCL_DEV_WATCH_BACKEND must be auto, poll, or inotify (got $BACKEND)" ;;
    esac
    is_uint "$POLL_MS" && [ "$POLL_MS" -gt 0 ] ||
        fail "ZCL_DEV_WATCH_POLL_MS must be a positive integer"
    is_uint "$DEBOUNCE_MS" ||
        fail "ZCL_DEV_WATCH_DEBOUNCE_MS must be a non-negative integer"
    case "$STATE_DIR" in
        /*) ;;
        *) fail "ZCL_DEV_WATCH_STATE_DIR must be absolute (got $STATE_DIR)" ;;
    esac
}

sleep_ms()
{
    local milliseconds="$1" seconds
    [ "$milliseconds" -gt 0 ] || return 0
    seconds="$(awk -v ms="$milliseconds" 'BEGIN { printf "%.3f", ms / 1000 }')"
    sleep "$seconds"
}

refresh_watch_paths()
{
    local candidate
    WATCH_PATHS=()
    for candidate in Makefile app application adapters config domain lib ports \
        src tools vendor/include; do
        [ -e "$ROOT/$candidate" ] && WATCH_PATHS+=("$candidate")
    done
    [ "${#WATCH_PATHS[@]}" -gt 0 ] ||
        fail "no watchable source paths found below $ROOT"
}

is_relevant_path()
{
    local path="${1#./}"
    case "$path" in
        Makefile|*.mk) return 0 ;;
        app/*|application/*|adapters/*|config/*|domain/*|lib/*|ports/*|src/*|tools/*|vendor/include/*)
            case "$path" in
                *.c|*.h|*.def|*.inc|*.mk|*.sh|*.py|*.css|*.js|*.html|*.tmpl)
                    return 0
                    ;;
            esac
            ;;
    esac
    return 1
}

# The manifest is cheap enough to poll: one find and one sort, with a SHA-256
# (or POSIX cksum fallback) over sorted path/mtime/size rows.  It intentionally
# does not hash every source file on every 500 ms tick.
write_manifest()
{
    local output="$1" tmp="$1.tmp"
    (
        cd "$ROOT" || exit 1
        find "${WATCH_PATHS[@]}" -type f \
            \( -name 'Makefile' -o -name '*.mk' -o -name '*.c' -o \
               -name '*.h' -o -name '*.def' -o -name '*.inc' -o \
               -name '*.sh' -o -name '*.py' -o -name '*.css' -o \
               -name '*.js' -o -name '*.html' -o -name '*.tmpl' \) \
            -printf '%T@:%s\t%p\n' 2>/dev/null | LC_ALL=C sort
    ) > "$tmp" || {
        rm -f "$tmp"
        return 1
    }
    mv -f "$tmp" "$output"
}

manifest_digest()
{
    local manifest="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$manifest" | awk '{print $1}'
    else
        cksum "$manifest" | awk '{print $1 ":" $2}'
    fi
}

manifests_equal()
{
    [ "$(manifest_digest "$1")" = "$(manifest_digest "$2")" ]
}

manifest_changed_paths()
{
    local old_manifest="$1" new_manifest="$2" output="$3"
    awk -F '\t' '
        FILENAME == ARGV[1] { old[$2] = $1; next }
        {
            current[$2] = $1
            if (!($2 in old) || old[$2] != $1) print $2
        }
        END {
            for (path in old)
                if (!(path in current)) print path
        }
    ' "$old_manifest" "$new_manifest" | LC_ALL=C sort -u > "$output"
}

append_relevant_path()
{
    local path="${1#./}" output="$2"
    [ -n "$path" ] || return 0
    is_relevant_path "$path" && printf '%s\n' "$path" >> "$output"
}

dirty_relevant_paths()
{
    local output="$1" path
    : > "$output"
    if [ -n "${ZCL_DEV_WATCH_ONCE_FILES_FILE:-}" ]; then
        [ -r "$ZCL_DEV_WATCH_ONCE_FILES_FILE" ] ||
            fail "once files list is unreadable: $ZCL_DEV_WATCH_ONCE_FILES_FILE"
        while IFS= read -r path; do
            append_relevant_path "$path" "$output"
        done < "$ZCL_DEV_WATCH_ONCE_FILES_FILE"
    fi
    if [ -n "${ZCL_DEV_WATCH_ONCE_FILES:-}" ]; then
        while IFS= read -r path; do
            append_relevant_path "$path" "$output"
        done < <(printf '%s\n' "$ZCL_DEV_WATCH_ONCE_FILES" | tr ' ,' '\n')
    fi
    if [ ! -s "$output" ] && [ -d "$ROOT/.git" ]; then
        while IFS= read -r path; do
            append_relevant_path "$path" "$output"
        done < <(
            cd "$ROOT" || exit 1
            {
                git diff --name-only HEAD -- 2>/dev/null || true
                git diff --cached --name-only -- 2>/dev/null || true
                git ls-files --others --exclude-standard 2>/dev/null || true
            } | LC_ALL=C sort -u
        )
    fi
    LC_ALL=C sort -u "$output" -o "$output"
}

path_is_hotswap_eligible()
{
    local wanted="$1" entry
    [ -r "$HOTSWAP_MANIFEST" ] || return 1
    while IFS= read -r entry; do
        entry="${entry#*\"}"
        entry="${entry%%\"*}"
        [ "$entry" = "$wanted" ] && return 0
    done < <(grep -E '^[[:space:]]*HOTSWAP_ELIGIBLE\("[^"]+"\)' \
        "$HOTSWAP_MANIFEST" 2>/dev/null || true)
    return 1
}

is_docs_only_path()
{
    case "$1" in
        docs/*|README.md|NOTICE|LICENSE|COPYING) return 0 ;;
        *) return 1 ;;
    esac
}

classify_cycle()
{
    local changed_file="$1" started path count=0 eligible=0 docs=0 blocker=""
    started="$(clock_ms)"
    SELECTED_PATH="reload"
    SELECTION_REASON="reload_required: change is outside the proven stateless hot-swap manifest"

    while IFS= read -r path; do
        [ -n "$path" ] || continue
        count=$((count + 1))
        if path_is_hotswap_eligible "$path"; then
            eligible=$((eligible + 1))
        elif is_docs_only_path "$path"; then
            docs=$((docs + 1))
        elif [ -z "$blocker" ]; then
            case "$path" in
                *.h|*.def|*.inc)
                    blocker="$path: reload_required: dependency fan-out cannot be proven generation-local" ;;
                app/jobs/*|lib/consensus/*|lib/validation/*|lib/storage/*|lib/net/*|lib/coins/*|domain/*|config/*|src/*)
                    blocker="$path: reload_required: consensus, state, wire, or process ownership is non-swappable" ;;
                *) blocker="$path: reload_required: no stateless ABI/quiescence contract" ;;
            esac
        fi
    done < "$changed_file"

    if [ "$count" -gt 0 ] && [ "$docs" -eq "$count" ]; then
        SELECTED_PATH="check"
        SELECTION_REASON="docs_only: verification required; no runtime activation"
    elif [ "$count" -gt 0 ] && [ "$eligible" -eq "$count" ]; then
        if [ -n "$HOTSWAP_COMMAND" ]; then
            SELECTED_PATH="hotswap"
            SELECTION_REASON="all changed translation units are manifest-eligible and a persistent dev-process transport is configured"
        else
            SELECTED_PATH="reload"
            SELECTION_REASON="reload_required: persistent hot-swap transport unavailable; one-shot mcpcall would not update the dev service"
        fi
    elif [ -n "$blocker" ]; then
        SELECTION_REASON="$blocker"
    fi

    case "$MODE" in
        auto) ;;
        hotswap)
            if [ "$eligible" -eq "$count" ] && [ "$count" -gt 0 ] &&
               [ -n "$HOTSWAP_COMMAND" ]; then
                SELECTED_PATH="hotswap"
                SELECTION_REASON="forced hotswap: eligibility and persistent transport proven"
            else
                SELECTED_PATH="rejected"
                SELECTION_REASON="hotswap refused: ${blocker:-persistent transport or manifest eligibility missing}"
            fi
            ;;
        reload) SELECTED_PATH="reload"; SELECTION_REASON="forced transactional process reload" ;;
        stage) SELECTED_PATH="stage"; SELECTION_REASON="forced immutable generation staging without restart" ;;
        check) SELECTED_PATH="check"; SELECTION_REASON="forced verification-only path" ;;
    esac
    CLASSIFY_MS=$(( $(clock_ms) - started ))
}

capture_impact_plan()
{
    IMPACT_PLAN="$WORK/impact-plan.json"
    if ! (cd "$ROOT" && tools/agent_fast_ci.sh plan-json) > "$IMPACT_PLAN" 2>/dev/null; then
        printf '%s\n' '{"schema":"zcl.agent_fast_plan.v1","status":"error","reason":"plan_json_failed"}' > "$IMPACT_PLAN"
    fi
}

resolve_backend()
{
    case "$BACKEND" in
        auto)
            if command -v inotifywait >/dev/null 2>&1; then
                BACKEND="inotify"
            else
                BACKEND="poll"
            fi
            ;;
        inotify)
            command -v inotifywait >/dev/null 2>&1 ||
                fail "inotify backend requested but inotifywait is unavailable"
            ;;
    esac
}

wait_for_possible_change()
{
    local absolute_paths=() path rc
    if [ "$BACKEND" = "poll" ]; then
        sleep_ms "$POLL_MS"
        return 0
    fi

    for path in "${WATCH_PATHS[@]}"; do
        absolute_paths+=("$ROOT/$path")
    done
    inotifywait -q -r -e close_write,create,delete,move,attrib \
        "${absolute_paths[@]}" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 0 ] && [ "$STOP_REQUESTED" -eq 0 ]; then
        log "inotify unavailable after watch start; falling back to polling"
        BACKEND="poll"
        sleep_ms "$POLL_MS"
    fi
}

# Starting with a manifest that is already known to differ, wait until no
# further relevant save arrives for one debounce window.
settle_manifest()
{
    local initial="$1" current="$WORK/settle-current" next="$WORK/settle-next"
    cp "$initial" "$current"
    while [ "$STOP_REQUESTED" -eq 0 ]; do
        sleep_ms "$DEBOUNCE_MS"
        write_manifest "$next" || return 1
        if manifests_equal "$current" "$next"; then
            SETTLED_MANIFEST="$WORK/settled"
            cp "$next" "$SETTLED_MANIFEST"
            return 0
        fi
        cp "$next" "$current"
    done
    return 1
}

wait_for_source_change()
{
    local observed="$1" candidate="$WORK/candidate"
    while [ "$STOP_REQUESTED" -eq 0 ]; do
        wait_for_possible_change
        [ "$STOP_REQUESTED" -eq 0 ] || return 1
        write_manifest "$candidate" || {
            log "manifest read failed; retrying"
            continue
        }
        if ! manifests_equal "$observed" "$candidate"; then
            settle_manifest "$candidate" || return 1
            NEXT_MANIFEST="$SETTLED_MANIFEST"
            return 0
        fi
    done
    return 1
}

run_check_command()
{
    if [ -n "$CHECK_COMMAND" ]; then
        (cd "$ROOT" && /bin/sh -c "$CHECK_COMMAND")
    else
        (cd "$ROOT" && tools/agent_fast_ci.sh)
    fi
}

run_rebuild_command()
{
    if [ -n "$REBUILD_COMMAND" ]; then
        (cd "$ROOT" && /bin/sh -c "$REBUILD_COMMAND")
    else
        (cd "$ROOT" && make --no-print-directory fast-rebuild)
    fi
}

run_activation_command()
{
    case "$SELECTED_PATH" in
        check)
            return 0
            ;;
        stage)
            if [ -n "$STAGE_COMMAND" ]; then
                (cd "$ROOT" && /bin/sh -c "$STAGE_COMMAND")
            else
                (cd "$ROOT" && ZCL_DEV_DEPLOY_BUILD=fast \
                    ZCL_DEV_USE_PREBUILT=1 \
                    ZCL_DEV_BUILD_ARTIFACT="$ROOT/build/bin/zclassic23-dev" \
                    tools/dev/deploy-dev-lane.sh --stage)
            fi
            ;;
        reload)
            if [ -n "$DEPLOY_COMMAND" ]; then
                (cd "$ROOT" && /bin/sh -c "$DEPLOY_COMMAND")
            else
                (cd "$ROOT" && ZCL_DEV_DEPLOY_BUILD=fast \
                    ZCL_DEV_USE_PREBUILT=1 \
                    ZCL_DEV_BUILD_ARTIFACT="$ROOT/build/bin/zclassic23-dev" \
                    tools/dev/deploy-dev-lane.sh)
            fi
            ;;
        hotswap)
            export ZCL_DEV_HOTSWAP_FILES_FILE="$ZCL_FAST_CHANGED_FILES_FILE"
            (cd "$ROOT" && /bin/sh -c "$HOTSWAP_COMMAND")
            ;;
        rejected)
            return 2
            ;;
        *)
            log "internal classifier error: unsupported path=$SELECTED_PATH"
            return 2
            ;;
    esac
}

schedule_async_immutable_build()
{
    local log_path="$STATE_DIR/async-immutable-build.log"
    mkdir -p "$STATE_DIR"
    (
        # Do not let the convergence worker inherit/extend the watcher's
        # singleton flock after the foreground watcher exits. The activation
        # script opens its own independent fd 9 for the generation lock.
        exec 9>&- 2>/dev/null || true
        cd "$ROOT" || exit 1
        make --no-print-directory fast-rebuild > "$log_path" 2>&1 &&
        ZCL_DEV_DEPLOY_BUILD=fast ZCL_DEV_USE_PREBUILT=1 \
            ZCL_DEV_BUILD_ARTIFACT="$ROOT/build/bin/zclassic23-dev" \
            tools/dev/deploy-dev-lane.sh --stage >> "$log_path" 2>&1
    ) &
    log "scheduled async immutable dev binary build+preflight stage pid=$! log=$log_path"
}

source_still_matches()
{
    local expected="$1" probe="$WORK/source-probe"
    write_manifest "$probe" && manifests_equal "$expected" "$probe"
}

elapsed_seconds()
{
    local started="$1" now
    now="$(date +%s)"
    printf '%s' "$((now - started))"
}

# Return: 0 accepted, 1 rejected, 3 superseded by a newer save.
run_cycle()
{
    local changed_file="$1" expected_manifest="$2" started count phase_started rc
    CYCLE=$((CYCLE + 1))
    started="$(date +%s)"
    CYCLE_STARTED_MS="$(clock_ms)"
    CYCLE_ID="${CYCLE_STARTED_MS}-$$-${CYCLE}"
    CLASSIFY_MS=0
    CHECK_MS=0
    LINK_MS=0
    ACTIVATE_MS=0
    FAILURE_PHASE=""
    FAILURE_DETAIL=""
    AGENT_NEXT_ACTION=""
    CHECK_RESULT="not_run"
    LINK_RESULT="not_run"
    ACTIVATION_RESULT="not_run"
    ROLLBACK_STATUS="not_needed"
    LAST_OUTCOME="in_progress"
    write_heartbeat checking
    count="$(sed '/^$/d' "$changed_file" | wc -l | tr -d ' ')"

    export ZCL_FAST_CHANGED_FILES_FILE="$changed_file"
    export ZCL_FAST_CHANGED_FILES_ONLY=1
    export ZCL_FAST_LIVE=0
    export ZCL_DEV_ACTIVATION_RESULT="${ZCL_DEV_ACTIVATION_RESULT:-$HOME/.zclassic-c23-dev/agent-deploy.json}"
    export ZCL_DEV_CYCLE_ID="$CYCLE_ID"
    export ZCL_DEV_HOTSWAP_RESULT="$HOTSWAP_RESULT_FILE"

    capture_impact_plan
    classify_cycle "$changed_file"
    log "cycle=$CYCLE check files=$count mode=$MODE path=$SELECTED_PATH"
    log "cycle=$CYCLE reason=$SELECTION_REASON"
    sed 's/^/[dev-watch]   changed: /' "$changed_file"

    if [ "$SELECTED_PATH" = "rejected" ]; then
        FAILURE_PHASE="classification"
        FAILURE_DETAIL="$SELECTION_REASON"
        AGENT_NEXT_ACTION="MODE=reload ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
        write_cycle_record "$changed_file" rejected
        log "cycle=$CYCLE rejected phase=classification; running dev service was not touched"
        return 1
    fi

    phase_started="$(clock_ms)"
    run_check_command
    rc=$?
    CHECK_MS=$(( $(clock_ms) - phase_started ))
    CHECK_RESULT="$([ "$rc" -eq 0 ] && printf passed || printf failed)"
    if [ "$rc" -ne 0 ]; then
        FAILURE_PHASE="check"
        FAILURE_DETAIL="focused verification failed with exit $rc"
        AGENT_NEXT_ACTION="MODE=check ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
        write_cycle_record "$changed_file" rejected
        log "cycle=$CYCLE rejected phase=check; running dev service was not touched"
        return 1
    fi
    if ! source_still_matches "$expected_manifest"; then
        FAILURE_PHASE="superseded"
        FAILURE_DETAIL="source changed after focused checks"
        AGENT_NEXT_ACTION="MODE=$MODE ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
        write_cycle_record "$changed_file" superseded
        log "cycle=$CYCLE superseded after checks; coalescing newest tree"
        return 3
    fi

    if [ "$SELECTED_PATH" = "reload" ] || [ "$SELECTED_PATH" = "stage" ]; then
        phase_started="$(clock_ms)"
        run_rebuild_command
        rc=$?
        LINK_MS=$(( $(clock_ms) - phase_started ))
        LINK_RESULT="$([ "$rc" -eq 0 ] && printf passed || printf failed)"
        if [ "$rc" -ne 0 ]; then
            FAILURE_PHASE="link"
            FAILURE_DETAIL="dev binary rebuild failed with exit $rc"
            AGENT_NEXT_ACTION="MODE=reload ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
            write_cycle_record "$changed_file" rejected
            log "cycle=$CYCLE rejected phase=rebuild; running dev service was not touched"
            return 1
        fi
        if ! source_still_matches "$expected_manifest"; then
            FAILURE_PHASE="superseded"
            FAILURE_DETAIL="source changed after dev link"
            AGENT_NEXT_ACTION="MODE=$MODE ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
            write_cycle_record "$changed_file" superseded
            log "cycle=$CYCLE superseded after rebuild; candidate not activated"
            return 3
        fi
    fi

    phase_started="$(clock_ms)"
    run_activation_command
    rc=$?
    ACTIVATE_MS=$(( $(clock_ms) - phase_started ))
    if [ "$SELECTED_PATH" = "check" ]; then
        ACTIVATION_RESULT="skipped"
    else
        ACTIVATION_RESULT="$([ "$rc" -eq 0 ] && printf passed || printf failed)"
    fi
    if [ "$rc" -eq 69 ] && [ "$SELECTED_PATH" = "hotswap" ] &&
       [ "$MODE" = "auto" ]; then
        log "cycle=$CYCLE persistent hot-swap transport unavailable; falling back to transactional reload"
        SELECTED_PATH="reload"
        SELECTION_REASON="reload_required: persistent dev-node hot-swap RPC unavailable at runtime"
        phase_started="$(clock_ms)"
        run_rebuild_command
        rc=$?
        LINK_MS=$(( $(clock_ms) - phase_started ))
        LINK_RESULT="$([ "$rc" -eq 0 ] && printf passed || printf failed)"
        if [ "$rc" -ne 0 ]; then
            FAILURE_PHASE="link"
            FAILURE_DETAIL="hot-swap fallback dev rebuild failed with exit $rc"
            AGENT_NEXT_ACTION="MODE=reload ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
            write_cycle_record "$changed_file" rejected
            return 1
        fi
        if ! source_still_matches "$expected_manifest"; then
            FAILURE_PHASE="superseded"
            FAILURE_DETAIL="source changed during hot-swap reload fallback"
            AGENT_NEXT_ACTION="MODE=$MODE ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
            write_cycle_record "$changed_file" superseded
            return 3
        fi
        phase_started="$(clock_ms)"
        run_activation_command
        rc=$?
        ACTIVATE_MS=$(( ACTIVATE_MS + $(clock_ms) - phase_started ))
        ACTIVATION_RESULT="$([ "$rc" -eq 0 ] && printf passed || printf failed)"
    fi
    if [ "$rc" -ne 0 ]; then
        FAILURE_PHASE="activation"
        FAILURE_DETAIL="$SELECTED_PATH activation failed with exit $rc"
        if [ "$rc" -eq 75 ]; then
            AGENT_NEXT_ACTION="MODE=$MODE ZCL_DEV_WATCH_ONCE_FILES_FILE=$changed_file make dev-watch-once"
            write_cycle_record "$changed_file" coalesced
            log "cycle=$CYCLE activation lock busy; coalescing newest tree"
            return 3
        fi
        AGENT_NEXT_ACTION="make agent-dev-status ARGS=--json"
        write_cycle_record "$changed_file" rejected
        log "cycle=$CYCLE activation failed path=$SELECTED_PATH; watcher remains alive"
        return 1
    fi

    AGENT_NEXT_ACTION="make agent-dev-status ARGS=--json"
    write_cycle_record "$changed_file" accepted
    case "$SELECTED_PATH" in
        reload) log "cycle=$CYCLE ready isolated-dev-lane elapsed_s=$(elapsed_seconds "$started")" ;;
        stage) log "cycle=$CYCLE staged-for-next-dev-restart elapsed_s=$(elapsed_seconds "$started")" ;;
        check) log "cycle=$CYCLE green checks-only elapsed_s=$(elapsed_seconds "$started")" ;;
        hotswap)
            log "cycle=$CYCLE committed hot-swap elapsed_s=$(elapsed_seconds "$started")"
            schedule_async_immutable_build
            ;;
    esac
    return 0
}

acquire_single_watcher_lock()
{
    local lock_parent="$ROOT/.cache" lock_file="$ROOT/.cache/zcl-dev-watch.lock"
    mkdir -p "$lock_parent"
    if command -v flock >/dev/null 2>&1; then
        exec 9>"$lock_file"
        flock -n 9 || fail "another dev watcher already owns $lock_file"
        return 0
    fi

    FALLBACK_LOCK_DIR="$lock_file.d"
    if ! mkdir "$FALLBACK_LOCK_DIR" 2>/dev/null; then
        fail "another dev watcher may own $FALLBACK_LOCK_DIR (remove it only if stale)"
    fi
    printf '%s\n' "$$" > "$FALLBACK_LOCK_DIR/pid"
}

cleanup()
{
    LAST_OUTCOME="stopped"
    write_heartbeat stopped
    [ -n "$WORK" ] && rm -rf "$WORK"
    [ -n "$FALLBACK_LOCK_DIR" ] && rm -rf "$FALLBACK_LOCK_DIR"
}

request_stop()
{
    STOP_REQUESTED=1
    log "stop requested; leaving the dev service in its current state"
}

selftest_fail()
{
    printf '[dev-watch-selftest] FAIL: %s\n' "$*" >&2
    return 1
}

self_test()
{
    local sandbox old new changed expected command_log rc latest
    sandbox="$(mktemp -d "${TMPDIR:-/tmp}/zcl-dev-watch-selftest.XXXXXX")" ||
        return 1
    ROOT="$sandbox/repo"
    WORK="$sandbox/work"
    STATE_DIR="$sandbox/state"
    HEARTBEAT="$STATE_DIR/watcher-heartbeat.json"
    mkdir -p "$ROOT/app" "$ROOT/tools/dev" "$WORK"
    printf 'all:\n\t@true\n' > "$ROOT/Makefile"
    printf 'int a;\n' > "$ROOT/app/a.c"
    printf 'int b;\n' > "$ROOT/app/b.h"
    refresh_watch_paths

    old="$WORK/old"
    new="$WORK/new"
    changed="$WORK/changed"
    write_manifest "$old" || { rm -rf "$sandbox"; return 1; }
    printf 'int a = 1;\n' > "$ROOT/app/a.c"
    rm "$ROOT/app/b.h"
    printf '#define C 1\n' > "$ROOT/app/c.h"
    write_manifest "$new" || { rm -rf "$sandbox"; return 1; }
    manifest_changed_paths "$old" "$new" "$changed"
    grep -qx 'app/a.c' "$changed" || selftest_fail "modified path missing" || { rm -rf "$sandbox"; return 1; }
    grep -qx 'app/b.h' "$changed" || selftest_fail "deleted path missing" || { rm -rf "$sandbox"; return 1; }
    grep -qx 'app/c.h' "$changed" || selftest_fail "created path missing" || { rm -rf "$sandbox"; return 1; }

    expected="$new"
    printf 'app/a.c\napp/c.h\n' > "$changed"
    command_log="$sandbox/commands.log"
    CHECK_COMMAND="printf 'check\\n' >> '$command_log'; exit 7"
    REBUILD_COMMAND="printf 'rebuild\\n' >> '$command_log'"
    DEPLOY_COMMAND="printf 'deploy\\n' >> '$command_log'"
    MODE="reload"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 1 ] || selftest_fail "failed check did not reject cycle" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check " ] ||
        selftest_fail "failed check reached rebuild/deploy" || { rm -rf "$sandbox"; return 1; }

    CHECK_COMMAND="printf 'check\\n' >> '$command_log'"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1 ||
        selftest_fail "green deploy cycle rejected" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check rebuild deploy " ] ||
        selftest_fail "green command order is wrong" || { rm -rf "$sandbox"; return 1; }

    STAGE_COMMAND="printf 'stage\\n' >> '$command_log'"
    MODE="stage"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1 ||
        selftest_fail "green stage cycle rejected" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check rebuild stage " ] ||
        selftest_fail "stage command order is wrong" || { rm -rf "$sandbox"; return 1; }

    MODE="check"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1 ||
        selftest_fail "green check cycle rejected" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check " ] ||
        selftest_fail "check mode unexpectedly linked or activated a lane" || { rm -rf "$sandbox"; return 1; }

    # Auto hot-swap is allowed only for exact manifest entries AND only when a
    # persistent-process transport is configured.  It skips the process link,
    # commits one hot-swap command, and writes the authoritative cycle record.
    mkdir -p "$ROOT/config"
    printf '%s\n' 'HOTSWAP_ELIGIBLE("app/a.c")' > "$ROOT/config/hotswap_eligible.def"
    HOTSWAP_MANIFEST="$ROOT/config/hotswap_eligible.def"
    HOTSWAP_COMMAND="printf 'hotswap\\n' >> '$command_log'"
    MODE="auto"
    printf 'app/a.c\n' > "$changed"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1 ||
        selftest_fail "eligible auto hot-swap cycle rejected" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check hotswap " ] ||
        selftest_fail "auto hot-swap command order is wrong" || { rm -rf "$sandbox"; return 1; }
    latest="$STATE_DIR/latest-cycle.json"
    [ -r "$latest" ] && grep -q '"schema":"zcl.dev_cycle.v1"' "$latest" &&
        grep -q '"selected_path":"hotswap"' "$latest" ||
        selftest_fail "durable hot-swap cycle record missing" || { rm -rf "$sandbox"; return 1; }
    if command -v jq >/dev/null 2>&1; then
        jq -e '.schema == "zcl.dev_cycle.v1" and
               (.changed_files | type == "array") and
               (.agent_next_action | type == "string")' "$latest" >/dev/null ||
            selftest_fail "durable cycle record is not valid contract JSON" || { rm -rf "$sandbox"; return 1; }
    fi

    # A runtime that predates the dev-only bridge returns EX_UNAVAILABLE.
    # Auto mode then links and transactionally reloads; a manifest/ABI failure
    # uses a different status and is never hidden this way.
    HOTSWAP_COMMAND="printf 'hotswap\n' >> '$command_log'; exit 69"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1 ||
        selftest_fail "transport-unavailable reload fallback rejected" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check hotswap rebuild deploy " ] ||
        selftest_fail "transport fallback command order is wrong" || { rm -rf "$sandbox"; return 1; }
    grep -q '"selected_path":"reload"' "$STATE_DIR/latest-cycle.json" ||
        selftest_fail "transport fallback cycle did not report reload" || { rm -rf "$sandbox"; return 1; }

    # Mixed/header changes fail closed to a transactional process reload.
    HOTSWAP_COMMAND="printf 'hotswap\n' >> '$command_log'"
    printf 'app/a.c\napp/c.h\n' > "$changed"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1 ||
        selftest_fail "mixed auto reload cycle rejected" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check rebuild deploy " ] ||
        selftest_fail "mixed auto change did not use reload" || { rm -rf "$sandbox"; return 1; }

    rm -rf "$sandbox"
    printf '[dev-watch-selftest] PASS: manifest diff, failure short-circuit, five modes, auto classification, durable cycle JSON\n'
}

run_once()
{
    local manifest="$WORK/once-manifest" changed="$WORK/once-changed" rc
    write_manifest "$manifest" || fail "could not read source manifest"
    dirty_relevant_paths "$changed"
    if [ ! -s "$changed" ]; then
        log "once: no relevant changed paths; nothing to do"
        return 0
    fi
    run_cycle "$changed" "$manifest"
    rc=$?
    return "$rc"
}

main_loop()
{
    local green="$WORK/green" observed="$WORK/observed"
    local changed="$WORK/changed" candidate post rc pending=0

    write_manifest "$green" || fail "could not read initial source manifest"
    cp "$green" "$observed"
    write_heartbeat watching
    log "watching backend=$BACKEND poll_ms=$POLL_MS debounce_ms=$DEBOUNCE_MS mode=$MODE"
    log "activation target is isolated zcl23-dev only; Ctrl-C stops the watcher"

    if is_true "$RUN_INITIAL"; then
        dirty_relevant_paths "$changed"
        [ -s "$changed" ] && pending=2
    fi

    while [ "$STOP_REQUESTED" -eq 0 ]; do
        write_heartbeat watching
        if [ "$pending" -eq 0 ]; then
            wait_for_source_change "$observed" || break
            candidate="$NEXT_MANIFEST"
            cp "$candidate" "$observed"
            manifest_changed_paths "$green" "$candidate" "$changed"
        elif [ "$pending" -eq 1 ]; then
            write_manifest "$WORK/immediate" || {
                log "manifest read failed; retrying"
                sleep_ms "$POLL_MS"
                continue
            }
            settle_manifest "$WORK/immediate" || break
            candidate="$SETTLED_MANIFEST"
            cp "$candidate" "$observed"
            manifest_changed_paths "$green" "$candidate" "$changed"
        else
            candidate="$observed"
        fi
        pending=0

        [ -s "$changed" ] || continue
        run_cycle "$changed" "$candidate"
        rc=$?

        post="$WORK/post"
        if ! write_manifest "$post"; then
            log "post-cycle manifest read failed; polling again"
            pending=1
            continue
        fi
        if ! manifests_equal "$candidate" "$post"; then
            log "new saves arrived during cycle=$CYCLE; scheduling one coalesced follow-up"
            pending=1
            continue
        fi
        if [ "$rc" -eq 0 ]; then
            cp "$candidate" "$green"
        elif [ "$rc" -eq 3 ]; then
            pending=1
        else
            log "waiting for the next save after rejected cycle=$CYCLE"
        fi
    done
    LAST_OUTCOME="stopped"
    write_heartbeat stopped
    log "stopped"
}

case "${1:-}" in
    --help|-h) usage; exit 0 ;;
    --once) RUN_ONCE=1 ;;
    --self-test|--selftest) self_test; exit $? ;;
    "") ;;
    *) usage >&2; exit 2 ;;
esac

validate_options
refresh_watch_paths
resolve_backend
WORK="$(mktemp -d "${TMPDIR:-/tmp}/zcl-dev-watch.XXXXXX")" ||
    fail "could not create temporary workspace"
mkdir -p "$STATE_DIR/cycles"
trap request_stop INT TERM
trap cleanup EXIT
acquire_single_watcher_lock

if is_true "$RUN_ONCE"; then
    run_once
    exit $?
fi

main_loop
