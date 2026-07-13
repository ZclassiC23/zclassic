/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * PURPOSE: Name and deduplicate proof-stage waits on a stale script receipt. */

#include "proof_validate_stage_internal.h"

#include "core/uint256.h"
#include "jobs/proof_validate_stage.h"
#include "util/blocker.h"

#include <stdio.h>

bool proof_validate_upstream_hash_ready(
    int height, const struct uint256 *selected_hash, bool receipt_has_hash,
    const struct uint256 *receipt_hash)
{
    if (selected_hash && receipt_has_hash && receipt_hash &&
        uint256_eq(receipt_hash, selected_hash)) {
        proof_validate_upstream_hash_clear();
        return true;
    }

    char selected_hex[65] = {0};
    char receipt_hex[65] = "MISSING";
    if (selected_hash)
        uint256_get_hex(selected_hash, selected_hex);
    if (receipt_has_hash && receipt_hash)
        uint256_get_hex(receipt_hash, receipt_hex);
    struct blocker_record rec;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "height=%d script_validate_log block_hash %.16s != selected "
             "block %.16s..; proof validation holds until script validation "
             "publishes a receipt for the selected branch",
             height, receipt_hex, selected_hex);
    if (blocker_init(&rec, PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID,
                     "proof_validate", BLOCKER_DEPENDENCY, reason)) {
        snprintf(rec.escape_action, sizeof(rec.escape_action),
                 "re-run script_validate for selected block hash");
        (void)blocker_set(&rec);
    }
    return false;
}

void proof_validate_upstream_hash_clear(void)
{
    blocker_clear(PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID);
}

job_result_t proof_validate_upstream_verdict_refuse(
    struct stage_step_ctx *ctx, int height, int verdict)
{
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "height=%d script_validate_log ok=%d is outside canonical "
             "{0,1}; proof validation holds until script validation "
             "re-publishes an exact typed verdict",
             height, verdict);
    if (!ctx)
        return JOB_FATAL;
    if (!blocker_init(&ctx->blocker,
            PROOF_VALIDATE_INVALID_UPSTREAM_BLOCKER_ID, "proof_validate",
            BLOCKER_DEPENDENCY, reason))
        return JOB_FATAL;
    snprintf(ctx->blocker.escape_action, sizeof(ctx->blocker.escape_action),
             "re-run script_validate for selected block hash");
    ctx->blocker.retry_budget = -1;
    return JOB_BLOCKED;
}

void proof_validate_upstream_verdict_clear(void)
{
    blocker_clear(PROOF_VALIDATE_INVALID_UPSTREAM_BLOCKER_ID);
}
