/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Build coordinator lifecycle, trust, and receipt verification. */

#include "services/build_fabric_service.h"

#include "base/hex.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>

enum { BUILD_FABRIC_ACTION_LIMIT = 256 };

static void bf_sha_text(struct sha3_256_ctx *sha, const char *text)
{
    uint64_t len = text ? strlen(text) : 0;
    unsigned char le[8];
    for (unsigned i = 0; i < sizeof(le); i++)
        le[i] = (unsigned char)((len >> (i * 8U)) & 0xffU);
    sha3_256_write(sha, le, sizeof(le));
    if (len) sha3_256_write(sha, (const unsigned char *)text, (size_t)len);
}

static void bf_sha_i64(struct sha3_256_ctx *sha, int64_t value)
{
    uint64_t raw = (uint64_t)value;
    unsigned char le[8];
    for (unsigned i = 0; i < sizeof(le); i++)
        le[i] = (unsigned char)((raw >> (i * 8U)) & 0xffU);
    sha3_256_write(sha, le, sizeof(le));
}

static void bf_sha_finish(struct sha3_256_ctx *sha,
                          char out_hex[BUILD_FABRIC_ID_HEX + 1])
{
    uint8_t digest[32];
    sha3_256_finalize(sha, digest);
    zcl_hex_encode(digest, sizeof(digest), out_hex);
}

struct zcl_result build_fabric_action_id(
    const struct db_build_job *job, const struct db_build_action *action,
    char out_hex[BUILD_FABRIC_ID_HEX + 1])
{
    if (!job || !action || !out_hex)
        return ZCL_ERR(-1, "action id requires a job, action, and output");
    static const char domain[] = "zcl.build_action.v1";
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const unsigned char *)domain, sizeof(domain));
    bf_sha_text(&sha, action->kind);
    bf_sha_text(&sha, job->source_sha256);
    bf_sha_text(&sha, job->source_cas_sha3);
    bf_sha_text(&sha, action->input_root_sha3);
    bf_sha_text(&sha, job->toolchain_sha3);
    bf_sha_text(&sha, action->target);
    bf_sha_text(&sha, job->profile);
    bf_sha_text(&sha, action->flags_sha3);
    bf_sha_text(&sha, action->environment_sha3);
    bf_sha_text(&sha, action->virtual_workdir);
    bf_sha_text(&sha, action->declared_outputs);
    bf_sha_text(&sha, action->resource_policy);
    bf_sha_i64(&sha, action->sequence);
    bf_sha_finish(&sha, out_hex);
    return ZCL_OK;
}

struct zcl_result build_fabric_job_id(
    const struct db_build_job *job, const char *action_id,
    char out_hex[BUILD_FABRIC_ID_HEX + 1])
{
    if (!job || !action_id || strlen(action_id) != BUILD_FABRIC_ID_HEX ||
        !out_hex)
        return ZCL_ERR(-1, "job id requires immutable job inputs and action id");
    static const char domain[] = "zcl.build_job.v1";
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const unsigned char *)domain, sizeof(domain));
    bf_sha_text(&sha, job->source_sha256);
    bf_sha_text(&sha, job->source_cas_sha3);
    bf_sha_text(&sha, job->toolchain_sha3);
    bf_sha_text(&sha, job->profile);
    bf_sha_text(&sha, action_id);
    bf_sha_finish(&sha, out_hex);
    return ZCL_OK;
}

struct zcl_result build_fabric_receipt_id(
    const struct db_build_receipt *receipt,
    char out_hex[BUILD_FABRIC_ID_HEX + 1])
{
    if (!receipt || !out_hex)
        return ZCL_ERR(-1, "receipt id requires a receipt and output buffer");
    static const char domain[] = "zcl.build_receipt.v1";
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const unsigned char *)domain, sizeof(domain));
    bf_sha_text(&sha, receipt->action_id);
    bf_sha_text(&sha, receipt->job_id);
    bf_sha_text(&sha, receipt->worker_id);
    bf_sha_text(&sha, receipt->action_sha3);
    bf_sha_text(&sha, receipt->output_sha3);
    bf_sha_text(&sha, receipt->confinement);
    bf_sha_i64(&sha, receipt->exit_status);
    bf_sha_i64(&sha, receipt->created_at);
    bf_sha_finish(&sha, out_hex);
    return ZCL_OK;
}

static bool bf_job_same_plan(const struct db_build_job *a,
                             const struct db_build_job *b)
{
    return strcmp(a->source_sha256, b->source_sha256) == 0 &&
           strcmp(a->source_cas_sha3, b->source_cas_sha3) == 0 &&
           strcmp(a->toolchain_sha3, b->toolchain_sha3) == 0 &&
           strcmp(a->profile, b->profile) == 0;
}

static bool bf_action_same_plan(const struct db_build_action *a,
                                const struct db_build_action *b)
{
    return strcmp(a->job_id, b->job_id) == 0 &&
           a->sequence == b->sequence && strcmp(a->kind, b->kind) == 0 &&
           strcmp(a->input_root_sha3, b->input_root_sha3) == 0 &&
           strcmp(a->target, b->target) == 0 &&
           strcmp(a->flags_sha3, b->flags_sha3) == 0 &&
           strcmp(a->environment_sha3, b->environment_sha3) == 0 &&
           strcmp(a->virtual_workdir, b->virtual_workdir) == 0 &&
           strcmp(a->declared_outputs, b->declared_outputs) == 0 &&
           strcmp(a->resource_policy, b->resource_policy) == 0;
}

struct zcl_result build_fabric_plan(struct node_db *ndb,
                                    const struct db_build_job *job,
                                    const struct db_build_action *action)
{
    if (!ndb || !ndb->open || !job || !action)
        return ZCL_ERR(-1, "build plan requires an open db, job, and action");
    if (strcmp(job->state, "PLANNED") != 0 ||
        strcmp(action->state, "SNAPSHOTTED") != 0 ||
        strcmp(job->job_id, action->job_id) != 0)
        return ZCL_ERR(-1, "build plan lifecycle or ownership is invalid");
    struct db_build_job prior_job;
    bool have_job = db_build_job_find(ndb, job->job_id, &prior_job);
    if (have_job && !bf_job_same_plan(&prior_job, job))
        return ZCL_ERR(-1, "job id collides with a different immutable plan");
    struct db_build_action prior_action;
    bool have_action = db_build_action_find(ndb, action->action_id,
                                             &prior_action);
    if (have_action && !bf_action_same_plan(&prior_action, action))
        return ZCL_ERR(-1, "action id collides with different immutable inputs");
    /* An idempotent plan replay proves identity only.  In particular, it must
     * never rewind an already-queued or completed durable record. */
    if (have_job && have_action)
        return ZCL_OK;
    if (!node_db_begin(ndb))
        return ZCL_ERR(-1, "cannot begin build plan transaction");
    bool ok = (have_job || db_build_job_save(ndb, job)) &&
              (have_action || db_build_action_save(ndb, action)) &&
              node_db_commit(ndb);
    if (!ok) {
        if (!node_db_rollback(ndb))
            LOG_ERROR("build_fabric", "plan save and rollback both failed");
        return ZCL_ERR(-1, "cannot persist build plan atomically");
    }
    return ZCL_OK;
}

static bool bf_terminal(const char *state)
{
    return state && (strcmp(state, "ACCEPTED") == 0 ||
                     strcmp(state, "CACHE_HIT") == 0 ||
                     strcmp(state, "CANCELLED") == 0 ||
                     strcmp(state, "FAILED") == 0 ||
                     strcmp(state, "DISPUTED") == 0);
}

struct zcl_result build_fabric_submit(struct node_db *ndb,
                                      const char *job_id, int64_t now)
{
    struct db_build_job job;
    if (!ndb || !ndb->open || !job_id || !db_build_job_find(ndb, job_id, &job))
        return ZCL_ERR(-1, "build job not found");
    if (strcmp(job.state, "QUEUED") == 0)
        return ZCL_OK;
    if (strcmp(job.state, "PLANNED") != 0 &&
        strcmp(job.state, "SNAPSHOTTED") != 0)
        return ZCL_ERR(-1, "job state %s cannot transition to QUEUED", job.state);
    struct db_build_action actions[BUILD_FABRIC_ACTION_LIMIT];
    int count = db_build_job_actions(ndb, job_id, actions,
                                     BUILD_FABRIC_ACTION_LIMIT);
    if (count <= 0)
        return ZCL_ERR(-1, "build job has no actions");
    if (!node_db_begin(ndb))
        return ZCL_ERR(-1, "cannot begin build submit transaction");
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        if (bf_terminal(actions[i].state)) {
            ok = false;
            break;
        }
        (void)snprintf(actions[i].state, sizeof(actions[i].state), "QUEUED");
        actions[i].updated_at = now;
        ok = db_build_action_save(ndb, &actions[i]);
    }
    (void)snprintf(job.state, sizeof(job.state), "QUEUED");
    job.updated_at = now;
    ok = ok && db_build_job_save(ndb, &job) && node_db_commit(ndb);
    if (!ok) {
        if (!node_db_rollback(ndb))
            LOG_ERROR("build_fabric", "submit and rollback both failed");
        return ZCL_ERR(-1, "cannot queue build job atomically");
    }
    return ZCL_OK;
}

struct zcl_result build_fabric_cancel(struct node_db *ndb,
                                      const char *job_id, int64_t now)
{
    struct db_build_job job;
    if (!ndb || !ndb->open || !job_id || !db_build_job_find(ndb, job_id, &job))
        return ZCL_ERR(-1, "build job not found");
    if (strcmp(job.state, "CANCELLED") == 0)
        return ZCL_OK;
    if (strcmp(job.state, "ACCEPTED") == 0 || strcmp(job.state, "CACHE_HIT") == 0)
        return ZCL_ERR(-1, "completed build job cannot be cancelled");
    struct db_build_action actions[BUILD_FABRIC_ACTION_LIMIT];
    int count = db_build_job_actions(ndb, job_id, actions,
                                     BUILD_FABRIC_ACTION_LIMIT);
    if (!node_db_begin(ndb))
        return ZCL_ERR(-1, "cannot begin cancellation transaction");
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        if (!bf_terminal(actions[i].state)) {
            (void)snprintf(actions[i].state, sizeof(actions[i].state),
                           "CANCELLED");
            (void)snprintf(actions[i].outcome, sizeof(actions[i].outcome),
                           "CANCELLED");
            actions[i].updated_at = now;
            ok = db_build_action_save(ndb, &actions[i]);
        }
    }
    (void)snprintf(job.state, sizeof(job.state), "CANCELLED");
    (void)snprintf(job.outcome, sizeof(job.outcome), "CANCELLED");
    job.cancel_requested = 1;
    job.updated_at = now;
    ok = ok && db_build_job_save(ndb, &job) && node_db_commit(ndb);
    if (!ok) {
        if (!node_db_rollback(ndb))
            LOG_ERROR("build_fabric", "cancel and rollback both failed");
        return ZCL_ERR(-1, "cannot cancel build job atomically");
    }
    return ZCL_OK;
}

struct zcl_result build_fabric_worker_approve(
    struct node_db *ndb, const struct db_build_worker *worker, int64_t now)
{
    if (!ndb || !ndb->open || !worker)
        return ZCL_ERR(-1, "worker approval requires an open db and worker");
    struct db_build_worker next = *worker;
    next.approved = 1;
    next.revoked = 0;
    if (next.approved_at == 0) next.approved_at = now;
    if (!db_build_worker_save(ndb, &next))
        return ZCL_ERR(-1, "worker approval could not be persisted");
    return ZCL_OK;
}

struct zcl_result build_fabric_worker_revoke(
    struct node_db *ndb, const char *worker_id, int64_t now)
{
    (void)now;
    struct db_build_worker worker;
    if (!ndb || !ndb->open || !worker_id ||
        !db_build_worker_find(ndb, worker_id, &worker))
        return ZCL_ERR(-1, "build worker not found");
    if (worker.revoked)
        return ZCL_OK;
    worker.revoked = 1;
    if (!db_build_worker_save(ndb, &worker))
        return ZCL_ERR(-1, "worker revocation could not be persisted");
    return ZCL_OK;
}

struct zcl_result build_fabric_receipt_accept(
    struct node_db *ndb, const struct db_build_receipt *receipt, int64_t now)
{
    if (!ndb || !ndb->open || !receipt)
        return ZCL_ERR(-1, "receipt acceptance requires an open db and receipt");
    struct db_build_worker worker;
    if (!db_build_worker_find(ndb, receipt->worker_id, &worker) ||
        !worker.approved || worker.revoked ||
        (worker.expires_at != 0 && now >= worker.expires_at))
        return ZCL_ERR(-1, "receipt signer is unapproved, expired, or revoked");
    struct db_build_action action;
    if (!db_build_action_find(ndb, receipt->action_id, &action) ||
        strcmp(action.job_id, receipt->job_id) != 0 ||
        strcmp(receipt->action_sha3, action.action_id) != 0)
        return ZCL_ERR(-1, "receipt is not bound to the named action and job");
    if (strcmp(action.state, "RUNNING") != 0 &&
        strcmp(action.state, "VERIFYING") != 0 &&
        strcmp(action.state, "ACCEPTED") != 0)
        return ZCL_ERR(-1, "action state %s cannot accept a receipt",
                       action.state);
    char expected_id[65];
    if (!build_fabric_receipt_id(receipt, expected_id).ok ||
        strcmp(expected_id, receipt->receipt_id) != 0)
        return ZCL_ERR(-1, "receipt id does not match its canonical preimage");
    uint8_t id[32], sig[64], pubkey[32];
    if (!zcl_hex_decode_lower(receipt->receipt_id, id, sizeof(id)) ||
        !zcl_hex_decode_lower(receipt->signature, sig, sizeof(sig)) ||
        !zcl_hex_decode_lower(worker.signer_pubkey, pubkey, sizeof(pubkey)) ||
        !ed25519_verify(sig, id, sizeof(id), pubkey))
        return ZCL_ERR(-1, "receipt Ed25519 signature is invalid");
    (void)snprintf(action.state, sizeof(action.state), "ACCEPTED");
    (void)snprintf(action.outcome, sizeof(action.outcome), "ACCEPTED");
    (void)snprintf(action.output_root_sha3, sizeof(action.output_root_sha3),
                   "%s", receipt->output_sha3);
    (void)snprintf(action.worker_id, sizeof(action.worker_id), "%s",
                   receipt->worker_id);
    action.updated_at = now;
    if (!node_db_begin(ndb))
        return ZCL_ERR(-1, "cannot begin receipt acceptance transaction");
    bool ok = db_build_receipt_save(ndb, receipt) &&
              db_build_action_save(ndb, &action);
    struct db_build_action actions[BUILD_FABRIC_ACTION_LIMIT];
    int count = ok ? db_build_job_actions(ndb, receipt->job_id, actions,
                                           BUILD_FABRIC_ACTION_LIMIT) : 0;
    bool all_accepted = count > 0;
    for (int i = 0; i < count; i++)
        if (strcmp(actions[i].state, "ACCEPTED") != 0 &&
            strcmp(actions[i].state, "CACHE_HIT") != 0)
            all_accepted = false;
    if (ok && all_accepted) {
        struct db_build_job job;
        ok = db_build_job_find(ndb, receipt->job_id, &job);
        if (ok) {
            (void)snprintf(job.state, sizeof(job.state), "ACCEPTED");
            (void)snprintf(job.outcome, sizeof(job.outcome), "ACCEPTED");
            job.updated_at = now;
            ok = db_build_job_save(ndb, &job);
        }
    }
    ok = ok && node_db_commit(ndb);
    if (!ok) {
        if (!node_db_rollback(ndb))
            LOG_ERROR("build_fabric", "receipt accept and rollback both failed");
        return ZCL_ERR(-1, "verified receipt could not be accepted atomically");
    }
    return ZCL_OK;
}
