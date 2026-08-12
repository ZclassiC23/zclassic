#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Three-full-node composition hook for zcode_dht_acceptance.sh. Sourced only
# after its seven isolated identities and authenticated sparse topology pass.

[ "${DHT_BUILDWORKERS:-0}" = 1 ] ||
    dht_die "async proof hook requires DHT_BUILDWORKERS=1"
[ "${DHT_PACKAGEHOST:-0}" = 1 ] ||
    dht_die "async proof hook requires DHT_PACKAGEHOST=1"
trap 'dht_die "async proof hook command failed at line $LINENO"' ERR

ZAP_PROJECT="$DHT_WORK/async-proof-project"
mkdir -p "$ZAP_PROJECT/src" "$ZAP_PROJECT/include" "$ZAP_PROJECT/tests"
printf '%s\n' 'MIT' >"$ZAP_PROJECT/LICENSE"
printf '%s\n' 'int x(void);' >"$ZAP_PROJECT/include/x.h"
printf '%s\n' 'int main(void) { return 0; }' >"$ZAP_PROJECT/tests/test.c"
printf '%s\n' '{"schema":1,"name":"acceptance/async-proof","semver":"0.1.0","language":"c23","license":"MIT","include_dir":"include","source_dir":"src","dependencies":[]}' >"$ZAP_PROJECT/zcode-package.json"

# Keep the package action observable long enough to stop a real executor after
# started_at is durable. Candidate changes still differ by only one source file
# and remain well below the task's fixed 16 MiB patch ceiling.
zap_write_source() {
    local path="$1" value="$2" i
    {
        printf 'int x(void) { return %s; }\n' "$value"
        i=0
        while [ "$i" -lt 1800 ]; do
            printf 'static int zap_%05d(int v) { return v + %d; }\n' "$i" "$i"
            i=$((i + 1))
        done
    } >"$path"
}
zap_write_source "$ZAP_PROJECT/src/x.c" 1

zap_field() {
    local expression="$1"
    python3 -c "import json,sys; d=json.load(sys.stdin); print($expression)"
}

zap_submit() {
    local node="$1" value="$2" goal="$3" max_cpu="${4:-600}"
    local start handoff candidate result started_ms finished_ms elapsed_ms
    local ok action submit_us feedback_us event request work
    start="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work start \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"goal\":\"$goal\",\"profile\":\"quick\",\"max_cpu_seconds\":$max_cpu}" || true)"
    ok="$(printf '%s' "$start" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    [ "$ok" = True ] || dht_die "node $node could not start async work: $start"
    work="$(printf '%s' "$start" | zap_field 'd["data"]["work_id"]')"
    handoff="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work run \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"work\":\"$work\",\"adapter\":\"manual\"}" || true)"
    candidate="$(printf '%s' "$handoff" | zap_field 'd["data"]["candidate_workspace"]' 2>/dev/null || true)"
    [ -d "$candidate/src" ] || dht_die "node $node did not materialize its candidate: $handoff"
    zap_write_source "$candidate/src/x.c" "$value"
    started_ms="$(date +%s%3N)"
    result="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work run \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"work\":\"$work\",\"adapter\":\"manual\",\"datadir\":\"${DDS[$node]}\"}" || true)"
    printf '%s\n' "$start" >"$DHT_WORK/async-submit-${node}-${work}-start.json"
    printf '%s\n' "$handoff" >"$DHT_WORK/async-submit-${node}-${work}-handoff.json"
    printf '%s\n' "$result" >"$DHT_WORK/async-submit-${node}-${work}-result.json"
    finished_ms="$(date +%s%3N)"; elapsed_ms=$((finished_ms - started_ms))
    ok="$(printf '%s' "$result" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    [ "$ok" = True ] || dht_die "node $node foreground admission failed: $result"
    action="$(printf '%s' "$result" | zap_field 'd["data"]["expert"]["action_id"]')"
    event="$(printf '%s' "$result" | zap_field 'd["data"]["async_proof_event_root"]')"
    request="$(printf '%s' "$result" | zap_field 'd["data"]["remote_request_id"]')"
    submit_us="$(printf '%s' "$result" | zap_field 'd["data"]["local_submit_us"]')"
    feedback_us="$(printf '%s' "$result" | zap_field 'd["data"]["local_first_feedback_us"]')"
    [ "${#action}" -eq 64 ] && [ "${#event}" -eq 64 ] &&
        [ "$request" -gt 0 ] && [ "$submit_us" -ge 0 ] &&
        [ "$feedback_us" -ge "$submit_us" ] ||
        dht_die "node $node response omitted root-bound async identity: $result"
    [ "$elapsed_ms" -lt 30000 ] ||
        dht_die "node $node foreground crossed 30s latency firewall: ${elapsed_ms}ms"
    [ "$(zap_sql_count "$node" "SELECT count(*) FROM build_actions WHERE action_id='$action'")" -eq 1 ] &&
    [ "$(zap_sql_count "$node" "SELECT count(*) FROM build_proof_events WHERE action_id='$action' AND state='REQUESTED'")" -eq 1 ] ||
        dht_die "node $node returned action $action for $work without owning its exact action/proof rows"
    ZAP_ACTION="$action"; ZAP_WORK="$work"; ZAP_FOREGROUND_MS="$elapsed_ms"
    ZAP_SUBMIT_US="$submit_us"; ZAP_FEEDBACK_US="$feedback_us"
}

zap_sql_count() {
    local node="$1" sql="$2" out
    out="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" \
        core storage query --sql="$sql" 2>/dev/null || true)"
    printf '%s' "$out" | zap_field 'int(d["data"]["rows"][0][0])' 2>/dev/null || printf '%s' -1
}

zap_sql_value() {
    local node="$1" sql="$2" out
    out="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" \
        core storage query --sql="$sql" 2>/dev/null || true)"
    printf '%s' "$out" | zap_field 'str(d["data"]["rows"][0][0])' 2>/dev/null || true
}

zap_approve_action_receipt() {
    local requester="$1" action="$2" identity worker pubkey response ok changed
    identity="$(zap_sql_value "$requester" "SELECT r.worker_id||':'||w.signer_pubkey FROM build_receipts r JOIN build_workers w ON w.worker_id=r.worker_id WHERE r.action_id='$action' AND r.trust_state='REMOTE_OBSERVED' AND w.approved=0 AND w.revoked=0 LIMIT 1")"
    [ -n "$identity" ] || return 1
    worker="${identity%%:*}"; pubkey="${identity#*:}"
    [ "${#worker}" -eq 64 ] && [ "${#pubkey}" -eq 64 ] ||
        dht_die "action $action did not project its exact remote signer"
    response="$(dht_native "${DDS[$requester]}" "${RPCS[$requester]}" \
        metaverse build worker approve \
        --input="{\"worker_id\":\"$worker\",\"signer_pubkey\":\"$pubkey\",\"capabilities\":\"p2p-approved,c23.package.recipe.v1\",\"datadir\":\"${DDS[$requester]}\"}" || true)"
    ok="$(printf '%s' "$response" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    [ "$ok" = True ] || dht_die "requester $requester refused action $action signer approval: $response"
    changed="$(printf '%s' "$response" | zap_field 'd["data"]["worker_id"]+":"+d["data"]["signer_pubkey"]+":"+str(d["data"]["approved"])' 2>/dev/null || true)"
    [ "$changed" = "$worker:$pubkey:True" ] ||
        dht_die "approval response did not name the exact worker owning action $action: $response"
}

zap_assert_requester_did_not_execute() {
    local requester="$1" action="$2"
    [ "$(zap_sql_count "$requester" "SELECT count(*) FROM build_actions WHERE action_id='$action' AND state='SNAPSHOTTED' AND attempt_count=0 AND started_at=0 AND length(worker_id)=0")" -eq 1 ] ||
        dht_die "requester $requester raced peer execution for $action"
}

zap_assert_responsive() {
    local node="$1" phase="$2" started_ms response elapsed_ms
    started_ms="$(date +%s%3N)"
    response="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" status || true)"
    elapsed_ms=$(( $(date +%s%3N) - started_ms ))
    [ -n "$response" ] && [ "$elapsed_ms" -lt 5000 ] ||
        dht_die "requester $node stopped responding during $phase (${elapsed_ms}ms): $response"
    if [ -n "${ZAP_A_DB_IDENTITIES:-}" ] && [ "$node" = "${ZAP_A:-}" ]; then
        zap_assert_db_identity "$node" "$phase"
    fi
    printf '%s,%s\n' "$phase" "$elapsed_ms" >>"$DHT_WORK/async-requester-responsiveness.csv"
}

zap_db_identity() {
    local node="$1" dd
    dd="${DDS[$node]}"
    stat -Lc '%d:%i' "$dd/node.db" "$dd/node.db-wal" "$dd/node.db-shm" \
        2>/dev/null | paste -sd, -
}

zap_assert_db_identity() {
    local node="$1" phase="$2" current
    current="$(zap_db_identity "$node" || true)"
    [ -n "$current" ] && [ "$current" = "$ZAP_A_DB_IDENTITIES" ] ||
        dht_die "requester $node database/WAL identity changed during $phase: expected=$ZAP_A_DB_IDENTITIES actual=$current"
    ! ls -l "/proc/${PIDS[$node]}/fd" 2>/dev/null |
        grep -Eq 'node\.db(-wal|-shm)? \(deleted\)' ||
        dht_die "requester $node retained a deleted live database descriptor during $phase"
}

zap_assert_db_lifetime_clean() {
    local node="$1" phase="$2" log
    log="${DDS[$node]}/node.log"
    zap_assert_db_identity "$node" "$phase"
    [ "$(zap_sql_count "$node" 'SELECT 1')" -eq 1 ] ||
        dht_die "requester $node could not read its live database after $phase"
    ! grep -Eq 'unauthorized=1|DATABASE_OWNERSHIP_CONFLICT|disk I/O error' \
        "$log" ||
        dht_die "requester $node recorded an unauthorized database lifecycle event during $phase"
}

zap_latest_state() {
    local node="$1" action="$2" sql out
    sql="SELECT state FROM build_proof_events WHERE action_id='$action' ORDER BY rowid DESC LIMIT 1"
    out="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" \
        core storage query --sql="$sql" 2>/dev/null || true)"
    printf '%s' "$out" | zap_field 'd["data"]["rows"][0][0]' 2>/dev/null || true
}

zap_wait_ready() {
    local node="$1" action="$2" deadline state bound
    deadline=$(( $(date +%s) + 180 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        state="$(zap_latest_state "$node" "$action")"
        if [ "$state" = READY_FOR_ACCEPTANCE ]; then
            bound="$(zap_sql_count "$node" "SELECT count(*) FROM build_receipts r JOIN build_workers w ON w.worker_id=r.worker_id WHERE r.action_id='$action' AND r.trust_state IN ('REMOTE_OBSERVED','QUORUM_MATCHED') AND w.approved=1 AND w.revoked=0 AND r.worker_id=w.worker_id")"
            [ "$bound" -eq 1 ] ||
                dht_die "readiness was not owned by the exact approved receipt worker for $action"
            return 0
        fi
        # Approval is selected from the canonical action receipt on every
        # observation. Once that exact worker is approved, the query returns
        # no row and this is a no-op; there is no harness-side lifecycle bit.
        zap_approve_action_receipt "$node" "$action" || true
        zap_assert_responsive "$node" "pending-$action"
        sleep 1
    done
    return 1
}

zap_wait_executor_started() {
    local node="$1" action="$2" deadline count accepted
    deadline=$(( $(date +%s) + 90 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        count="$(zap_sql_count "$node" "SELECT count(*) FROM build_actions WHERE action_id='$action' AND started_at>0 AND state IN ('RUNNING','VERIFYING')")"
        [ "$count" -eq 1 ] 2>/dev/null && return 0
        accepted="$(zap_sql_count "$node" "SELECT count(*) FROM build_actions WHERE action_id='$action' AND state IN ('ACCEPTED','CACHE_HIT','FAILED')")"
        [ "$accepted" -eq 0 ] 2>/dev/null || return 2
        sleep 0.1
    done
    return 1
}

zap_stop_executor_mid_action() {
    local node="$1" deadline child
    deadline=$(( $(date +%s) + 90 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        child="$(pgrep -P "${PIDS[$node]}" | head -1 || true)"
        if [ -n "$child" ]; then
            kill -STOP "-${PIDS[$node]}" ||
                dht_die "could not stop executor $node with live action child $child"
            return 0
        fi
        sleep 0.01
    done
    return 1
}

zap_assert_evidence() {
    local node="$1" action="$2" evidence
    evidence="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode evidence \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"datadir\":\"${DDS[$node]}\",\"action_id\":\"$action\"}")"
    printf '%s\n' "$evidence" >"$DHT_WORK/async-evidence-$action.json"
    python3 - "$evidence" <<'PY' || dht_die "async timing/evidence report was incomplete: $evidence"
import json,sys
d=json.loads(sys.argv[1]); assert d.get("ok") is True,d
x=d["data"]; assert x["async_timings_available"] is True,x
t=x["latency"]
keys=("local_submit_us","peer_discovery_us","transfer_us","remote_queue_us",
      "remote_execution_us","receipt_verification_us","total_background_proof_us")
assert all(isinstance(t.get(k),int) and t[k]>=0 for k in keys),t
assert x["policy_satisfied"] is True and x["authority"]!="UNTRUSTED",x
PY
    local bound mismatched
    bound="$(zap_sql_count "$node" "SELECT count(*) FROM build_proof_events e JOIN build_actions a ON a.action_id=e.action_id JOIN build_jobs j ON j.job_id=a.job_id WHERE e.action_id='$action' AND length(e.source_root_sha3)=64 AND e.source_root_sha3=j.source_cas_sha3")"
    mismatched="$(zap_sql_count "$node" "SELECT count(*) FROM build_proof_events e JOIN build_actions a ON a.action_id=e.action_id JOIN build_jobs j ON j.job_id=a.job_id WHERE e.action_id='$action' AND e.source_root_sha3<>j.source_cas_sha3")"
    [ "$bound" -ge 8 ] && [ "$mismatched" -eq 0 ] ||
        dht_die "async events lost direct source-root binding for $action"
}

zap_assert_same_action_identity() {
    local requester="$1" executor="$2" action="$3" sql requester_id executor_id
    sql="SELECT a.task_root_sha3||':'||a.candidate_root_sha3||':'||j.source_cas_sha3||':'||a.input_root_sha3||':'||j.toolchain_sha3 FROM build_actions a JOIN build_jobs j ON j.job_id=a.job_id WHERE a.action_id='$action'"
    requester_id="$(zap_sql_value "$requester" "$sql")"
    executor_id="$(zap_sql_value "$executor" "$sql")"
    [ -n "$requester_id" ] && [ "$requester_id" = "$executor_id" ] ||
        dht_die "executor $executor did not receive the exact immutable identity for $action"
}

zap_assert_receipt_bindings() {
    local requester="$1" executor="$2" action="$3" bad total lease_bound
    total="$(zap_sql_count "$requester" "SELECT count(*) FROM build_receipts WHERE action_id='$action'")"
    bad="$(zap_sql_count "$requester" "SELECT count(*) FROM build_receipts r JOIN build_actions a ON a.action_id=r.action_id JOIN build_jobs j ON j.job_id=a.job_id JOIN build_workers w ON w.worker_id=r.worker_id WHERE r.action_id='$action' AND (r.job_id<>a.job_id OR r.action_sha3<>a.action_id OR length(a.task_root_sha3)<>64 OR length(a.candidate_root_sha3)<>64 OR length(j.source_cas_sha3)<>64 OR length(a.input_root_sha3)<>64 OR length(a.proof_policy_root_sha3)<>64 OR length(j.toolchain_sha3)<>64 OR length(r.worker_id)<>64 OR length(w.signer_pubkey)<>64 OR length(r.lease_id)<>64 OR length(r.output_sha3)<>64 OR length(r.work_receipt_sha3)<>64 OR length(r.signature)<>128 OR length(r.confinement)=0)")"
    [ "$total" -ge 1 ] && [ "$bad" -eq 0 ] ||
        dht_die "requester receipt projection lost an exact authority binding for $action"
    lease_bound="$(zap_sql_count "$executor" "SELECT count(*) FROM build_receipts r JOIN build_actions a ON a.action_id=r.action_id JOIN build_jobs j ON j.job_id=a.job_id JOIN build_workers w ON w.worker_id=r.worker_id WHERE r.action_id='$action' AND r.job_id=a.job_id AND r.action_sha3=a.action_id AND r.lease_id=a.lease_id AND r.output_sha3=a.output_root_sha3 AND length(a.task_root_sha3)=64 AND length(a.candidate_root_sha3)=64 AND length(j.source_cas_sha3)=64 AND length(a.input_root_sha3)=64 AND length(a.proof_policy_root_sha3)=64 AND length(j.toolchain_sha3)=64 AND length(w.signer_pubkey)=64 AND length(r.signature)=128")"
    [ "$lease_bound" -ge 1 ] ||
        dht_die "executor receipt did not bind its exact action lease/output for $action"
}

zap_assert_exact_reuse() {
    local requester="$1" executor="$2" action="$3" work="$4"
    local before_attempts before_actions before_requests before_receipts result ok state
    before_attempts="$(zap_sql_count "$executor" "SELECT attempt_count FROM build_actions WHERE action_id='$action'")"
    before_actions="$(zap_sql_count "$requester" "SELECT count(*) FROM build_actions WHERE action_id='$action'")"
    before_requests="$(zap_sql_count "$requester" "SELECT count(*) FROM build_proof_events WHERE action_id='$action' AND state='REQUESTED'")"
    before_receipts="$(zap_sql_count "$requester" "SELECT count(*) FROM build_receipts WHERE action_id='$action'")"
    result="$(dht_native "${DDS[$requester]}" "${RPCS[$requester]}" zcode work run \
        --input="{\"workspace\":\"$ZAP_PROJECT\",\"work\":\"$work\",\"adapter\":\"manual\",\"datadir\":\"${DDS[$requester]}\"}" || true)"
    ok="$(printf '%s' "$result" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    state="$(printf '%s' "$result" | zap_field 'd["data"]["state"]' 2>/dev/null || true)"
    [ "$ok" = True ] && [ "$state" = EVIDENCE_READY ] ||
        dht_die "exact reuse did not resolve through the canonical task projection: $result"
    [ "$(zap_sql_count "$executor" "SELECT attempt_count FROM build_actions WHERE action_id='$action'")" -eq "$before_attempts" ] &&
    [ "$(zap_sql_count "$requester" "SELECT count(*) FROM build_actions WHERE action_id='$action'")" -eq "$before_actions" ] &&
    [ "$(zap_sql_count "$requester" "SELECT count(*) FROM build_proof_events WHERE action_id='$action' AND state='REQUESTED'")" -eq "$before_requests" ] &&
    [ "$(zap_sql_count "$requester" "SELECT count(*) FROM build_receipts WHERE action_id='$action'")" -eq "$before_receipts" ] ||
        dht_die "exact action reuse scheduled duplicate computation for $action"
    printf '%s\n' 1 >>"$DHT_WORK/async-exact-reuse-eliminated.txt"
}

zap_dump_failure() {
    local node="$1" action="$2" sql
    sql="SELECT state,peer_id,deadline_at,elapsed_us,event_root FROM build_proof_events WHERE action_id='$action' ORDER BY rowid"
    dht_note "async proof lifecycle: $(dht_native "${DDS[$node]}" \
        "${RPCS[$node]}" core storage query --sql="$sql" 2>/dev/null || true)"
    dht_note "async proof log tail follows"
    tail -80 "${DDS[$node]}/node.log" >&2 || true
}

zap_start_node() {
    local node="$1" connect="${2:-}"
    if [ -n "$connect" ]; then
        PIDS[$node]="$(dht_spawn "${DDS[$node]}" "${PORTS[$node]}" \
            "${RPCS[$node]}" "${FSPORTS[$node]}" "${HTTPSPORTS[$node]}" \
            "127.0.0.1:${PORTS[$connect]}")"
    else
        PIDS[$node]="$(dht_spawn "${DDS[$node]}" "${PORTS[$node]}" \
            "${RPCS[$node]}" "${FSPORTS[$node]}" "${HTTPSPORTS[$node]}" \
            "127.0.0.1:$DEAD_SINK")"
    fi
    dht_wait_rpc "${DDS[$node]}" "${RPCS[$node]}" "${PIDS[$node]}" ||
        dht_die "async proof node $node failed to start"
}

zap_connect() {
    local from="$1" to="$2"
    dht_rpc "${DDS[$from]}" "${RPCS[$from]}" addnode \
        "\"127.0.0.1:${PORTS[$to]}\"" '"onetry"' >/dev/null || true
    dht_wait_auth "${DDS[$from]}" "${RPCS[$from]}" 1 &&
        dht_wait_auth "${DDS[$to]}" "${RPCS[$to]}" 1 ||
        dht_die "async proof nodes $from/$to did not authenticate"
}

# Collapse the prior seven-node discovery fixture to three equal full-node
# processes. No role-specific binary or configuration is introduced.
ZAP_A="$ORIGIN"; ZAP_B="$NEXT"; ZAP_C="$TARGET"
for i in 0 1 2 3 4 5 6; do
    dht_kill_group "${PIDS[$i]:-}"; PIDS[$i]=""
done
DHT_PGID_A=""; DHT_PGID_B=""; DHT_EXTRA_PGIDS=()
zap_start_node "$ZAP_B"
zap_start_node "$ZAP_A" "$ZAP_B"
zap_connect "$ZAP_A" "$ZAP_B"
DHT_EXTRA_PGIDS=("${PIDS[@]}")
A_ORIGINAL_PID="${PIDS[$ZAP_A]}"
ZAP_A_DB_IDENTITIES="$(zap_db_identity "$ZAP_A" || true)"
[ -n "$ZAP_A_DB_IDENTITIES" ] ||
    dht_die "A did not expose stable node.db/WAL/SHM identities after boot"
zap_assert_responsive "$ZAP_A" "before-first-admission"

dht_note "async proof: A stays live while B executes the first fixed action"
zap_submit "$ZAP_A" 2 "Change x to two"
FIRST_ACTION="$ZAP_ACTION"; FIRST_WORK="$ZAP_WORK"; FIRST_MS="$ZAP_FOREGROUND_MS"
zap_assert_responsive "$ZAP_A" "after-first-admission"
zap_wait_ready "$ZAP_A" "$FIRST_ACTION" ||
    { zap_dump_failure "$ZAP_A" "$FIRST_ACTION";
      dht_die "A did not reach READY_FOR_ACCEPTANCE from B evidence"; }
[ "$(zap_sql_count "$ZAP_B" "SELECT count(*) FROM build_actions WHERE action_id='$FIRST_ACTION' AND state IN ('ACCEPTED','CACHE_HIT')")" -eq 1 ] ||
    dht_die "B did not independently execute A's first fixed action"
[ "$(zap_sql_count "$ZAP_B" "SELECT count(*) FROM build_actions WHERE action_id='$FIRST_ACTION' AND attempt_count=1")" -eq 1 ] &&
[ "$(zap_sql_count "$ZAP_A" "SELECT count(*) FROM build_proof_events WHERE action_id='$FIRST_ACTION' AND state='REQUESTED'")" -eq 1 ] ||
    dht_die "exact first request did not deduplicate to one execution/request"
zap_assert_requester_did_not_execute "$ZAP_A" "$FIRST_ACTION"
zap_assert_same_action_identity "$ZAP_A" "$ZAP_B" "$FIRST_ACTION"
zap_assert_evidence "$ZAP_A" "$FIRST_ACTION"
zap_assert_receipt_bindings "$ZAP_A" "$ZAP_B" "$FIRST_ACTION"
zap_assert_exact_reuse "$ZAP_A" "$ZAP_B" "$FIRST_ACTION" "$FIRST_WORK"

dht_note "async proof: A remains the same process while C executes another action"
dht_kill_group "${PIDS[$ZAP_B]}"; PIDS[$ZAP_B]=""
sleep 2
zap_start_node "$ZAP_C"
zap_connect "$ZAP_A" "$ZAP_C"
[ "${PIDS[$ZAP_A]}" = "$A_ORIGINAL_PID" ] &&
    kill -0 "-$A_ORIGINAL_PID" 2>/dev/null ||
    dht_die "A did not continue operating while executor roles changed"
[ "$(zap_sql_count "$ZAP_C" "SELECT count(*) FROM build_actions WHERE action_id='$FIRST_ACTION'")" -eq 0 ] ||
    dht_die "C falsely inherited B's first action"
zap_submit "$ZAP_A" 3 "Change x to three"
SECOND_ACTION="$ZAP_ACTION"; SECOND_WORK="$ZAP_WORK"; SECOND_MS="$ZAP_FOREGROUND_MS"
[ "$SECOND_ACTION" != "$FIRST_ACTION" ] || dht_die "distinct candidates aliased"
[ "$SECOND_WORK" != "$FIRST_WORK" ] || dht_die "distinct tasks aliased"
[ "$(zap_sql_count "$ZAP_A" "SELECT count(DISTINCT task_root_sha3) FROM build_actions WHERE action_id IN ('$FIRST_ACTION','$SECOND_ACTION')")" -eq 2 ] &&
[ "$(zap_sql_count "$ZAP_A" "SELECT count(DISTINCT candidate_root_sha3) FROM build_actions WHERE action_id IN ('$FIRST_ACTION','$SECOND_ACTION')")" -eq 2 ] ||
    dht_die "exact-action dedup conflated different task/candidate authority"
zap_wait_ready "$ZAP_A" "$SECOND_ACTION" ||
    { zap_dump_failure "$ZAP_A" "$SECOND_ACTION";
      dht_die "A did not reach READY_FOR_ACCEPTANCE from C evidence"; }
[ "$(zap_sql_count "$ZAP_C" "SELECT count(*) FROM build_actions WHERE action_id='$SECOND_ACTION' AND state IN ('ACCEPTED','CACHE_HIT')")" -eq 1 ] ||
    dht_die "C did not independently execute A's second fixed action"
zap_assert_requester_did_not_execute "$ZAP_A" "$SECOND_ACTION"
zap_assert_same_action_identity "$ZAP_A" "$ZAP_C" "$SECOND_ACTION"
zap_assert_evidence "$ZAP_A" "$SECOND_ACTION"
zap_assert_receipt_bindings "$ZAP_A" "$ZAP_C" "$SECOND_ACTION"
zap_assert_db_lifetime_clean "$ZAP_A" "B-to-C executor replacement"

dht_note "async proof: B dies after started_at; lease retry moves exact work to C"
dht_kill_group "${PIDS[$ZAP_C]}"; PIDS[$ZAP_C]=""
sleep 2
zap_start_node "$ZAP_B"
zap_connect "$ZAP_A" "$ZAP_B"
zap_submit "$ZAP_A" 4 "Change x to four with lease recovery" 10
RETRY_ACTION="$ZAP_ACTION"; RETRY_MS="$ZAP_FOREGROUND_MS"
STALE_B_WORKER="$(zap_sql_value "$ZAP_B" "SELECT worker_id FROM build_workers WHERE approved=1 AND revoked=0 AND capabilities LIKE '%c23.package.recipe.v1%' ORDER BY last_seen_at DESC LIMIT 1")"
[ "${#STALE_B_WORKER}" -eq 64 ] || dht_die "B's stale worker identity was not durable"
zap_stop_executor_mid_action "$ZAP_B" ||
    dht_die "B did not spawn the fixed package action before the death probe"
zap_assert_responsive "$ZAP_A" "B-hard-stopped-mid-action"
zap_start_node "$ZAP_C"
zap_connect "$ZAP_A" "$ZAP_C"
zap_wait_ready "$ZAP_A" "$RETRY_ACTION" || {
    kill -CONT "-${PIDS[$ZAP_B]}" 2>/dev/null || true
    zap_dump_failure "$ZAP_A" "$RETRY_ACTION"
    dht_die "B lease loss did not retry the exact action on C"
}
[ "$(zap_sql_count "$ZAP_C" "SELECT count(*) FROM build_actions WHERE action_id='$RETRY_ACTION' AND state IN ('ACCEPTED','CACHE_HIT')")" -eq 1 ] ||
    dht_die "C did not execute the retried exact action"
zap_assert_requester_did_not_execute "$ZAP_A" "$RETRY_ACTION"
WINNING_C_WORKER="$(zap_sql_value "$ZAP_C" "SELECT worker_id FROM build_actions WHERE action_id='$RETRY_ACTION'")"
[ "${#WINNING_C_WORKER}" -eq 64 ] && [ "$WINNING_C_WORKER" != "$STALE_B_WORKER" ] ||
    dht_die "retry did not move the exact action to C's distinct worker"
zap_assert_same_action_identity "$ZAP_A" "$ZAP_C" "$RETRY_ACTION"
[ "$(zap_sql_count "$ZAP_A" "SELECT count(DISTINCT peer_id) FROM build_proof_events WHERE action_id='$RETRY_ACTION' AND peer_id<>0")" -ge 2 ] &&
[ "$(zap_sql_count "$ZAP_A" "SELECT count(*) FROM build_proof_events WHERE action_id='$RETRY_ACTION' AND state='PEER_DISCOVERED'")" -ge 2 ] ||
    dht_die "retry evidence did not preserve both peer leases"
STALE_EVENT_COUNT="$(zap_sql_count "$ZAP_A" "SELECT count(*) FROM build_proof_events WHERE action_id='$RETRY_ACTION'")"
kill -CONT "-${PIDS[$ZAP_B]}" || dht_die "could not resume stale B"
STALE_DEADLINE=$(( $(date +%s) + 90 ))
STALE_REFUSAL=0
while [ "$(date +%s)" -lt "$STALE_DEADLINE" ]; do
    [ "$(zap_sql_count "$ZAP_A" "SELECT count(*) FROM build_receipts WHERE action_id='$RETRY_ACTION' AND worker_id='$STALE_B_WORKER'")" -eq 0 ] ||
        dht_die "stale B entered A's receipt set while its late result was pending"
    if [ "$(zap_sql_count "$ZAP_B" "SELECT count(*) FROM build_actions WHERE action_id='$RETRY_ACTION' AND state IN ('ACCEPTED','CACHE_HIT','FAILED','LOCAL_FALLBACK')")" -eq 1 ] &&
       grep -Eq "result [0-9]+: work-lease-expired" \
           "${DDS[$ZAP_B]}/node.log"; then
        STALE_REFUSAL=1
        break
    fi
    zap_assert_responsive "$ZAP_A" "stale-B-finishing"
    sleep 1
done
[ "$STALE_REFUSAL" -eq 1 ] ||
    dht_die "stale B did not finish and produce a named late-result refusal"
[ "$(zap_sql_count "$ZAP_B" "SELECT count(*) FROM build_actions WHERE action_id='$RETRY_ACTION' AND worker_id='$STALE_B_WORKER' AND started_at>0 AND attempt_count=1")" -eq 1 ] ||
    dht_die "B was not durably stopped inside the exact leased action"
[ "$(zap_latest_state "$ZAP_A" "$RETRY_ACTION")" = READY_FOR_ACCEPTANCE ] &&
[ "$(zap_sql_count "$ZAP_A" "SELECT count(*) FROM build_proof_events WHERE action_id='$RETRY_ACTION'")" -eq "$STALE_EVENT_COUNT" ] ||
    dht_die "stale B advanced A's proof projection after losing its lease"
[ "$(zap_sql_count "$ZAP_A" "SELECT count(*) FROM build_receipts WHERE action_id='$RETRY_ACTION' AND worker_id='$WINNING_C_WORKER'")" -eq 1 ] &&
[ "$(zap_sql_count "$ZAP_A" "SELECT count(*) FROM build_receipts WHERE action_id='$RETRY_ACTION' AND worker_id='$STALE_B_WORKER'")" -eq 0 ] ||
    dht_die "stale B result entered A's receipt set or C's winning receipt was lost"
if ! grep -Eq "result [0-9]+: work-lease-expired" \
        "${DDS[$ZAP_B]}/node.log"; then
    dht_die "stale B did not produce the named work-lease-expired refusal"
fi
zap_assert_evidence "$ZAP_A" "$RETRY_ACTION"
zap_assert_receipt_bindings "$ZAP_A" "$ZAP_C" "$RETRY_ACTION"
zap_assert_db_lifetime_clean "$ZAP_A" "lease loss, C takeover, and stale-B refusal"

# A is only a task role. Kill it, then use the unchanged full-node code on B
# to originate and C to execute one final request.
dht_note "async proof: kill A; B originates through the same full-node path"
zap_assert_db_lifetime_clean "$ZAP_A" "complete A requester lifecycle"
dht_kill_group "${PIDS[$ZAP_A]}"; PIDS[$ZAP_A]=""
sleep 2
zap_connect "$ZAP_B" "$ZAP_C"
zap_submit "$ZAP_B" 5 "Change x to five after A disappears"
FINAL_ACTION="$ZAP_ACTION"; FINAL_MS="$ZAP_FOREGROUND_MS"
zap_wait_ready "$ZAP_B" "$FINAL_ACTION" ||
    { zap_dump_failure "$ZAP_B" "$FINAL_ACTION";
      dht_die "B did not originate successfully after A disappeared"; }
[ "$(zap_sql_count "$ZAP_C" "SELECT count(*) FROM build_actions WHERE action_id='$FINAL_ACTION' AND state IN ('ACCEPTED','CACHE_HIT')")" -eq 1 ] ||
    dht_die "C did not execute B's post-A action"
zap_assert_requester_did_not_execute "$ZAP_B" "$FINAL_ACTION"
zap_assert_same_action_identity "$ZAP_B" "$ZAP_C" "$FINAL_ACTION"
zap_assert_evidence "$ZAP_B" "$FINAL_ACTION"
zap_assert_receipt_bindings "$ZAP_B" "$ZAP_C" "$FINAL_ACTION"

dht_note "async proof PASS: actions=$FIRST_ACTION,$SECOND_ACTION,$RETRY_ACTION,$FINAL_ACTION foreground_ms=$FIRST_MS,$SECOND_MS,$RETRY_MS,$FINAL_MS github_contacted=false equal_full_nodes=true"
