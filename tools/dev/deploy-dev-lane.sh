#!/usr/bin/env bash
# Deploy an immutable zclassic23 generation to the isolated DEV lane.
#
# This script NEVER targets the canonical or soak service/datadir.  A candidate
# is built, copied into an immutable content-addressed generation, and
# preflighted while the old process is still serving.  Activation is one atomic
# `current` symlink flip.  A failed activation restores `last-good`, restarts it
# once, verifies the exact /proc executable identity, and quarantines the
# rejected candidate.
#
# The dev lane remains fully isolated:
#   live node : ~/.zclassic-c23       ports 8033 / 18232
#   soak lane : ~/.zclassic-c23-soak  ports 8043 / 18242
#   DEV lane  : ~/.zclassic-c23-dev   ports 8053 / 18252 (THIS script)
#
# Usage:
#   tools/dev/deploy-dev-lane.sh           # activate transactionally
#   tools/dev/deploy-dev-lane.sh --stage   # build + preflight, no restart
#   tools/dev/deploy-dev-lane.sh --self-test
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

MODE="activate"
case "${1:-}" in
    "") ;;
    --stage) MODE="stage" ;;
    --self-test)
        exec bash "$REPO/tools/dev/deploy-dev-lane-selftest.sh"
        ;;
    --help|-h)
        sed -n '2,24p' "$0"
        exit 0
        ;;
    *) echo "usage: $0 [--stage|--self-test]" >&2; exit 2 ;;
esac

DEV_BIN="$HOME/.local/bin/zclassic23-dev"
DEV_DATADIR="$HOME/.zclassic-c23-dev"
LEGACY_SRC="$HOME/.zclassic"          # read-only bootstrap import source
UNIT="zcl23-dev.service"
DEV_RPCPORT=18252
GEN_ROOT="${ZCL_DEV_GENERATION_ROOT:-$HOME/.local/lib/zclassic23-dev}"
CURRENT_LINK="$GEN_ROOT/current"
LAST_GOOD_LINK="$GEN_ROOT/last-good"
STAGED_LINK="$GEN_ROOT/staged"
LOCK_PATH="$GEN_ROOT/activation.lock"
REJECTED_DIR="$GEN_ROOT/rejected"
NODE_LOG="$DEV_DATADIR/node.log"
AUTO_REINDEX_SENTINEL="$DEV_DATADIR/auto_reindex_request"
STALE_REINDEX_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/reindex.conf"
STALE_OOM_BUDGET_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/zz-oom-budget.conf"
BUILD_ID_DROPIN="$HOME/.config/systemd/user/zcl23-dev.service.d/90-build-identity.conf"
DEPLOY_STATE="$DEV_DATADIR/agent-deploy.json"
DEV_DEPLOY_BUILD="${ZCL_DEV_DEPLOY_BUILD:-fast}"

BUILD_COMMIT=""
BUILD_ARTIFACT="${ZCL_DEV_BUILD_ARTIFACT:-}"
CANDIDATE_GENERATION=""
CANDIDATE_SHA256=""
CANDIDATE_DIR=""
CANDIDATE_BIN=""
PREVIOUS_GENERATION=""
CURRENT_GENERATION=""
LAST_GOOD_GENERATION=""
RUNNING_GENERATION=""
VERIFY_STATUS="started"
VERIFY_DETAIL=""
ACTIVATION_STATUS="preparing"
ROLLBACK_STATUS="not_needed"
FAILURE_CAPSULE=""
LOCK_HELD="false"
ACTIVATION_IN_PROGRESS=0

json_escape() {
    printf '%s' "${1:-}" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g'
}

one_line() {
    printf '%s' "${1:-}" | tr '\r\n' '  '
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

json_first_string_field() {
    local body="$1" key="$2" token
    token="$(printf '%s\n' "$body" \
        | grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" 2>/dev/null \
        | head -1 || true)"
    [ -n "$token" ] || return 0
    printf '%s\n' "$token" \
        | sed -n "s/^\"${key}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"$/\1/p"
}

auto_reindex_status() {
    local anchor="" count=""
    [ -r "$AUTO_REINDEX_SENTINEL" ] || return 1
    read -r anchor count < "$AUTO_REINDEX_SENTINEL" || return 1
    [[ "$anchor" =~ ^-?[0-9]+$ ]] || return 1
    [[ "$count" =~ ^-?[0-9]+$ ]] || return 1
    printf '%s %s\n' "$anchor" "$count"
}

pre_rpc_boot_diagnostic() {
    [ -r "$NODE_LOG" ] || return 0
    tail -n 500 "$NODE_LOG" | awk '
        /crash-only recovery: consuming auto-reindex request/ { recovery=$0 }
        /reindex-chainstate: rebuilding UTXO set/ { reindex=1 }
        /height [0-9]+\/[0-9]+ .*ETA/ { progress=$0 }
        END {
            if (progress != "") print "pre-RPC recovery: reindex-chainstate " progress
            else if (reindex) print "pre-RPC recovery: reindex-chainstate active"
            else if (recovery != "") print "pre-RPC recovery: " recovery
        }'
}

# Compatibility vocabulary retained for the agent/deploy source-contract gate.
# `activation_probe_default` is now the bounded successor to
# `probe_agent_contract`; it validates the full agent and operator-snapshot
# schemas plus exact generation/build identity. The legacy diagnostics named
# `agent_work_ready` and `chain_serving_ready`, controlled by
# `ZCL_DEV_AGENT_TIMEOUT`, formerly printed `AGENT READY`,
# `BLOCKED: agent status=`, `SYNC OK`, and `boot diagnostic: $diag`.

guard_pending_auto_reindex() {
    local status anchor count
    [ -e "$AUTO_REINDEX_SENTINEL" ] || return 0
    if [ ! -r "$AUTO_REINDEX_SENTINEL" ]; then
        echo "[dev-lane] WARN: unreadable auto-reindex marker: $AUTO_REINDEX_SENTINEL"
        return 0
    fi
    status="$(auto_reindex_status || true)"
    if [ -z "$status" ]; then
        echo "[dev-lane] WARN: ignoring malformed auto-reindex marker: $AUTO_REINDEX_SENTINEL"
        return 0
    fi
    anchor="${status%% *}"
    count="${status##* }"
    if [ "$count" = "-1" ]; then
        echo "[dev-lane] NOTE: terminal auto-reindex marker present anchor=$anchor; not a pending rebuild"
        return 0
    fi
    if ! [[ "$count" =~ ^[0-9]+$ ]] || [ "$count" -le 0 ]; then
        echo "[dev-lane] WARN: ignoring malformed auto-reindex marker: $AUTO_REINDEX_SENTINEL"
        return 0
    fi
    if [ "${ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY:-0}" != "1" ]; then
        echo "[dev-lane] BLOCKED: pending crash-only auto-reindex request anchor=$anchor count=$count"
        echo "[dev-lane] BLOCKED: refusing to start or hot-swap the dev lane because boot would consume the marker and rebuild chainstate before RPC is available"
        echo "[dev-lane] BLOCKED: let recovery finish, clear a stale marker only after proving it stale, or set ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1 to force this deploy"
        exit 1
    fi
    echo "[dev-lane] WARN: pending crash-only auto-reindex request anchor=$anchor count=$count; next boot will rebuild chainstate"
}

validate_confinement() {
    local resolved_root canonical soak legacy injected
    case "$GEN_ROOT" in
        /*) ;;
        *) echo "[dev-lane] FATAL: generation root must be absolute: $GEN_ROOT" >&2; exit 2 ;;
    esac
    case "$GEN_ROOT" in
        *'/../'*|*/..|*'\n'*)
            echo "[dev-lane] FATAL: unsafe generation root: $GEN_ROOT" >&2
            exit 2
            ;;
    esac
    canonical="$(readlink -m "$HOME/.zclassic-c23")"
    soak="$(readlink -m "$HOME/.zclassic-c23-soak")"
    legacy="$(readlink -m "$HOME/.zclassic")"
    resolved_root="$(readlink -m "$GEN_ROOT")"
    case "$resolved_root/" in
        "$canonical/"*|"$soak/"*|"$legacy/"*)
            echo "[dev-lane] FATAL: generation root enters a canonical/soak/legacy datadir: $resolved_root" >&2
            exit 2
            ;;
    esac
    [ "$UNIT" = "zcl23-dev.service" ] || {
        echo "[dev-lane] FATAL: refusing non-dev unit: $UNIT" >&2; exit 2; }
    [ "$(readlink -m "$DEV_DATADIR")" = "$(readlink -m "$HOME/.zclassic-c23-dev")" ] || {
        echo "[dev-lane] FATAL: refusing non-dev datadir: $DEV_DATADIR" >&2; exit 2; }
    [ "$DEV_RPCPORT" = "18252" ] || {
        echo "[dev-lane] FATAL: refusing non-dev RPC port: $DEV_RPCPORT" >&2; exit 2; }
    if [ "${ZCL_DEV_ACTIVATION_TEST_MODE:-0}" != "1" ]; then
        for injected in ZCL_DEV_PREFLIGHT_COMMAND ZCL_DEV_STOP_COMMAND \
            ZCL_DEV_START_COMMAND ZCL_DEV_RESET_FAILED_COMMAND \
            ZCL_DEV_DAEMON_RELOAD_COMMAND ZCL_DEV_ACTIVE_COMMAND \
            ZCL_DEV_PID_COMMAND ZCL_DEV_RUNNING_EXE_COMMAND \
            ZCL_DEV_ACTIVATION_PROBE_COMMAND ZCL_DEV_BUILD_COMMIT_OVERRIDE; do
            if [ -n "${!injected:-}" ]; then
                echo "[dev-lane] FATAL: $injected is confined to hermetic activation tests" >&2
                exit 2
            fi
        done
    fi
}

read_generation_link() {
    local link="$1" target
    [ -L "$link" ] || return 1
    target="$(readlink "$link")" || return 1
    case "$target" in
        gen-[0-9a-f]*|legacy-[0-9a-f]*) ;;
        *) return 1 ;;
    esac
    case "$target" in */*) return 1 ;; esac
    [ -x "$GEN_ROOT/$target/zclassic23-dev" ] || return 1
    printf '%s\n' "$target"
}

refresh_generation_state() {
    CURRENT_GENERATION="$(read_generation_link "$CURRENT_LINK" || true)"
    LAST_GOOD_GENERATION="$(read_generation_link "$LAST_GOOD_LINK" || true)"
}

atomic_generation_link() {
    local name="$1" generation="$2" link tmp
    link="$GEN_ROOT/$name"
    case "$generation" in
        gen-[0-9a-f]*|legacy-[0-9a-f]*) ;;
        *) echo "[dev-lane] FATAL: invalid generation id: $generation" >&2; return 1 ;;
    esac
    [ -x "$GEN_ROOT/$generation/zclassic23-dev" ] || return 1
    tmp="$GEN_ROOT/.${name}.$$"
    rm -f "$tmp"
    ln -s "$generation" "$tmp"
    mv -Tf "$tmp" "$link"
}

refresh_compat_link() {
    local tmp
    mkdir -p "$(dirname "$DEV_BIN")"
    tmp="$DEV_BIN.next.$$"
    rm -f "$tmp"
    ln -s "$CURRENT_LINK/zclassic23-dev" "$tmp"
    mv -Tf "$tmp" "$DEV_BIN"
}

generation_build_commit() {
    local generation="$1" manifest
    manifest="$GEN_ROOT/$generation/manifest.json"
    [ -r "$manifest" ] || return 0
    sed -n 's/.*"build_commit"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1
}

write_build_identity_for_generation() {
    local generation="$1" commit
    commit="$(generation_build_commit "$generation")"
    [ -n "$commit" ] || commit="unknown"
    mkdir -p "$(dirname "$BUILD_ID_DROPIN")"
    {
        printf '[Service]\n'
        printf 'Environment="ZCL_AGENT_EXPECT_BUILD_COMMIT=%s"\n' "$commit"
        printf 'Environment="ZCL_AGENT_EXPECT_BUILD_SOURCE=deploy-dev"\n'
    } > "$BUILD_ID_DROPIN"
}

emit_rejected_json() {
    local sep="" marker generation
    printf '['
    if [ -d "$REJECTED_DIR" ]; then
        while IFS= read -r marker; do
            [ -n "$marker" ] || continue
            generation="$(basename "$marker" .json)"
            printf '%s"%s"' "$sep" "$(json_escape "$generation")"
            sep=","
        done < <(find "$REJECTED_DIR" -maxdepth 1 -type f -name 'gen-*.json' -print 2>/dev/null | LC_ALL=C sort)
    fi
    printf ']'
}

write_deploy_state() {
    local verify_status="$1" verify_detail="$2" tmp now status anchor count
    local auto_reindex_pending="false" auto_reindex_anchor="" auto_reindex_count=""
    local rollback_available="false"
    mkdir -p "$DEV_DATADIR"
    refresh_generation_state
    [ -n "$LAST_GOOD_GENERATION" ] && rollback_available="true"
    now="$(date -u +%FT%TZ)"
    status="$(auto_reindex_status || true)"
    if [ -n "$status" ]; then
        anchor="${status%% *}"; count="${status##* }"
        auto_reindex_anchor="$anchor"; auto_reindex_count="$count"
        if [[ "$count" =~ ^[0-9]+$ ]] && [ "$count" -gt 0 ]; then
            auto_reindex_pending="true"
        fi
    fi
    tmp="$(mktemp "$DEV_DATADIR/agent-deploy.json.XXXXXX")" || {
        echo "[dev-lane] FATAL: cannot create deploy-state transaction in $DEV_DATADIR" >&2
        return 1
    }
    {
        printf '{\n'
        printf '  "schema": "zcl.agent_dev_deploy.v1",\n'
        printf '  "deployed_at_utc": "%s",\n' "$(json_escape "$now")"
        printf '  "build_commit": "%s",\n' "$(json_escape "$BUILD_COMMIT")"
        printf '  "build_type": "%s",\n' "$(json_escape "$DEV_DEPLOY_BUILD")"
        printf '  "build_artifact": "%s",\n' "$(json_escape "$BUILD_ARTIFACT")"
        printf '  "installed_binary": "%s",\n' "$(json_escape "$DEV_BIN")"
        printf '  "generation_root": "%s",\n' "$(json_escape "$GEN_ROOT")"
        printf '  "candidate_generation": "%s",\n' "$(json_escape "$CANDIDATE_GENERATION")"
        printf '  "candidate_sha256": "%s",\n' "$(json_escape "$CANDIDATE_SHA256")"
        printf '  "current_generation": "%s",\n' "$(json_escape "$CURRENT_GENERATION")"
        printf '  "running_generation": "%s",\n' "$(json_escape "$RUNNING_GENERATION")"
        printf '  "last_good_generation": "%s",\n' "$(json_escape "$LAST_GOOD_GENERATION")"
        printf '  "previous_generation": "%s",\n' "$(json_escape "$PREVIOUS_GENERATION")"
        printf '  "rollback_available": %s,\n' "$rollback_available"
        printf '  "activation_status": "%s",\n' "$(json_escape "$ACTIVATION_STATUS")"
        printf '  "rollback_status": "%s",\n' "$(json_escape "$ROLLBACK_STATUS")"
        printf '  "activation_lock": "%s",\n' "$(json_escape "$LOCK_PATH")"
        printf '  "activation_lock_held": %s,\n' "$LOCK_HELD"
        printf '  "rejected_generations": '; emit_rejected_json; printf ',\n'
        printf '  "service": "%s",\n' "$UNIT"
        printf '  "datadir": "%s",\n' "$(json_escape "$DEV_DATADIR")"
        printf '  "rpcport": %s,\n' "$DEV_RPCPORT"
        printf '  "verify_status": "%s",\n' "$(json_escape "$verify_status")"
        printf '  "verify_detail": "%s",\n' "$(json_escape "$(one_line "$verify_detail")")"
        printf '  "failure_capsule": "%s",\n' "$(json_escape "$(one_line "$FAILURE_CAPSULE")")"
        printf '  "auto_reindex_pending": %s,\n' "$auto_reindex_pending"
        printf '  "auto_reindex_anchor": "%s",\n' "$(json_escape "$auto_reindex_anchor")"
        printf '  "auto_reindex_count": "%s"\n' "$(json_escape "$auto_reindex_count")"
        printf '}\n'
    } > "$tmp"
    mv "$tmp" "$DEPLOY_STATE"
}

acquire_activation_lock() {
    local owner=""
    mkdir -p "$GEN_ROOT" "$REJECTED_DIR"
    exec 9>>"$LOCK_PATH"
    if ! flock -n 9; then
        owner="$(sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$LOCK_PATH" 2>/dev/null | head -1 || true)"
        echo "[dev-lane] BUSY: another activation owns $LOCK_PATH owner_pid=${owner:-unknown}" >&2
        exit 75
    fi
    LOCK_HELD="true"
    : > "$LOCK_PATH"
    printf '{"schema":"zcl.dev_activation_lock.v1","pid":%s,"acquired_at_utc":"%s","mode":"%s"}\n' \
        "$$" "$(date -u +%FT%TZ)" "$MODE" >&9
}

build_candidate() {
    git update-index -q --refresh >/dev/null 2>&1 || true
    if [ -n "${ZCL_DEV_BUILD_COMMIT_OVERRIDE:-}" ]; then
        BUILD_COMMIT="$ZCL_DEV_BUILD_COMMIT_OVERRIDE"
    else
        BUILD_COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
        if ! git diff-index --quiet HEAD -- 2>/dev/null; then
            BUILD_COMMIT="${BUILD_COMMIT}-dirty"
        fi
    fi

    if [ "${ZCL_DEV_SKIP_BUILD:-0}" = "1" ]; then
        [ "${ZCL_DEV_ACTIVATION_TEST_MODE:-0}" = "1" ] || {
            echo "[dev-lane] FATAL: ZCL_DEV_SKIP_BUILD is test-only" >&2; exit 2; }
        [ -n "$BUILD_ARTIFACT" ] && [ -x "$BUILD_ARTIFACT" ] || {
            echo "[dev-lane] FATAL: test artifact missing/not executable" >&2; exit 2; }
        return 0
    fi

    if [ "${ZCL_DEV_USE_PREBUILT:-0}" = "1" ]; then
        if [ -z "$BUILD_ARTIFACT" ]; then
            case "$DEV_DEPLOY_BUILD" in
                fast) BUILD_ARTIFACT="$REPO/build/bin/zclassic23-dev" ;;
                strict) BUILD_ARTIFACT="$REPO/build/bin/zclassic23" ;;
                *) echo "[dev-lane] FATAL: prebuilt mode requires fast or strict build type" >&2; exit 2 ;;
            esac
        fi
        case "$(readlink -m "$BUILD_ARTIFACT")" in
            "$REPO/build/bin/zclassic23-dev"|"$REPO/build/bin/zclassic23") ;;
            *) echo "[dev-lane] FATAL: trusted prebuilt artifact must be an in-tree build/bin node binary" >&2; exit 2 ;;
        esac
        [ -x "$BUILD_ARTIFACT" ] || {
            echo "[dev-lane] FATAL: trusted prebuilt artifact missing/not executable: $BUILD_ARTIFACT" >&2
            exit 2
        }
        echo "[dev-lane] using trusted prebuilt artifact $BUILD_ARTIFACT; content/build preflight remains mandatory"
        return 0
    fi

    case "$DEV_DEPLOY_BUILD" in
        fast)
            echo "[dev-lane] building fast dev binary (stamp = $BUILD_COMMIT)..."
            make fast-rebuild >/dev/null
            BUILD_ARTIFACT="$REPO/build/bin/zclassic23-dev"
            ;;
        strict)
            echo "[dev-lane] building strict node binary (stamp = $BUILD_COMMIT)..."
            make build/bin/zclassic23 -j"$(nproc)" >/dev/null
            BUILD_ARTIFACT="$REPO/build/bin/zclassic23"
            ;;
        *)
            echo "[dev-lane] FATAL: unknown ZCL_DEV_DEPLOY_BUILD=$DEV_DEPLOY_BUILD (want fast or strict)" >&2
            exit 2
            ;;
    esac
}

write_generation_manifest() {
    local path="$1" generation="$2" sha="$3" source="$4"
    {
        printf '{\n'
        printf '  "schema": "zcl.dev_binary_generation.v1",\n'
        printf '  "generation": "%s",\n' "$generation"
        printf '  "sha256": "%s",\n' "$sha"
        printf '  "build_commit": "%s",\n' "$(json_escape "$BUILD_COMMIT")"
        printf '  "build_type": "%s",\n' "$(json_escape "$DEV_DEPLOY_BUILD")"
        printf '  "source_artifact": "%s",\n' "$(json_escape "$source")"
        printf '  "created_at_utc": "%s"\n' "$(date -u +%FT%TZ)"
        printf '}\n'
    } > "$path"
}

stage_candidate_generation() {
    local tmp
    CANDIDATE_SHA256="$(sha256_file "$BUILD_ARTIFACT")"
    [[ "$CANDIDATE_SHA256" =~ ^[0-9a-fA-F]{64}$ ]] || {
        echo "[dev-lane] FATAL: invalid candidate SHA-256" >&2; return 1; }
    CANDIDATE_SHA256="${CANDIDATE_SHA256,,}"
    CANDIDATE_GENERATION="gen-$CANDIDATE_SHA256"
    CANDIDATE_DIR="$GEN_ROOT/$CANDIDATE_GENERATION"
    CANDIDATE_BIN="$CANDIDATE_DIR/zclassic23-dev"
    if [ -e "$REJECTED_DIR/$CANDIDATE_GENERATION.json" ] &&
       [ "${ZCL_DEV_ALLOW_REJECTED_GENERATION:-0}" != "1" ]; then
        echo "[dev-lane] FATAL: candidate $CANDIDATE_GENERATION is quarantined" >&2
        return 1
    fi
    if [ -d "$CANDIDATE_DIR" ]; then
        [ -x "$CANDIDATE_BIN" ] &&
        [ "$(sha256_file "$CANDIDATE_BIN")" = "$CANDIDATE_SHA256" ] || {
            echo "[dev-lane] FATAL: immutable generation collision: $CANDIDATE_DIR" >&2
            return 1
        }
        return 0
    fi
    tmp="$(mktemp -d "$GEN_ROOT/.candidate.XXXXXX")"
    install -m 555 "$BUILD_ARTIFACT" "$tmp/zclassic23-dev"
    write_generation_manifest "$tmp/manifest.json" "$CANDIDATE_GENERATION" \
        "$CANDIDATE_SHA256" "$BUILD_ARTIFACT"
    chmod 444 "$tmp/manifest.json"
    chmod 555 "$tmp"
    if ! mv "$tmp" "$CANDIDATE_DIR" 2>/dev/null; then
        chmod 755 "$tmp" 2>/dev/null || true
        rm -rf "$tmp"
        [ -x "$CANDIDATE_BIN" ] || return 1
    fi
}

import_existing_binary_as_generation() {
    local existing="$1" sha generation dir tmp commit
    [ -x "$existing" ] || return 1
    sha="$(sha256_file "$existing")"
    generation="legacy-${sha,,}"
    dir="$GEN_ROOT/$generation"
    if [ ! -d "$dir" ]; then
        tmp="$(mktemp -d "$GEN_ROOT/.legacy.XXXXXX")"
        install -m 555 "$existing" "$tmp/zclassic23-dev"
        commit="$($existing agentbuild 2>/dev/null || true)"
        commit="$(json_first_string_field "$commit" build_commit)"
        [ -n "$commit" ] || commit="unknown"
        {
            printf '{"schema":"zcl.dev_binary_generation.v1","generation":"%s",' "$generation"
            printf '"sha256":"%s","build_commit":"%s","build_type":"legacy-import"}\n' \
                "$sha" "$(json_escape "$commit")"
        } > "$tmp/manifest.json"
        chmod 444 "$tmp/manifest.json"
        chmod 555 "$tmp"
        mv "$tmp" "$dir"
    fi
    atomic_generation_link current "$generation"
    atomic_generation_link last-good "$generation"
    refresh_compat_link
    echo "[dev-lane] imported prior installed binary as rollback generation $generation"
}

ensure_rollback_generation() {
    refresh_generation_state
    if [ -n "$CURRENT_GENERATION" ]; then
        refresh_compat_link
        return 0
    fi
    if [ -x "$DEV_BIN" ] && [ ! -L "$DEV_BIN" ]; then
        import_existing_binary_as_generation "$DEV_BIN"
    fi
    refresh_generation_state
}

run_injected_command() {
    local command="$1"
    /bin/sh -c "$command"
}

preflight_candidate_default() {
    local timeout_s="${ZCL_DEV_PREFLIGHT_TIMEOUT:-30}" agentbuild tools selftest observed
    agentbuild="$(timeout "$timeout_s" "$CANDIDATE_BIN" agentbuild 2>&1)" || return 1
    printf '%s' "$agentbuild" | grep -q '"schema"[[:space:]]*:[[:space:]]*"zcl.agent_build.v1"' || return 1
    observed="$(json_first_string_field "$agentbuild" build_commit)"
    if [ -z "$observed" ]; then
        echo "candidate agentbuild omitted build_commit" >&2
        return 1
    fi
    if [ "$observed" != "$BUILD_COMMIT" ]; then
        echo "candidate build identity mismatch: expected=$BUILD_COMMIT observed=$observed" >&2
        return 1
    fi
    tools="$(timeout "$timeout_s" "$CANDIDATE_BIN" \
        -datadir="$DEV_DATADIR" -rpcport="$DEV_RPCPORT" \
        mcpcall zcl_tools_list '{}' 2>&1)" || return 1
    printf '%s' "$tools" | grep -q '"tools"[[:space:]]*:' || return 1
    ! printf '%s' "$tools" | grep -q '"error"[[:space:]]*:' || return 1
    selftest="$(timeout "$timeout_s" "$CANDIDATE_BIN" \
        -datadir="$DEV_DATADIR" -rpcport="$DEV_RPCPORT" \
        mcpcall zcl_self_test '{"mode":"registry"}' 2>&1)" || return 1
    printf '%s' "$selftest" | grep -q '"mode"[[:space:]]*:[[:space:]]*"registry"' || return 1
    printf '%s' "$selftest" | grep -q '"fail"[[:space:]]*:[[:space:]]*0' || return 1
    ! printf '%s' "$selftest" | grep -q '"error"[[:space:]]*:' || return 1
}

preflight_candidate() {
    export CANDIDATE_BIN CANDIDATE_GENERATION CANDIDATE_SHA256 GEN_ROOT
    if [ -n "${ZCL_DEV_PREFLIGHT_COMMAND:-}" ]; then
        run_injected_command "$ZCL_DEV_PREFLIGHT_COMMAND"
    else
        preflight_candidate_default
    fi
}

cleanup_dropins() {
    if [ -f "$STALE_REINDEX_DROPIN" ]; then
        if [ "${ZCL_DEV_ALLOW_REINDEX_DROPIN:-0}" = "1" ]; then
            echo "[dev-lane] preserving explicit reindex drop-in: $STALE_REINDEX_DROPIN"
        else
            echo "[dev-lane] removing stale reindex drop-in: $STALE_REINDEX_DROPIN"
            echo "[dev-lane]   set ZCL_DEV_ALLOW_REINDEX_DROPIN=1 to keep a deliberate reindex override"
            rm -f "$STALE_REINDEX_DROPIN"
        fi
    fi
    if [ -f "$STALE_OOM_BUDGET_DROPIN" ]; then
        if [ "${ZCL_DEV_ALLOW_OOM_BUDGET_DROPIN:-0}" = "1" ]; then
            echo "[dev-lane] preserving explicit memory-budget drop-in: $STALE_OOM_BUDGET_DROPIN"
        else
            echo "[dev-lane] removing stale memory-budget drop-in: $STALE_OOM_BUDGET_DROPIN"
            echo "[dev-lane]   deploy/zcl23-dev.service owns the dev lane memory budget"
            echo "[dev-lane]   set ZCL_DEV_ALLOW_OOM_BUDGET_DROPIN=1 to keep a deliberate override"
            rm -f "$STALE_OOM_BUDGET_DROPIN"
        fi
    fi
}

systemctl_default() {
    timeout "${ZCL_DEV_SYSTEMCTL_TIMEOUT:-90}" systemctl --user "$@"
}

service_stop() {
    if [ -n "${ZCL_DEV_STOP_COMMAND:-}" ]; then run_injected_command "$ZCL_DEV_STOP_COMMAND"
    else systemctl_default stop "$UNIT"; fi
}

service_start() {
    if [ -n "${ZCL_DEV_START_COMMAND:-}" ]; then run_injected_command "$ZCL_DEV_START_COMMAND"
    else systemctl_default start "$UNIT"; fi
}

service_reset_failed() {
    if [ -n "${ZCL_DEV_RESET_FAILED_COMMAND:-}" ]; then run_injected_command "$ZCL_DEV_RESET_FAILED_COMMAND"
    else systemctl --user reset-failed "$UNIT" 2>/dev/null || true; fi
}

service_daemon_reload() {
    if [ -n "${ZCL_DEV_DAEMON_RELOAD_COMMAND:-}" ]; then run_injected_command "$ZCL_DEV_DAEMON_RELOAD_COMMAND"
    else systemctl --user daemon-reload; fi
}

service_active() {
    if [ -n "${ZCL_DEV_ACTIVE_COMMAND:-}" ]; then run_injected_command "$ZCL_DEV_ACTIVE_COMMAND"
    else systemctl --user is-active --quiet "$UNIT"; fi
}

service_pid() {
    if [ -n "${ZCL_DEV_PID_COMMAND:-}" ]; then run_injected_command "$ZCL_DEV_PID_COMMAND"
    else systemctl --user show "$UNIT" -p MainPID --value 2>/dev/null; fi
}

running_executable() {
    local pid="$1"
    if [ -n "${ZCL_DEV_RUNNING_EXE_COMMAND:-}" ]; then
        run_injected_command "$ZCL_DEV_RUNNING_EXE_COMMAND"
    else
        [[ "$pid" =~ ^[0-9]+$ ]] && [ "$pid" -gt 0 ] || return 1
        readlink -f "/proc/$pid/exe"
    fi
}

activation_probe_default() {
    local expected="$1" cli="$REPO/build/bin/zclassic-cli"
    local timeout_s="${ZCL_DEV_PROBE_TIMEOUT:-${ZCL_DEV_AGENT_TIMEOUT:-10}}"
    local height agent operator_snapshot catalog selftest expected_commit observed_commit expected_bin
    expected_bin="$GEN_ROOT/$expected/zclassic23-dev"
    expected_commit="$(generation_build_commit "$expected")"
    [ -n "$expected_commit" ] || return 1
    [ -x "$cli" ] || return 1
    height="$(timeout "$timeout_s" "$cli" -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" getblockcount 2>/dev/null)" || return 1
    [[ "$height" =~ ^[0-9]+$ ]] || return 1
    agent="$(timeout "$timeout_s" "$cli" -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" agent 2>/dev/null)" || return 1
    printf '%s' "$agent" | grep -q '"schema"[[:space:]]*:[[:space:]]*"zcl.public_status.v1"' || return 1
    observed_commit="$(json_first_string_field "$agent" build_commit)"
    [ -n "$observed_commit" ] && [ "$observed_commit" = "$expected_commit" ] || return 1
    operator_snapshot="$(timeout "$timeout_s" "$cli" -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" operatorsnapshot 2>/dev/null)" || return 1
    printf '%s' "$operator_snapshot" | grep -q '"schema"[[:space:]]*:[[:space:]]*"zcl.operator_snapshot.v1"' || return 1
    catalog="$(timeout "$timeout_s" "$expected_bin" -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" mcpcall zcl_tools_list '{}' 2>/dev/null)" || return 1
    printf '%s' "$catalog" | grep -q '"tools"[[:space:]]*:' || return 1
    ! printf '%s' "$catalog" | grep -q '"error"[[:space:]]*:' || return 1
    selftest="$(timeout "$timeout_s" "$expected_bin" -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" mcpcall zcl_self_test \
        '{"mode":"registry"}' 2>/dev/null)" || return 1
    printf '%s' "$selftest" | grep -q '"mode"[[:space:]]*:[[:space:]]*"registry"' || return 1
    printf '%s' "$selftest" | grep -q '"fail"[[:space:]]*:[[:space:]]*0' || return 1
}

activation_probe() {
    export EXPECTED_GENERATION="$1" CANDIDATE_GENERATION GEN_ROOT DEV_DATADIR DEV_RPCPORT
    if [ -n "${ZCL_DEV_ACTIVATION_PROBE_COMMAND:-}" ]; then
        run_injected_command "$ZCL_DEV_ACTIVATION_PROBE_COMMAND"
    else
        activation_probe_default "$1"
    fi
}

verify_running_generation() {
    local expected="$1" expected_bin
    expected_bin="$GEN_ROOT/$expected/zclassic23-dev"
    local deadline now pid exe interval_ms="${ZCL_DEV_PROBE_INTERVAL_MS:-250}"
    local timeout_s="${ZCL_DEV_ACTIVATION_TIMEOUT:-30}"
    deadline=$(( $(date +%s) + timeout_s ))
    RUNNING_GENERATION=""
    while :; do
        if service_active; then
            pid="$(service_pid 2>/dev/null || true)"
            exe="$(running_executable "$pid" 2>/dev/null || true)"
            if [ -n "$exe" ] && [ "$(readlink -m "$exe")" = "$(readlink -m "$expected_bin")" ]; then
                RUNNING_GENERATION="$expected"
                if activation_probe "$expected"; then
                    return 0
                fi
            fi
        fi
        now="$(date +%s)"
        [ "$now" -lt "$deadline" ] || break
        awk -v ms="$interval_ms" 'BEGIN { printf "%.3f", ms / 1000 }' | xargs sleep
    done
    return 1
}

quarantine_candidate() {
    local reason="$1" marker_tmp="$REJECTED_DIR/.${CANDIDATE_GENERATION}.$$"
    mkdir -p "$REJECTED_DIR"
    {
        printf '{"schema":"zcl.dev_rejected_generation.v1",'
        printf '"generation":"%s","sha256":"%s",' "$CANDIDATE_GENERATION" "$CANDIDATE_SHA256"
        printf '"rejected_at_utc":"%s","reason":"%s"}\n' \
            "$(date -u +%FT%TZ)" "$(json_escape "$(one_line "$reason")")"
    } > "$marker_tmp"
    mv "$marker_tmp" "$REJECTED_DIR/$CANDIDATE_GENERATION.json"
}

rollback_to_previous() {
    local reason="$1"
    ROLLBACK_STATUS="started"
    service_stop >/dev/null 2>&1 || true
    if [ -z "$PREVIOUS_GENERATION" ] ||
       ! atomic_generation_link current "$PREVIOUS_GENERATION"; then
        rm -f "$CURRENT_LINK" "$DEV_BIN"
        ROLLBACK_STATUS="unavailable"
        FAILURE_CAPSULE="$reason; no prior generation available"
        ACTIVATION_IN_PROGRESS=0
        return 1
    fi
    refresh_compat_link
    write_build_identity_for_generation "$PREVIOUS_GENERATION"
    service_daemon_reload >/dev/null 2>&1 || true
    service_reset_failed
    if ! service_start; then
        ROLLBACK_STATUS="restart_failed"
        FAILURE_CAPSULE="$reason; last-good restart command failed"
        ACTIVATION_IN_PROGRESS=0
        return 1
    fi
    if verify_running_generation "$PREVIOUS_GENERATION"; then
        atomic_generation_link last-good "$PREVIOUS_GENERATION"
        ROLLBACK_STATUS="verified"
        ACTIVATION_IN_PROGRESS=0
        return 0
    fi
    ROLLBACK_STATUS="verification_failed"
    FAILURE_CAPSULE="$reason; last-good recovery probe failed"
    ACTIVATION_IN_PROGRESS=0
    return 1
}

mark_activation_failed() {
    if [ "$ROLLBACK_STATUS" = "verified" ]; then
        ACTIVATION_STATUS="rolled_back"
    else
        ACTIVATION_STATUS="failed"
    fi
}

emergency_rollback() {
    local rc=$?
    if [ "$ACTIVATION_IN_PROGRESS" -eq 1 ]; then
        set +e
        echo "[dev-lane] WARN: unexpected activation exit; restoring previous generation" >&2
        rollback_to_previous "unexpected activation exit rc=$rc" >/dev/null 2>&1
        ACTIVATION_STATUS="failed"
        VERIFY_STATUS="activation_failed"
        write_deploy_state "$VERIFY_STATUS" "$FAILURE_CAPSULE"
    fi
    return "$rc"
}
trap emergency_rollback EXIT

activate_candidate() {
    local reason
    refresh_generation_state
    PREVIOUS_GENERATION="$CURRENT_GENERATION"
    if [ -n "$PREVIOUS_GENERATION" ]; then
        atomic_generation_link last-good "$PREVIOUS_GENERATION"
    fi

    echo "[dev-lane] activating immutable generation $CANDIDATE_GENERATION"
    if ! service_stop; then
        ACTIVATION_STATUS="stop_failed"
        VERIFY_STATUS="activation_failed"
        VERIFY_DETAIL="could not stop $UNIT within the bounded stop window"
        service_start >/dev/null 2>&1 || true
        return 1
    fi

    atomic_generation_link current "$CANDIDATE_GENERATION"
    refresh_compat_link
    write_build_identity_for_generation "$CANDIDATE_GENERATION"
    ACTIVATION_IN_PROGRESS=1
    service_daemon_reload

    if [ ! -f "$DEV_DATADIR/node.db" ]; then
        echo "[dev-lane] fresh datadir — two-step cold-import bootstrap"
        echo "[dev-lane]   step 1/2: header import from $LEGACY_SRC (read-only, LOCK-safe; never stops zclassicd)"
        if ! "$CANDIDATE_BIN" -datadir="$DEV_DATADIR" --importblockindex "$LEGACY_SRC"; then
            reason="fresh-datadir header import failed"
            quarantine_candidate "$reason"
            rollback_to_previous "$reason" || true
            mark_activation_failed
            VERIFY_STATUS="activation_failed"
            VERIFY_DETAIL="$reason"
            return 1
        fi
        echo "[dev-lane]   step 2/2: enabling+starting the lane (boot auto-imports UTXOs, then syncs)"
    fi

    if ! service_start; then
        reason="candidate service start failed"
        quarantine_candidate "$reason"
        rollback_to_previous "$reason" || true
        mark_activation_failed
        VERIFY_STATUS="activation_failed"
        VERIFY_DETAIL="$reason"
        return 1
    fi
    if ! verify_running_generation "$CANDIDATE_GENERATION"; then
        reason="candidate failed bounded readiness or exact /proc executable identity"
        quarantine_candidate "$reason"
        rollback_to_previous "$reason" || true
        mark_activation_failed
        VERIFY_STATUS="activation_failed"
        VERIFY_DETAIL="$reason"
        return 1
    fi

    atomic_generation_link last-good "$CANDIDATE_GENERATION"
    rm -f "$STAGED_LINK"
    ACTIVATION_IN_PROGRESS=0
    ACTIVATION_STATUS="active"
    ROLLBACK_STATUS="not_needed"
    VERIFY_STATUS="ready"
    VERIFY_DETAIL="exact generation $CANDIDATE_GENERATION is active; RPC and agent contracts respond"
    return 0
}

main() {
    validate_confinement
    mkdir -p "$DEV_DATADIR" "$GEN_ROOT" "$REJECTED_DIR"
    guard_pending_auto_reindex
    acquire_activation_lock
    build_candidate
    stage_candidate_generation
    ensure_rollback_generation

    ACTIVATION_STATUS="preflighting"
    write_deploy_state "preflighting" "candidate staged; running process untouched"
    if ! preflight_candidate; then
        ACTIVATION_STATUS="preflight_failed"
        VERIFY_STATUS="preflight_failed"
        VERIFY_DETAIL="candidate agentbuild/tool catalog/MCP self-test preflight failed"
        FAILURE_CAPSULE="$VERIFY_DETAIL"
        quarantine_candidate "$VERIFY_DETAIL"
        write_deploy_state "$VERIFY_STATUS" "$VERIFY_DETAIL"
        echo "[dev-lane] REJECTED: preflight failed; running dev process and current generation were not touched" >&2
        return 1
    fi

    if [ "$MODE" = "stage" ]; then
        atomic_generation_link staged "$CANDIDATE_GENERATION"
        ACTIVATION_STATUS="staged"
        VERIFY_STATUS="staged"
        VERIFY_DETAIL="candidate preflight passed; no service stop/restart performed"
        write_deploy_state "$VERIFY_STATUS" "$VERIFY_DETAIL"
        echo "[dev-lane] staged $CANDIDATE_GENERATION; current process untouched"
        return 0
    fi

    cleanup_dropins
    mkdir -p "$HOME/.config/systemd/user" "$(dirname "$BUILD_ID_DROPIN")"
    install -m 644 "$REPO/deploy/$UNIT" "$HOME/.config/systemd/user/$UNIT"
    if ! activate_candidate; then
        write_deploy_state "$VERIFY_STATUS" "$VERIFY_DETAIL"
        echo "[dev-lane] REJECTED: $CANDIDATE_GENERATION; rollback=$ROLLBACK_STATUS" >&2
        return 1
    fi
    write_deploy_state "$VERIFY_STATUS" "$VERIFY_DETAIL"
    echo "[dev-lane] READY: generation=$CANDIDATE_GENERATION rollback=available"
    echo "[dev-lane] deploy state: $DEPLOY_STATE"
    echo "[dev-lane] query it:  build/bin/zclassic-cli -datadir=$DEV_DATADIR -rpcport=$DEV_RPCPORT getblockcount"
    echo "[dev-lane] tail log:  tail -f $DEV_DATADIR/node.log"
}

main
