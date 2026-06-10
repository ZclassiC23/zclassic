/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot tip-publication hooks — the process_block -> chain-state seam.
 *
 * When the validation engine publishes a new active tip (or clears it on a
 * disconnect-past-genesis), it calls these hooks, which route through the
 * chain_evidence_controller when evidence is present, else the chain_state
 * repository (csr) single-writer. boot_register_process_block_hooks() wires
 * them — plus the gap-fill kick — into process_block during app_init_services.
 *
 * These are pure adapters: every input arrives by parameter, so the file owns
 * no shared boot state. boot_internal.h supplies boot_svc_ctx + the main_state
 * and coins types; the rest come from the service/validation headers below.
 */

#include "config/boot_internal.h"
#include "validation/process_block.h"
#include "services/chain_state_service.h"
#include "services/chain_evidence_authority_service.h"
#include "services/chain_tip.h"          /* chain_set_active_tip, TIP_FROM_* (ZCL_TESTING paths) */
#include "services/gap_fill_service.h"
#include "validation/mirror_consensus.h"
#include "util/log_macros.h"

static void boot_gap_fill_kick(void *ctx)
{
    (void)ctx;
    gap_fill_kick();
}

static enum process_block_tip_publish_result
boot_process_block_result_from_csr(enum csr_result rc)
{
    switch (rc) {
    case CSR_OK:
        return PROCESS_BLOCK_TIP_PUBLISH_OK;
    case CSR_REJECTED_NOT_INITIALIZED:
        return PROCESS_BLOCK_TIP_PUBLISH_REJECTED_NOT_INITIALIZED;
    case CSR_REJECTED_DB_BUSY:
        return PROCESS_BLOCK_TIP_PUBLISH_REJECTED_DB_BUSY;
    case CSR_REJECTED_PERSIST:
        return PROCESS_BLOCK_TIP_PUBLISH_REJECTED_PERSIST;
    default:
        return PROCESS_BLOCK_TIP_PUBLISH_REJECTED;
    }
}

static enum process_block_tip_publish_result
boot_process_block_result_from_cec(enum chain_evidence_controller_result rc)
{
    switch (rc) {
    case CEC_OK:
        return PROCESS_BLOCK_TIP_PUBLISH_OK;
    case CEC_REJECTED_PERSIST:
        return PROCESS_BLOCK_TIP_PUBLISH_REJECTED_PERSIST;
    default:
        return PROCESS_BLOCK_TIP_PUBLISH_REJECTED;
    }
}

static struct chain_evidence_record boot_process_block_evidence(
    const struct process_block_tip_evidence *src)
{
    struct chain_evidence_record out = {0};

    if (!src)
        return out;
    out.header_ancestry_linked = src->header_ancestry_linked;
    out.chainwork_recomputed = src->chainwork_recomputed;
    out.nakamoto_selected_best_work = src->nakamoto_selected_best_work;
    out.block_bytes_hash_checked = src->block_bytes_hash_checked;
    out.utxo_sha3_verified = src->utxo_sha3_verified;
    out.mmb_flyclient_proof_verified = src->mmb_flyclient_proof_verified;
    out.chunk_hash_coverage_verified = src->chunk_hash_coverage_verified;
    out.full_validation_complete = src->full_validation_complete;
    return out;
}

static enum process_block_tip_publish_result boot_process_block_commit_tip(
    void *ctx,
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    struct block_index *new_tip,
    const char *reason,
    bool update_header_tip,
    bool persist_coins_best,
    const struct process_block_tip_evidence *verified)
{
    struct boot_svc_ctx *svc = ctx;

#ifndef ZCL_TESTING
    (void)coins_tip;
#endif
    if (!new_tip || !new_tip->phashBlock)
        return PROCESS_BLOCK_TIP_PUBLISH_REJECTED;

    if (verified && svc && svc->node_db && csr_instance()->initialized) {
        struct chain_evidence_controller authority;
        struct chain_evidence_controller_tip_request req = {
            .new_tip = new_tip,
            .utxo_max_height = new_tip->nHeight,
            .update_header_tip = update_header_tip,
            .reason = reason ? reason : "process_block.commit_tip",
            .verified = boot_process_block_evidence(verified),
        };
        chain_evidence_controller_init(&authority, svc->node_db,
                                       csr_instance());
        enum chain_evidence_controller_result er =
            chain_evidence_controller_promote_tip(&authority, &req);
        if (er == CEC_OK)
            return PROCESS_BLOCK_TIP_PUBLISH_OK;
        LOG_WARN("validation",
                 "evidence controller rejected process-block tip (%s) h=%d reason=%s",
                 chain_evidence_controller_result_name(er),
                 new_tip->nHeight, reason ? reason : "");
        return boot_process_block_result_from_cec(er);
    }

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_VALIDATION,
        .decision = POLICY_ALLOW,
        .from_height = ms ? active_chain_height(&ms->chain_active) : -1,
        .to_height = new_tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "validation_path_vetted",
        .reason = reason ? reason : "process_block.commit_tip",
    };
    struct chain_state_commit commit = {
        .new_tip = new_tip,
        .new_coins_best = *new_tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip = update_header_tip,
        .persist_coins_best = persist_coins_best,
        .rollback_auth = &rollback_auth,
        .wallet_scan_height = -1,
        .reason = reason,
    };
    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED && ms) {
        (void)chain_set_active_tip(ms, new_tip, TIP_FROM_CONNECT,
                                   reason ? reason : "csr_uninit_fallback");
        if (update_header_tip)
            ms->pindex_best_header = new_tip;
        if (coins_tip)
            coins_view_cache_set_best_block(coins_tip, new_tip->phashBlock);
        return PROCESS_BLOCK_TIP_PUBLISH_OK;
    }
#endif
    if (rc != CSR_OK) {
        LOG_WARN("validation", "csr rejected process-block tip (%s) h=%d reason=%s",
                 csr_result_name(rc), new_tip->nHeight, reason ? reason : "");
        if (rc == CSR_REJECTED_DB_BUSY)
            mirror_consensus_record_blocker("db-writer-busy");
        else if (rc == CSR_REJECTED_PERSIST)
            mirror_consensus_record_blocker("csr-persist-failed");
    }
    return boot_process_block_result_from_csr(rc);
}

static enum process_block_tip_publish_result boot_process_block_clear_tip(
    void *ctx,
    struct main_state *ms,
    const char *reason)
{
    (void)ctx;
    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_VALIDATION,
        .decision = POLICY_ALLOW,
        .from_height = ms ? active_chain_height(&ms->chain_active) : -1,
        .to_height = -1,
        .max_depth = INT64_MAX,
        .evidence_class = "validation_disconnect_complete",
        .reason = reason ? reason : "disconnect_past_genesis",
    };
    struct chain_state_clear_commit clear = {
        .rollback_auth = &rollback_auth,
        .reason = reason ? reason : "disconnect_past_genesis",
    };
    enum csr_result rc = csr_clear_active_tip(csr_instance(), &clear);

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED && ms) {
        (void)chain_set_active_tip(ms, NULL, TIP_FROM_DISCONNECT,
                                   reason ? reason : "disconnect_past_genesis");
        return PROCESS_BLOCK_TIP_PUBLISH_OK;
    }
#endif
    if (rc != CSR_OK)
        LOG_WARN("validation", "csr rejected process-block tip clear (%s)",
                 csr_result_name(rc));
    return boot_process_block_result_from_csr(rc);
}

/* Wire the tip-publication hooks + the gap-fill kick into the validation
 * engine. Called once from app_init_services; the teardown counterpart
 * (process_block_set_tip_publication_hooks(NULL, NULL, NULL)) stays inline in
 * app_shutdown_svc since it references no moved symbol. */
void boot_register_process_block_hooks(struct boot_svc_ctx *svc)
{
    process_block_set_gap_fill_kick(boot_gap_fill_kick, svc);
    process_block_set_tip_publication_hooks(boot_process_block_commit_tip,
                                            boot_process_block_clear_tip,
                                            svc);
}
