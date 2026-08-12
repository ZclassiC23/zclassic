/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Requester-local durable state for asynchronous peer proof. */

#ifndef ZCL_SERVICES_BUILD_FABRIC_ASYNC_H
#define ZCL_SERVICES_BUILD_FABRIC_ASYNC_H

#include "base/result.h"
#include "models/build_proof_event.h"

#include <stdbool.h>
#include <stdint.h>

uint64_t build_fabric_proof_request_id(const char *action_id);

/* Exact immutable actions deduplicate to one REQUESTED event. Requesting a
 * newer candidate for the same task appends SUPERSEDED to older active
 * candidates; their receipts remain in the existing receipt ledger. */
struct zcl_result build_fabric_proof_request(
    struct node_db *ndb, const char *action_id, const char *workspace,
    uint64_t peer_hint,
    int64_t now, struct db_build_proof_event *out, bool *created);

/* Append one closed transition. Roots not supplied inherit from the prior
 * event; the model recomputes and verifies the deterministic event root. */
struct zcl_result build_fabric_proof_transition(
    struct node_db *ndb, const char *action_id, const char *state,
    uint64_t peer_id, uint64_t request_id, const char *context_root,
    const char *receipt_root, int64_t deadline_at, int64_t elapsed_us,
    int64_t now,
    struct db_build_proof_event *out);

#endif /* ZCL_SERVICES_BUILD_FABRIC_ASYNC_H */
