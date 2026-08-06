/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: One confined, content-addressed ZBuild V1 worker dispatch. */

#ifndef ZCL_SERVICES_BUILD_FABRIC_WORKER_H
#define ZCL_SERVICES_BUILD_FABRIC_WORKER_H

#include "base/result.h"
#include "models/build_fabric.h"

#include <stdint.h>

/* Execute one already-claimed action. The caller owns lease acquisition;
 * this path rechecks it at start, verification, and signed publication. */
struct zcl_result build_fabric_worker_execute(
    struct node_db *ndb, const char *workspace_root, const char *datadir,
    const char *action_id,
    const char *lease_id, const uint8_t signer_secret[32],
    const uint8_t signer_pubkey[32], struct db_build_receipt *out_receipt);

/* Load or atomically create the operator-owned local worker key. The returned
 * row is suitable for explicit -buildworker self-approval. */
struct zcl_result build_fabric_worker_identity_load(
    const char *datadir, struct db_build_worker *worker,
    uint8_t signer_secret[32], uint8_t signer_pubkey[32]);

#endif /* ZCL_SERVICES_BUILD_FABRIC_WORKER_H */
