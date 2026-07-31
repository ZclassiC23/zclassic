/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Typed workflows for the durable ZBuild coordinator ledger. */

#ifndef ZCL_SERVICES_BUILD_FABRIC_SERVICE_H
#define ZCL_SERVICES_BUILD_FABRIC_SERVICE_H

#include "base/result.h"
#include "models/build_fabric.h"

#include <stdint.h>

/* Persist one immutable job/action plan atomically. Repeating the exact plan
 * is idempotent; an id collision with different immutable inputs refuses. */
struct zcl_result build_fabric_plan(struct node_db *ndb,
                                    const struct db_build_job *job,
                                    const struct db_build_action *action);

/* Advance PLANNED/SNAPSHOTTED work to QUEUED. */
struct zcl_result build_fabric_submit(struct node_db *ndb,
                                      const char *job_id, int64_t now);

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
 * Ed25519 signature over receipt_id before accepting the output receipt. */
struct zcl_result build_fabric_receipt_accept(
    struct node_db *ndb, const struct db_build_receipt *receipt, int64_t now);

/* Canonical build_receipt.v1 id (signature excluded). */
struct zcl_result build_fabric_receipt_id(
    const struct db_build_receipt *receipt,
    char out_hex[BUILD_FABRIC_ID_HEX + 1]);

#endif /* ZCL_SERVICES_BUILD_FABRIC_SERVICE_H */
