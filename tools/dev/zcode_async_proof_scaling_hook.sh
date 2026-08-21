#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Real three-node async-proof scaling characterization.  This is observation
# only: task/action/receipt rows remain the canonical lifecycle authority.

ZAP_HELPERS_ONLY=1
# shellcheck source=tools/dev/zcode_async_proof_acceptance_hook.sh
. "$SCRIPT_DIR/zcode_async_proof_acceptance_hook.sh"
unset ZAP_HELPERS_ONLY

JSONQ="${JSONQ:-$REPO_ROOT/build/bin/jsonq}"
[ -x "$JSONQ" ] || dht_die "build/bin/jsonq is missing — run make jsonq"

ZAPS_ROOT="$DHT_WORK/async-proof-scale-projects"
ZAPS_SUMMARY="$DHT_WORK/async-proof-scaling.csv"
ZAPS_RESPONSIVENESS="$DHT_WORK/async-proof-responsiveness.csv"
ZAPS_BLOCKCHAIN="$DHT_WORK/async-proof-blockchain.csv"
ZAPS_RESOURCES="$DHT_WORK/async-proof-resource-snapshots.csv"
mkdir -p "$ZAPS_ROOT"

zaps_json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

zaps_hex_rev() {
    local hex="$1" out="" i
    i=${#hex}
    [ $((i % 2)) -eq 0 ] || return 1
    while [ "$i" -gt 0 ]; do
        i=$((i - 2))
        out="${out}${hex:i:2}"
    done
    printf '%s' "$out"
}

zaps_percentile() {
    local pct="$1" n idx
    local sorted
    mapfile -t sorted < <(sort -n)
    n=${#sorted[@]}
    [ "$n" -gt 0 ] || return 1
    idx=$(awk -v n="$n" -v pct="$pct" 'BEGIN {
        x = n * pct
        c = int(x)
        if (c < x) c++
        i = c - 1
        if (i < 0) i = 0
        if (i >= n) i = n - 1
        print i
    }')
    printf '%s' "${sorted[$idx]}"
}
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
    ok="$(printf '%s' "$response" | "$JSONQ" get ok 2>/dev/null || true)"
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
    local node="$1" unspent n i amount best_i=0 best_amt=""
    local txid vout inputs outputs out_amount esc
    local raw_response raw signed_response signed sent_response
    unspent="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" listunspent || true)"
    printf '%s' "$unspent" | "$JSONQ" eq error null || return 1
    n="$(printf '%s' "$unspent" | "$JSONQ" count result)" || return 1
    [ "$n" -gt 0 ] || return 1
    i=0
    while [ "$i" -lt "$n" ]; do
        amount="$(printf '%s' "$unspent" | "$JSONQ" get "result[$i].amount")"
        if [ -z "$best_amt" ] || awk -v a="$amount" -v b="$best_amt" \
            'BEGIN { exit !(a + 0 > b + 0) }'
        then
            best_amt="$amount"
            best_i="$i"
        fi
        i=$((i + 1))
    done
    txid="$(printf '%s' "$unspent" | "$JSONQ" get "result[$best_i].txid")"
    vout="$(printf '%s' "$unspent" | "$JSONQ" get "result[$best_i].vout")"
    out_amount="$(awk -v a="$best_amt" 'BEGIN {
        v = a - 0.00100000
        if (!(v > 0)) exit 1
        printf "%.8f\n", v
    }')" || return 1
    esc="$(zaps_json_escape "$ADDR")"
    inputs="[{\"txid\":\"${txid}\",\"vout\":${vout}}]"
    outputs="{\"${esc}\":${out_amount}}"
    [ -n "$inputs" ] && [ -n "$outputs" ] || return 1

    raw_response="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
        createrawtransaction "$inputs" "$outputs" || true)"
    raw="$(printf '%s' "$raw_response" | dht_result 2>/dev/null || true)"
    [ -n "$raw" ] || return 1
    signed_response="$(dht_rpc "${DDS[$node]}" "${RPCS[$node]}" \
        signrawtransaction "\"$raw\"" || true)"
    printf '%s' "$signed_response" | "$JSONQ" eq error null || return 1
    printf '%s' "$signed_response" | "$JSONQ" eq result.complete true || return 1
    signed="$(printf '%s' "$signed_response" | "$JSONQ" get result.hex \
        2>/dev/null || true)"
    [ "${#signed}" -gt 0 ] || return 1
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
    txid_rev="$(zaps_hex_rev "$txid")"

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
    ok="$(printf '%s' "$start" | "$JSONQ" get ok 2>/dev/null || true)"
    [ "$ok" = true ] || dht_die "scale $slot could not start: $start"
    work="$(printf '%s' "$start" | "$JSONQ" get data.work_id)"
    handoff="$(dht_native "${DDS[$node]}" "${RPCS[$node]}" zcode work run \
        --input="{\"workspace\":\"$project\",\"work\":\"$work\",\"adapter\":\"manual\"}" || true)"
    candidate="$(printf '%s' "$handoff" | "$JSONQ" get data.candidate_workspace 2>/dev/null || true)"
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
        --input="{\"workspace\":\"$project\",\"work\":\"$work\",\"adapter\":\"manual\",\"datadir\":\"${DDS[$node]}\",\"details\":true}" || true)"
    end_ns="$(date +%s%N)"
    ok="$(printf '%s' "$result" | "$JSONQ" get ok 2>/dev/null || true)"
    [ "$ok" = true ] || dht_die "scale $slot foreground admission failed: $result"
    action="$(printf '%s' "$result" | "$JSONQ" get data.expert.action_id)"
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

zaps_assert_campaign_receipt() {
    local n=0 n_one=0 n_two=0 n_three=0
    local campaign slot requester executor action fg bg cpu
    declare -A actions=()
    while IFS=, read -r campaign slot requester executor action fg bg cpu; do
        [ "$campaign" = campaign ] && continue
        n=$((n + 1))
        case "$campaign" in
            one) n_one=$((n_one + 1)) ;;
            two) n_two=$((n_two + 1)) ;;
            three) n_three=$((n_three + 1)) ;;
        esac
        actions[$action]=1
        [ "$fg" -lt 30000 ] || return 1
        [ "$requester" != "$executor" ] || return 1
    done <"$ZAPS_SUMMARY"
    [ "$n" -eq 6 ] || return 1
    [ "$n_one" -eq 1 ] && [ "$n_two" -eq 2 ] && [ "$n_three" -eq 3 ] || return 1
    [ "${#actions[@]}" -eq 6 ] || return 1
}

zaps_write_responsiveness_report() {
    local phase surface node elapsed ok n_rows=0
    local idle_n loaded_n idle_p50 idle_p95 loaded_p50 loaded_p95
    local surf_json="" piece
    mkdir -p "$ZAPS_ROOT/resp"
    for surface in chain peer sync command; do
        : >"$ZAPS_ROOT/resp/idle-$surface"
        : >"$ZAPS_ROOT/resp/loaded-$surface"
    done
    while IFS=, read -r phase surface node elapsed ok; do
        [ "$phase" = phase ] && continue
        n_rows=$((n_rows + 1))
        [ "$ok" = true ] || return 1
        case "$phase" in
            idle|loaded) ;;
            *) return 1 ;;
        esac
        printf '%s\n' "$elapsed" >>"$ZAPS_ROOT/resp/${phase}-${surface}"
    done <"$ZAPS_RESPONSIVENESS"
    [ "$n_rows" -gt 0 ] || return 1
    for surface in chain peer sync command; do
        idle_n=$(wc -l <"$ZAPS_ROOT/resp/idle-$surface" | tr -d ' ')
        loaded_n=$(wc -l <"$ZAPS_ROOT/resp/loaded-$surface" | tr -d ' ')
        [ "$idle_n" -eq 9 ] && [ "$loaded_n" -eq 18 ] || return 1
        idle_p50=$(zaps_percentile 0.50 <"$ZAPS_ROOT/resp/idle-$surface")
        idle_p95=$(zaps_percentile 0.95 <"$ZAPS_ROOT/resp/idle-$surface")
        loaded_p50=$(zaps_percentile 0.50 <"$ZAPS_ROOT/resp/loaded-$surface")
        loaded_p95=$(zaps_percentile 0.95 <"$ZAPS_ROOT/resp/loaded-$surface")
        [ "$loaded_p95" -lt 1000 ] || return 1
        [ "$loaded_p95" -le $((idle_p95 + 500)) ] || return 1
        piece=$(printf '"%s":{"idle_samples":%s,"loaded_samples":%s,"idle_p50_ms":%s,"idle_p95_ms":%s,"loaded_p50_ms":%s,"loaded_p95_ms":%s,"p95_added_ms":%s}' \
            "$surface" "$idle_n" "$loaded_n" "$idle_p50" "$idle_p95" \
            "$loaded_p50" "$loaded_p95" "$((loaded_p95 - idle_p95))")
        surf_json="${surf_json}${surf_json:+,}${piece}"
    done
    printf '{"schema":"zcl.async_proof_responsiveness.v1","probe_client_processes":%s,"surfaces":{%s},"simultaneous_requesters":3,"simultaneous_actions":3,"worker_slots_per_node":1,"action_cpu_slots":1,"read_surfaces_bounded":true,"verdict":"PASS"}\n' \
        "$n_rows" "$surf_json"
}

zaps_csv_count() {
    awk -F, -v p="$1" -v m="$2" 'NR > 1 && $1 == p && $2 == m { c++ } END { print c + 0 }' \
        "$ZAPS_BLOCKCHAIN"
}

zaps_csv_max_elapsed() {
    awk -F, -v p="$1" -v m="$2" '
        NR > 1 && $1 == p && $2 == m { if ($4 + 0 > max) max = $4 + 0 }
        END { print max + 0 }
    ' "$ZAPS_BLOCKCHAIN"
}

zaps_write_blockchain_priority_report() {
    local hz="$1" phase metric want idle loaded absolute added
    local n_block=0 n_bad=0 min_peer
    local lat_json="" res_json="" piece
    local prefix node cpu nread nwrite rss_peak cpu_s
    declare -A cpu_ticks=() read_bytes=() write_bytes=() rss_kb=() height_snap=()
    declare -A nodes=() loaded_actions=() transferred=()
    local campaign slot requester executor action fg bg cpu_ms
    local max_bg=0 context_bytes=0 elapsed throughput
    local path line act bytes n_transferred

    while IFS=, read -r phase metric node elapsed want ok; do
        [ "$phase" = phase ] && continue
        n_block=$((n_block + 1))
        [ "$ok" = true ] || n_bad=$((n_bad + 1))
    done <"$ZAPS_BLOCKCHAIN"
    [ "$n_block" -gt 0 ] && [ "$n_bad" -eq 0 ] || return 1

    for phase in idle loaded; do
        [ "$(zaps_csv_count "$phase" send_rpc)" -eq 1 ] || return 1
        [ "$(zaps_csv_count "$phase" tx_relay)" -eq 2 ] || return 1
        [ "$(zaps_csv_count "$phase" block_production)" -eq 1 ] || return 1
        [ "$(zaps_csv_count "$phase" block_sync)" -eq 2 ] || return 1
        [ "$(zaps_csv_count "$phase" peer_connections)" -eq 3 ] || return 1
        min_peer=$(awk -F, -v p="$phase" '
            NR > 1 && $1 == p && $2 == "peer_connections" {
                v = $5 + 0
                if (min == "" || v < min) min = v
            }
            END { print min + 0 }
        ' "$ZAPS_BLOCKCHAIN")
        [ "$min_peer" -ge 2 ] || return 1
    done

    for metric in send_rpc tx_relay block_production block_sync; do
        case "$metric" in
            send_rpc) absolute=2000; added=1000 ;;
            tx_relay) absolute=10000; added=5000 ;;
            block_production) absolute=5000; added=2000 ;;
            block_sync) absolute=10000; added=5000 ;;
        esac
        idle=$(zaps_csv_max_elapsed idle "$metric")
        loaded=$(zaps_csv_max_elapsed loaded "$metric")
        [ "$loaded" -lt "$absolute" ] || return 1
        [ "$loaded" -le $((idle + added)) ] || return 1
        piece=$(printf '"%s":{"idle_max_ms":%s,"loaded_max_ms":%s,"added_ms":%s}' \
            "$metric" "$idle" "$loaded" "$((loaded - idle))")
        lat_json="${lat_json}${lat_json:+,}${piece}"
    done

    while IFS=, read -r phase node cpu nread nwrite rss conns height; do
        [ "$phase" = phase ] && continue
        nodes[$node]=1
        cpu_ticks["$phase|$node"]=$cpu
        read_bytes["$phase|$node"]=$nread
        write_bytes["$phase|$node"]=$nwrite
        rss_kb["$phase|$node"]=$rss
        height_snap["$phase|$node"]=$height
    done <"$ZAPS_RESOURCES"
    [ "${#nodes[@]}" -eq 3 ] || return 1

    for prefix in idle loaded; do
        cpu=0; nread=0; nwrite=0; rss_peak=0
        for node in "${!nodes[@]}"; do
            cpu=$((cpu + ${cpu_ticks[${prefix}_after|$node]} - ${cpu_ticks[${prefix}_before|$node]}))
            nread=$((nread + ${read_bytes[${prefix}_after|$node]} - ${read_bytes[${prefix}_before|$node]}))
            nwrite=$((nwrite + ${write_bytes[${prefix}_after|$node]} - ${write_bytes[${prefix}_before|$node]}))
            [ "${rss_kb[${prefix}_after|$node]}" -gt "$rss_peak" ] &&
                rss_peak="${rss_kb[${prefix}_after|$node]}"
            [ "${height_snap[${prefix}_after|$node]}" -ge $((${height_snap[${prefix}_before|$node]} + 1)) ] ||
                return 1
        done
        [ "$cpu" -ge 0 ] && [ "$nread" -ge 0 ] && [ "$nwrite" -ge 0 ] || return 1
        cpu_s=$(awk -v c="$cpu" -v hz="$hz" 'BEGIN { printf "%.3f", c / hz }')
        piece=$(printf '"%s":{"node_cpu_seconds":%s,"node_read_bytes":%s,"node_write_bytes":%s,"rss_peak_kb":%s}' \
            "$prefix" "$cpu_s" "$nread" "$nwrite" "$rss_peak")
        res_json="${res_json}${res_json:+,}${piece}"
    done

    while IFS=, read -r campaign slot requester executor action fg bg cpu_ms; do
        [ "$campaign" = campaign ] && continue
        if [ "$campaign" = three ]; then
            loaded_actions[$action]=1
            [ "$bg" -gt "$max_bg" ] && max_bg=$bg
        fi
    done <"$ZAPS_SUMMARY"
    [ "${#loaded_actions[@]}" -eq 3 ] || return 1

    for path in "$DHT_WORK"/*/node.log; do
        [ -f "$path" ] || continue
        while IFS= read -r line; do
            case "$line" in
                *action=*stage=remote_admission*transferred_bytes=*)
                    act=$(printf '%s' "$line" | sed -n 's/.*action=\([0-9a-f]\{64\}\).*/\1/p')
                    bytes=$(printf '%s' "$line" | sed -n 's/.*transferred_bytes=\([0-9][0-9]*\).*/\1/p')
                    [ "${#act}" -eq 64 ] || continue
                    [ -n "$act" ] && [ -n "${loaded_actions[$act]+x}" ] || continue
                    if [ -z "${transferred[$act]:-}" ] || [ "$bytes" -gt "${transferred[$act]}" ]; then
                        transferred[$act]=$bytes
                    fi
                    ;;
            esac
        done <"$path"
    done
    n_transferred=0
    context_bytes=0
    for act in "${!loaded_actions[@]}"; do
        [ -n "${transferred[$act]:-}" ] || return 1
        n_transferred=$((n_transferred + 1))
        context_bytes=$((context_bytes + transferred[$act]))
    done
    [ "$n_transferred" -eq 3 ] || return 1
    elapsed=$max_bg
    [ "$context_bytes" -gt 0 ] && [ "$elapsed" -gt 0 ] || return 1
    throughput=$(awk -v c="$context_bytes" -v e="$elapsed" \
        'BEGIN { printf "%.0f", (c * 1000) / e }')

    printf '{"schema":"zcl.async_proof_blockchain_priority.v1","same_three_full_nodes":true,"simultaneous_actions":3,"state_changing_activity":{"transactions_relayed":2,"blocks_produced":2,"all_peers_synchronized":true},"latency":{%s},"resources":{%s},"loaded_context_bytes":%s,"loaded_context_throughput_bytes_per_second":%s,"worker_slots_per_node":1,"peer_connection_floor":2,"blockchain_priority_preserved":true,"verdict":"PASS"}\n' \
        "$lat_json" "$res_json" "$context_bytes" "$throughput"
}

zaps_assert_campaign_receipt || dht_die "scaling campaign receipt failed"
zaps_write_responsiveness_report \
    >"$DHT_WORK/async-proof-responsiveness-report.json" ||
    dht_die "background proof responsiveness contract failed"
if ! zaps_write_blockchain_priority_report "$(getconf CLK_TCK)" \
    >"$DHT_WORK/async-proof-blockchain-priority-report.json"
then
    dht_die "state-changing blockchain priority contract failed"
fi

dht_note "async proof scaling PASS: campaigns=1,2,3 simultaneous_actions=6 duplicate_avoided=1 equal_full_nodes=true state_changing_blockchain_priority=true"
