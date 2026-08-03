/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_zcode_swarm — see config/include/config/boot_zcode_swarm.h. This
 * unit is the ONLY place where lib/net peer facts (node id, host key,
 * peer_state, peer_scoring) meet the swarm engine's contract; both sides
 * stay pure of each other. */
#include "config/boot_zcode_swarm.h"
#include "config/boot_zcode_dht.h"
#include "config/boot_internal.h"
#include "config/runtime.h"
#include "config/boot_zcode_work_authority.h"
#include "base/hex.h"
#include "base/safe_alloc.h"
#include "vcs/package_reward.h"
#include "vcs/package_service.h"
#include "vcs/package_store.h"
#include "vcs/package_swarm_node.h"
#include "vcs/build_action.h"
#include "vcs/build_artifact_manifest.h"
#include "vcs/zcode_work_node.h"
#include "vcs/zcode_work_context.h"
#include "vcs/vcs_object.h"
#include "crypto/sha3.h"
#include "event/event.h"
#include "net/fast_sync.h"
#include "net/net.h"
#include "net/peer_identity.h"
#include "net/peer_scoring.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/supervisor.h"
#include "util/sync.h"
#include "util/util.h"
#include "services/build_fabric_worker.h"
#include "services/build_fabric_service.h"
#include "supervisors/domains.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Session pseudo-key domain (0x02 || SHA3-256(domain || host)). The host
 * identity comes from zcl_peer_host_key (onion hostname or IP), so the
 * key scopes the service book to a transport endpoint, survives a
 * reconnect of the same host, and is never a contributor identity. */
#define ZCODE_SWARM_KEY_DOMAIN "zcl.zcode_swarm_peer.v1"

/* Membership sync / scheduler-tick throttle (seconds). The swarm is
 * clock-driven: a supervisor child (net.zcode_swarm, 1 s period) runs the
 * periodic sync/tick/drain even when no peer has inbound traffic — the
 * per-peer message-cycle hook (boot_zcode_swarm_tick) only fires while a
 * peer has queued messages, so an idle-but-healthy connection would
 * otherwise never announce, never WANT, and never drain. Both paths share
 * the same throttled helpers below. */
#define ZCODE_SWARM_SYNC_PERIOD_SEC 15
#define ZCODE_SWARM_TICK_PERIOD_SEC 1

static zcl_mutex_t s_lock;
static bool s_lock_init;
static struct vcs_swarm_engine *s_engine;   /* owned here */
static struct vcs_zcode_work_node *s_work;  /* owned work adapter */
static struct vcs_service_book *s_book;     /* owned here */
static struct vcs_reward_ledger *s_ledger;  /* owned here; may be NULL */
static char s_zcode_dir[4400];
static int64_t s_last_sync;                 /* wall seconds */
static int64_t s_last_tick;
static uint8_t s_work_secret[32];
static uint8_t s_work_pubkey[32];
static bool s_work_key_ready;
static char s_work_workspace[4096];
static struct boot_svc_ctx *s_svc;          /* borrowed; set by wire() */
static struct liveness_contract s_timer_contract;
static supervisor_child_id s_timer_child = SUPERVISOR_INVALID_ID;
static uint64_t s_frames_sent;              /* supervisor progress marker */

static bool boot_zcode_work_workspace(void)
{
    if (s_work_workspace[0]) return true;
    return getcwd(s_work_workspace, sizeof(s_work_workspace)) != NULL;
}

static bool boot_zcode_work_active_state(const char *state)
{
    return state && (strcmp(state, "QUEUED") == 0 ||
                     strcmp(state, "CLAIMED") == 0 ||
                     strcmp(state, "RUNNING") == 0 ||
                     strcmp(state, "VERIFYING") == 0 ||
                     strcmp(state, "ACCEPTED") == 0 ||
                     strcmp(state, "CACHE_HIT") == 0);
}

static const char *boot_zcode_work_action_kind(uint8_t work_kind)
{
    if (work_kind == VCS_ZCODE_WORK_BUILD)
        return VCS_BUILD_ACTION_KIND_V1;
    if (work_kind == VCS_ZCODE_WORK_TEST)
        return VCS_BUILD_ACTION_KIND_TEST_V1;
    if (work_kind == VCS_ZCODE_WORK_FUZZ)
        return VCS_BUILD_ACTION_KIND_FUZZ_V1;
    return NULL;
}

/* Rebuild content.v2 into the fixed action; only ZBuild state is mutable. */
static struct zcl_result boot_zcode_work_admit(
    const struct vcs_zcode_work_request_v1 *request, int64_t now)
{
    struct vcs_package_store *store = vcs_package_store_global();
    struct node_db *ndb = app_runtime_node_db();
    if (!request || !store || !ndb || !ndb->open ||
        !boot_zcode_work_workspace())
        return ZCL_ERR(-1, "work admission owners unavailable");
    struct vcs_zcode_work_context_v1 context;
    enum vcs_zcode_work_context_result loaded =
        vcs_zcode_work_context_get(store, request->context_root, now,
                                   &context);
    if (loaded != VCS_ZCODE_WORK_CONTEXT_OK)
        return ZCL_ERR(-1, "context: %s",
                       vcs_zcode_work_context_result_string(loaded));
    const char *action_kind = boot_zcode_work_action_kind(request->work_kind);
    uint8_t action_root[32], input_root[32], task_root[32];
    uint8_t candidate_root[32], policy_root[32];
    loaded = action_kind
        ? vcs_zcode_work_context_action_root_for_kind(
              &context, action_kind, now, action_root, input_root)
        : VCS_ZCODE_WORK_CONTEXT_ACTION;
    bool bound = loaded == VCS_ZCODE_WORK_CONTEXT_OK &&
        vcs_zcode_task_root(&context.task, task_root) == VCS_ZCODE_DEV_OK &&
        vcs_zcode_candidate_root(&context.candidate, candidate_root) ==
            VCS_ZCODE_DEV_OK &&
        vcs_zcode_proof_policy_root(&context.proof_policy, policy_root) ==
            VCS_ZCODE_DEV_OK &&
        memcmp(action_root, request->action_root, 32) == 0 &&
        memcmp(input_root, request->input_root, 32) == 0 &&
        memcmp(task_root, request->task_root, 32) == 0 &&
        memcmp(candidate_root, request->candidate_root, 32) == 0 &&
        memcmp(policy_root, request->proof_policy_root, 32) == 0 &&
        memcmp(context.task.toolchain_capsule_root,
               request->toolchain_capsule_root, 32) == 0 &&
        action_kind != NULL &&
        request->max_cpu_seconds <= context.task.max_cpu_seconds &&
        request->max_memory_bytes <= context.task.max_memory_bytes &&
        request->max_output_bytes <= context.task.max_output_bytes;
    if (!bound) {
        vcs_zcode_work_context_free(&context);
        return ZCL_ERR(-1, "context does not reconstruct the signed request");
    }
    struct zcl_result authority = boot_zcode_work_authority_import(
        store, request->context_root, s_work_workspace, &context);
    if (!authority.ok) {
        vcs_zcode_work_context_free(&context);
        return authority;
    }
    uint8_t task_wire[VCS_ZCODE_TASK_WIRE_BYTES];
    uint8_t candidate_wire[VCS_ZCODE_CANDIDATE_WIRE_BYTES];
    uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
    bool stored = vcs_zcode_task_serialize(&context.task, task_wire) ==
                      VCS_ZCODE_DEV_OK &&
        vcs_zcode_candidate_serialize(&context.candidate, candidate_wire) ==
                      VCS_ZCODE_DEV_OK &&
        vcs_zcode_proof_policy_serialize(
            &context.proof_policy, policy_wire) == VCS_ZCODE_DEV_OK &&
        vcs_object_store_init(s_work_workspace) &&
        vcs_object_put_addressed(s_work_workspace, task_root, task_wire,
                                 sizeof(task_wire)) &&
        vcs_object_put_addressed(s_work_workspace, candidate_root,
                                 candidate_wire, sizeof(candidate_wire)) &&
        vcs_object_put_addressed(s_work_workspace, policy_root, policy_wire,
                                 sizeof(policy_wire)) &&
        vcs_object_put_addressed(s_work_workspace, input_root,
                                 context.fixed_input,
                                 context.fixed_input_len);
    if (!stored) {
        vcs_zcode_work_context_free(&context);
        return ZCL_ERR(-1, "context objects could not enter workspace CAS");
    }
    struct db_build_job job = {0};
    struct db_build_action action = {0};
    zcl_hex_encode(context.source_sha256, 32, job.source_sha256);
    zcl_hex_encode(context.candidate.candidate_source_root, 32,
                   job.source_cas_sha3);
    zcl_hex_encode(context.task.toolchain_capsule_root, 32,
                   job.toolchain_sha3);
    (void)snprintf(job.profile, sizeof(job.profile), "%s", context.profile);
    (void)snprintf(job.state, sizeof(job.state), "PLANNED");
    job.created_at = job.updated_at = now;
    action.sequence = 0;
    (void)snprintf(action.kind, sizeof(action.kind), "%s",
                   action_kind);
    (void)snprintf(action.state, sizeof(action.state), "SNAPSHOTTED");
    zcl_hex_encode(input_root, 32, action.input_root_sha3);
    zcl_hex_encode(task_root, 32, action.task_root_sha3);
    zcl_hex_encode(candidate_root, 32, action.candidate_root_sha3);
    zcl_hex_encode(policy_root, 32, action.proof_policy_root_sha3);
    zcl_hex_encode(request->context_root, 32, action.context_root_sha3);
    (void)snprintf(action.target, sizeof(action.target), "%s",
                   VCS_BUILD_TARGET_V1);
    uint8_t fixed_flags[32], fixed_environment[32];
    const char *workdir = NULL, *output = NULL, *resource = NULL;
    if (!vcs_build_action_v1_descriptors(
            action_kind, &workdir, &output, &resource) ||
        !vcs_build_action_v1_fixed_flags_root_for_kind(
            action_kind, fixed_flags) ||
        !vcs_build_action_v1_fixed_environment_root_for_kind(
            action_kind, fixed_environment)) {
        vcs_zcode_work_context_free(&context);
        return ZCL_ERR(-1, "fixed action descriptor disappeared");
    }
    zcl_hex_encode(fixed_flags, 32, action.flags_sha3);
    zcl_hex_encode(fixed_environment, 32, action.environment_sha3);
    (void)snprintf(action.virtual_workdir, sizeof(action.virtual_workdir),
                   "%s", workdir);
    (void)snprintf(action.declared_outputs, sizeof(action.declared_outputs),
                   "%s", output);
    (void)snprintf(action.resource_policy, sizeof(action.resource_policy),
                   "%s", resource);
    action.created_at = action.updated_at = now;
    struct zcl_result ids = build_fabric_action_id(
        &job, &action, action.action_id);
    if (ids.ok && strcmp(action.action_id, "") != 0) {
        uint8_t checked[32];
        ids.ok = zcl_hex_decode_lower(action.action_id, checked, 32) &&
                 memcmp(checked, request->action_root, 32) == 0;
    }
    if (ids.ok)
        ids = build_fabric_job_id(&job, action.action_id, job.job_id);
    if (ids.ok)
        (void)snprintf(action.job_id, sizeof(action.job_id), "%s",
                       job.job_id);
    vcs_zcode_work_context_free(&context);
    if (!ids.ok) return ZCL_ERR(-1, "context action identity mismatch");
    ZCL_CHECK(build_fabric_plan(ndb, &job, &action));
    struct db_build_action current;
    if (!db_build_action_find(ndb, action.action_id, &current))
        return ZCL_ERR(-1, "planned remote action disappeared");
    if (strcmp(current.state, "SNAPSHOTTED") == 0)
        return build_fabric_submit(ndb, job.job_id, now);
    if (!boot_zcode_work_active_state(current.state))
        return ZCL_ERR(-1, "remote action is terminal: %s", current.state);
    return ZCL_OK;
}

static void boot_zcode_work_drain_admissions(int64_t now)
{
    if (!s_work || !GetBoolArg("-buildworker", false)) return;
    for (;;) {
        uint64_t peer = 0;
        struct vcs_zcode_work_request_v1 request;
        if (!vcs_zcode_work_node_peek_request(s_work, &peer, &request))
            break;
        struct vcs_package_store_status status;
        struct vcs_package_store *store = vcs_package_store_global();
        if (!store || !vcs_package_store_package_status(
                store, request.context_root, &status) || !status.complete)
            break;
        struct zcl_result admitted = boot_zcode_work_admit(&request, now);
        uint64_t drained_peer = 0;
        struct vcs_zcode_work_request_v1 drained;
        if (!vcs_zcode_work_node_next_request(
                s_work, &drained_peer, &drained) || drained_peer != peer ||
            drained.request_id != request.request_id) {
            LOG_ERROR("net.zcode_swarm", "work admission FIFO changed");
            break;
        }
        if (!admitted.ok)
            LOG_WARN("net.zcode_swarm", "request %llu refused: %s",
                     (unsigned long long)request.request_id,
                     admitted.message);
    }
}

static void boot_zcode_work_drain_cancels(int64_t now)
{
    struct node_db *ndb = app_runtime_node_db();
    if (!s_work || !ndb || !ndb->open) return;
    uint64_t peer = 0;
    struct vcs_zcode_work_cancel_v1 cancel;
    while (vcs_zcode_work_node_next_cancel(s_work, &peer, &cancel)) {
        struct vcs_zcode_work_request_v1 request;
        bool cancelled = false;
        if (!vcs_zcode_work_node_inbound_request(
                s_work, peer, cancel.request_id, &request, &cancelled) ||
            !cancelled)
            continue;
        char action_id[65];
        zcl_hex_encode(request.action_root, 32, action_id);
        struct db_build_action action;
        if (db_build_action_find(ndb, action_id, &action)) {
            struct zcl_result result = build_fabric_cancel(
                ndb, action.job_id, now);
            if (!result.ok)
                LOG_WARN("net.zcode_swarm", "cancel %llu: %s",
                         (unsigned long long)cancel.request_id,
                         result.message);
        }
    }
}

static void boot_zcode_work_publish_results(int64_t now)
{
    (void)now;
    struct node_db *ndb = app_runtime_node_db();
    if (!s_work || !ndb || !ndb->open || !s_work_key_ready ||
        !boot_zcode_work_workspace())
        return;
    uint64_t peers[VCS_ZCODE_WORK_NODE_MAX_REQUESTS];
    struct vcs_zcode_work_request_v1 requests[
        VCS_ZCODE_WORK_NODE_MAX_REQUESTS];
    size_t count = vcs_zcode_work_node_inbound_requests(
        s_work, peers, requests, VCS_ZCODE_WORK_NODE_MAX_REQUESTS);
    for (size_t i = 0; i < count; i++) {
        char action_id[65];
        zcl_hex_encode(requests[i].action_root, 32, action_id);
        struct db_build_action action;
        if (!db_build_action_find(ndb, action_id, &action) ||
            (strcmp(action.state, "ACCEPTED") != 0 &&
             strcmp(action.state, "CACHE_HIT") != 0 &&
             strcmp(action.state, "FAILED") != 0))
            continue;
        struct db_build_receipt receipts[8];
        int receipt_count = db_build_job_receipts(
            ndb, action.job_id, receipts, 8);
        for (int j = 0; j < receipt_count; j++) {
            if (strcmp(receipts[j].action_id, action_id) != 0) continue;
            uint8_t receipt_root[32]; uint8_t *wire = NULL;
            size_t wire_len = 0;
            if (!zcl_hex_decode_lower(receipts[j].work_receipt_sha3,
                                      receipt_root, 32) ||
                vcs_object_load_raw(s_work_workspace, receipt_root, &wire,
                                    &wire_len) != 0)
                continue;
            struct vcs_zcode_work_result_v1 result = {
                .request_id = requests[i].request_id,
            };
            memcpy(result.task_root, requests[i].task_root, 32);
            memcpy(result.candidate_root, requests[i].candidate_root, 32);
            memcpy(result.action_root, requests[i].action_root, 32);
            bool parsed = vcs_zcode_work_receipt_parse(
                wire, wire_len, &result.receipt) == VCS_ZCODE_DEV_OK;
            free(wire);
            if (!parsed) continue;
            memcpy(result.output_root, result.receipt.output_root, 32);
            if (!vcs_zcode_work_result_verify(
                    &requests[i], &result, s_work_pubkey))
                continue;
            enum vcs_zcode_work_node_result published =
                vcs_zcode_work_node_publish_result(
                    s_work, peers[i], &result);
            if (published != VCS_ZCODE_WORK_NODE_OK)
                LOG_WARN("net.zcode_swarm", "result %llu: %s",
                         (unsigned long long)requests[i].request_id,
                         vcs_zcode_work_node_result_string(published));
            break;
        }
    }
}

static void boot_zcode_work_observe_results(int64_t now)
{
    struct node_db *ndb = app_runtime_node_db();
    if (!s_work || !ndb || !ndb->open || !boot_zcode_work_workspace())
        return;
    for (;;) {
        uint64_t peer = 0;
        struct vcs_zcode_work_result_v1 result;
        if (!vcs_zcode_work_node_peek_result(s_work, &peer, &result)) break;
        struct vcs_zcode_work_request_v1 request;
        if (!vcs_zcode_work_node_outbound_request(
                s_work, peer, result.request_id, &request)) {
            LOG_ERROR("net.zcode_swarm", "verified result lost its request");
            break;
        }
        char receipt_id[65];
        struct zcl_result observed = build_fabric_receipt_observe_remote(
            ndb, s_work_workspace, &request, &result, now, receipt_id);
        if (!observed.ok) {
            LOG_WARN("net.zcode_swarm", "result %llu not durable: %s",
                     (unsigned long long)result.request_id,
                     observed.message);
            break;
        }
        uint64_t drained_peer = 0;
        struct vcs_zcode_work_result_v1 drained;
        if (!vcs_zcode_work_node_next_result(
                s_work, &drained_peer, &drained) || drained_peer != peer ||
            drained.request_id != result.request_id) {
            LOG_ERROR("net.zcode_swarm", "work result FIFO changed");
            break;
        }
        LOG_INFO("net.zcode_swarm", "remote receipt %s observed untrusted",
                 receipt_id);
    }
}

static bool boot_zcode_work_refresh(struct boot_svc_ctx *svc, int64_t wall)
{
    if (!s_work || !GetBoolArg("-buildworker", false)) return true;
    if (!s_work_key_ready) {
        struct db_build_worker worker;
        struct zcl_result loaded = build_fabric_worker_identity_load(
            svc->datadir, &worker, s_work_secret, s_work_pubkey);
        if (!loaded.ok)
            LOG_FAIL("net.zcode_swarm", "work identity: %s", loaded.message);
        s_work_key_ready = true;
    }
    struct vcs_toolchain_capsule_v1 capsule;
    struct vcs_zcode_work_capability_v1 capability = {0};
    if (!vcs_toolchain_capsule_v1_capture_gcc(&capsule) ||
        !vcs_toolchain_capsule_v1_root(
            &capsule, capability.toolchain_capsule_root))
        LOG_FAIL("net.zcode_swarm", "work toolchain capture failed");
    capability.work_kinds = (UINT32_C(1) << VCS_ZCODE_WORK_BUILD) |
                            (UINT32_C(1) << VCS_ZCODE_WORK_TEST) |
                            (UINT32_C(1) << VCS_ZCODE_WORK_FUZZ);
    capability.target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3;
    capability.confinement = VCS_ZCODE_WORK_CONFINEMENT_V1_MASK;
    capability.max_cpu_seconds = 580;
    capability.max_memory_bytes = UINT64_C(2) * 1024u * 1024u * 1024u;
    capability.max_output_bytes = VCS_BUILD_ARTIFACT_MAX_BYTES;
    capability.max_lease_seconds = 600;
    capability.slots = 1;
    capability.queue_headroom = 1;
    capability.expires_unix = wall + 600;
    if (!vcs_zcode_work_capability_seal(
            &capability, s_work_secret, s_work_pubkey) ||
        !vcs_zcode_work_node_set_local_capability(s_work, &capability))
        LOG_FAIL("net.zcode_swarm", "work capability signing failed");
    return true;
}

static void boot_zcode_swarm_lock(void)
{
    if (!s_lock_init) {
        /* wire() initializes eagerly before the supervisor child can
         * fire; this fallback covers a frame arriving on an unwired
         * msg_processor (single message-handler thread at that point). */
        zcl_mutex_init(&s_lock);
        s_lock_init = true;
    }
    zcl_mutex_lock(&s_lock);
}

/* score_fn for the engine: earned score from the reward ledger. Pseudo-
 * keys never appear in the ledger (transport scope, not identity), so
 * this resolves to zero in practice today; it exists so a future slice
 * that swaps in authenticated peer keys needs no engine change. */
static uint64_t boot_zcode_swarm_score(const uint8_t contributor[33],
                                       void *ctx)
{
    (void)ctx;
    if (!s_ledger)
        return 0;
    struct vcs_reward_contributor_totals t;
    memset(&t, 0, sizeof(t));
    vcs_reward_contributor_totals(s_ledger, contributor, &t);
    return t.earned_score;
}

/* Engine peer handle for a node. node->id starts at 0 on a fresh
 * net_manager (nm->last_node_id++), and the engine reserves peer id 0
 * (the next_outbound no-filter convention), so translate by +1. */
static uint64_t boot_zcode_swarm_peer_id(const struct p2p_node *node)
{
    return (uint64_t)node->id + 1u;
}

static bool boot_zcode_swarm_peer_key(const struct p2p_node *node,
                                      uint8_t out[33])
{
    char host[ZCL_PEER_HOST_KEY_MAX];
    if (!zcl_peer_host_key(node, host, sizeof(host)))
        return false;
    out[0] = 0x02;
    struct sha3_256_ctx h;
    sha3_256_init(&h);
    sha3_256_write(&h, (const unsigned char *)ZCODE_SWARM_KEY_DOMAIN,
                   sizeof(ZCODE_SWARM_KEY_DOMAIN) - 1);
    sha3_256_write(&h, (const unsigned char *)host, strlen(host));
    sha3_256_finalize(&h, out + 1);
    return true;
}

/* Lazily create the node-global engine. Caller holds s_lock. Returns
 * NULL (named, logged) when hosting is off, the store is not open, the
 * datadir is missing, or allocation fails — the swarm simply stays off
 * for that frame/tick; nothing here is fatal. */
static struct vcs_swarm_engine *boot_zcode_swarm_ensure(
    struct boot_svc_ctx *svc)
{
    if (s_engine)
        return s_engine;
    if (!svc || !svc->datadir)
        return NULL;
    if (!GetBoolArg("-packagehost", false))
        return NULL;
    struct vcs_package_store *store = vcs_package_store_global();
    if (!store)
        return NULL;
    int n = snprintf(s_zcode_dir, sizeof(s_zcode_dir), "%s/zcode",
                     svc->datadir);
    if (n < 0 || (size_t)n >= sizeof(s_zcode_dir))
        LOG_NULL("net.zcode_swarm", "datadir path too long");
    s_book = vcs_service_book_load(s_zcode_dir);
    if (!s_book)
        LOG_NULL("net.zcode_swarm",
                 "service book unavailable; swarm off");
    /* The ledger is optional: tier resolution degrades to NEW_USER. */
    s_ledger = vcs_reward_ledger_load(s_zcode_dir);
    s_engine = vcs_swarm_engine_create(store, s_book, s_zcode_dir,
                                       boot_zcode_swarm_score, NULL);
    if (!s_engine) {
        vcs_reward_ledger_free(s_ledger);
        s_ledger = NULL;
        vcs_service_book_free(s_book);
        s_book = NULL;
        LOG_NULL("net.zcode_swarm", "engine create failed; swarm off");
    }
    vcs_swarm_engine_set_global(s_engine);
    s_work = vcs_zcode_work_node_create();
    if (!s_work) {
        vcs_swarm_engine_set_global(NULL);
        vcs_swarm_engine_free(s_engine); s_engine = NULL;
        vcs_reward_ledger_free(s_ledger); s_ledger = NULL;
        vcs_service_book_free(s_book); s_book = NULL;
        LOG_NULL("net.zcode_swarm", "work adapter create failed; swarm off");
    }
    vcs_zcode_work_node_set_global(s_work);
    if (!boot_zcode_work_refresh(svc,
            (int64_t)platform_time_wall_time_t())) {
        vcs_zcode_work_node_set_global(NULL);
        vcs_zcode_work_node_free(s_work); s_work = NULL;
        vcs_swarm_engine_set_global(NULL);
        vcs_swarm_engine_free(s_engine); s_engine = NULL;
        vcs_reward_ledger_free(s_ledger); s_ledger = NULL;
        vcs_service_book_free(s_book); s_book = NULL;
        LOG_NULL("net.zcode_swarm", "work capability failed; swarm off");
    }
    return s_engine;
}

/* A node is swarm-eligible once the version handshake completed, it
 * speaks the zclassic23 service bit, and it is not being torn down. */
static bool boot_zcode_swarm_eligible(const struct p2p_node *node)
{
    enum peer_state st = atomic_load(&node->state);
    if (st < PEER_HANDSHAKE_COMPLETE || st > PEER_SNAPSHOT_RECEIVING)
        return false;
    if (atomic_load(&node->disconnect))
        return false;
    if (node->is_feeler || node->one_shot)
        return false;
    return peer_supports_fast_sync(node->services);
}

static enum peer_offence boot_zcode_swarm_offence(
    enum vcs_swarm_penalty penalty)
{
    switch (penalty) {
    case VCS_SWARM_PENALTY_MALFORMED:
        return PEER_OFFENCE_INVALID_MESSAGE;
    case VCS_SWARM_PENALTY_ANNOUNCE_FLOOD:
    case VCS_SWARM_PENALTY_REQUEST_FLOOD:
        return PEER_OFFENCE_FLOOD;
    case VCS_SWARM_PENALTY_REPLAYED_REQUEST:
    case VCS_SWARM_PENALTY_REPLAYED_DATA:
        return PEER_OFFENCE_INVALID_PAYLOAD;
    case VCS_SWARM_PENALTY_UNREQUESTED_DATA:
        return PEER_OFFENCE_UNREQUESTED;
    case VCS_SWARM_PENALTY_INVALID_DATA:
        return PEER_OFFENCE_INVALID_CHUNK;
    case VCS_SWARM_PENALTY_NONE:
    default:
        return PEER_OFFENCE_NONE;
    }
}

static void boot_zcode_swarm_send(struct msg_processor *mp,
                                  struct p2p_node *node,
                                  const uint8_t *frame, size_t frame_len)
{
    if (!p2p_node_begin_message(node, "zpkgswm",
                                mp->params->pchMessageStart)) {
        LOG_ERROR("net.zcode_swarm", "begin_message failed for peer %lld",
                  (long long)node->id);
        return;
    }
    p2p_node_write_message_data(node, frame, frame_len);
    if (!p2p_node_end_message(node))
        LOG_ERROR("net.zcode_swarm", "end_message failed for peer %lld",
                  (long long)node->id);
}

bool boot_zcode_swarm_frame(struct msg_processor *mp, struct p2p_node *node,
                            const uint8_t *payload, size_t payload_len,
                            void *ctx)
{
    if (!mp || !node || !payload)
        LOG_FAIL("net.zcode_swarm", "null mp/node/payload");
    if (boot_zcode_dht_frame(mp, node, payload, payload_len,
                             (struct boot_svc_ctx *)ctx))
        return true;
    boot_zcode_swarm_lock();
    struct vcs_swarm_engine *engine =
        boot_zcode_swarm_ensure((struct boot_svc_ctx *)ctx);
    if (!engine) {
        zcl_mutex_unlock(&s_lock);
        return true; /* hosting off: drop quietly (never an offence) */
    }
    uint8_t key[33];
    if (!boot_zcode_swarm_peer_key(node, key)) {
        zcl_mutex_unlock(&s_lock);
        return true; /* no usable host identity: drop, never an offence */
    }
    (void)vcs_swarm_engine_peer_add(engine, boot_zcode_swarm_peer_id(node),
                                    key);
    int64_t day = (int64_t)platform_time_wall_time_t() / 86400;
    uint64_t now = (uint64_t)platform_time_wall_time_t();
    uint64_t peer_id = boot_zcode_swarm_peer_id(node);
    (void)vcs_zcode_work_node_peer_add(s_work, peer_id);
    if (payload_len >= 4 && memcmp(payload, "ZCWS", 4) == 0) {
        enum vcs_zcode_work_node_result wr =
            vcs_zcode_work_node_handle_frame(s_work, peer_id, payload,
                                              payload_len, (int64_t)now);
        if (wr == VCS_ZCODE_WORK_NODE_OK) {
            struct vcs_zcode_work_swarm_message message;
            if (vcs_zcode_work_swarm_parse(payload, payload_len, &message) &&
                message.type == VCS_ZCODE_WORK_SWARM_REQUEST)
                (void)vcs_swarm_engine_fetch(
                    engine, message.body.request.context_root, day, now);
        }
        zcl_mutex_unlock(&s_lock);
        if (wr == VCS_ZCODE_WORK_NODE_MALFORMED ||
            wr == VCS_ZCODE_WORK_NODE_REPLAY ||
            wr == VCS_ZCODE_WORK_NODE_UNREQUESTED ||
            wr == VCS_ZCODE_WORK_NODE_BINDING) {
            char context[96];
            (void)snprintf(context, sizeof(context), "zcode work: %s",
                           vcs_zcode_work_node_result_string(wr));
            if (mp->net_mgr)
                peer_scoring_record(mp->net_mgr, node,
                                    PEER_OFFENCE_INVALID_PAYLOAD, context);
        }
        return true;
    }
    struct vcs_swarm_frame_result ev = vcs_swarm_engine_handle_frame(
        engine, boot_zcode_swarm_peer_id(node), payload, payload_len, day,
        now);
    zcl_mutex_unlock(&s_lock);

    if (ev.penalty != VCS_SWARM_PENALTY_NONE && mp->net_mgr) {
        char context[96];
        (void)snprintf(context, sizeof(context), "zcode swarm: %s",
                       ev.rule ? ev.rule : "penalty");
        peer_scoring_record(mp->net_mgr, node,
                            boot_zcode_swarm_offence(ev.penalty), context);
    }
    if (ev.reply) {
        if (ev.reply_len > 0 && !atomic_load(&node->disconnect))
            boot_zcode_swarm_send(mp, node, ev.reply, ev.reply_len);
        free(ev.reply);
    }
    if (ev.disconnect_peer)
        atomic_store(&node->disconnect, true);
    return true;
}

/* Membership sync: under cs_nodes, add every eligible node (announcing
 * tracked packages to every known peer — deduped per peer, so only
 * newly complete roots are queued each sync) and drop engine peers whose
 * node is gone or no longer eligible. announce_to only queues frames —
 * no network I/O under cs_nodes. Caller holds s_lock. */
static void boot_zcode_swarm_sync_membership(struct msg_processor *mp,
                                             struct vcs_swarm_engine *engine)
{
    struct net_manager *nm = mp->net_mgr;
    if (!nm)
        return;
    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes; i++) {
        struct p2p_node *node = nm->nodes[i];
        if (!node || !boot_zcode_swarm_eligible(node))
            continue;
        uint8_t key[33];
        if (!boot_zcode_swarm_peer_key(node, key))
            continue;
        (void)vcs_swarm_engine_peer_add(engine,
                                        boot_zcode_swarm_peer_id(node),
                                        key);
        /* Deduped per peer: only newly complete roots queue, so calling
         * every sync propagates content published after the peer joined. */
        (void)vcs_swarm_engine_announce_to(
            engine, boot_zcode_swarm_peer_id(node));
        (void)vcs_zcode_work_node_peer_add(
            s_work, boot_zcode_swarm_peer_id(node));
    }
    uint64_t ids[VCS_SWARM_MAX_PEERS];
    size_t count = vcs_swarm_engine_peer_ids(engine, ids,
                                             VCS_SWARM_MAX_PEERS);
    for (size_t i = 0; i < count; i++) {
        bool live = false;
        for (size_t j = 0; j < nm->num_nodes; j++) {
            struct p2p_node *node = nm->nodes[j];
            if (node && boot_zcode_swarm_peer_id(node) == ids[i] &&
                boot_zcode_swarm_eligible(node)) {
                live = true;
                break;
            }
        }
        if (!live)
            vcs_swarm_engine_peer_drop(engine, ids[i]);
        if (!live)
            vcs_zcode_work_node_peer_drop(s_work, ids[i]);
    }
    zcl_mutex_unlock(&nm->cs_nodes);
}

/* Throttled periodic work shared by the message-cycle tick and the
 * supervisor timer: membership sync every SYNC_PERIOD, engine scheduler
 * tick + work-node drains every TICK_PERIOD. Caller holds s_lock. */
static void boot_zcode_swarm_periodic(struct msg_processor *mp,
                                      struct vcs_swarm_engine *engine,
                                      struct boot_svc_ctx *svc, int64_t wall)
{
    if (wall - s_last_sync >= ZCODE_SWARM_SYNC_PERIOD_SEC) {
        s_last_sync = wall;
        boot_zcode_swarm_sync_membership(mp, engine);
    }
    if (wall - s_last_tick >= ZCODE_SWARM_TICK_PERIOD_SEC) {
        s_last_tick = wall;
        vcs_swarm_engine_tick(engine, wall / 86400, (uint64_t)wall);
        vcs_zcode_work_node_tick(s_work, wall);
        boot_zcode_work_drain_admissions(wall);
        boot_zcode_work_drain_cancels(wall);
        boot_zcode_work_publish_results(wall);
        boot_zcode_work_observe_results(wall);
        if (GetBoolArg("-buildworker", false) && wall % 300 == 0)
            (void)boot_zcode_work_refresh(svc, wall);
    }
}

/* Drain queued frames for ONE node (bounded by the engine's outbound
 * queue; WANT/CANCEL/ANNOUNCE frames only — DATA replies go out
 * synchronously from the frame hook). Returns frames sent. Caller holds
 * s_lock; node must be ref-held by the caller. */
static size_t boot_zcode_swarm_drain_node(struct msg_processor *mp,
                                          struct vcs_swarm_engine *engine,
                                          struct p2p_node *node)
{
    if (!boot_zcode_swarm_eligible(node))
        return 0;
    size_t sent = 0;
    uint8_t frame[VCS_SWARM_OUTBOUND_FRAME_MAX];
    for (;;) {
        uint64_t peer_out = 0;
        size_t frame_len = 0;
        uint64_t peer_id = boot_zcode_swarm_peer_id(node);
        if (!vcs_swarm_engine_next_outbound(engine, peer_id,
                                            &peer_out, frame,
                                            &frame_len))
            break;
        if (peer_out != peer_id || frame_len == 0)
            break; /* defensive: filter contract violated */
        boot_zcode_swarm_send(mp, node, frame, frame_len);
        sent++;
    }
    uint8_t work_frame[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES];
    for (;;) {
        uint64_t peer_out = 0; size_t frame_len = 0;
        uint64_t peer_id = boot_zcode_swarm_peer_id(node);
        if (!vcs_zcode_work_node_next_outbound(
                s_work, peer_id, &peer_out, work_frame, &frame_len))
            break;
        if (peer_out != peer_id || frame_len == 0) break;
        boot_zcode_swarm_send(mp, node, work_frame, frame_len);
        sent++;
    }
    return sent;
}

void boot_zcode_swarm_tick(struct msg_processor *mp, struct p2p_node *node,
                           void *ctx)
{
    if (!mp || !node)
        return;
    int64_t wall = (int64_t)platform_time_wall_time_t();
    boot_zcode_swarm_lock();
    struct vcs_swarm_engine *engine =
        boot_zcode_swarm_ensure((struct boot_svc_ctx *)ctx);
    if (engine) {
        boot_zcode_swarm_periodic(mp, engine, (struct boot_svc_ctx *)ctx,
                                  wall);
        (void)boot_zcode_swarm_drain_node(mp, engine, node);
    }
    zcl_mutex_unlock(&s_lock);
}

/* Supervisor on_tick: the swarm's real clock. Fires every
 * ZCODE_SWARM_TICK_PERIOD_SEC regardless of inbound peer traffic — the
 * message-cycle hook above only runs while a peer has queued messages,
 * and an idle healthy connection still needs announces, WANTs, and
 * outbound drains. Progress marker = cumulative frames sent; a quiet
 * child with no active downloads reports idle, a quiet child WITH active
 * downloads reports neither (a wedged download should raise NO_PROGRESS). */
static void boot_zcode_swarm_timer_tick(struct liveness_contract *self)
{
    (void)self;
    struct boot_svc_ctx *svc = s_svc;
    if (!svc || !svc->msg_processor)
        return; /* not wired: nothing legitimate to report */
    struct msg_processor *mp = svc->msg_processor;
    boot_zcode_dht_periodic(mp, svc);
    int64_t wall = (int64_t)platform_time_wall_time_t();
    boot_zcode_swarm_lock();
    if (svc != s_svc) {
        zcl_mutex_unlock(&s_lock);
        return; /* shutdown raced us */
    }
    struct vcs_swarm_engine *engine = boot_zcode_swarm_ensure(svc);
    if (!engine) {
        zcl_mutex_unlock(&s_lock);
        /* Hosting off is a legitimate nothing-to-do; an ensure FAILURE
         * (store closed, alloc) is already logged and is not idleness. */
        if (!GetBoolArg("-packagehost", false))
            supervisor_progress_idle(s_timer_child);
        return;
    }
    boot_zcode_swarm_periodic(mp, engine, svc, wall);
    /* Drain ALL eligible peers: snapshot under cs_nodes (connman's
     * message-cycle pattern), send outside it. Lock order here is
     * s_lock -> cs_nodes, same as sync_membership. */
    size_t sent = 0;
    struct net_manager *nm = mp->net_mgr;
    if (nm) {
        size_t snap_count = 0;
        struct p2p_node **snap = NULL;
        zcl_mutex_lock(&nm->cs_nodes);
        size_t num = nm->num_nodes;
        if (num > 0) {
            snap = zcl_malloc(num * sizeof(*snap), "zcode_swarm_snap");
            if (snap) {
                for (size_t i = 0; i < num; i++) {
                    struct p2p_node *node = nm->nodes[i];
                    if (!node || atomic_load(&node->disconnect))
                        continue;
                    snap[snap_count++] = node;
                    p2p_node_add_ref(node);
                }
            }
        }
        zcl_mutex_unlock(&nm->cs_nodes);
        for (size_t i = 0; i < snap_count; i++) {
            sent += boot_zcode_swarm_drain_node(mp, engine, snap[i]);
            p2p_node_release(snap[i]);
        }
        free(snap);
    }
    size_t active = vcs_swarm_engine_active_downloads(engine);
    zcl_mutex_unlock(&s_lock);
    if (sent > 0) {
        s_frames_sent += sent;
        supervisor_progress(s_timer_child, (int64_t)s_frames_sent);
    } else if (active == 0) {
        supervisor_progress_idle(s_timer_child);
    }
}

void boot_zcode_swarm_wire(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->msg_processor) {
        LOG_ERROR("net.zcode_swarm", "wire: null svc/msg_processor");
        return;
    }
    /* Eager mutex init: the supervisor tick-runner races the lazy path
     * in boot_zcode_swarm_lock otherwise. wire() runs single-threaded
     * at boot before the child is armed. */
    if (!s_lock_init) {
        zcl_mutex_init(&s_lock);
        s_lock_init = true;
    }
    msg_processor_set_zcode_swarm(svc->msg_processor,
                                  boot_zcode_swarm_frame,
                                  boot_zcode_swarm_tick, svc);
    s_svc = svc;
    liveness_contract_init(&s_timer_contract, "net.zcode_swarm");
    s_timer_contract.on_tick = boot_zcode_swarm_timer_tick;
    supervisor_domains_init();
    s_timer_child = supervisor_register_in_domain(g_net_sup,
                                                  &s_timer_contract);
    if (s_timer_child == SUPERVISOR_INVALID_ID) {
        LOG_ERROR("net.zcode_swarm",
                  "supervisor register failed; swarm is message-driven only");
        return;
    }
    supervisor_set_period(s_timer_child, ZCODE_SWARM_TICK_PERIOD_SEC);
    supervisor_set_deadline(s_timer_child, 30);
    /* ARMED progress policy (gate-recognised form: one line, plain
     * non-zero literal — 30 min in us). A seeder with no peers is
     * legitimately quiet (idle-reported); a downloader that sends
     * nothing for 30 minutes is wedged. */
    supervisor_set_progress_max_quiet(s_timer_child, 1800000000);
}

void boot_zcode_swarm_shutdown(void)
{
    if (s_timer_child != SUPERVISOR_INVALID_ID) {
        supervisor_unregister(s_timer_child);
        s_timer_child = SUPERVISOR_INVALID_ID;
    }
    boot_zcode_dht_shutdown();
    boot_zcode_swarm_lock();
    s_svc = NULL;
    vcs_swarm_engine_set_global(NULL);
    vcs_zcode_work_node_set_global(NULL);
    if (s_work)
        vcs_zcode_work_node_free(s_work);
    s_work = NULL;
    if (s_engine)
        vcs_swarm_engine_free(s_engine);
    s_engine = NULL;
    if (s_book)
        vcs_service_book_free(s_book);
    s_book = NULL;
    if (s_ledger)
        vcs_reward_ledger_free(s_ledger);
    s_ledger = NULL;
    s_last_sync = 0;
    s_last_tick = 0;
    s_frames_sent = 0;
    memset(s_work_secret, 0, sizeof(s_work_secret));
    memset(s_work_pubkey, 0, sizeof(s_work_pubkey));
    s_work_key_ready = false;
    memset(s_work_workspace, 0, sizeof(s_work_workspace));
    zcl_mutex_unlock(&s_lock);
}
