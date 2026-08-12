#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Three-full-node composition hook for zcode_dht_acceptance.sh. Sourced only
# after its seven isolated identities and authenticated sparse topology pass.

[ "${DHT_BUILDWORKERS:-0}" = 1 ] ||
    dht_die "async proof hook requires DHT_BUILDWORKERS=1"
[ "${DHT_PACKAGEHOST:-0}" = 1 ] ||
    dht_die "async proof hook requires DHT_PACKAGEHOST=1"

ZAP_PROJECT="$DHT_WORK/async-proof-project"
mkdir -p "$ZAP_PROJECT/src" "$ZAP_PROJECT/include" "$ZAP_PROJECT/tests"
printf '%s\n' 'MIT' >"$ZAP_PROJECT/LICENSE"
printf '%s\n' 'int x(void);' >"$ZAP_PROJECT/include/x.h"
printf '%s\n' 'int x(void) { return 1; }' >"$ZAP_PROJECT/src/x.c"
printf '%s\n' 'int main(void) { return 0; }' >"$ZAP_PROJECT/tests/test.c"
printf '%s\n' '{"schema":1,"name":"acceptance/async-proof","semver":"0.1.0","language":"c23","license":"MIT","include_dir":"include","source_dir":"src","dependencies":[]}' >"$ZAP_PROJECT/zcode-package.json"

zap_field() {
    local expression="$1"
    python3 -c "import json,sys; d=json.load(sys.stdin); print($expression)"
}

zap_submit() {
    local node="$1" value="$2" goal="$3" start handoff candidate result
    local started_ms finished_ms elapsed_ms ok action submit_us event request
    start="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work start \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"goal\":\"$goal\",\"profile\":\"quick\"}")"
    ok="$(printf '%s' "$start" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    [ "$ok" = True ] || dht_die "node $node could not start async work: $start"
    handoff="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work run \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"work\":\"latest\",\"adapter\":\"manual\"}")"
    candidate="$(printf '%s' "$handoff" | zap_field 'd["data"]["candidate_workspace"]' 2>/dev/null || true)"
    [ -d "$candidate/src" ] || dht_die "node $node did not materialize its candidate: $handoff"
    printf 'int x(void) { return %s; }\n' "$value" >"$candidate/src/x.c"
    started_ms="$(date +%s%3N)"
    result="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work run \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"work\":\"latest\",\"adapter\":\"manual\",\"datadir\":\"${DDS[$node]}\"}")"
    finished_ms="$(date +%s%3N)"; elapsed_ms=$((finished_ms - started_ms))
    ok="$(printf '%s' "$result" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    [ "$ok" = True ] || dht_die "node $node foreground admission failed: $result"
    action="$(printf '%s' "$result" | zap_field 'd["data"]["expert"]["action_id"]')"
    event="$(printf '%s' "$result" | zap_field 'd["data"]["async_proof_event_root"]')"
    request="$(printf '%s' "$result" | zap_field 'd["data"]["remote_request_id"]')"
    submit_us="$(printf '%s' "$result" | zap_field 'd["data"]["local_submit_us"]')"
    [ "${#action}" -eq 64 ] && [ "${#event}" -eq 64 ] &&
        [ "$request" -gt 0 ] && [ "$submit_us" -ge 0 ] ||
        dht_die "node $node response omitted root-bound async identity: $result"
    [ "$elapsed_ms" -lt 30000 ] ||
        dht_die "node $node foreground crossed 30s latency firewall: ${elapsed_ms}ms"
    ZAP_ACTION="$action"; ZAP_FOREGROUND_MS="$elapsed_ms"
}

zap_state() {
    local node="$1" action="$2" sql out
    sql="SELECT state FROM build_proof_events WHERE action_id='$action' ORDER BY rowid DESC LIMIT 1"
    out="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" \
        core storage query --sql="$sql" 2>/dev/null || true)"
    printf '%s' "$out" | python3 -c '
import json,sys
try: print("READY_FOR_ACCEPTANCE" if "READY_FOR_ACCEPTANCE" in json.dumps(json.load(sys.stdin)) else "")
except Exception: print("")'
}

zap_wait_ready() {
    local node="$1" action="$2" deadline
    deadline=$(( $(date +%s) + 120 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ "$(zap_state "$node" "$action")" = READY_FOR_ACCEPTANCE ] && return 0
        sleep 1
    done
    return 1
}

zap_dump_failure() {
    local node="$1" action="$2" sql
    sql="SELECT state,peer_id,deadline_at,elapsed_us,event_root FROM build_proof_events WHERE action_id='$action' ORDER BY rowid"
    dht_note "async proof lifecycle: $(dht_native "${DDS[$node]}" \
        "${RPCS[$node]}" core storage query --sql="$sql" 2>/dev/null || true)"
    dht_note "async proof log tail follows"
    tail -80 "${DDS[$node]}/node.log" >&2 || true
}

dht_note "async proof: A foreground admission and background peer evidence"
ZAP_ACTION=""; ZAP_FOREGROUND_MS=0
zap_submit "$ORIGIN" 2 "Change x to two"
FIRST_ACTION="$ZAP_ACTION"; FIRST_MS="$ZAP_FOREGROUND_MS"
zap_wait_ready "$ORIGIN" "$FIRST_ACTION" ||
    { zap_dump_failure "$ORIGIN" "$FIRST_ACTION";
      dht_die "A did not reach READY_FOR_ACCEPTANCE from signed peer evidence"; }

# A is only a role, not infrastructure authority. Remove it and make the next
# live full node originate the next candidate through the same native path.
dht_note "async proof: kill A; B originates and a remaining peer proves"
dht_kill_group "${PIDS[$ORIGIN]}"; PIDS[$ORIGIN]=""
sleep 2
zap_submit "$NEXT" 3 "Change x to three"
SECOND_ACTION="$ZAP_ACTION"; SECOND_MS="$ZAP_FOREGROUND_MS"
zap_wait_ready "$NEXT" "$SECOND_ACTION" ||
    { zap_dump_failure "$NEXT" "$SECOND_ACTION";
      dht_die "B did not reach READY_FOR_ACCEPTANCE after A disappeared"; }

dht_note "async proof PASS: first_action=$FIRST_ACTION second_action=$SECOND_ACTION foreground_ms=$FIRST_MS,$SECOND_MS github_contacted=false"
