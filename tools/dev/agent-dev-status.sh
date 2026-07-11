#!/usr/bin/env bash
# Print the no-build development-lane status for agents and operators.
#
# This is intentionally read-only. It never restarts the dev lane and never
# builds. Use it before deciding whether to run agent-stage-dev or deploy-dev.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

DEV_BIN="${ZCL_AGENT_DEV_BIN:-$HOME/.local/bin/zclassic23-dev}"
SRC_BIN="${ZCL_AGENT_SRC_BIN:-build/bin/zclassic23-dev}"
DEV_DATADIR="${ZCL_AGENT_DEV_DATADIR:-$HOME/.zclassic-c23-dev}"
DEV_RPCPORT="${ZCL_AGENT_DEV_RPCPORT:-18252}"
UNIT="${ZCL_AGENT_DEV_UNIT:-zcl23-dev.service}"
GEN_ROOT="${ZCL_DEV_GENERATION_ROOT:-$HOME/.local/lib/zclassic23-dev}"
CURRENT_LINK="$GEN_ROOT/current"
LAST_GOOD_LINK="$GEN_ROOT/last-good"
STAGED_LINK="$GEN_ROOT/staged"
ACTIVATION_LOCK="$GEN_ROOT/activation.lock"
REJECTED_DIR="$GEN_ROOT/rejected"
NODE_LOG="$DEV_DATADIR/node.log"
DEPLOY_STATE="$DEV_DATADIR/agent-deploy.json"
AUTO_REINDEX_SENTINEL="$DEV_DATADIR/auto_reindex_request"
DEV_LOOP_STATE_DIR="${ZCL_DEV_WATCH_STATE_DIR:-$HOME/.local/state/zclassic23-dev}"
LATEST_CYCLE="$DEV_LOOP_STATE_DIR/latest-cycle.json"
WATCHER_HEARTBEAT="$DEV_LOOP_STATE_DIR/watcher-heartbeat.json"
QUALITY_STATE_DIR="${ZCL_QUALITY_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/zclassic23-quality}"
SCHEMA="zcl.agent_dev_status.v1"
MODE="${1:-text}"

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

artifact_json() {
    local path="$1" schema="$2"
    if [ -r "$path" ] &&
       grep -q "\"schema\"[[:space:]]*:[[:space:]]*\"${schema}\"" "$path"; then
        cat "$path"
    else
        printf '{"schema":"%s","status":"unavailable","path":"%s","agent_next_action":"%s"}' \
            "$(json_escape "$schema")" "$(json_escape "$path")" \
            "$(json_escape "generate $schema from the repository")"
    fi
}

command_json() {
    local command="$1" schema="$2" body
    body="$(cd "$REPO" && /bin/sh -c "$command" 2>/dev/null || true)"
    if printf '%s' "$body" |
       grep -q "\"schema\"[[:space:]]*:[[:space:]]*\"${schema}\""; then
        printf '%s' "$body"
    else
        printf '{"schema":"%s","status":"unavailable","collector_command":"%s"}' \
            "$(json_escape "$schema")" "$(json_escape "$command")"
    fi
}

watcher_status_json() {
    local present=false mtime=0 age=-1 stale=true now pid="" alive=false
    if [ -r "$WATCHER_HEARTBEAT" ]; then
        present=true
        mtime="$(stat -c '%Y' "$WATCHER_HEARTBEAT" 2>/dev/null || printf 0)"
        pid="$(sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
            "$WATCHER_HEARTBEAT" | head -1)"
        if is_uint "$pid" && [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
            alive=true
            stale=false
        fi
        now="$(date +%s)"
        if is_uint "$mtime" && [ "$mtime" -gt 0 ]; then
            age=$((now - mtime))
            if [ "$alive" != true ] && [ "$age" -le 5 ]; then
                stale=false
            fi
        fi
    fi
    printf '{"schema":"zcl.dev_watcher_status.v1","present":%s,"path":"%s","pid":"%s","alive":%s,"mtime_epoch":%s,"age_seconds":%s,"stale":%s,"heartbeat":' \
        "$present" "$(json_escape "$WATCHER_HEARTBEAT")" \
        "$(json_escape "$pid")" "$alive" "$mtime" "$age" "$stale"
    artifact_json "$WATCHER_HEARTBEAT" zcl.dev_watch_heartbeat.v1
    printf '}'
}

quality_freshness_json() {
    local expected="$1" lane file commit status freshness
    local present=0 current=0 stale=0 unknown=0 sep=""
    printf '{"schema":"zcl.background_quality_freshness.v1","state_dir":"%s","expected_commit":"%s","lanes":[' \
        "$(json_escape "$QUALITY_STATE_DIR")" "$(json_escape "$expected")"
    for lane in fuzz coverage tests; do
        file="$QUALITY_STATE_DIR/status/$lane.json"
        commit=""; status="missing"; freshness="no_verdict"
        if [ -r "$file" ]; then
            present=$((present + 1))
            commit="$(sed -n 's/.*"commit"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$file" | head -1)"
            status="$(sed -n 's/.*"status"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$file" | head -1)"
            [ -n "$status" ] || status="invalid"
            if [ -z "$commit" ] || [ -z "$expected" ]; then
                freshness="unknown"; unknown=$((unknown + 1))
            elif [ "$commit" = "$expected" ] ||
                 [ "${expected#"$commit"}" != "$expected" ] ||
                 [ "${commit#"$expected"}" != "$commit" ]; then
                freshness="current"; current=$((current + 1))
            else
                freshness="stale"; stale=$((stale + 1))
            fi
        fi
        printf '%s{"lane":"%s","status":"%s","commit":"%s","freshness":"%s","path":"%s"}' \
            "$sep" "$lane" "$(json_escape "$status")" \
            "$(json_escape "$commit")" "$freshness" "$(json_escape "$file")"
        sep=","
    done
    printf '],"counts":{"present":%s,"current":%s,"stale":%s,"unknown":%s},' \
        "$present" "$current" "$stale" "$unknown"
    if [ "$stale" -gt 0 ]; then freshness="stale"
    elif [ "$present" -eq 0 ]; then freshness="no_verdict"
    elif [ "$unknown" -gt 0 ]; then freshness="partial"
    else freshness="current"
    fi
    printf '"freshness":"%s","agent_next_action":"%s"}' "$freshness" \
        "$([ "$freshness" = current ] && printf 'background verdicts match this build' || printf 'make quality-linger-status')"
}

file_state_json() {
    local path="$1" executable="false" size="" mtime="" build_commit=""
    if [ -x "$path" ]; then
        executable="true"
        size="$(stat -c '%s' "$path" 2>/dev/null || true)"
        mtime="$(stat -c '%y' "$path" 2>/dev/null || true)"
        build_commit="$("$path" agentbuild 2>/dev/null |
            sed -n 's/.*"build_commit"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
            head -1 || true)"
    fi
    printf '{"path":"%s","executable":%s,"size":"%s","mtime":"%s","build_commit":"%s"}' \
        "$(json_escape "$path")" "$executable" \
        "$(json_escape "$size")" "$(json_escape "$mtime")" \
        "$(json_escape "$build_commit")"
}

pre_rpc_boot_diagnostic() {
    [ -r "$NODE_LOG" ] || return 0
    tail -n 500 "$NODE_LOG" | awk '
        /crash-only recovery: consuming auto-reindex request/ {
            recovery=$0
        }
        /reindex-chainstate: rebuilding UTXO set/ {
            reindex=1
        }
        /\[coins\] flush ok: max_height=/ {
            progress=$0
        }
        /height [0-9]+\/[0-9]+ .*ETA/ {
            progress=$0
        }
        END {
            if (progress != "") {
                print "pre-RPC recovery: reindex-chainstate " progress
            } else if (reindex) {
                print "pre-RPC recovery: reindex-chainstate active"
            } else if (recovery != "") {
                print "pre-RPC recovery: " recovery
            }
        }'
}

auto_reindex_json() {
    local anchor="" count="" pending="false" malformed="false"
    if [ -r "$AUTO_REINDEX_SENTINEL" ]; then
        read -r anchor count < "$AUTO_REINDEX_SENTINEL" || malformed="true"
        if ! [[ "$anchor" =~ ^-?[0-9]+$ ]] ||
           ! [[ "$count" =~ ^-?[0-9]+$ ]]; then
            malformed="true"
        elif [[ "$count" =~ ^[0-9]+$ ]] && [ "$count" -gt 0 ]; then
            pending="true"
        fi
    fi
    printf '{"path":"%s","present":%s,"pending":%s,"malformed":%s,"anchor":"%s","count":"%s"}' \
        "$(json_escape "$AUTO_REINDEX_SENTINEL")" \
        "$([ -e "$AUTO_REINDEX_SENTINEL" ] && printf true || printf false)" \
        "$pending" "$malformed" "$(json_escape "$anchor")" \
        "$(json_escape "$count")"
}

worker_lane_json() {
    printf '{"name":"dev","role":"worker","purpose":"fresh-build development lane for frequent agent iteration","unit":"%s","datadir":"%s","rpcport":"%s","mutation_policy":"noncanonical_dev_only","canonical_guard":"never_touches_live_or_soak","status_command":"make agent-dev-status","deploy_command":"make agent-deploy-fast","stage_command":"tools/dev/deploy-dev-lane.sh --stage","legacy_stage_command":"make agent-stage-dev","recover_command":"make agent-dev-recover"}' \
        "$(json_escape "$UNIT")" "$(json_escape "$DEV_DATADIR")" \
        "$(json_escape "$DEV_RPCPORT")"
}

json_bool_field() {
    local body="$1" key="$2" token
    token="$(printf '%s\n' "$body" |
        grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\\(true\\|false\\)" 2>/dev/null |
        head -1 || true)"
    case "$token" in
        *true) printf true ;;
        *false) printf false ;;
        *) printf unknown ;;
    esac
}

json_string_field() {
    local body="$1" key="$2" token
    token="$(printf '%s\n' "$body" |
        grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" 2>/dev/null |
        head -1 || true)"
    [ -n "$token" ] || return 0
    printf '%s\n' "$token" |
        sed -n "s/^\"${key}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"$/\1/p"
}

agent_marker_clear_probe() {
    local body operator_needed validation_pack_ok agent_work_ready blocker
    local timeout_s="${ZCL_AGENT_DEV_STATUS_AGENT_TIMEOUT:-3}"
    if ! body="$(timeout "$timeout_s" build/bin/zclassic-cli \
        -datadir="$DEV_DATADIR" -rpcport="$DEV_RPCPORT" agent 2>/dev/null)"; then
        printf 'unknown|agent_contract_unavailable'
        return 0
    fi
    operator_needed="$(json_bool_field "$body" "operator_needed")"
    validation_pack_ok="$(json_bool_field "$body" "validation_pack_ok")"
    agent_work_ready="$(json_bool_field "$body" "agent_work_ready")"
    blocker="$(json_string_field "$body" "primary_blocker")"
    if [ "$operator_needed" = "true" ]; then
        printf 'false|%s' "${blocker:-operator_needed}"
    elif [ "$validation_pack_ok" = "false" ]; then
        printf 'false|validation_pack_not_ok'
    elif [ "$agent_work_ready" = "false" ]; then
        printf 'false|agent_work_not_ready'
    else
        printf 'true|ready'
    fi
}

service_field() {
    local field="$1"
    systemctl --user show "$UNIT" -p "$field" --value 2>/dev/null || true
}

is_uint() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

memory_pressure_state() {
    local current="$1" high="$2" threshold
    if is_uint "$current" && is_uint "$high" && [ "$high" -gt 0 ]; then
        threshold=$((high * 85 / 100))
        if [ "$current" -ge "$high" ]; then
            printf 'warn'
        elif [ "$current" -ge "$threshold" ]; then
            printf 'watch'
        else
            printf 'ok'
        fi
    else
        printf 'unknown'
    fi
}

service_json() {
    local active pid started linger mem_current mem_high mem_max pressure wchan=""
    active="$(service_field ActiveState)"
    pid="$(service_field MainPID)"
    started="$(service_field ExecMainStartTimestamp)"
    linger="$(loginctl show-user "$USER" -p Linger --value 2>/dev/null || true)"
    mem_current="$(service_field MemoryCurrent)"
    mem_high="$(service_field MemoryHigh)"
    mem_max="$(service_field MemoryMax)"
    pressure="$(memory_pressure_state "$mem_current" "$mem_high")"
    if is_uint "$pid" && [ "$pid" -gt 0 ] && [ -r "/proc/$pid/wchan" ]; then
        wchan="$(cat "/proc/$pid/wchan" 2>/dev/null || true)"
    fi
    printf '{"unit":"%s","active_state":"%s","main_pid":"%s","started":"%s","linger":"%s","memory_current_bytes":"%s","memory_high_bytes":"%s","memory_max_bytes":"%s","memory_pressure":"%s","wait_channel":"%s"}' \
        "$(json_escape "$UNIT")" "$(json_escape "$active")" \
        "$(json_escape "$pid")" "$(json_escape "$started")" \
        "$(json_escape "$linger")" "$(json_escape "$mem_current")" \
        "$(json_escape "$mem_high")" "$(json_escape "$mem_max")" \
        "$(json_escape "$pressure")" "$(json_escape "$wchan")"
}

rpc_json() {
    local height="" status="unreachable" detail="" diag=""
    height="$(build/bin/zclassic-cli -datadir="$DEV_DATADIR" \
        -rpcport="$DEV_RPCPORT" getblockcount 2>/dev/null || true)"
    if [[ "$height" =~ ^[0-9]+$ ]]; then
        status="ok"
        detail="height=$height"
    else
        diag="$(pre_rpc_boot_diagnostic || true)"
        detail="${diag:-rpc unavailable}"
    fi
    printf '{"status":"%s","height":"%s","detail":"%s","rpcport":"%s"}' \
        "$(json_escape "$status")" "$(json_escape "$height")" \
        "$(json_escape "$detail")" "$(json_escape "$DEV_RPCPORT")"
}

deploy_state_summary_json() {
    local verify_status="" verify_detail="" build_commit="" deployed_at=""
    local candidate="" current="" running="" last_good="" activation_status=""
    local rollback_status="" rollback_available="false" failure_capsule=""
    if [ -r "$DEPLOY_STATE" ] && command -v jq >/dev/null 2>&1; then
        verify_status="$(jq -r '.verify_status // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        verify_detail="$(jq -r '.verify_detail // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        build_commit="$(jq -r '.build_commit // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        deployed_at="$(jq -r '.deployed_at_utc // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        candidate="$(jq -r '.candidate_generation // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        current="$(jq -r '.current_generation // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        running="$(jq -r '.running_generation // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        last_good="$(jq -r '.last_good_generation // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        activation_status="$(jq -r '.activation_status // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        rollback_status="$(jq -r '.rollback_status // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
        rollback_available="$(jq -r '.rollback_available // false' "$DEPLOY_STATE" 2>/dev/null || printf false)"
        failure_capsule="$(jq -r '.failure_capsule // ""' "$DEPLOY_STATE" 2>/dev/null || true)"
    fi
    case "$rollback_available" in true|false) ;; *) rollback_available="false" ;; esac
    printf '{"path":"%s","present":%s,"build_commit":"%s","deployed_at_utc":"%s","verify_status":"%s","verify_detail":"%s","candidate_generation":"%s","current_generation":"%s","running_generation":"%s","last_good_generation":"%s","activation_status":"%s","rollback_status":"%s","rollback_available":%s,"failure_capsule":"%s"}' \
        "$(json_escape "$DEPLOY_STATE")" \
        "$([ -r "$DEPLOY_STATE" ] && printf true || printf false)" \
        "$(json_escape "$build_commit")" "$(json_escape "$deployed_at")" \
        "$(json_escape "$verify_status")" "$(json_escape "$verify_detail")" \
        "$(json_escape "$candidate")" "$(json_escape "$current")" \
        "$(json_escape "$running")" "$(json_escape "$last_good")" \
        "$(json_escape "$activation_status")" "$(json_escape "$rollback_status")" \
        "$rollback_available" "$(json_escape "$failure_capsule")"
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

rejected_generations_json() {
    local marker generation sep=""
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

activation_lock_json() {
    local present="false" held="false" owner_pid="" fd
    if [ -e "$ACTIVATION_LOCK" ]; then
        present="true"
        owner_pid="$(sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$ACTIVATION_LOCK" 2>/dev/null | head -1 || true)"
        exec {fd}>>"$ACTIVATION_LOCK"
        if flock -n "$fd"; then
            flock -u "$fd"
            owner_pid=""
        else
            held="true"
        fi
        exec {fd}>&-
    fi
    printf '{"path":"%s","present":%s,"held":%s,"owner_pid":"%s"}' \
        "$(json_escape "$ACTIVATION_LOCK")" "$present" "$held" \
        "$(json_escape "$owner_pid")"
}

generation_state_json() {
    local current="" last_good="" staged="" running="" running_exe="" pid=""
    local rollback_available="false" running_matches_current="false" rejected_count
    current="$(read_generation_link "$CURRENT_LINK" || true)"
    last_good="$(read_generation_link "$LAST_GOOD_LINK" || true)"
    staged="$(read_generation_link "$STAGED_LINK" || true)"
    [ -n "$last_good" ] && rollback_available="true"
    pid="$(service_field MainPID)"
    if is_uint "$pid" && [ "$pid" -gt 0 ]; then
        running_exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
        case "$running_exe" in
            "$GEN_ROOT"/*/zclassic23-dev)
                running="$(basename "$(dirname "$running_exe")")"
                ;;
        esac
    fi
    [ -n "$current" ] && [ "$running" = "$current" ] && running_matches_current="true"
    if [ -d "$REJECTED_DIR" ]; then
        rejected_count="$(find "$REJECTED_DIR" -maxdepth 1 -type f -name 'gen-*.json' 2>/dev/null | wc -l | tr -d ' ')"
    else
        rejected_count=0
    fi
    printf '{"schema":"zcl.dev_generation_status.v1","root":"%s","current_generation":"%s","last_good_generation":"%s","staged_generation":"%s","running_generation":"%s","running_executable":"%s","running_matches_current":%s,"rollback_available":%s,"rejected_count":%s,"rejected_generations":' \
        "$(json_escape "$GEN_ROOT")" "$(json_escape "$current")" \
        "$(json_escape "$last_good")" "$(json_escape "$staged")" \
        "$(json_escape "$running")" "$(json_escape "$running_exe")" \
        "$running_matches_current" "$rollback_available" "$rejected_count"
    rejected_generations_json
    printf ',"activation_lock":%s}' "$(activation_lock_json)"
}

staged_matches() {
    [ -x "$SRC_BIN" ] && [ -x "$DEV_BIN" ] && cmp -s "$SRC_BIN" "$DEV_BIN"
}

next_action() {
    local rpc_status="$1" active_state="$2" auto_pending="$3"
    local stale_candidate="$4"
    if [ "$auto_pending" = "true" ]; then
        if [ "$stale_candidate" = "true" ]; then
            printf 'make agent-clear-stale-dev-reindex'
        else
            printf 'make agent-dev-recover (or ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1 for a deliberate recovery boot)'
        fi
    elif [ "$rpc_status" = "ok" ]; then
        printf 'make agent-mcp-call-dev TOOL=zcl_status'
    elif [ "$active_state" = "active" ]; then
        printf 'wait; tail -f %s' "$NODE_LOG"
    else
        printf 'make agent-deploy-fast'
    fi
}

emit_json() {
    local src installed service rpc deploy auto worker generations active_state rpc_status auto_pending
    local rpc_height auto_anchor auto_count agent_probe agent_ready agent_reason
    local deploy_blocker="false"
    local deploy_blocker_reason="" stale_candidate="false" staged="false" action
    local source_commit cycle watcher index_status latency_status quality_status
    src="$(file_state_json "$SRC_BIN")"
    installed="$(file_state_json "$DEV_BIN")"
    service="$(service_json)"
    rpc="$(rpc_json)"
    deploy="$(deploy_state_summary_json)"
    auto="$(auto_reindex_json)"
    worker="$(worker_lane_json)"
    generations="$(generation_state_json)"
    source_commit="$(printf '%s\n' "$src" |
        sed -n 's/.*"build_commit":"\([^"]*\)".*/\1/p')"
    cycle="$(artifact_json "$LATEST_CYCLE" zcl.dev_cycle.v1)"
    watcher="$(watcher_status_json)"
    index_status="$(command_json 'bash tools/dev/generate-compdb.sh --status' zcl.agent_index_runtime.v1)"
    latency_status="$(command_json 'bash tools/dev/dev-loop-bench.sh --status' zcl.dev_loop_bench.v1)"
    quality_status="$(quality_freshness_json "$source_commit")"
    staged_matches && staged="true"
    active_state="$(printf '%s\n' "$service" |
        sed -n 's/.*"active_state":"\([^"]*\)".*/\1/p')"
    rpc_status="$(printf '%s\n' "$rpc" |
        sed -n 's/.*"status":"\([^"]*\)".*/\1/p')"
    rpc_height="$(printf '%s\n' "$rpc" |
        sed -n 's/.*"height":"\([^"]*\)".*/\1/p')"
    auto_pending="$(printf '%s\n' "$auto" |
        sed -n 's/.*"pending":\([^,}]*\).*/\1/p')"
    auto_anchor="$(printf '%s\n' "$auto" |
        sed -n 's/.*"anchor":"\([^"]*\)".*/\1/p')"
    auto_count="$(printf '%s\n' "$auto" |
        sed -n 's/.*"count":"\([^"]*\)".*/\1/p')"
    if [ "$auto_pending" = "true" ]; then
        deploy_blocker="true"
        deploy_blocker_reason="pending_auto_reindex_requires_explicit_recovery_boot"
        if [ "$rpc_status" = "ok" ] && is_uint "$rpc_height" &&
           is_uint "$auto_anchor" && [ "$rpc_height" -ge "$auto_anchor" ]; then
            agent_probe="$(agent_marker_clear_probe)"
            agent_ready="${agent_probe%%|*}"
            agent_reason="${agent_probe#*|}"
            if [ "$agent_ready" = "true" ]; then
                stale_candidate="true"
            fi
        fi
    fi
    agent_ready="${agent_ready:-not_checked}"
    agent_reason="${agent_reason:-not_checked}"
    action="$(next_action "$rpc_status" "$active_state" "$auto_pending" \
        "$stale_candidate")"
    printf '{\n'
    printf '  "schema": "%s",\n' "$SCHEMA"
    printf '  "worker_lane": %s,\n' "$worker"
    printf '  "source_binary": %s,\n' "$src"
    printf '  "installed_binary": %s,\n' "$installed"
    printf '  "installed_matches_source": %s,\n' "$staged"
    printf '  "service": %s,\n' "$service"
    printf '  "rpc": %s,\n' "$rpc"
    printf '  "deploy_state": %s,\n' "$deploy"
    printf '  "activation": %s,\n' "$generations"
    printf '  "current_cycle": %s,\n' "$cycle"
    printf '  "watcher": %s,\n' "$watcher"
    printf '  "index_freshness": %s,\n' "$index_status"
    printf '  "latency_slo": %s,\n' "$latency_status"
    printf '  "background_quality_freshness": %s,\n' "$quality_status"
    printf '  "auto_reindex": %s,\n' "$auto"
    printf '  "deploy_blocker": %s,\n' "$deploy_blocker"
    printf '  "deploy_blocker_reason": "%s",\n' \
        "$(json_escape "$deploy_blocker_reason")"
    printf '  "explicit_recovery_env": "ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY",\n'
    printf '  "agent_contract_ready_for_marker_clear": "%s",\n' \
        "$(json_escape "$agent_ready")"
    printf '  "agent_contract_marker_clear_reason": "%s",\n' \
        "$(json_escape "$agent_reason")"
    printf '  "auto_reindex_stale_candidate": %s,\n' "$stale_candidate"
    printf '  "next_action": "%s"\n' "$(json_escape "$action")"
    printf '}\n'
}

emit_text() {
    local json
    json="$(emit_json)"
    if command -v jq >/dev/null 2>&1; then
        printf '%s\n' "$json" | jq -r '
            "[agent-dev-status] service=" + .service.active_state +
            " pid=" + .service.main_pid +
            " rpc=" + .rpc.status +
            " detail=" + .rpc.detail,
            "[agent-dev-status] worker_lane=" + .worker_lane.name +
            " role=" + .worker_lane.role +
            " policy=" + .worker_lane.mutation_policy +
            " guard=" + .worker_lane.canonical_guard,
            "[agent-dev-status] source_bin=" + .source_binary.path +
            " installed_bin=" + .installed_binary.path +
            " source_commit=" + .source_binary.build_commit +
            " installed_commit=" + .installed_binary.build_commit +
            " staged_matches_source=" + (.installed_matches_source|tostring),
            "[agent-dev-status] memory current=" + .service.memory_current_bytes +
            " high=" + .service.memory_high_bytes +
            " max=" + .service.memory_max_bytes +
            " pressure=" + .service.memory_pressure +
            " wait_channel=" + .service.wait_channel,
            "[agent-dev-status] auto_reindex pending=" + (.auto_reindex.pending|tostring) +
            " anchor=" + .auto_reindex.anchor +
            " count=" + .auto_reindex.count,
            "[agent-dev-status] deploy_blocker=" + (.deploy_blocker|tostring) +
            " reason=" + .deploy_blocker_reason +
            " stale_candidate=" + (.auto_reindex_stale_candidate|tostring) +
            " agent_marker_clear=" + .agent_contract_ready_for_marker_clear +
            " marker_clear_reason=" + .agent_contract_marker_clear_reason,
            "[agent-dev-status] deploy_state=" + .deploy_state.path +
            " present=" + (.deploy_state.present|tostring) +
            " verify=" + .deploy_state.verify_status +
            " activation=" + .deploy_state.activation_status +
            " rollback=" + .deploy_state.rollback_status,
            "[agent-dev-status] generation current=" + .activation.current_generation +
            " running=" + .activation.running_generation +
            " last_good=" + .activation.last_good_generation +
            " staged=" + .activation.staged_generation +
            " exact=" + (.activation.running_matches_current|tostring) +
            " rollback_available=" + (.activation.rollback_available|tostring) +
            " rejected=" + (.activation.rejected_count|tostring) +
            " activation_lock_held=" + (.activation.activation_lock.held|tostring),
            "[agent-dev-status] watcher present=" + (.watcher.present|tostring) +
            " alive=" + (.watcher.alive|tostring) +
            " stale=" + (.watcher.stale|tostring) +
            " cycle=" + (.current_cycle.cycle_id // "") +
            " cycle_outcome=" + (.current_cycle.outcome // .current_cycle.status // "unknown"),
            "[agent-dev-status] index=" + (.index_freshness.freshness // "unavailable") +
            " hot_swap_slo=" + (.latency_slo.slo.hot_swap.status // "not_measured") +
            " reload_slo=" + (.latency_slo.slo.process_reload.status // "not_measured") +
            " background_quality=" + (.background_quality_freshness.freshness // "unknown"),
            "[agent-dev-status] next=" + .next_action'
    else
        printf '%s\n' "$json"
    fi
}

case "$MODE" in
    --json|json)
        emit_json
        ;;
    ""|text)
        emit_text
        ;;
    *)
        echo "usage: tools/dev/agent-dev-status.sh [--json]" >&2
        exit 2
        ;;
esac
