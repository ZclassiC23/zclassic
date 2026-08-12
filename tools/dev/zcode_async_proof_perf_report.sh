#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Derive performance observations from immutable action-correlated node logs.
# This report is telemetry only: it never mutates proof or acceptance state.
set -euo pipefail

usage() {
    printf 'usage: %s <zcode-dht-acceptance-artifact>\n' "$0" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
root="$1"
[ -d "$root" ] || { printf 'artifact directory not found: %s\n' "$root" >&2; exit 2; }

shopt -s nullglob
logs=("$root"/*/node.log)
results=("$root"/async-submit-*-result.json)
[ "${#logs[@]}" -gt 0 ] || { printf 'no node logs under %s\n' "$root" >&2; exit 1; }
[ "${#results[@]}" -gt 0 ] || { printf 'no async result records under %s\n' "$root" >&2; exit 1; }

scratch="$(mktemp -d "${TMPDIR:-/tmp}/zcl-proof-perf.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT INT TERM

quantile() {
    local name="$1" file="$2" unit="$3"
    [ -s "$file" ] || { printf 'missing metric: %s\n' "$name" >&2; exit 1; }
    sort -n "$file" | awk -v name="$name" -v unit="$unit" '
        { values[NR]=$1; sum+=$1 }
        END {
            p50=int((NR*50+99)/100); p95=int((NR*95+99)/100)
            printf "%s n=%d p50_%s=%d p95_%s=%d mean_%s=%.0f max_%s=%d\n",
                   name,NR,unit,values[p50],unit,values[p95],unit,sum/NR,
                   unit,values[NR]
        }'
}

# API-local foreground spans do not depend on synchronized host clocks.
for file in "${results[@]}"; do
    for key in foreground_request_creation_us durable_action_lookup_dedup_us \
               local_submit_us local_first_feedback_us live_rpc_admission_us \
               live_rpc_request_bytes live_rpc_response_bytes; do
        sed -n "s/.*\"$key\":\([0-9][0-9]*\).*/\1/p" "$file" >>"$scratch/$key"
    done
done

# Every lifecycle log carries action=<immutable root>, stage=<projection>, and
# at_unix_us=<observation>. Cross-host deltas are valid when the campaign's
# clocks are synchronized; local durations remain valid independently.
awk -v out="$scratch" '
function field(name,    i,prefix) {
    prefix=name "="
    for (i=1;i<=NF;i++) if (index($i,prefix)==1) return substr($i,length(prefix)+1)
    return ""
}
function first(key,value) { if (!(key in at) || value < at[key]) at[key]=value }
function emit(name,value) { if (value >= 0) print value >> (out "/" name) }
index($0,"[zcode.proof_perf]") {
    action=field("action"); stage=field("stage"); timestamp=field("at_unix_us")+0
    if (length(action)!=64 || stage=="" || timestamp<=0) next
    key=action SUBSEP stage
    if (stage=="requester_dispatch") {
        emit("context_prepare_us",field("context_prepare_us")+0)
        emit("peer_selection_us",field("peer_selection_us")+0)
        emit("request_submit_us",field("request_submit_us")+0)
        emit("context_prepared_bytes",field("context_bytes")+0)
        request_bytes[action]+=field("request_wire_bytes")+0
        dispatches[action]++
        context_cache_hits+=field("context_cache_hit")+0
        context_cache_samples++
        if (field("retry")!="0") retries++
    }
    if (stage=="requester_dispatch" && field("retry")!="0") next
    first(key,timestamp)
    if (stage=="worker_execute") {
        total=field("total_us")+0
        if (total < 10000000) {
            emit("remote_execution_us",total)
            emit("remote_cpu_us",field("child_cpu_us")+0)
            emit("sandbox_prepare_us",field("sandbox_prepare_us")+0)
            emit("execution_us",field("execution_us")+0)
            emit("output_cas_us",field("output_cas_us")+0)
            emit("receipt_sign_us",field("receipt_sign_us")+0)
            emit("worker_action_lookup_us",field("lookup_us")+0)
            emit("input_reconstruction_us",field("input_reconstruction_us")+0)
            emit("output_verify_us",field("output_verify_us")+0)
            emit("worker_revalidation_us",field("revalidation_us")+0)
            emit("worker_projection_us",field("projection_us")+0)
            emit("worker_input_bytes",field("input_bytes")+0)
            emit("worker_output_bytes",field("output_bytes")+0)
            emit("worker_processes",field("processes")+0)
            emit("compiler_processes",field("compiler_processes")+0)
            emit("test_processes",field("test_processes")+0)
            output_bytes[action]=field("output_bytes")+0
            worker_cache_hits+=field("cache_hit")+0
            worker_cache_samples++
        }
    } else if (stage=="remote_admission") {
        emit("remote_admission_us",field("admission_us")+0)
        emit("context_transferred_bytes",field("transferred_bytes")+0)
        context_transfer[action]=field("transferred_bytes")+0
    } else if (stage=="worker_lease") {
        emit("worker_claim_us",field("claim_us")+0)
        emit("remote_queue_us",field("queue_us")+0)
    } else if (stage=="requester_result") {
        emit("receipt_verification_us",field("receipt_verification_us")+0)
        emit("requester_result_projection_us",field("projection_us")+0)
        result_bytes[action]+=field("result_wire_bytes")+0
    } else if (stage=="acceptance_ready") {
        emit("acceptance_local_verification_us",field("local_verification_us")+0)
        emit("acceptance_projection_us",field("projection_us")+0)
    } else if (stage=="worker_progress_publish") {
        progress_bytes[action]+=field("progress_wire_bytes")+0
    }
}
END {
    for (key in at) {
        split(key,p,SUBSEP); actions[p[1]]=1
    }
    for (action in actions) {
        foreground=at[action SUBSEP "foreground_return"]
        dispatch=at[action SUBSEP "requester_dispatch"]
        admission=at[action SUBSEP "remote_admission"]
        lease=at[action SUBSEP "worker_lease"]
        publish=at[action SUBSEP "worker_result_publish"]
        result=at[action SUBSEP "requester_result"]
        ready=at[action SUBSEP "acceptance_ready"]
        if (foreground && dispatch) emit("foreground_to_dispatch_us",dispatch-foreground)
        if (dispatch && admission) emit("dispatch_to_admission_us",admission-dispatch)
        if (admission && lease) emit("admission_to_lease_us",lease-admission)
        if (publish && result) emit("result_transport_precise_us",result-publish)
        if (result && ready) emit("result_to_acceptance_us",ready-result)
        if (foreground && ready) emit("background_total_precise_us",ready-foreground)
        payload=context_transfer[action]+request_bytes[action]+progress_bytes[action]+result_bytes[action]+output_bytes[action]
        if (payload > 0) emit("network_payload_lower_bound_bytes",payload)
    }
    print retries+0 > (out "/retry_dispatches_total")
    print context_cache_hits+0 > (out "/context_cache_hits_total")
    print context_cache_samples+0 > (out "/context_cache_samples_total")
    print worker_cache_hits+0 > (out "/worker_cache_hits_total")
    print worker_cache_samples+0 > (out "/worker_cache_samples_total")
}' "${logs[@]}"

printf 'schema=zcl.async_proof_perf_report.v1\n'
printf 'artifact=%s\n' "$root"
printf 'clock_note=cross-host deltas require synchronized realtime clocks; local spans do not\n'
for metric in foreground_request_creation_us durable_action_lookup_dedup_us \
              local_submit_us local_first_feedback_us live_rpc_admission_us \
              foreground_to_dispatch_us context_prepare_us peer_selection_us \
              request_submit_us dispatch_to_admission_us \
              remote_admission_us admission_to_lease_us remote_queue_us \
              worker_claim_us worker_action_lookup_us input_reconstruction_us \
              sandbox_prepare_us execution_us remote_execution_us remote_cpu_us \
              output_verify_us output_cas_us worker_revalidation_us \
              receipt_sign_us worker_projection_us result_transport_precise_us \
              receipt_verification_us requester_result_projection_us \
              result_to_acceptance_us acceptance_local_verification_us \
              acceptance_projection_us \
              background_total_precise_us; do
    quantile "$metric" "$scratch/$metric" us
done
for metric in live_rpc_request_bytes live_rpc_response_bytes \
              context_prepared_bytes context_transferred_bytes \
              worker_input_bytes worker_output_bytes \
              network_payload_lower_bound_bytes worker_processes \
              compiler_processes test_processes; do
    quantile "$metric" "$scratch/$metric" count
done

dedup=0
[ -f "$root/async-exact-reuse-eliminated.txt" ] &&
    dedup="$(awk '{sum+=$1} END{print sum+0}' "$root/async-exact-reuse-eliminated.txt")"
printf 'duplicate_executions_avoided=%s\n' "$dedup"
printf 'retry_dispatches=%s\n' "$(cat "$scratch/retry_dispatches_total")"
printf 'context_cache_hits=%s/%s\n' \
    "$(cat "$scratch/context_cache_hits_total")" \
    "$(cat "$scratch/context_cache_samples_total")"
printf 'worker_cache_hits=%s/%s\n' \
    "$(cat "$scratch/worker_cache_hits_total")" \
    "$(cat "$scratch/worker_cache_samples_total")"
