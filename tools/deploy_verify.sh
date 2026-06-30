#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Post-restart RPC health check for `make deploy`.
#
# The previous `make deploy` printed "Deployed." whenever systemd reported
# the unit active for >2s. That includes binaries that segfault on first
# RPC call. This script replaces that false-positive with a real probe:
# poll RPC every 2s and only succeed when the node answers with an integer
# height and the public-node hardening diagnostics are registered by the
# running daemon.
#
# The default deadline must absorb a full cold boot of a ~22 GB datadir:
# block-file scan + pprev repair + index reconcile routinely exceed two
# minutes, and a slow boot is not a failed deploy (120s false-FAILed a
# healthy deploy on 2026-06-10).
#
# Exit codes:
#   0  — RPC live, block count observed, diagnostic contract present
#   1  — RPC/diagnostic contract did not come up within the deadline
#
# Usage: ./tools/deploy_verify.sh [rpc_tool] [timeout_seconds]
set -eu

RPC_TOOL="${1:-./build/bin/zclassic-cli}"
TIMEOUT="${2:-${ZCL_DEPLOY_VERIFY_TIMEOUT:-600}}"
RPC_CALL_TIMEOUT="${ZCL_DEPLOY_RPC_TIMEOUT:-20}"
INTERVAL=2

systemd_exec_arg() {
    key="$1"
    command -v systemctl >/dev/null 2>&1 || return 1
    systemctl --user show zclassic23 -p ExecStart --value 2>/dev/null |
        tr ' ' '\n' |
        sed -n "s/^-${key}=//p" |
        head -1
}

# The build_commit we expect the restarted daemon to be running. `make deploy`
# passes ZCL_DEPLOY_EXPECT_COMMIT=$(BUILD_COMMIT) (the exact baked value,
# including any -dirty suffix); a by-hand run inside the repo falls back to
# HEAD. Empty (e.g. deploying from a tarball with no git) => the staleness
# assertion is skipped with a warning, never a hard fail.
EXPECT_COMMIT="${ZCL_DEPLOY_EXPECT_COMMIT:-$(git rev-parse --short HEAD 2>/dev/null || true)}"

if [ ! -x "$RPC_TOOL" ]; then
    alt="./build/bin/zcl-rpc"
    if [ -x "$alt" ]; then
        RPC_TOOL="$alt"
    fi
fi

DEFAULT_DATADIR="$HOME/.zclassic-c23"
RPC_DATADIR="${ZCL_DATADIR:-$DEFAULT_DATADIR}"
if [ -z "${ZCL_DATADIR:-}" ]; then
    SERVICE_DATADIR="$(systemd_exec_arg datadir || true)"
    if [ -n "$SERVICE_DATADIR" ]; then
        RPC_DATADIR="$SERVICE_DATADIR"
    fi
fi

RPCPORT="${ZCL_RPCPORT:-18232}"
if [ -z "${ZCL_RPCPORT:-}" ]; then
    SERVICE_RPCPORT="$(systemd_exec_arg rpcport || true)"
    if [ -n "$SERVICE_RPCPORT" ]; then
        RPCPORT="$SERVICE_RPCPORT"
    fi
fi

rpc_exec() {
    rc=0
    if command -v timeout >/dev/null 2>&1; then
        timeout "${RPC_CALL_TIMEOUT}s" "$@" || rc=$?
        if [ "$rc" -eq 124 ]; then
            echo "rpc timed out after ${RPC_CALL_TIMEOUT}s: $*" >&2
        fi
    else
        "$@" || rc=$?
    fi
    return "$rc"
}

rpc_call() {
    name=$(basename "$RPC_TOOL")
    case "$name" in
        zclassic-cli|zcl-rpc)
            if [ -n "${ZCL_RPCCONNECT:-}" ]; then
                rpc_exec "$RPC_TOOL" "-datadir=$RPC_DATADIR" "-rpcport=$RPCPORT" \
                    "-rpcconnect=$ZCL_RPCCONNECT" "$@"
            else
                rpc_exec "$RPC_TOOL" "-datadir=$RPC_DATADIR" "-rpcport=$RPCPORT" "$@"
            fi
            ;;
        *)
            rpc_exec "$RPC_TOOL" "$@"
            ;;
    esac
}

deadline=$(( $(date +%s) + TIMEOUT ))
attempt=0
last_err=""

json_has_key() {
    printf '%s\n' "$1" | grep -q "\"$2\"[[:space:]]*:"
}

json_not_has_key() {
    ! json_has_key "$1" "$2"
}

json_key_is_true() {
    printf '%s\n' "$1" | grep -q "\"$2\"[[:space:]]*:[[:space:]]*true"
}

json_key_is_string() {
    printf '%s\n' "$1" |
        grep -q "\"$2\"[[:space:]]*:[[:space:]]*\"$3\""
}

json_top_key_is_true() {
    command -v python3 >/dev/null 2>&1 || return 1
    printf '%s\n' "$1" |
        python3 -c 'import json, sys; d=json.load(sys.stdin); sys.exit(0 if d.get(sys.argv[1]) is True else 1)' "$2" 2>/dev/null
}

json_top_has_key() {
    command -v python3 >/dev/null 2>&1 || return 1
    printf '%s\n' "$1" |
        python3 -c 'import json, sys; d=json.load(sys.stdin); sys.exit(0 if sys.argv[1] in d else 1)' "$2" 2>/dev/null
}

json_top_key_is_string() {
    command -v python3 >/dev/null 2>&1 || return 1
    printf '%s\n' "$1" |
        python3 -c 'import json, sys; d=json.load(sys.stdin); sys.exit(0 if d.get(sys.argv[1]) == sys.argv[2] else 1)' "$2" "$3" 2>/dev/null
}

extract_health_height() {
    command -v python3 >/dev/null 2>&1 || return 1
    printf '%s\n' "$1" | python3 -c '
import json
import sys
d = json.load(sys.stdin)
checks = d.get("checks") or {}
checks_ca = checks.get("chain_advance") or {}
top_ca = d.get("chain_advance") or {}
for value in (checks_ca.get("local_height"), top_ca.get("local_height"), checks.get("log_head")):
    if isinstance(value, int) and value > 0:
        print(value)
        sys.exit(0)
sys.exit(1)
' 2>/dev/null
}

json_key_is_int() {
    printf '%s\n' "$1" |
        grep -q "\"$2\"[[:space:]]*:[[:space:]]*$3\\([^0-9]\\|$\\)"
}

extract_height() {
    height=$(printf '%s' "$1" |
        grep -oE '"result"[[:space:]]*:[[:space:]]*[0-9]+' |
        grep -oE '[0-9]+' | head -1)
    if [ -z "$height" ]; then
        plain=$(printf '%s' "$1" | tr -d '[:space:]')
        case "$plain" in
            [0-9]*) height="$plain" ;;
        esac
    fi
    printf '%s' "$height"
}

extract_build_commit() {
    printf '%s\n' "$1" |
        grep -oE '"build_commit"[[:space:]]*:[[:space:]]*"[^"]*"' |
        head -1 |
        sed -E 's/.*"build_commit"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/'
}

# Normalise a build_commit for comparison: lowercase, drop a trailing -dirty.
norm_commit() { printf '%s' "$1" | tr 'A-Z' 'a-z' | sed -E 's/-dirty$//'; }

rpc_dumpstate() {
    component="$1"
    out=$(rpc_call dumpstate "$component" 2>&1 || true)
    if json_has_key "$out" "$2"; then
        printf '%s\n' "$out"
        return 0
    fi

    # build/bin/zcl-rpc wraps remaining argv directly into a JSON params array,
    # so string arguments need quotes. zclassic-cli accepts the unquoted
    # form above, but this fallback keeps deploy verification portable.
    out=$(rpc_call dumpstate "\"$component\"" 2>&1 || true)
    printf '%s\n' "$out"
}

verify_contract() {
    height="$1"

    ca=$(rpc_dumpstate chain_advance_coordinator initialized)
    json_key_is_true "$ca" initialized ||
        { last_err="chain_advance_coordinator not initialized: $ca"; return 1; }
    json_key_is_true "$ca" has_connman ||
        { last_err="chain_advance_coordinator missing connman: $ca"; return 1; }
    json_key_is_true "$ca" has_main_state ||
        { last_err="chain_advance_coordinator missing main_state: $ca"; return 1; }
    json_key_is_true "$ca" has_node_db ||
        { last_err="chain_advance_coordinator missing node_db: $ca"; return 1; }
    json_key_is_string "$ca" authority local_consensus_validation ||
        { last_err="chain_advance authority contract missing: $ca"; return 1; }
    json_has_key "$ca" selected_source ||
        { last_err="chain_advance selected_source missing: $ca"; return 1; }
    json_has_key "$ca" candidate_source ||
        { last_err="chain_advance candidate_source missing: $ca"; return 1; }
    json_has_key "$ca" sources ||
        { last_err="chain_advance sources missing: $ca"; return 1; }

    evidence=$(rpc_dumpstate chain_evidence health_reason)
    json_has_key "$evidence" health_reason ||
        { last_err="chain_evidence diagnostics missing health_reason: $evidence"; return 1; }
    printf '%s\n' "$evidence" | grep -q '"health_reason"[[:space:]]*:[[:space:]]*"chain_evidence_gap"' &&
        { last_err="chain_evidence reports generic gap: $evidence"; return 1; }
    printf '%s\n' "$evidence" | grep -q '"health_reason"[[:space:]]*:[[:space:]]*"[^"]' &&
        { last_err="chain_evidence is frozen/degraded: $evidence"; return 1; }

    net=$(rpc_call getnetworkinfo 2>&1 || true)
    for key in advertised_subver advertised_services inbound_connections outbound_connections handshaked_connections \
               inbound_handshake_seen remote_handshake_seen magicbean_peers \
               zclassic_c23_peers peer_lifecycle; do
        json_has_key "$net" "$key" ||
            { last_err="getnetworkinfo missing $key: $net"; return 1; }
    done
    printf '%s\n' "$net" | grep -q '"advertised_subver"[[:space:]]*:[[:space:]]*"/MagicBean:' ||
        { last_err="node is not advertising MagicBean-compatible subver: $net"; return 1; }

    peer=$(rpc_dumpstate peer_lifecycle summary)
    json_has_key "$peer" summary ||
        { last_err="peer_lifecycle summary missing: $peer"; return 1; }
    json_has_key "$peer" sources ||
        { last_err="peer_lifecycle sources missing: $peer"; return 1; }
    json_has_key "$peer" legacy_compatible_handshakes ||
        { last_err="peer_lifecycle missing legacy handshake canary: $peer"; return 1; }
    json_has_key "$peer" pre_handshake_disconnects ||
        { last_err="peer_lifecycle missing pre-handshake disconnect counter: $peer"; return 1; }
    if ! printf '%s\n' "$peer" | grep -q '"legacy_compatible_handshakes"[[:space:]]*:[[:space:]]*[1-9]'; then
        printf '%s\n' "$peer" | grep -q '"attempted"[[:space:]]*:[[:space:]]*0' ||
            { last_err="no legacy-compatible handshake observed and peers were reachable: $peer"; return 1; }
    fi

    mirror=$(rpc_dumpstate legacy_mirror consensus_authority)
    json_has_key "$mirror" consensus_authority ||
        { last_err="legacy_mirror authority missing: $mirror"; return 1; }
    json_key_is_string "$mirror" consensus_authority local_consensus_validation ||
        { last_err="legacy_mirror must not claim zclassicd authority: $mirror"; return 1; }
    json_not_has_key "$mirror" mirror_authorization_enabled ||
        { last_err="legacy_mirror exposes deleted mirror_authorization_enabled: $mirror"; return 1; }
    json_not_has_key "$mirror" mirror_consensus_authority ||
        { last_err="legacy_mirror exposes deleted mirror_consensus_authority: $mirror"; return 1; }
    json_has_key "$mirror" candidate_source ||
        { last_err="legacy_mirror candidate_source missing: $mirror"; return 1; }
    json_key_is_string "$mirror" candidate_source legacy_advisory ||
        { last_err="legacy_mirror must expose advisory candidate source: $mirror"; return 1; }
    json_has_key "$mirror" legacy_advisory_gated_by_native_retries ||
        { last_err="legacy_mirror advisory/native retry gate missing: $mirror"; return 1; }
    json_has_key "$mirror" blockers_total ||
        { last_err="legacy_mirror blockers_total missing: $mirror"; return 1; }
    json_has_key "$mirror" stalls_total ||
        { last_err="legacy_mirror stalls_total missing: $mirror"; return 1; }
    json_has_key "$mirror" unsafe_overrides_total ||
        { last_err="legacy_mirror unsafe_overrides_total missing: $mirror"; return 1; }
    json_key_is_int "$mirror" unsafe_overrides_total 0 ||
        { last_err="legacy_mirror unsafe overrides are unhealthy: $mirror"; return 1; }
    json_has_key "$mirror" last_override_safe ||
        { last_err="legacy_mirror last_override_safe missing: $mirror"; return 1; }
    json_has_key "$mirror" last_override_scope ||
        { last_err="legacy_mirror last_override_scope missing: $mirror"; return 1; }

    health=$(rpc_call healthcheck 2>&1 || true)
    json_top_key_is_string "$health" consensus_authority local_consensus_validation ||
        { last_err="healthcheck authority contract missing: $health"; return 1; }
    json_not_has_key "$health" mirror_authorization_enabled ||
        { last_err="healthcheck exposes deleted mirror_authorization_enabled: $health"; return 1; }
    json_not_has_key "$health" mirror_consensus_authority ||
        { last_err="healthcheck exposes deleted mirror_consensus_authority: $health"; return 1; }
    json_top_has_key "$health" candidate_source ||
        { last_err="healthcheck candidate_source missing: $health"; return 1; }
    json_top_has_key "$health" candidate_trust ||
        { last_err="healthcheck candidate_trust missing: $health"; return 1; }
    json_top_key_is_true "$health" healthy ||
        { last_err="healthcheck is not healthy: $health"; return 1; }
    printf '%s\n' "$health" | grep -q '"degraded_reason"[[:space:]]*:[[:space:]]*"chain_evidence_gap"' &&
        { last_err="healthcheck reports generic evidence gap: $health"; return 1; }
    health_height=$(extract_health_height "$health" || true)
    if [ -n "$health_height" ]; then
        height="$health_height"
    fi
    case "$height" in
        ''|0)
            last_err="healthcheck is healthy but no positive verified height was available: $health"
            return 1
            ;;
    esac

    # Staleness guard: the binary actually answering RPC must be the one we just
    # built. A restart that silently kept the OLD binary (stale unit, a rebuild
    # that errored, wrong path on $PATH) otherwise reads as a green deploy — the
    # exact footgun that shipped stale binaries for hours (MEMORY: "live binary
    # still stale", "23h STALE"). Fail loud on a commit mismatch.
    running_commit=$(extract_build_commit "$health")
    if [ -n "$EXPECT_COMMIT" ] && [ -n "$running_commit" ]; then
        r=$(norm_commit "$running_commit"); e=$(norm_commit "$EXPECT_COMMIT")
        # Prefix-match either direction so differing short-hash lengths
        # (7 vs 9 vs 10 hex) compare equal while a real divergence fails.
        case "$r" in
            "$e"*) : ;;
            *) case "$e" in
                   "$r"*) : ;;
                   *) last_err="STALE DEPLOY: running build_commit '$running_commit' != expected '$EXPECT_COMMIT' (restart kept an old binary)"; return 1 ;;
               esac ;;
        esac
    elif [ -z "$running_commit" ]; then
        echo "deploy_verify: WARNING — running daemon exposes no build_commit; cannot assert freshness" >&2
    elif [ -z "$EXPECT_COMMIT" ]; then
        echo "deploy_verify: WARNING — no expected commit (not in a git tree); skipping staleness assertion" >&2
    fi

    echo "Deployed + RPC live at block $height (build_commit ${running_commit:-unknown}); canonical diagnostics ready."
    return 0
}

while [ "$(date +%s)" -lt "$deadline" ]; do
    attempt=$((attempt + 1))
    if out=$(rpc_call getblockcount 2>&1); then
        # Accept either a plain integer (zclassic-cli) or a JSON
        # envelope with "result":<integer> (build/bin/zcl-rpc). Any other
        # output keeps the loop polling.
        height=$(extract_height "$out")
        if [ -n "$height" ] && verify_contract "$height"; then
            exit 0
        fi
        if [ -z "$height" ]; then
            last_err="$out"
        fi
    else
        last_err="$out"
    fi
    sleep "$INTERVAL"
done

echo "DEPLOY FAILED: RPC/diagnostic contract did not become ready within ${TIMEOUT}s (attempts=$attempt)"
if [ -n "$last_err" ]; then
    echo "last error: $last_err"
fi
exit 1
