#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Save-driven development loop for the isolated zcl23-dev lane.
#
# This is deliberately a process-reload loop, not shared-library hot reload:
# consensus code keeps one process-wide state model and the persistent dev
# datadir is the reload boundary.  The default activation command is the
# existing dev-only deploy workflow; canonical and soak targets never appear
# in this script.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROOT="${ZCL_DEV_WATCH_ROOT:-$DEFAULT_ROOT}"
MODE="${ZCL_DEV_WATCH_MODE:-deploy}"
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

STOP_REQUESTED=0
CYCLE=0
WORK=""
FALLBACK_LOCK_DIR=""
NEXT_MANIFEST=""
SETTLED_MANIFEST=""

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
  ZCL_DEV_WATCH_MODE=deploy|stage|off  activation after a green rebuild
  ZCL_DEV_WATCH_POLL_MS=500           polling interval without inotifywait
  ZCL_DEV_WATCH_DEBOUNCE_MS=500       quiet window used to coalesce saves
  ZCL_DEV_WATCH_BACKEND=auto|poll|inotify
  ZCL_DEV_WATCH_ONCE=1                run one deterministic cycle and exit
  ZCL_DEV_WATCH_ONCE_FILES='a.c b.h'  exact once-mode paths
  ZCL_DEV_WATCH_ONCE_FILES_FILE=path  newline-delimited once-mode paths
  ZCL_DEV_WATCH_INITIAL=1             check current dirty relevant paths first

Examples:
  make dev-watch
  ZCL_DEV_WATCH_MODE=stage make dev-watch
  ZCL_DEV_WATCH_MODE=off make dev-watch
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

validate_options()
{
    [ -d "$ROOT" ] || fail "repository root does not exist: $ROOT"
    case "$MODE" in
        deploy|stage|off) ;;
        *) fail "ZCL_DEV_WATCH_MODE must be deploy, stage, or off (got $MODE)" ;;
    esac
    case "$BACKEND" in
        auto|poll|inotify) ;;
        *) fail "ZCL_DEV_WATCH_BACKEND must be auto, poll, or inotify (got $BACKEND)" ;;
    esac
    is_uint "$POLL_MS" && [ "$POLL_MS" -gt 0 ] ||
        fail "ZCL_DEV_WATCH_POLL_MS must be a positive integer"
    is_uint "$DEBOUNCE_MS" ||
        fail "ZCL_DEV_WATCH_DEBOUNCE_MS must be a non-negative integer"
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
    case "$MODE" in
        off)
            return 0
            ;;
        stage)
            if [ -n "$STAGE_COMMAND" ]; then
                (cd "$ROOT" && /bin/sh -c "$STAGE_COMMAND")
            else
                (cd "$ROOT" && make --no-print-directory agent-stage-dev)
            fi
            ;;
        deploy)
            if [ -n "$DEPLOY_COMMAND" ]; then
                (cd "$ROOT" && /bin/sh -c "$DEPLOY_COMMAND")
            else
                (cd "$ROOT" && make --no-print-directory agent-deploy-fast)
            fi
            ;;
    esac
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
    local changed_file="$1" expected_manifest="$2" started count
    CYCLE=$((CYCLE + 1))
    started="$(date +%s)"
    count="$(sed '/^$/d' "$changed_file" | wc -l | tr -d ' ')"

    log "cycle=$CYCLE check files=$count mode=$MODE"
    sed 's/^/[dev-watch]   changed: /' "$changed_file"

    export ZCL_FAST_CHANGED_FILES_FILE="$changed_file"
    export ZCL_FAST_CHANGED_FILES_ONLY=1
    export ZCL_FAST_LIVE=0

    if ! run_check_command; then
        log "cycle=$CYCLE rejected phase=check; running dev service was not touched"
        return 1
    fi
    if ! source_still_matches "$expected_manifest"; then
        log "cycle=$CYCLE superseded after checks; coalescing newest tree"
        return 3
    fi

    if ! run_rebuild_command; then
        log "cycle=$CYCLE rejected phase=rebuild; running dev service was not touched"
        return 1
    fi
    if ! source_still_matches "$expected_manifest"; then
        log "cycle=$CYCLE superseded after rebuild; candidate not activated"
        return 3
    fi

    if ! run_activation_command; then
        log "cycle=$CYCLE activation failed mode=$MODE; watcher remains alive"
        return 1
    fi

    case "$MODE" in
        deploy) log "cycle=$CYCLE ready isolated-dev-lane elapsed_s=$(elapsed_seconds "$started")" ;;
        stage) log "cycle=$CYCLE staged-for-next-dev-restart elapsed_s=$(elapsed_seconds "$started")" ;;
        off) log "cycle=$CYCLE green build-only elapsed_s=$(elapsed_seconds "$started")" ;;
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
    local sandbox old new changed expected command_log rc
    sandbox="$(mktemp -d "${TMPDIR:-/tmp}/zcl-dev-watch-selftest.XXXXXX")" ||
        return 1
    ROOT="$sandbox/repo"
    WORK="$sandbox/work"
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
    MODE="deploy"
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

    MODE="off"
    : > "$command_log"
    run_cycle "$changed" "$expected" >/dev/null 2>&1 ||
        selftest_fail "green off cycle rejected" || { rm -rf "$sandbox"; return 1; }
    [ "$(tr '\n' ' ' < "$command_log")" = "check rebuild " ] ||
        selftest_fail "off mode unexpectedly activated a lane" || { rm -rf "$sandbox"; return 1; }

    rm -rf "$sandbox"
    printf '[dev-watch-selftest] PASS: manifest create/modify/delete, failure short-circuit, deploy/stage/off order\n'
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
    log "watching backend=$BACKEND poll_ms=$POLL_MS debounce_ms=$DEBOUNCE_MS mode=$MODE"
    log "activation target is isolated zcl23-dev only; Ctrl-C stops the watcher"

    if is_true "$RUN_INITIAL"; then
        dirty_relevant_paths "$changed"
        [ -s "$changed" ] && pending=2
    fi

    while [ "$STOP_REQUESTED" -eq 0 ]; do
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
trap request_stop INT TERM
trap cleanup EXIT
acquire_single_watcher_lock

if is_true "$RUN_ONCE"; then
    run_once
    exit $?
fi

main_loop
