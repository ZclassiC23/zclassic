/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Background requester-local dispatch over the existing work swarm. */

#include "config/boot_zcode_async_proof.h"

#include "base/hex.h"
#include "config/boot_internal.h"
#include "config/runtime.h"
#include "models/build_fabric.h"
#include "models/build_proof_event.h"
#include "platform/time_compat.h"
#include "services/build_fabric_async.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_worker.h"
#include "util/log_macros.h"
#include "vcs/build_action.h"
#include "vcs/package_store.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_candidate_bundle.h"
#include "vcs/zcode_task_authority_bundle.h"
#include "vcs/zcode_work_context.h"
#include "vcs/zcode_work_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ASYNC_PROOF_BATCH = 16 };

static uint8_t s_requester_secret[32];
static uint8_t s_requester_pubkey[32];
static bool s_requester_ready;
static char s_requester_datadir[4096];

static bool async_identity(const struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir) return false;
    if (s_requester_ready &&
        strcmp(s_requester_datadir, svc->datadir) == 0)
        return true;
    struct db_build_worker requester;
    struct zcl_result loaded = build_fabric_worker_identity_load(
        svc->datadir, &requester, s_requester_secret, s_requester_pubkey);
    if (!loaded.ok) {
        LOG_WARN("net.zcode_async", "requester identity: %s",
                 loaded.message);
        return false;
    }
    int n = snprintf(s_requester_datadir, sizeof(s_requester_datadir),
                     "%s", svc->datadir);
    if (n <= 0 || (size_t)n >= sizeof(s_requester_datadir)) {
        memset(s_requester_secret, 0, sizeof(s_requester_secret));
        memset(s_requester_pubkey, 0, sizeof(s_requester_pubkey));
        return false;
    }
    s_requester_ready = true;
    return true;
}

static bool async_load_object(
    const char *workspace, const char *root_hex, size_t limit,
    uint8_t **wire, size_t *wire_len)
{
    uint8_t root[32];
    *wire = NULL;
    *wire_len = 0;
    return zcl_hex_decode_lower(root_hex, root, 32) &&
           vcs_object_load_raw_bounded(workspace, root, limit,
                                       wire, wire_len) == 0;
}

static bool async_load_task(const char *workspace,
                            const struct db_build_action *action,
                            struct vcs_zcode_task_v1 *task)
{
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    bool ok = async_load_object(workspace, action->task_root_sha3,
                                VCS_ZCODE_TASK_WIRE_BYTES,
                                &wire, &wire_len) &&
        vcs_zcode_task_parse(wire, wire_len, task) == VCS_ZCODE_DEV_OK;
    free(wire);
    return ok;
}

static struct zcl_result async_context_build(
    struct node_db *ndb, const struct db_build_job *job,
    struct db_build_action *action, int64_t now,
    const char *workspace, struct vcs_zcode_task_v1 *task,
    uint8_t context_root[32])
{
    struct vcs_package_store *store = vcs_package_store_global();
    if (!store || !workspace || workspace[0] != '/')
        return ZCL_ERR(-1, "package/CAS owners unavailable");
    if (!async_load_task(workspace, action, task))
        return ZCL_ERR(-1, "task object unavailable");
    if (action->context_root_sha3[0]) {
        struct vcs_package_store_status status;
        if (!zcl_hex_decode_lower(action->context_root_sha3,
                                  context_root, 32) ||
            !vcs_package_store_package_status(store, context_root, &status) ||
            !status.complete)
            return ZCL_ERR(-1, "bound context package is incomplete");
        return ZCL_OK;
    }
    struct vcs_zcode_candidate_v1 candidate;
    struct vcs_zcode_proof_policy_v1 policy;
    uint8_t *candidate_wire = NULL, *policy_wire = NULL, *input_wire = NULL;
    size_t candidate_len = 0, policy_len = 0, input_len = 0;
    bool loaded = async_load_object(workspace,
            action->candidate_root_sha3, VCS_ZCODE_CANDIDATE_WIRE_BYTES,
            &candidate_wire, &candidate_len) &&
        vcs_zcode_candidate_parse(candidate_wire, candidate_len,
                                  &candidate) == VCS_ZCODE_DEV_OK &&
        async_load_object(workspace,
            action->proof_policy_root_sha3,
            VCS_ZCODE_PROOF_POLICY_WIRE_BYTES, &policy_wire, &policy_len) &&
        vcs_zcode_proof_policy_parse(policy_wire, policy_len,
                                     &policy) == VCS_ZCODE_DEV_OK &&
        async_load_object(workspace, action->input_root_sha3,
                          task->max_context_bytes,
                          &input_wire, &input_len);
    free(policy_wire);
    free(candidate_wire);
    if (!loaded) {
        free(input_wire);
        return ZCL_ERR(-1, "candidate, policy, or fixed input unavailable");
    }
    struct vcs_zcode_work_context_v1 context;
    vcs_zcode_work_context_init(&context);
    bool source_ok = zcl_hex_decode_lower(
        job->source_sha256, context.source_sha256, 32);
    (void)snprintf(context.profile, sizeof(context.profile), "%s",
                   job->profile);
    context.task = *task;
    context.candidate = candidate;
    context.proof_policy = policy;
    context.fixed_input = input_wire;
    context.fixed_input_len = input_len;
    enum vcs_zcode_candidate_bundle_result candidate_bundle = source_ok
        ? vcs_zcode_candidate_bundle_export(
              workspace, task, &candidate,
              &context.candidate_authority,
              &context.candidate_authority_len)
        : VCS_ZCODE_CANDIDATE_BUNDLE_CAS;
    enum vcs_zcode_task_authority_result task_bundle =
        candidate_bundle == VCS_ZCODE_CANDIDATE_BUNDLE_OK
            ? vcs_zcode_task_authority_bundle_export(
                  workspace, task, &context.task_authority,
                  &context.task_authority_len)
            : VCS_ZCODE_TASK_AUTHORITY_CAS;
    uint8_t action_root[32];
    enum vcs_zcode_work_context_result packed =
        candidate_bundle == VCS_ZCODE_CANDIDATE_BUNDLE_OK &&
        task_bundle == VCS_ZCODE_TASK_AUTHORITY_OK
            ? vcs_zcode_work_context_put_for_kind_with_candidate(
                  store, &context, action->kind, now,
                  workspace, context_root, action_root)
            : VCS_ZCODE_WORK_CONTEXT_STALE;
    context.fixed_input = NULL;
    vcs_zcode_work_context_free(&context);
    free(input_wire);
    uint8_t expected_action[32];
    char context_hex[65];
    if (packed != VCS_ZCODE_WORK_CONTEXT_OK ||
        !zcl_hex_decode_lower(action->action_id, expected_action, 32) ||
        memcmp(action_root, expected_action, 32) != 0) {
        return ZCL_ERR(-1, "context package does not bind the action");
    }
    zcl_hex_encode(context_root, 32, context_hex);
    if (!db_build_action_bind_context(ndb, action->action_id, context_hex))
        return ZCL_ERR(-1, "context root could not bind to the action");
    (void)snprintf(action->context_root_sha3,
                   sizeof(action->context_root_sha3), "%s", context_hex);
    return ZCL_OK;
}

static bool async_capability_allows(
    const struct vcs_zcode_work_capability_v1 *capability,
    const struct db_build_job *job, uint8_t work_kind)
{
    uint8_t toolchain[32];
    return capability &&
        zcl_hex_decode_lower(job->toolchain_sha3, toolchain, 32) &&
        capability->queue_headroom > 0 &&
        capability->max_lease_seconds > 0 &&
        capability->target == VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3 &&
        (capability->work_kinds & (UINT32_C(1) << work_kind)) != 0 &&
        (capability->confinement & VCS_ZCODE_WORK_CONFINEMENT_V1_MASK) ==
            VCS_ZCODE_WORK_CONFINEMENT_V1_MASK &&
        memcmp(capability->toolchain_capsule_root, toolchain, 32) == 0;
}

static bool async_select_peer(
    struct vcs_zcode_work_node *work,
    const struct db_build_proof_event *event,
    const struct db_build_job *job, uint8_t work_kind, int64_t now,
    uint64_t *peer_out, struct vcs_zcode_work_capability_v1 *capability_out)
{
    if (event->peer_id && strcmp(event->state, "REQUESTED") == 0 &&
        vcs_zcode_work_node_peer_capability(
            work, event->peer_id, now, capability_out) &&
        async_capability_allows(capability_out, job, work_kind)) {
        *peer_out = event->peer_id;
        return true;
    }
    uint64_t peers[VCS_ZCODE_WORK_NODE_MAX_PEERS];
    struct vcs_zcode_work_capability_v1 capabilities[
        VCS_ZCODE_WORK_NODE_MAX_PEERS];
    size_t count = vcs_zcode_work_node_capable_peers(
        work, now, peers, capabilities, VCS_ZCODE_WORK_NODE_MAX_PEERS);
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            if ((pass == 0 && peers[i] == event->peer_id) ||
                !async_capability_allows(&capabilities[i], job, work_kind))
                continue;
            *peer_out = peers[i];
            *capability_out = capabilities[i];
            return true;
        }
    }
    return false;
}

static void async_dispatch(
    struct boot_svc_ctx *svc, struct vcs_zcode_work_node *work,
    struct node_db *ndb, const struct db_build_proof_event *event,
    int64_t now)
{
    struct db_build_action action;
    struct db_build_job job;
    if (!db_build_action_find(ndb, event->action_id, &action) ||
        !db_build_job_find(ndb, action.job_id, &job))
        return;
    uint8_t work_kind = vcs_build_action_v1_work_kind(action.kind);
    if (work_kind != VCS_ZCODE_WORK_BUILD &&
        work_kind != VCS_ZCODE_WORK_TEST &&
        work_kind != VCS_ZCODE_WORK_FUZZ)
        return;
    struct vcs_zcode_task_v1 task;
    uint8_t context_root[32];
    struct zcl_result packed = async_context_build(
        ndb, &job, &action, now, event->workspace, &task, context_root);
    if (!packed.ok) return;
    uint64_t peer = 0;
    struct vcs_zcode_work_capability_v1 capability;
    if (!async_select_peer(work, event, &job, work_kind, now,
                           &peer, &capability) ||
        !async_identity(svc))
        return;
    uint32_t requested_lease = task.max_cpu_seconds < UINT32_MAX - 10u
        ? task.max_cpu_seconds + 10u : task.max_cpu_seconds;
    if (requested_lease < 15u) requested_lease = 15u;
    if (requested_lease > capability.max_lease_seconds)
        requested_lease = capability.max_lease_seconds;
    int64_t lease_end = now + requested_lease;
    int64_t deadline = lease_end < task.expires_unix
        ? lease_end : task.expires_unix - 1;
    if (deadline <= now) return;
    struct vcs_zcode_work_request_v1 request = {
        .request_id = event->request_id,
        .work_kind = work_kind,
        .target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3,
        .max_cpu_seconds = task.max_cpu_seconds < capability.max_cpu_seconds
            ? task.max_cpu_seconds : capability.max_cpu_seconds,
        .max_memory_bytes = task.max_memory_bytes < capability.max_memory_bytes
            ? task.max_memory_bytes : capability.max_memory_bytes,
        .max_output_bytes = task.max_output_bytes < capability.max_output_bytes
            ? task.max_output_bytes : capability.max_output_bytes,
        .deadline_unix = deadline,
    };
    (void)zcl_hex_decode_lower(action.task_root_sha3, request.task_root, 32);
    (void)zcl_hex_decode_lower(action.candidate_root_sha3,
                               request.candidate_root, 32);
    (void)zcl_hex_decode_lower(action.action_id, request.action_root, 32);
    (void)zcl_hex_decode_lower(action.input_root_sha3,
                               request.input_root, 32);
    memcpy(request.context_root, context_root, 32);
    (void)zcl_hex_decode_lower(action.proof_policy_root_sha3,
                               request.proof_policy_root, 32);
    (void)zcl_hex_decode_lower(job.toolchain_sha3,
                               request.toolchain_capsule_root, 32);
    if (!vcs_zcode_work_request_seal(
            &request, s_requester_secret, s_requester_pubkey))
        return;
    int64_t submit_started = platform_time_monotonic_us();
    enum vcs_zcode_work_node_result submitted = vcs_zcode_work_node_submit(
        work, peer, &request, now);
    int64_t submit_us = platform_time_monotonic_us() - submit_started;
    if (submitted != VCS_ZCODE_WORK_NODE_OK &&
        submitted != VCS_ZCODE_WORK_NODE_REPLAY)
        return;
    char context_hex[65];
    zcl_hex_encode(context_root, 32, context_hex);
    struct db_build_proof_event next;
    if (strcmp(event->state, "PEER_DISCOVERED") != 0) {
        int64_t discovery_us = now > event->created_at
            ? (now - event->created_at) * INT64_C(1000000) : 0;
        struct zcl_result discovered = build_fabric_proof_transition(
            ndb, action.action_id, "PEER_DISCOVERED", peer,
            request.request_id, context_hex, NULL, deadline,
            discovery_us, now, &next);
        if (!discovered.ok) return;
    }
    struct zcl_result running = build_fabric_proof_transition(
        ndb, action.action_id, "RUNNING", peer, request.request_id,
        context_hex, NULL, deadline, submit_us < 0 ? 0 : submit_us,
        now, &next);
    if (!running.ok) return;
}
bool boot_zcode_async_proof_observe_result(
    struct node_db *ndb, uint64_t peer,
    const struct vcs_zcode_work_request_v1 *request,
    const struct vcs_zcode_work_result_v1 *result,
    const char *receipt_root, int64_t now)
{
    if (!ndb || !ndb->open || !request || !result || !receipt_root ||
        !receipt_root[0] || peer == 0 || now <= 0)
        return false;
    char action_id[65];
    zcl_hex_encode(request->action_root, 32, action_id);
    struct db_build_proof_event current, remote, verified;
    if (!db_build_proof_event_latest(ndb, action_id, &current) ||
        current.request_id != request->request_id ||
        current.peer_id != peer)
        return false;
    if (strcmp(current.state, "RECEIPT_VERIFIED") == 0)
        return strcmp(current.receipt_root_sha3, receipt_root) == 0;
    bool running = strcmp(current.state, "RUNNING") == 0;
    bool remote_recorded = strcmp(current.state, "REMOTE_GREEN") == 0 ||
        strcmp(current.state, "REMOTE_RED") == 0;
    if ((!running && !remote_recorded) ||
        (running && current.deadline_at > 0 && now >= current.deadline_at))
        return false;
    int64_t remote_us = now > current.created_at
        ? (now - current.created_at) * INT64_C(1000000) : 0;
    const char *remote_state =
        result->receipt.status == VCS_ZCODE_WORK_PASS &&
        result->receipt.exit_status == 0 ? "REMOTE_GREEN" : "REMOTE_RED";
    if (running) {
        struct zcl_result marked = build_fabric_proof_transition(
            ndb, action_id, remote_state, peer, request->request_id, NULL,
            receipt_root, current.deadline_at, remote_us, now, &remote);
        if (!marked.ok) return false;
        current = remote;
    } else if (strcmp(current.receipt_root_sha3, receipt_root) != 0) {
        return false;
    }
    struct zcl_result marked = build_fabric_proof_transition(
        ndb, action_id, "RECEIPT_VERIFIED", peer, request->request_id,
        NULL, receipt_root, current.deadline_at, 0, now, &verified);
    return marked.ok;
}

bool boot_zcode_async_proof_workspace(
    struct node_db *ndb, const struct vcs_zcode_work_request_v1 *request,
    char out[4096])
{
    if (out) out[0] = '\0';
    if (!ndb || !ndb->open || !request || !out) return false;
    char action_id[65];
    zcl_hex_encode(request->action_root, 32, action_id);
    struct db_build_proof_event event;
    if (!db_build_proof_event_latest(ndb, action_id, &event) ||
        event.request_id != request->request_id || !event.workspace[0])
        return false;
    int n = snprintf(out, 4096, "%s", event.workspace);
    return n > 0 && n < 4096;
}

static void async_reproduce(
    struct node_db *ndb, const struct db_build_proof_event *event,
    int64_t now)
{
    struct build_fabric_proof_evaluation evaluation;
    struct zcl_result evaluated = build_fabric_proof_evaluate(
        ndb, event->workspace, event->action_id, now, &evaluation);
    if (!evaluated.ok || !evaluation.local_reproduced) return;
    struct db_build_proof_event reproduced, ready;
    if (!build_fabric_proof_transition(
            ndb, event->action_id, "REPRODUCED", event->peer_id,
            event->request_id, NULL, NULL, event->deadline_at, 0, now,
            &reproduced).ok)
        return;
    ZCL_IGNORE_RESULT(build_fabric_proof_transition(
        ndb, event->action_id, "READY_FOR_ACCEPTANCE", event->peer_id,
        event->request_id, NULL, NULL, event->deadline_at, 0, now, &ready),
        "REPRODUCED stays durable and the next async tick retries readiness");
}

void boot_zcode_async_proof_tick(
    struct boot_svc_ctx *svc, struct vcs_zcode_work_node *work, int64_t now)
{
    struct node_db *ndb = app_runtime_node_db();
    if (!svc || !work || !ndb || !ndb->open || now <= 0) return;
    struct db_build_proof_event events[ASYNC_PROOF_BATCH];
    int count = db_build_proof_events_pending(
        ndb, events, ASYNC_PROOF_BATCH);
    for (int i = 0; i < count; i++) {
        bool dispatchable = strcmp(events[i].state, "REQUESTED") == 0 ||
            strcmp(events[i].state, "PEER_DISCOVERED") == 0 ||
            (strcmp(events[i].state, "RUNNING") == 0 &&
             events[i].deadline_at > 0 && now >= events[i].deadline_at);
        if (dispatchable)
            async_dispatch(svc, work, ndb, &events[i], now);
        else if (strcmp(events[i].state, "REMOTE_GREEN") == 0 ||
                 strcmp(events[i].state, "REMOTE_RED") == 0) {
            struct db_build_proof_event verified;
            ZCL_IGNORE_RESULT(build_fabric_proof_transition(
                ndb, events[i].action_id, "RECEIPT_VERIFIED",
                events[i].peer_id, events[i].request_id, NULL, NULL,
                events[i].deadline_at, 0, now, &verified),
                "remote evidence stays durable and the result FIFO retries");
        }
        else if (strcmp(events[i].state, "RECEIPT_VERIFIED") == 0)
            async_reproduce(ndb, &events[i], now);
    }
}
