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

log() {
    printf 'remote_node_update: %s\n' "$*"
}

fail() {
    printf 'remote_node_update: REFUSE: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
usage: tools/scripts/remote_node_update.sh HOST [HOST...]
       ZCL_REMOTE_HOSTS='host1 host2' tools/scripts/remote_node_update.sh
       tools/scripts/remote_node_update.sh self

Safely update zclassic23 source/build state on remote nodes.

Defaults:
  ZCL_REMOTE_DRY_RUN=1            print the plan only
  ZCL_REMOTE_BRANCH=main          refuse any other checked-out branch
  ZCL_REMOTE_MAIN_REF=origin/main refuse non-main remote refs
  ZCL_REMOTE_BUILD=fast-rebuild   cache-aware non-LTO dev binary when enabled
  ZCL_REMOTE_RESTART=0            never restart by default

Useful opt-ins:
  ZCL_REMOTE_DRY_RUN=0
  ZCL_REMOTE_BUILD=release        build build/bin/zclassic23
  ZCL_REMOTE_INSTALL_BIN=/home/rhett/bin/zclassic23
  ZCL_REMOTE_RESTART=1
  ZCL_REMOTE_UNIT=zclassic23-test.service

No Python is required. Canonical restarts remain guarded by
tools/deploy_guard.sh / zcl.agent_deploy_guard.v1.
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

log() {
    printf 'remote_node_update: %s\n' "$*"
}

fail() {
    printf 'remote_node_update: REFUSE: %s\n' "$*" >&2
    exit 1
}

is_true() {
    case "${1:-}" in
        1|true|TRUE|yes|YES|on|ON) return 0 ;;
        *) return 1 ;;
    esac
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

run_build() {
    local build="$1"
    ZCL_REMOTE_ARTIFACT=""
    case "$build" in
        none|status)
            log "build=none"
            ;;
        fast-rebuild|dev)
            log "build_command=make fast-rebuild"
            make fast-rebuild
            ZCL_REMOTE_ARTIFACT="build/bin/zclassic23-dev"
            ;;
        build-only|strict)
            log "build_command=make build-only"
            make build-only
            ;;
        fast-ci)
            log "build_command=ZCL_FAST_LIVE=0 make fast-ci"
            ZCL_FAST_LIVE="${ZCL_FAST_LIVE:-0}" make fast-ci
            ;;
        release|zclassic23)
            log "build_command=make zclassic23"
            make zclassic23
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
    install -d "$parent"
    tmp="${target}.tmp.$$"
    install -m 755 "$artifact" "$tmp"
    mv -f "$tmp" "$target"
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

    ZCL_DEPLOY_GUARD_UNIT="$unit" ./tools/deploy_guard.sh "$action"
    systemctl --user restart "$unit"
    log "restarted_unit=$unit guard_action=$action"
}

main() {
    local repo branch expect_branch main_ref build dry_run allow_dirty
    local install_bin unit dirty head origin_head active execstart

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

    git fetch --prune origin main
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
            log "plan=already_current"
        else
            log "plan=fast_forward_then_build"
        fi
        log "dry_run_complete=1"
        exit 0
    fi

    git merge --ff-only origin/main
    run_build "$build"
    install_artifact "$install_bin"
    guarded_restart "$unit"
    log "complete=1 final_head=$(git rev-parse HEAD) active=$(service_active "$unit")"
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
            bash -s
        return
    fi

    ssh_bin="${ZCL_REMOTE_SSH:-ssh}"
    remote_payload | "$ssh_bin" "$host" "$env_prefix bash -s"
}

main() {
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
