#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Safe remote/self update primitive for zclassic23 node fleets.
# Default behavior is observe-only: verify main, fetch origin/main, print the
# fast-forward/build/restart plan, and exit. Installation and restarts are
# explicit opt-ins so this can be used by agents and timers without surprising
# long-running nodes.

set -euo pipefail

SCHEMA="zcl.remote_node_update.v1"

is_true() {
    case "${1:-}" in
        1|true|TRUE|yes|YES|on|ON) return 0 ;;
        *) return 1 ;;
    esac
}

log() {
    if is_true "${ZCL_REMOTE_JSON:-0}"; then
        printf 'remote_node_update: %s\n' "$*" >&2
    else
        printf 'remote_node_update: %s\n' "$*"
    fi
}

fail() {
    printf 'remote_node_update: REFUSE: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
usage: tools/scripts/remote_node_update.sh [--json] HOST [HOST...]
       ZCL_REMOTE_HOSTS='host1 host2' tools/scripts/remote_node_update.sh
       tools/scripts/remote_node_update.sh self

Safely update zclassic23 source/build state on remote nodes.

Defaults:
  ZCL_REMOTE_DRY_RUN=1            print the plan only
  ZCL_REMOTE_BRANCH=main          refuse any other checked-out branch
  ZCL_REMOTE_MAIN_REF=origin/main refuse non-main remote refs
  ZCL_REMOTE_BUILD=fast-rebuild   cache-aware non-LTO dev binary when enabled
  ZCL_REMOTE_RESTART=0            never restart by default
  ZCL_REMOTE_JSON=0               line logs; set 1 or pass --json for JSON

Useful opt-ins:
  --json
  ZCL_REMOTE_DRY_RUN=0
  ZCL_REMOTE_BUILD=release        build build/bin/zclassic23
  ZCL_REMOTE_INSTALL_BIN=/home/rhett/bin/zclassic23
  ZCL_REMOTE_RESTART=1
  ZCL_REMOTE_UNIT=zclassic23-test.service

No Python or jq is required. Canonical restarts remain guarded by
tools/deploy_guard.sh / zcl.agent_deploy_guard.v1. With --json, operational
logs go to stderr and each target emits one zcl.remote_node_update.v1 JSON
object on stdout.
USAGE
}

is_self_host() {
    case "${1:-}" in
        self|local|localhost|127.0.0.1|.) return 0 ;;
        *) return 1 ;;
    esac
}

shell_quote() {
    printf '%q' "$1"
}

ensure_local_main() {
    local root branch
    root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    [ -n "$root" ] || return 0
    branch="$(git -C "$root" symbolic-ref --short HEAD 2>/dev/null || true)"
    [ -z "$branch" ] || [ "$branch" = "main" ] ||
        fail "local checkout must be main before remote updates (got $branch)"
}

remote_payload() {
    cat <<'REMOTE_PAYLOAD'
set -euo pipefail

is_true() {
    case "${1:-}" in
        1|true|TRUE|yes|YES|on|ON) return 0 ;;
        *) return 1 ;;
    esac
}

log() {
    if is_true "${ZCL_REMOTE_JSON:-0}"; then
        printf 'remote_node_update: %s\n' "$*" >&2
    else
        printf 'remote_node_update: %s\n' "$*"
    fi
}

fail() {
    printf 'remote_node_update: REFUSE: %s\n' "$*" >&2
    exit 1
}

run_cmd() {
    if is_true "${ZCL_REMOTE_JSON:-0}"; then
        "$@" >&2
    else
        "$@"
    fi
}

json_escape() {
    local s="${1:-}"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//$'\n'/\\n}"
    s="${s//$'\r'/\\r}"
    s="${s//$'\t'/\\t}"
    printf '%s' "$s"
}

json_bool() {
    if is_true "${1:-0}"; then
        printf 'true'
    else
        printf 'false'
    fi
}

emit_json_summary() {
    local status="$1" plan="$2" updated="$3" build_ran="$4"
    local install_ran="$5" restart_ran="$6" final_head="$7"
    local safe_next_action="$8" host_name json
    is_true "${ZCL_REMOTE_JSON:-0}" || return 0
    host_name="$(hostname -f 2>/dev/null || hostname)"
    json="{\"schema\":\"zcl.remote_node_update.v1\""
    json="${json},\"api_version\":\"v1\""
    json="${json},\"status\":\"$(json_escape "$status")\""
    json="${json},\"host\":\"$(json_escape "$host_name")\""
    json="${json},\"repo\":\"$(json_escape "${repo:-}")\""
    json="${json},\"branch\":\"$(json_escape "${branch:-}")\""
    json="${json},\"head\":\"$(json_escape "${head:-}")\""
    json="${json},\"origin_main\":\"$(json_escape "${origin_head:-}")\""
    json="${json},\"final_head\":\"$(json_escape "$final_head")\""
    json="${json},\"dry_run\":$(json_bool "${dry_run:-0}")"
    json="${json},\"build\":\"$(json_escape "${build:-}")\""
    json="${json},\"install_bin\":\"$(json_escape "${install_bin:-}")\""
    json="${json},\"restart\":$(json_bool "${ZCL_REMOTE_RESTART:-0}")"
    json="${json},\"unit\":\"$(json_escape "${unit:-}")\""
    json="${json},\"unit_active\":\"$(json_escape "${active:-}")\""
    json="${json},\"plan\":\"$(json_escape "$plan")\""
    if [ -n "${ZCL_REMOTE_ERROR:-}" ]; then
        json="${json},\"error\":\"$(json_escape "$ZCL_REMOTE_ERROR")\""
    fi
    json="${json},\"updated\":$(json_bool "$updated")"
    json="${json},\"build_ran\":$(json_bool "$build_ran")"
    json="${json},\"install_ran\":$(json_bool "$install_ran")"
    json="${json},\"restart_ran\":$(json_bool "$restart_ran")"
    json="${json},\"main_only\":true"
    json="${json},\"fast_forward_only\":true"
    json="${json},\"safe_next_action\":\"$(json_escape "$safe_next_action")\"}"
    printf '%s\n' "$json"
}

service_active() {
    local unit="$1"
    [ -n "$unit" ] || return 0
    systemctl --user is-active "$unit" 2>/dev/null || true
}

service_execstart() {
    local unit="$1"
    [ -n "$unit" ] || return 0
    systemctl --user show "$unit" -p ExecStart --value 2>/dev/null |
        sed -n '1p' || true
}

missing_vendor_archives() {
    local missing="" archive
    for archive in libcrypto.a libssl.a libevent.a libevent_openssl.a \
        libevent_pthreads.a libleveldb.a libsqlite3.a libz.a \
        libtor_stub.a; do
        if [ ! -f "vendor/lib/$archive" ]; then
            missing="${missing}${missing:+ }$archive"
        fi
    done
    printf '%s' "$missing"
}

require_tool_for_preflight() {
    local tool="$1" detail="$2"
    if ! command -v "$tool" >/dev/null 2>&1; then
        preflight_error="missing_build_tool:$tool ($detail)"
        return 1
    fi
    return 0
}

preflight_build() {
    local mode="$1" missing
    preflight_error=""
    case "$mode" in
        none|status) return 0 ;;
    esac

    require_tool_for_preflight make "required by build target" || return 1
    require_tool_for_preflight cc "required by C build" || return 1
    require_tool_for_preflight ar "required by vendor archives" || return 1
    require_tool_for_preflight sha256sum "required by vendor verification" ||
        return 1
    require_tool_for_preflight tar "required by vendor extraction" || return 1

    missing="$(missing_vendor_archives)"
    [ -z "$missing" ] && return 0

    if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
        preflight_error="missing_build_tool:curl_or_wget (required for missing vendor archives: $missing)"
        return 1
    fi

    case " $missing " in
        *" libsqlite3.a "*)
            require_tool_for_preflight unzip "required for libsqlite3.a" ||
                return 1
            ;;
    esac
    case " $missing " in
        *" libleveldb.a "*)
            require_tool_for_preflight cmake "required for libleveldb.a" ||
                return 1
            ;;
    esac

    return 0
}

run_build() {
    local build="$1"
    ZCL_REMOTE_ARTIFACT=""
    case "$build" in
        none|status)
            log "build=none"
            ;;
        fast-rebuild|dev)
            log "build_command=make fast-rebuild"
            run_cmd make fast-rebuild || return $?
            ZCL_REMOTE_ARTIFACT="build/bin/zclassic23-dev"
            ;;
        build-only|strict)
            log "build_command=make build-only"
            run_cmd make build-only || return $?
            ;;
        fast-ci)
            log "build_command=ZCL_FAST_LIVE=0 make fast-ci"
            if is_true "${ZCL_REMOTE_JSON:-0}"; then
                ZCL_FAST_LIVE="${ZCL_FAST_LIVE:-0}" make fast-ci >&2 ||
                    return $?
            else
                ZCL_FAST_LIVE="${ZCL_FAST_LIVE:-0}" make fast-ci ||
                    return $?
            fi
            ;;
        release|zclassic23)
            log "build_command=make zclassic23"
            run_cmd make zclassic23 || return $?
            ZCL_REMOTE_ARTIFACT="build/bin/zclassic23"
            ;;
        *)
            fail "unknown ZCL_REMOTE_BUILD=$build"
            ;;
    esac
}

install_artifact() {
    local target="$1" artifact parent tmp
    [ -n "$target" ] || return 0
    artifact="${ZCL_REMOTE_INSTALL_ARTIFACT:-$ZCL_REMOTE_ARTIFACT}"
    [ -n "$artifact" ] ||
        fail "ZCL_REMOTE_INSTALL_BIN requires release/dev build or ZCL_REMOTE_INSTALL_ARTIFACT"
    [ -x "$artifact" ] || fail "install artifact is not executable: $artifact"
    parent="$(dirname "$target")"
    install -d "$parent" || return $?
    tmp="${target}.tmp.$$"
    install -m 755 "$artifact" "$tmp" || return $?
    mv -f "$tmp" "$target" || return $?
    log "installed_artifact=$artifact target=$target"
}

guarded_restart() {
    local unit="$1" action
    is_true "${ZCL_REMOTE_RESTART:-0}" || return 0
    [ -n "$unit" ] || fail "ZCL_REMOTE_RESTART=1 requires ZCL_REMOTE_UNIT"
    [ -x ./tools/deploy_guard.sh ] ||
        fail "missing executable tools/deploy_guard.sh for guarded restart"

    action="${ZCL_REMOTE_GUARD_ACTION:-}"
    if [ -z "$action" ]; then
        case "$unit" in
            zclassic23|zclassic23.service) action="canonical-restart" ;;
            *) action="restart-dev" ;;
        esac
    fi

    ZCL_DEPLOY_GUARD_UNIT="$unit" ./tools/deploy_guard.sh "$action" ||
        return $?
    run_cmd systemctl --user restart "$unit" || return $?
    log "restarted_unit=$unit guard_action=$action"
}

main() {
    local repo branch expect_branch main_ref build dry_run allow_dirty
    local install_bin unit dirty head origin_head active execstart
    local plan updated build_ran install_ran restart_ran final_head
    local preflight_error

    log "schema=zcl.remote_node_update.v1"
    repo="${ZCL_REMOTE_REPO:-$HOME/github/zclassic23}"
    expect_branch="${ZCL_REMOTE_BRANCH:-main}"
    main_ref="${ZCL_REMOTE_MAIN_REF:-origin/main}"
    build="${ZCL_REMOTE_BUILD:-fast-rebuild}"
    dry_run="${ZCL_REMOTE_DRY_RUN:-1}"
    allow_dirty="${ZCL_REMOTE_ALLOW_DIRTY:-0}"
    install_bin="${ZCL_REMOTE_INSTALL_BIN:-}"
    unit="${ZCL_REMOTE_UNIT:-zclassic23-test.service}"

    case "$main_ref" in
        origin/main|refs/remotes/origin/main) ;;
        *) fail "only origin/main may be used for remote node updates (got $main_ref)" ;;
    esac

    [ -d "$repo/.git" ] || fail "repo not found: $repo"
    cd "$repo"

    branch="$(git symbolic-ref --short HEAD 2>/dev/null || true)"
    [ "$branch" = "$expect_branch" ] ||
        fail "remote checkout must be $expect_branch (got ${branch:-detached})"
    [ "$expect_branch" = "main" ] ||
        fail "remote update branch must be main (got $expect_branch)"

    dirty="$(git status --porcelain=v1 --untracked-files=no)"
    if [ -n "$dirty" ] && ! is_true "$allow_dirty"; then
        printf '%s\n' "$dirty" >&2
        fail "remote checkout has tracked local changes; set ZCL_REMOTE_ALLOW_DIRTY=1 only after review"
    fi

    if ! run_cmd git fetch --prune origin main; then
        head="$(git rev-parse HEAD 2>/dev/null || true)"
        origin_head=""
        active="$(service_active "$unit")"
        ZCL_REMOTE_ERROR="git_fetch_failed"
        log "fetch_failed=1"
        emit_json_summary "error" "fetch_failed" 0 0 0 0 "$head" \
            "check_network_or_remote_origin"
        exit 2
    fi
    head="$(git rev-parse HEAD)"
    origin_head="$(git rev-parse origin/main)"
    git merge-base --is-ancestor HEAD origin/main ||
        fail "remote main has local commits not contained in origin/main"

    active="$(service_active "$unit")"
    execstart="$(service_execstart "$unit")"
    log "host=$(hostname -f 2>/dev/null || hostname)"
    log "repo=$repo branch=$branch head=$head origin_main=$origin_head"
    log "dry_run=$dry_run build=$build install_bin=${install_bin:-none} restart=${ZCL_REMOTE_RESTART:-0} unit=${unit:-none} active=${active:-unknown}"
    [ -z "$execstart" ] || log "unit_execstart=$execstart"

    if is_true "$dry_run"; then
        if [ "$head" = "$origin_head" ]; then
            plan="already_current"
        else
            plan="fast_forward_then_build"
        fi
        log "plan=$plan"
        log "dry_run_complete=1"
        emit_json_summary "ok" "$plan" 0 0 0 0 "$head" \
            "dry_run_only_no_install_no_restart"
        exit 0
    fi

    updated=0
    build_ran=0
    install_ran=0
    restart_ran=0
    [ "$head" = "$origin_head" ] || updated=1

    if ! preflight_build "$build"; then
        final_head="$head"
        active="$(service_active "$unit")"
        plan="preflight_failed"
        ZCL_REMOTE_ERROR="$preflight_error"
        log "preflight_failed=$preflight_error"
        emit_json_summary "error" "$plan" "$updated" 0 0 0 "$final_head" \
            "install_missing_build_prerequisites_before_update"
        exit 2
    fi

    if ! run_cmd git merge --ff-only origin/main; then
        final_head="$(git rev-parse HEAD 2>/dev/null || printf '%s' "$head")"
        active="$(service_active "$unit")"
        ZCL_REMOTE_ERROR="git_merge_ff_only_failed"
        log "merge_failed=1"
        emit_json_summary "error" "merge_failed" "$updated" 0 0 0 \
            "$final_head" "inspect_remote_repo_before_retry"
        exit 2
    fi
    if ! run_build "$build"; then
        final_head="$(git rev-parse HEAD 2>/dev/null || printf '%s' "$head")"
        active="$(service_active "$unit")"
        ZCL_REMOTE_ERROR="build_failed:$build"
        log "build_failed=$build"
        emit_json_summary "error" "build_failed" "$updated" 0 0 0 \
            "$final_head" "inspect_remote_build_prerequisites_and_logs"
        exit 2
    fi
    case "$build" in
        none|status) ;;
        *) build_ran=1 ;;
    esac
    if ! install_artifact "$install_bin"; then
        final_head="$(git rev-parse HEAD 2>/dev/null || printf '%s' "$head")"
        active="$(service_active "$unit")"
        ZCL_REMOTE_ERROR="install_failed:$install_bin"
        log "install_failed=$install_bin"
        emit_json_summary "error" "install_failed" "$updated" "$build_ran" \
            0 0 "$final_head" "inspect_install_path_permissions"
        exit 2
    fi
    [ -z "$install_bin" ] || install_ran=1
    if ! guarded_restart "$unit"; then
        final_head="$(git rev-parse HEAD 2>/dev/null || printf '%s' "$head")"
        active="$(service_active "$unit")"
        ZCL_REMOTE_ERROR="restart_failed:$unit"
        log "restart_failed=$unit"
        emit_json_summary "error" "restart_failed" "$updated" "$build_ran" \
            "$install_ran" 0 "$final_head" \
            "inspect_deploy_guard_and_unit_logs"
        exit 2
    fi
    if is_true "${ZCL_REMOTE_RESTART:-0}"; then
        restart_ran=1
    fi
    final_head="$(git rev-parse HEAD)"
    active="$(service_active "$unit")"
    if [ "$updated" = "1" ]; then
        plan="fast_forward_applied"
    else
        plan="already_current"
    fi
    log "complete=1 final_head=$final_head active=$active"
    emit_json_summary "ok" "$plan" "$updated" "$build_ran" \
        "$install_ran" "$restart_ran" "$final_head" \
        "inspect_unit_health_before_promoting_public_ports"
}

main "$@"
REMOTE_PAYLOAD
}

run_one_host() {
    local host="$1" ssh_bin env_prefix
    [ -n "$host" ] || fail "empty host"
    log "target=$host schema=$SCHEMA"

    env_prefix="ZCL_REMOTE_REPO=$(shell_quote "${ZCL_REMOTE_REPO:-}")"
    env_prefix="$env_prefix ZCL_REMOTE_BRANCH=$(shell_quote "${ZCL_REMOTE_BRANCH:-main}")"
    env_prefix="$env_prefix ZCL_REMOTE_MAIN_REF=$(shell_quote "${ZCL_REMOTE_MAIN_REF:-origin/main}")"
    env_prefix="$env_prefix ZCL_REMOTE_BUILD=$(shell_quote "${ZCL_REMOTE_BUILD:-fast-rebuild}")"
    env_prefix="$env_prefix ZCL_REMOTE_DRY_RUN=$(shell_quote "${ZCL_REMOTE_DRY_RUN:-1}")"
    env_prefix="$env_prefix ZCL_REMOTE_ALLOW_DIRTY=$(shell_quote "${ZCL_REMOTE_ALLOW_DIRTY:-0}")"
    env_prefix="$env_prefix ZCL_REMOTE_INSTALL_BIN=$(shell_quote "${ZCL_REMOTE_INSTALL_BIN:-}")"
    env_prefix="$env_prefix ZCL_REMOTE_INSTALL_ARTIFACT=$(shell_quote "${ZCL_REMOTE_INSTALL_ARTIFACT:-}")"
    env_prefix="$env_prefix ZCL_REMOTE_RESTART=$(shell_quote "${ZCL_REMOTE_RESTART:-0}")"
    env_prefix="$env_prefix ZCL_REMOTE_UNIT=$(shell_quote "${ZCL_REMOTE_UNIT:-zclassic23-test.service}")"
    env_prefix="$env_prefix ZCL_REMOTE_GUARD_ACTION=$(shell_quote "${ZCL_REMOTE_GUARD_ACTION:-}")"
    env_prefix="$env_prefix ZCL_REMOTE_JSON=$(shell_quote "${ZCL_REMOTE_JSON:-0}")"

    if is_self_host "$host"; then
        remote_payload | env \
            ZCL_REMOTE_REPO="${ZCL_REMOTE_REPO:-}" \
            ZCL_REMOTE_BRANCH="${ZCL_REMOTE_BRANCH:-main}" \
            ZCL_REMOTE_MAIN_REF="${ZCL_REMOTE_MAIN_REF:-origin/main}" \
            ZCL_REMOTE_BUILD="${ZCL_REMOTE_BUILD:-fast-rebuild}" \
            ZCL_REMOTE_DRY_RUN="${ZCL_REMOTE_DRY_RUN:-1}" \
            ZCL_REMOTE_ALLOW_DIRTY="${ZCL_REMOTE_ALLOW_DIRTY:-0}" \
            ZCL_REMOTE_INSTALL_BIN="${ZCL_REMOTE_INSTALL_BIN:-}" \
            ZCL_REMOTE_INSTALL_ARTIFACT="${ZCL_REMOTE_INSTALL_ARTIFACT:-}" \
            ZCL_REMOTE_RESTART="${ZCL_REMOTE_RESTART:-0}" \
            ZCL_REMOTE_UNIT="${ZCL_REMOTE_UNIT:-zclassic23-test.service}" \
            ZCL_REMOTE_GUARD_ACTION="${ZCL_REMOTE_GUARD_ACTION:-}" \
            ZCL_REMOTE_JSON="${ZCL_REMOTE_JSON:-0}" \
            bash -s
        return
    fi

    ssh_bin="${ZCL_REMOTE_SSH:-ssh}"
    remote_payload | "$ssh_bin" "$host" "$env_prefix bash -s"
}

main() {
    while [ "$#" -gt 0 ]; do
        case "${1:-}" in
            --json)
                ZCL_REMOTE_JSON=1
                export ZCL_REMOTE_JSON
                shift
                ;;
            --)
                shift
                break
                ;;
            *) break ;;
        esac
    done

    if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
        usage
        exit 0
    fi

    ensure_local_main

    if [ "$#" -eq 0 ]; then
        # shellcheck disable=SC2086
        set -- ${ZCL_REMOTE_HOSTS:-}
    fi
    [ "$#" -gt 0 ] || {
        usage >&2
        fail "provide HOST or ZCL_REMOTE_HOSTS"
    }

    local host
    for host in "$@"; do
        run_one_host "$host"
    done
}

main "$@"
