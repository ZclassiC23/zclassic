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
INTERVAL=2

if [ ! -x "$RPC_TOOL" ]; then
    alt="./build/bin/zcl-rpc"
    if [ -x "$alt" ]; then
        RPC_TOOL="$alt"
    fi
fi

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

rpc_dumpstate() {
    component="$1"
    out=$("$RPC_TOOL" dumpstate "$component" 2>&1 || true)
    if json_has_key "$out" "$2"; then
        printf '%s\n' "$out"
        return 0
    fi

    # build/bin/zcl-rpc wraps remaining argv directly into a JSON params array,
    # so string arguments need quotes. zclassic-cli accepts the unquoted
    # form above, but this fallback keeps deploy verification portable.
    out=$("$RPC_TOOL" dumpstate "\"$component\"" 2>&1 || true)
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

    net=$("$RPC_TOOL" getnetworkinfo 2>&1 || true)
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

    health=$("$RPC_TOOL" healthcheck 2>&1 || true)
    json_key_is_string "$health" consensus_authority local_consensus_validation ||
        { last_err="healthcheck authority contract missing: $health"; return 1; }
    json_not_has_key "$health" mirror_authorization_enabled ||
        { last_err="healthcheck exposes deleted mirror_authorization_enabled: $health"; return 1; }
    json_not_has_key "$health" mirror_consensus_authority ||
        { last_err="healthcheck exposes deleted mirror_consensus_authority: $health"; return 1; }
    json_has_key "$health" candidate_source ||
        { last_err="healthcheck candidate_source missing: $health"; return 1; }
    json_has_key "$health" candidate_trust ||
        { last_err="healthcheck candidate_trust missing: $health"; return 1; }
    json_key_is_true "$health" healthy ||
        { last_err="healthcheck is not healthy: $health"; return 1; }
    printf '%s\n' "$health" | grep -q '"degraded_reason"[[:space:]]*:[[:space:]]*"chain_evidence_gap"' &&
        { last_err="healthcheck reports generic evidence gap: $health"; return 1; }

    echo "Deployed + RPC live at block $height; canonical diagnostics ready."
    return 0
}

while [ "$(date +%s)" -lt "$deadline" ]; do
    attempt=$((attempt + 1))
    if out=$("$RPC_TOOL" getblockcount 2>&1); then
        # Accept either a plain integer (zclassic-cli) or a JSON
        # envelope with "result":<integer> (build/bin/zcl-rpc). Any other
        # output keeps the loop polling.
        height=$(extract_height "$out")
        if [ -n "$height" ] && verify_contract "$height"; then
            exit 0
        fi
    fi
    if [ -z "$last_err" ]; then
        last_err="$out"
    fi
    sleep "$INTERVAL"
done

echo "DEPLOY FAILED: RPC/diagnostic contract did not become ready within ${TIMEOUT}s (attempts=$attempt)"
if [ -n "$last_err" ]; then
    echo "last error: $last_err"
fi
exit 1
