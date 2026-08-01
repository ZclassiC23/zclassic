/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Typed workflows for the durable ZBuild coordinator ledger. */

#ifndef ZCL_SERVICES_BUILD_FABRIC_SERVICE_H
#define ZCL_SERVICES_BUILD_FABRIC_SERVICE_H

#include "base/result.h"
#include "models/build_fabric.h"

#include <stdint.h>

struct vcs_zcode_work_request_v1;
struct vcs_zcode_work_result_v1;

enum {
    BUILD_FABRIC_LEASE_SECONDS_MIN = 5,
    BUILD_FABRIC_LEASE_SECONDS_MAX = 600,
    BUILD_FABRIC_LEASE_SECONDS_DEFAULT = 120,
};

/* Domain-separated immutable identities. The action identity binds every
 * V1 execution input; the job identity additionally binds the source oracle
 * and the ordered action. Neither lifecycle state nor wall time participates. */
struct zcl_result build_fabric_action_id(
    const struct db_build_job *job, const struct db_build_action *action,
    char out_hex[BUILD_FABRIC_ID_HEX + 1]);
struct zcl_result build_fabric_job_id(
    const struct db_build_job *job, const char *action_id,
    char out_hex[BUILD_FABRIC_ID_HEX + 1]);

/* Persist one immutable job/action plan atomically. Repeating the exact plan
 * is idempotent; an id collision with different immutable inputs refuses. */
struct zcl_result build_fabric_plan(struct node_db *ndb,
                                    const struct db_build_job *job,
                                    const struct db_build_action *action);

/* Advance PLANNED/SNAPSHOTTED work to QUEUED. */
struct zcl_result build_fabric_submit(struct node_db *ndb,
                                      const char *job_id, int64_t now);

/* A worker lease is an atomic, expiring ownership token. Every transition
 * after claim compares the exact prior state and lease id; restart recovery
 * returns expired work to QUEUED without allowing the old owner to publish. */
struct zcl_result build_fabric_claim(
    struct node_db *ndb, const char *worker_id, const char *lease_id,
    int64_t now, int64_t lease_seconds, struct db_build_action *out,
    bool *claimed);
struct zcl_result build_fabric_start(
    struct node_db *ndb, const char *action_id, const char *lease_id,
    int64_t now);
struct zcl_result build_fabric_heartbeat(
    struct node_db *ndb, const char *action_id, const char *lease_id,
    int64_t now, int64_t lease_seconds);
struct zcl_result build_fabric_begin_verify(
    struct node_db *ndb, const char *action_id, const char *lease_id,
    int64_t now);
struct zcl_result build_fabric_recover_expired(
    struct node_db *ndb, int64_t now, size_t *requeued);
struct zcl_result build_fabric_finish_leased(
    struct node_db *ndb, const char *action_id, const char *lease_id,
    const char *outcome, const char *detail, int64_t now);

/* Idempotently cancel every nonterminal action and the owning job. */
struct zcl_result build_fabric_cancel(struct node_db *ndb,
                                      const char *job_id, int64_t now);

/* Operator trust transitions. Approve creates/updates a worker; revoke never
 * deletes its receipts and is idempotent. */
struct zcl_result build_fabric_worker_approve(
    struct node_db *ndb, const struct db_build_worker *worker, int64_t now);
struct zcl_result build_fabric_worker_revoke(
    struct node_db *ndb, const char *worker_id, int64_t now);

/* Verify action binding, canonical receipt id, signer approval/expiry and the
 * Ed25519 signature over receipt_id before accepting the evidence. A zero
 * exit advances the action; an authentic nonzero fixed-action result stores
 * the receipt and atomically finishes the action/job as FAILED. */
struct zcl_result build_fabric_receipt_accept(
    struct node_db *ndb, const struct db_build_receipt *receipt, int64_t now);

/* Persist a self-authenticating remote work_receipt.v1 as explicitly
 * untrusted evidence. It never advances the action lifecycle and never makes
 * the signer approved; local reproduction or policy quorum is a later step. */
struct zcl_result build_fabric_receipt_observe_remote(
    struct node_db *ndb, const char *workspace,
    const struct vcs_zcode_work_request_v1 *request,
    const struct vcs_zcode_work_result_v1 *result, int64_t now,
    char receipt_id[BUILD_FABRIC_ID_HEX + 1]);

struct build_fabric_proof_evaluation {
    size_t valid_receipts;
    size_t approved_distinct_signers;
    size_t matching_receipts;
    size_t compile_receipts;
    size_t test_receipts;
    size_t fuzz_receipts;
    size_t review_receipts;
    bool local_reproduced;
    bool quorum_satisfied;
    bool compile_satisfied;
    bool test_satisfied;
    bool fuzz_satisfied;
    bool review_satisfied;
    bool release_identity_satisfied;
    bool policy_satisfied;
    char output_root_sha3[BUILD_FABRIC_ID_HEX + 1];
    char proof_set_root_sha3[BUILD_FABRIC_ID_HEX + 1];
};

/* Re-verify canonical receipt bytes from CAS, apply the task's exact proof
 * policy, and promote observations only through local reproduction or a
 * distinct approved-signer quorum. */
struct zcl_result build_fabric_proof_evaluate(
    struct node_db *ndb, const char *workspace, const char *action_id,
    int64_t now, struct build_fabric_proof_evaluation *out);

/* Canonical build_receipt.v2 projection id (signature excluded). */
struct zcl_result build_fabric_receipt_id(
    const struct db_build_receipt *receipt,
    char out_hex[BUILD_FABRIC_ID_HEX + 1]);

#endif /* ZCL_SERVICES_BUILD_FABRIC_SERVICE_H */
