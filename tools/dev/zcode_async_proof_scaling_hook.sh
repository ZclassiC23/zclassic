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
ZAPS_BLOCKCHAIN="$DHT_WORK/async-proof-blockchain.csv"
ZAPS_RESOURCES="$DHT_WORK/async-proof-resource-snapshots.csv"
mkdir -p "$ZAPS_ROOT"
printf '%s\n' \
    'campaign,slot,requester,executor,action,foreground_ms,background_ms,requester_cpu_ms' \
    >"$ZAPS_SUMMARY"
printf '%s\n' 'phase,surface,node,elapsed_ms,ok' \
    >"$ZAPS_RESPONSIVENESS"
printf '%s\n' 'phase,metric,node,elapsed_ms,value,ok' \
    >"$ZAPS_BLOCKCHAIN"
printf '%s\n' \
    'phase,node,cpu_ticks,read_bytes,write_bytes,rss_kb,connections,height' \
    >"$ZAPS_RESOURCES"

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

zaps_proc_io_value() {
    local node="$1" key="$2"
    awk -v key="$key" '$1 == key ":" { print $2; found=1 } END { if (!found) print 0 }' \
        "/proc/${PIDS[$node]}/io"
}

zaps_resource_snapshot() {
    local phase="$1" node cpu read_bytes write_bytes rss connections height
    for node in "$ZAP_A" "$ZAP_B" "$ZAP_C"; do
        cpu="$(zaps_proc_cpu_ticks "$node")"
        read_bytes="$(zaps_proc_io_value "$node" read_bytes)"
        write_bytes="$(zaps_proc_io_value "$node" write_bytes)"
        rss="$(awk '$1 == "VmRSS:" { print $2; found=1 } END { if (!found) print 0 }' \
            "/proc/${PIDS[$node]}/status" 2>/dev/null || true)"
        connections="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
            getconnectioncount | dht_result)"
        height="$(dht_height "${DDS[$node]}" "${RPCS[$node]}")"
        printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$phase" "$node" "$cpu" "$read_bytes" "$write_bytes" \
            "${rss:-0}" "$connections" "$height" >>"$ZAPS_RESOURCES"
    done
}

zaps_record_blockchain() {
    local phase="$1" metric="$2" node="$3" elapsed_ms="$4" value="$5" ok="$6"
    printf '%s,%s,%s,%s,%s,%s\n' \
        "$phase" "$metric" "$node" "$elapsed_ms" "$value" "$ok" \
        >>"$ZAPS_BLOCKCHAIN"
}

zaps_wait_mempool_tx() {
    local node="$1" txid="$2" txid_rev="$3" started_ns="$4"
    local deadline mempool elapsed_ms
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        mempool="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
            getrawmempool 2>/dev/null | dht_result 2>/dev/null || true)"
        case "$mempool" in
            *"$txid"*|*"$txid_rev"*)
                elapsed_ms=$((($(date +%s%N)-started_ns)/1000000))
                printf '%s' "$elapsed_ms"
                return 0
                ;;
        esac
        sleep 0.1
    done
    return 1
}

zaps_wait_height_at_least() {
    local node="$1" target="$2" started_ns="$3" deadline height elapsed_ms
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        height="$(dht_height "${DDS[$node]}" "${RPCS[$node]}" 2>/dev/null || true)"
        case "$height" in
            ''|*[!0-9]*) ;;
            *)
                if [ "$height" -ge "$target" ]; then
                    elapsed_ms=$((($(date +%s%N)-started_ns)/1000000))
                    printf '%s' "$elapsed_ms"
                    return 0
                fi
                ;;
        esac
        sleep 0.1
    done
    return 1
}

# Build one exact-input transaction with no change output. This deliberately
# avoids making blockchain-load acceptance depend on replenishing the wallet's
# receive/change keypool: listunspent identifies an owned mature coin, the raw
# transaction spends it minus a fixed fee back to the already-owned fixture
# address, and the unlocked wallet signs that immutable input.
zaps_create_transaction() {
    local node="$1" unspent pair inputs outputs raw_response raw signed_response
    local signed complete sent_response txid
    unspent="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" listunspent || true)"
    pair="$(printf '%s' "$unspent" | python3 -c '
import decimal,json,sys
d=json.load(sys.stdin)
assert d.get("error") is None,d
rows=d.get("result")
assert isinstance(rows,list) and rows,rows
row=max(rows,key=lambda x:decimal.Decimal(str(x["amount"])))
amount=decimal.Decimal(str(row["amount"]))-decimal.Decimal("0.00100000")
assert amount>0,row
print(json.dumps([{"txid":row["txid"],"vout":int(row["vout"])}],separators=(",",":")))
print("{"+json.dumps(sys.argv[1])+":"+format(amount,".8f")+"}")
' "$ADDR")" || return 1
    inputs="$(printf '%s\n' "$pair" | sed -n '1p')"
    outputs="$(printf '%s\n' "$pair" | sed -n '2p')"
    [ -n "$inputs" ] && [ -n "$outputs" ] || return 1

    raw_response="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
        createrawtransaction "$inputs" "$outputs" || true)"
    raw="$(printf '%s' "$raw_response" | dht_result 2>/dev/null || true)"
    [ -n "$raw" ] || return 1
    signed_response="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
        signrawtransaction "\"$raw\"" || true)"
    signed="$(printf '%s' "$signed_response" | python3 -c '
import json,sys
d=json.load(sys.stdin)
r=d.get("result")
assert d.get("error") is None and isinstance(r,dict),d
assert r.get("complete") is True,r
print(r["hex"])
' 2>/dev/null || true)"
    complete="${#signed}"
    [ "$complete" -gt 0 ] || return 1
    sent_response="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
        sendrawtransaction "\"$signed\"" || true)"
    txid="$(printf '%s' "$sent_response" | dht_result 2>/dev/null || true)"
    [ "${#txid}" -eq 64 ] || return 1
    printf '%s' "$txid"
}

# Exercise the public-node path with state-changing work, not read probes.
# Bounded polling only observes durable mempool/height facts; no sleep or
# elapsed-time assumption establishes relay or synchronization correctness.
zaps_blockchain_activity() {
    local phase="$1" sender="$ZAPS_CHAIN_NODE" node response txid txid_rev
    local started_ns elapsed_ms start_height target_height connections
    local expected_start current_start

    expected_start="$(awk '{print $22}' "/proc/${PIDS[$sender]}/stat")"
    start_height="$(dht_height "${DDS[$sender]}" "${RPCS[$sender]}")"

    started_ns="$(date +%s%N)"
    txid="$(zaps_create_transaction "$sender" || true)"
    elapsed_ms=$((($(date +%s%N)-started_ns)/1000000))
    [ "${#txid}" -eq 64 ] || dht_die "$phase raw transaction creation/sign/relay failed"
    zaps_record_blockchain "$phase" send_rpc "$sender" "$elapsed_ms" "$txid" true
    txid_rev="$(python3 -c \
        'import sys; print(bytes.fromhex(sys.argv[1])[::-1].hex())' "$txid")"

    for node in "$ZAP_A" "$ZAP_B" "$ZAP_C"; do
        [ "$node" = "$sender" ] && continue
        elapsed_ms="$(zaps_wait_mempool_tx "$node" "$txid" "$txid_rev" "$started_ns")" ||
            dht_die "$phase transaction $txid did not relay to node $node"
        zaps_record_blockchain "$phase" tx_relay "$node" "$elapsed_ms" "$txid" true
    done

    started_ns="$(date +%s%N)"
    response="$(dht_rpc "${DDS[$sender]}" "${RPCS[$sender]}" \
        generatetoaddress 1 "\"$ADDR\"" || true)"
    printf '%s' "$response" | dht_result >/dev/null 2>&1 ||
        dht_die "$phase block production failed: $response"
    elapsed_ms=$((($(date +%s%N)-started_ns)/1000000))
    target_height=$((start_height + 1))
    zaps_record_blockchain "$phase" block_production "$sender" \
        "$elapsed_ms" "$target_height" true

    for node in "$ZAP_A" "$ZAP_B" "$ZAP_C"; do
        [ "$node" = "$sender" ] && continue
        elapsed_ms="$(zaps_wait_height_at_least "$node" "$target_height" "$started_ns")" ||
            dht_die "$phase block $target_height did not synchronize to node $node"
        zaps_record_blockchain "$phase" block_sync "$node" \
            "$elapsed_ms" "$target_height" true
    done

    for node in "$ZAP_A" "$ZAP_B" "$ZAP_C"; do
        connections="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
            getconnectioncount | dht_result)"
        [ "$connections" -ge 2 ] ||
            dht_die "$phase package load starved peer liveness on node $node"
        current_start="$(awk '{print $22}' "/proc/${PIDS[$node]}/stat" 2>/dev/null || true)"
        [ -n "$current_start" ] || dht_die "$phase node $node exited during package load"
        zaps_record_blockchain "$phase" peer_connections "$node" 0 \
            "$connections" true
    done
    current_start="$(awk '{print $22}' "/proc/${PIDS[$sender]}/stat" 2>/dev/null || true)"
    [ "$current_start" = "$expected_start" ] ||
        dht_die "$phase chain producer was replaced during package load"
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

# The public-node coin-generation matrix composes these measurement and
# immutable-action helpers with the same DHT/full-node owner.  Returning here
# keeps one implementation of resource snapshots, proof admission, and
# receipt completion without making the scaling campaign a second lifecycle
# authority.
if [ "${ZAPS_HELPERS_ONLY:-0}" = 1 ]; then
    return 0
fi

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

# Node 1 owns the independently-created B wallet; every other role was cloned
# from A's closed chain fixture and owns the funded key behind ADDR. Select a
# funded role deterministically without assigning that role any network or
# proof authority, then wait for its exact chain/wallet facts before measuring.
ZAPS_CHAIN_NODE=""
for node in "$ZAP_A" "$ZAP_B" "$ZAP_C"; do
    if [ "$node" -ne 1 ]; then ZAPS_CHAIN_NODE="$node"; break; fi
done
[ -n "$ZAPS_CHAIN_NODE" ] || dht_die "scaling topology has no funded chain producer"
ZAPS_CHAIN_HEIGHT="$(dht_height "${DDS[$ZAPS_CHAIN_NODE]}" \
    "${RPCS[$ZAPS_CHAIN_NODE]}")"
dht_wait_chain_loaded "${DDS[$ZAPS_CHAIN_NODE]}" \
    "${RPCS[$ZAPS_CHAIN_NODE]}" "$ZAPS_CHAIN_HEIGHT" ||
    dht_die "scaling chain producer did not load its active chain"
dht_unlock_wallet "${DDS[$ZAPS_CHAIN_NODE]}" "${RPCS[$ZAPS_CHAIN_NODE]}" ||
    dht_die "scaling chain producer wallet unlock failed"
dht_wait_spendable "${DDS[$ZAPS_CHAIN_NODE]}" "${RPCS[$ZAPS_CHAIN_NODE]}" ||
    dht_die "scaling chain producer has no spendable fixture output"

# Establish each read surface's same-process idle distribution before proof
# traffic. The loaded phase below runs the identical probes while three
# independent contexts transfer and three confined builds execute.
zaps_resource_snapshot idle_before
zaps_blockchain_activity idle
zaps_resource_snapshot idle_after
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
zaps_resource_snapshot loaded_before
zaps_admit_parallel three-a three-b three-c
zaps_blockchain_activity loaded & ZAPS_BLOCKCHAIN_PID="$!"
zaps_probe_rounds loaded 6 & ZAPS_PROBE_PID="$!"
for slot in three-a three-b three-c; do zaps_finish "$slot"; done
wait "$ZAPS_PROBE_PID" || dht_die "loaded responsiveness probes failed"
wait "$ZAPS_BLOCKCHAIN_PID" || dht_die "loaded blockchain activity failed"
zaps_resource_snapshot loaded_after

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
               "read_surfaces_bounded":True,"verdict":"PASS"})
print(json.dumps(report,separators=(",",":")))
PY

if ! python3 - "$ZAPS_BLOCKCHAIN" "$ZAPS_RESOURCES" "$ZAPS_SUMMARY" \
    "$DHT_WORK" "$(getconf CLK_TCK)" \
    >"$DHT_WORK/async-proof-blockchain-priority-report.json" <<'PY'
import csv,glob,json,re,sys
block_path,resource_path,summary_path,root,hz=sys.argv[1:]
hz=int(hz)
block=list(csv.DictReader(open(block_path, encoding="utf-8")))
resources=list(csv.DictReader(open(resource_path, encoding="utf-8")))
summary=list(csv.DictReader(open(summary_path, encoding="utf-8")))
assert block and all(r["ok"]=="true" for r in block),block

def timings(phase, metric):
    return [int(r["elapsed_ms"]) for r in block
            if r["phase"]==phase and r["metric"]==metric]

expected={"send_rpc":1,"tx_relay":2,"block_production":1,
          "block_sync":2,"peer_connections":3}
for phase in ("idle","loaded"):
    for metric,count in expected.items():
        rows=[r for r in block if r["phase"]==phase and r["metric"]==metric]
        assert len(rows)==count,(phase,metric,rows)
    assert min(int(r["value"]) for r in block
               if r["phase"]==phase and r["metric"]=="peer_connections") >= 2

bounds={"send_rpc":(2000,1000),"tx_relay":(10000,5000),
        "block_production":(5000,2000),"block_sync":(10000,5000)}
latencies={}
for metric,(absolute,added) in bounds.items():
    idle=max(timings("idle",metric)); loaded=max(timings("loaded",metric))
    assert loaded < absolute,(metric,loaded,absolute)
    assert loaded <= idle+added,(metric,idle,loaded,added)
    latencies[metric]={"idle_max_ms":idle,"loaded_max_ms":loaded,
                       "added_ms":loaded-idle}

snap={(r["phase"],r["node"]):r for r in resources}
nodes={r["node"] for r in resources}
assert len(nodes)==3,nodes
resource_delta={}
for prefix in ("idle","loaded"):
    before={n:snap[(prefix+"_before",n)] for n in nodes}
    after={n:snap[(prefix+"_after",n)] for n in nodes}
    cpu=sum(int(after[n]["cpu_ticks"])-int(before[n]["cpu_ticks"]) for n in nodes)
    read=sum(int(after[n]["read_bytes"])-int(before[n]["read_bytes"]) for n in nodes)
    write=sum(int(after[n]["write_bytes"])-int(before[n]["write_bytes"]) for n in nodes)
    assert cpu>=0 and read>=0 and write>=0,(prefix,cpu,read,write)
    assert all(int(after[n]["height"])>=int(before[n]["height"])+1 for n in nodes)
    resource_delta[prefix]={"node_cpu_seconds":round(cpu/hz,3),
                            "node_read_bytes":read,"node_write_bytes":write,
                            "rss_peak_kb":max(int(after[n]["rss_kb"]) for n in nodes)}

loaded_actions={r["action"] for r in summary if r["campaign"]=="three"}
assert len(loaded_actions)==3,loaded_actions
transferred={}
pattern=re.compile(r"\baction=([0-9a-f]{64})\b.*\bstage=remote_admission\b.*\btransferred_bytes=(\d+)\b")
for path in glob.glob(root+"/*/node.log"):
    with open(path,encoding="utf-8",errors="replace") as stream:
        for line in stream:
            match=pattern.search(line)
            if match and match.group(1) in loaded_actions:
                transferred[match.group(1)]=max(transferred.get(match.group(1),0),
                                                  int(match.group(2)))
assert set(transferred)==loaded_actions,(loaded_actions,transferred)
context_bytes=sum(transferred.values())
elapsed=max(int(r["background_ms"]) for r in summary if r["campaign"]=="three")
assert context_bytes>0 and elapsed>0,(context_bytes,elapsed)

report={"schema":"zcl.async_proof_blockchain_priority.v1",
        "same_three_full_nodes":True,"simultaneous_actions":3,
        "state_changing_activity":{"transactions_relayed":2,"blocks_produced":2,
                                    "all_peers_synchronized":True},
        "latency":latencies,"resources":resource_delta,
        "loaded_context_bytes":context_bytes,
        "loaded_context_throughput_bytes_per_second":round(context_bytes*1000/elapsed),
        "worker_slots_per_node":1,"peer_connection_floor":2,
        "blockchain_priority_preserved":True,"verdict":"PASS"}
print(json.dumps(report,separators=(",",":")))
PY
then
    dht_die "state-changing blockchain priority contract failed"
fi

dht_note "async proof scaling PASS: campaigns=1,2,3 simultaneous_actions=6 duplicate_avoided=1 equal_full_nodes=true state_changing_blockchain_priority=true"
