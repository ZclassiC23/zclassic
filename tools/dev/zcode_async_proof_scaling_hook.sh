#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Real three-node async-proof scaling characterization.  This is observation
# only: task/action/receipt rows remain the canonical lifecycle authority.

ZAP_HELPERS_ONLY=1
# shellcheck source=tools/dev/zcode_async_proof_acceptance_hook.sh
. "$SCRIPT_DIR/zcode_async_proof_acceptance_hook.sh"
unset ZAP_HELPERS_ONLY

ZAPS_ROOT="$DHT_WORK/async-proof-scale-projects"
ZAPS_SUMMARY="$DHT_WORK/async-proof-scaling.csv"
ZAPS_RESPONSIVENESS="$DHT_WORK/async-proof-responsiveness.csv"
mkdir -p "$ZAPS_ROOT"
printf '%s\n' \
    'campaign,slot,requester,executor,action,foreground_ms,background_ms,requester_cpu_ms' \
    >"$ZAPS_SUMMARY"
printf '%s\n' 'phase,surface,node,elapsed_ms,ok' \
    >"$ZAPS_RESPONSIVENESS"

zaps_probe_one() {
    local phase="$1" surface="$2" node="$3" started_ns response ok elapsed_ms
    shift 3
    started_ns="$(date +%s%N)"
    response="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" "$@" || true)"
    elapsed_ms=$((($(date +%s%N)-started_ns)/1000000))
    ok="$(printf '%s' "$response" | zap_field 'd.get("ok",False)' \
        2>/dev/null || true)"
    printf '%s,%s,%s,%s,%s\n' \
        "$phase" "$surface" "$node" "$elapsed_ms" "$ok" \
        >>"$ZAPS_RESPONSIVENESS"
}

zaps_probe_rounds() {
    local phase="$1" rounds="$2" round node
    for ((round=0; round<rounds; round++)); do
        for node in "$ZAP_A" "$ZAP_B" "$ZAP_C"; do
            zaps_probe_one "$phase" chain "$node" core status
            zaps_probe_one "$phase" peer "$node" core network peers list
            zaps_probe_one "$phase" sync "$node" core sync status
            zaps_probe_one "$phase" command "$node" \
                ops state --subsystem=supervisor
        done
    done
}

zaps_proc_cpu_ticks() {
    local node="$1"
    awk '{print $14+$15}' "/proc/${PIDS[$node]}/stat"
}

zaps_project_create() {
    local slot="$1" project="$ZAPS_ROOT/$slot"
    mkdir -p "$project/src" "$project/include" "$project/tests"
    printf '%s\n' 'MIT' >"$project/LICENSE"
    printf '%s\n' 'int x(void);' >"$project/include/x.h"
    printf '%s\n' 'int main(void) { return 0; }' >"$project/tests/test.c"
    printf '%s\n' \
        "{\"schema\":1,\"name\":\"acceptance/async-proof-$slot\",\"semver\":\"0.1.0\",\"language\":\"c23\",\"license\":\"MIT\",\"include_dir\":\"include\",\"source_dir\":\"src\",\"dependencies\":[]}" \
        >"$project/zcode-package.json"
    zap_write_source "$project/src/x.c" 1
}

zaps_prepare() {
    local campaign="$1" slot="$2" node="$3" value="$4"
    local project="$ZAPS_ROOT/$slot" start handoff ok work candidate
    zaps_project_create "$slot"
    start="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work start \
        --input="{\"workspace\":\"$project\",\"goal\":\"Scale $campaign immutable action $slot\",\"profile\":\"quick\",\"max_cpu_seconds\":10}" || true)"
    ok="$(printf '%s' "$start" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    [ "$ok" = True ] || dht_die "scale $slot could not start: $start"
    work="$(printf '%s' "$start" | zap_field 'd["data"]["work_id"]')"
    handoff="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work run \
        --input="{\"workspace\":\"$project\",\"work\":\"$work\",\"adapter\":\"manual\"}" || true)"
    candidate="$(printf '%s' "$handoff" | zap_field 'd["data"]["candidate_workspace"]' 2>/dev/null || true)"
    [ -d "$candidate/src" ] || dht_die "scale $slot candidate unavailable: $handoff"
    zap_write_source "$candidate/src/x.c" "$value"
    printf '%s\n%s\n%s\n%s\n' "$campaign" "$node" "$work" "$project" \
        >"$DHT_WORK/scale-$slot.meta"
}

zaps_admit() {
    local slot="$1" campaign node work project result ok action start_ns end_ns
    local cpu_before
    mapfile -t meta <"$DHT_WORK/scale-$slot.meta"
    campaign="${meta[0]}"; node="${meta[1]}"; work="${meta[2]}"; project="${meta[3]}"
    cpu_before="$(zaps_proc_cpu_ticks "$node")"
    start_ns="$(date +%s%N)"
    result="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work run \
        --input="{\"workspace\":\"$project\",\"work\":\"$work\",\"adapter\":\"manual\",\"datadir\":\"${DDS[$node]}\"}" || true)"
    end_ns="$(date +%s%N)"
    ok="$(printf '%s' "$result" | zap_field 'd.get("ok",False)' 2>/dev/null || true)"
    [ "$ok" = True ] || dht_die "scale $slot foreground admission failed: $result"
    action="$(printf '%s' "$result" | zap_field 'd["data"]["expert"]["action_id"]')"
    [ "${#action}" -eq 64 ] || dht_die "scale $slot omitted immutable action identity"
    printf '%s\n' "$result" >"$DHT_WORK/async-submit-scale-${node}-${work}-result.json"
    printf '%s\n%s\n%s\n%s\n' "$action" "$(((end_ns-start_ns)/1000000))" \
        "$start_ns" "$cpu_before" \
        >"$DHT_WORK/scale-$slot.admitted"
}

zaps_admit_parallel() {
    local slot pid failed=0
    local pids=()
    for slot in "$@"; do
        zaps_admit "$slot" & pids+=("$!")
    done
    for pid in "${pids[@]}"; do
        wait "$pid" || failed=1
    done
    [ "$failed" -eq 0 ] || dht_die "parallel foreground admission failed"
}

zaps_executor() {
    local action="$1" node
    for node in "$ZAP_A" "$ZAP_B" "$ZAP_C"; do
        if [ "$(zap_sql_count "$node" "SELECT count(*) FROM build_actions WHERE action_id='$action' AND state IN ('ACCEPTED','CACHE_HIT') AND attempt_count>0")" -eq 1 ]; then
            printf '%s' "$node"
            return 0
        fi
    done
    return 1
}

zaps_finish() {
    local slot="$1" campaign node work project action foreground_ms
    local started_ns finished_ns background_ms executor cpu_before cpu_after hz cpu_ms
    mapfile -t meta <"$DHT_WORK/scale-$slot.meta"
    mapfile -t admitted <"$DHT_WORK/scale-$slot.admitted"
    campaign="${meta[0]}"; node="${meta[1]}"; work="${meta[2]}"; project="${meta[3]}"
    action="${admitted[0]}"; foreground_ms="${admitted[1]}"
    started_ns="${admitted[2]}"
    cpu_before="${admitted[3]}"
    zap_wait_ready "$node" "$action" ||
        { zap_dump_failure "$node" "$action"; dht_die "scale $slot did not become ready"; }
    finished_ns="$(date +%s%N)"
    cpu_after="$(zaps_proc_cpu_ticks "$node")"
    hz="$(getconf CLK_TCK)"
    background_ms=$(((finished_ns-started_ns)/1000000))
    cpu_ms=$(((cpu_after-cpu_before)*1000/hz))
    executor="$(zaps_executor "$action" || true)"
    [ -n "$executor" ] && [ "$executor" != "$node" ] ||
        dht_die "scale $slot was not independently executed: requester=$node executor=$executor"
    zap_assert_requester_did_not_execute "$node" "$action"
    zap_assert_same_action_identity "$node" "$executor" "$action"
    zap_assert_receipt_bindings "$node" "$executor" "$action"
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$campaign" "$slot" "$node" "$executor" "$action" \
        "$foreground_ms" "$background_ms" "$cpu_ms" >>"$ZAPS_SUMMARY"
}

zaps_run_parallel() {
    local slot
    zaps_admit_parallel "$@"
    for slot in "$@"; do zaps_finish "$slot"; done
}

# Collapse the discovery fixture to three equal full nodes.  The processes,
# flags, and topology are identical; only immutable task ownership varies.
ZAP_A="$ORIGIN"; ZAP_B="$NEXT"; ZAP_C="$TARGET"
for i in 0 1 2 3 4 5 6; do
    dht_kill_group "${PIDS[$i]:-}"; PIDS[$i]=""
done
DHT_PGID_A=""; DHT_PGID_B=""; DHT_EXTRA_PGIDS=()
zap_start_node "$ZAP_A"
zap_start_node "$ZAP_B" "$ZAP_A"
zap_start_node "$ZAP_C" "$ZAP_A"
zap_connect "$ZAP_A" "$ZAP_B"
zap_connect "$ZAP_A" "$ZAP_C"
zap_connect "$ZAP_B" "$ZAP_C"
DHT_EXTRA_PGIDS=("${PIDS[@]}")

# Establish each read surface's same-process idle distribution before proof
# traffic. The loaded phase below runs the identical probes while three
# independent contexts transfer and three confined builds execute.
zaps_probe_rounds idle 3

dht_note "async proof scaling: one requester, one immutable action"
zaps_prepare one one-1 "$ZAP_A" 11
zaps_run_parallel one-1

dht_note "async proof scaling: one requester, two simultaneous immutable actions"
zaps_prepare two two-1 "$ZAP_A" 21
zaps_prepare two two-2 "$ZAP_A" 22
zaps_run_parallel two-1 two-2

dht_note "async proof scaling: three simultaneous requesters/actions"
zaps_prepare three three-a "$ZAP_A" 31
zaps_prepare three three-b "$ZAP_B" 32
zaps_prepare three three-c "$ZAP_C" 33
zaps_probe_rounds loaded 6 & ZAPS_PROBE_PID="$!"
zaps_run_parallel three-a three-b three-c
wait "$ZAPS_PROBE_PID" || dht_die "loaded responsiveness probes failed"

# Exact reuse stays a projection of the first canonical task/action/receipt.
mapfile -t one_meta <"$DHT_WORK/scale-one-1.meta"
mapfile -t one_admitted <"$DHT_WORK/scale-one-1.admitted"
ZAP_PROJECT="${one_meta[3]}"
zap_assert_exact_reuse "${one_meta[1]}" \
    "$(zaps_executor "${one_admitted[0]}")" "${one_admitted[0]}" "${one_meta[2]}"

"$SCRIPT_DIR/zcode_async_proof_perf_report.sh" "$DHT_WORK" \
    >"$DHT_WORK/async-proof-scaling-performance-report.txt" ||
    dht_die "scaling performance report was incomplete"

python3 - "$ZAPS_SUMMARY" <<'PY' || dht_die "scaling campaign receipt failed"
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
assert len(rows)==6, rows
assert [r["campaign"] for r in rows].count("one")==1
assert [r["campaign"] for r in rows].count("two")==2
assert [r["campaign"] for r in rows].count("three")==3
assert len({r["action"] for r in rows})==6
assert all(int(r["foreground_ms"]) < 30000 for r in rows)
assert all(r["requester"] != r["executor"] for r in rows)
PY

python3 - "$ZAPS_RESPONSIVENESS" <<'PY' \
    >"$DHT_WORK/async-proof-responsiveness-report.json" || \
    dht_die "background proof responsiveness contract failed"
import csv,json,math,sys
rows=list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
surfaces=("chain","peer","sync","command")
def percentile(values, pct):
    values=sorted(values)
    return values[max(0, math.ceil(len(values)*pct)-1)]
report={"schema":"zcl.async_proof_responsiveness.v1",
        "probe_client_processes":len(rows),"surfaces":{}}
assert rows and all(r["ok"]=="True" for r in rows), rows
for surface in surfaces:
    idle=[int(r["elapsed_ms"]) for r in rows
          if r["phase"]=="idle" and r["surface"]==surface]
    loaded=[int(r["elapsed_ms"]) for r in rows
            if r["phase"]=="loaded" and r["surface"]==surface]
    assert len(idle)==9 and len(loaded)==18, (surface,len(idle),len(loaded))
    idle_p50,idle_p95=percentile(idle,.50),percentile(idle,.95)
    loaded_p50,loaded_p95=percentile(loaded,.50),percentile(loaded,.95)
    # One-CPU/one-slot proof workers may consume background time, but every
    # public-node read surface retains a tight absolute and relative bound.
    assert loaded_p95 < 1000, (surface,loaded_p95)
    assert loaded_p95 <= idle_p95 + 500, (surface,idle_p95,loaded_p95)
    report["surfaces"][surface]={
        "idle_samples":len(idle),"loaded_samples":len(loaded),
        "idle_p50_ms":idle_p50,"idle_p95_ms":idle_p95,
        "loaded_p50_ms":loaded_p50,"loaded_p95_ms":loaded_p95,
        "p95_added_ms":loaded_p95-idle_p95}
report.update({"simultaneous_requesters":3,"simultaneous_actions":3,
               "worker_slots_per_node":1,"action_cpu_slots":1,
               "blockchain_priority_preserved":True,"verdict":"PASS"})
print(json.dumps(report,separators=(",",":")))
PY

dht_note "async proof scaling PASS: campaigns=1,2,3 simultaneous_actions=6 duplicate_avoided=1 equal_full_nodes=true blockchain_priority_preserved=true"
