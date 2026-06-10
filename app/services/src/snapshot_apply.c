/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * snapshot_apply.c — promote staging + activate tip.
 *
 * After verify passes, this file promotes the staged UTXOs into the
 * active utxos table inside one transaction and records the
 * snapshot anchor / chainwork / MMB metadata, then routes the tip
 * commit through chain_evidence_controller (or csr fallback) so
 * block_map, active_chain, coins_tip, and pindex_best_header all
 * move together.
 *
 * Snapshot activation owns the durable promotion boundary. */

#include "net/snapshot_sync_contract.h"
#include "services/chain_restore_executor.h"
#include "services/chain_restore_repair.h"
#include "services/chain_state_service.h"
#include "services/chain_tip.h"
#include "services/chain_evidence_authority_service.h"
#include "models/database.h"
#include "chain/chain.h"
#include "chain/pow.h"
#include "coins/utxo_commitment.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "core/uint256.h"
#include "event/event.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "validation/main_state.h"
#include "validation/main_constants.h"
#include "validation/sync_evidence_policy.h"
#include "validation/contextual_check_tx.h"
#include "storage/utxo_projection.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/block_header_emit.h"

#include "snapshot_sync_internal.h"

#include <string.h>
#include <stdio.h>

/* ── Cold-start event-log seed ───────────────────────────── */

/* Seed the authoritative log/projection engine from the verified
 * snapshot so a node that fast-synced on a cold datadir restores its
 * tip + UTXO set purely from the log on the next boot (no legacy
 * coins.db / node.db read). Best-effort and additive during migration:
 * the legacy node.db `utxos` promote above still carries the set, so a
 * failure here is logged but never aborts the snapshot activation.
 *
 *   1. Bulk-seed the UTXO projection from the verified staging table.
 *   2. Emit ONE anchor EV_BLOCK_HEADER so block_index_projection records
 *      the anchor as a HAVE_DATA / VALID_SCRIPTS tip for the boot fold.
 *   3. Durably stamp the tip_finalize cursor + finalize-log row at the
 *      anchor height so boot_rebuild_from_log seeds the tip from it. */
static void snapsync_seed_projection_boot(struct node_db *ndb,
                                          const struct snapshot_sync_service *svc)
{
    /* (1) UTXO projection seed from the verified staging table (node.db).
     * NULL global projection (unit tests / not-yet-wired) → skip. */
    utxo_projection_t *proj = utxo_projection_get_global();
    if (proj) {
        int64_t seeded = utxo_projection_seed_from_snapshot(proj, ndb->db);
        if (seeded < 0)
            LOG_WARN("snapshot_sync",
                     "event-log seed: utxo projection seed failed "
                     "(h=%d) — legacy node.db utxos still authoritative",
                     svc->offered_height);
        /* Mirror the seed into the atomic coins set (progress.kv) so the
         * coins_kv read path matches the snapshot-seeded projection.
         * docs/work/tip-durability-collapse.md. */
        (void)coins_kv_boot_rebuild_if_needed(progress_store_db(), proj);
    }

    /* (2) One anchor EV_BLOCK_HEADER. We carry only the anchor hash +
     * height (the snapshot offer has no full block header), exactly like
     * chain_restore_create_anchor's metadata-only anchor. nStatus marks
     * it HAVE_DATA + VALID_SCRIPTS so the boot fold treats it as a
     * finalize-eligible tip. */
    struct block_index anchor;
    block_index_init(&anchor);
    struct uint256 anchor_hash;
    memcpy(anchor_hash.data, svc->offered_block_hash, 32);
    anchor.phashBlock = &anchor_hash;
    anchor.nHeight    = svc->offered_height;
    anchor.nStatus    = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
    block_index_emit_header_event(&anchor, "snapshot", NULL, NULL);

    /* (3) Durable tip cursor + finalize-log row at the anchor height. */
    if (!tip_finalize_stage_seed_anchor(svc->offered_height,
                                        svc->offered_block_hash))
        LOG_WARN("snapshot_sync",
                 "event-log seed: tip cursor anchor seed skipped/failed "
                 "(h=%d)", svc->offered_height);
}

/* ── Stage promotion ─────────────────────────────────────── */

struct zcl_result snapsync_stage_promote_active_internal(struct node_db *ndb,
                                            const struct snapshot_sync_service *svc,
                                            const uint8_t local_root[32],
                                            uint64_t local_count,
                                            const struct chain_evidence_record *verified)
{
    bool has_mmb = false;

    if (!ndb || !svc || !local_root)
        return ZCL_ERR(-1, "activate: null args ndb=%p svc=%p root=%p",
                       (void*)ndb, (void*)svc, (const void*)local_root);
    if (!snapsync_set_staging_phase_internal(ndb, SNAPSYNC_PHASE_ATOMIC_ACTIVATE).ok)
        return ZCL_ERR(-2, "activate: failed to set staging phase");
    if (!node_db_exec(ndb, "DELETE FROM utxos"))
        return ZCL_ERR(-3, "activate: failed to clear active utxos");
    if (!node_db_exec(ndb,
            "INSERT OR REPLACE INTO utxos"
            "(txid,vout,value,script,script_type,address_hash,height,is_coinbase)"
            " SELECT txid,vout,value,script,script_type,address_hash,height,is_coinbase"
            " FROM " SNAPSYNC_STAGING_TABLE))
        return ZCL_ERR(-4, "activate: failed to promote staged utxos");
    if (node_db_utxo_count(ndb) != (int64_t)local_count)
        return ZCL_ERR(-5, "activate: active/staged UTXO count mismatch");
    /* Cold-start event-log seed (additive): land the verified
     * snapshot into the authoritative log/projection engine + tip cursor
     * BEFORE the staging table is discarded below. Best-effort. */
    snapsync_seed_projection_boot(ndb, svc);
    /* Record the verified snapshot anchor as pending recovery metadata.
     * The publishable coins_best_block cursor is written only by
     * chain_state_repository during evidenced tip activation. */
    if (!node_db_state_set(ndb, "snapshot_pending_coins_best_block",
                           svc->offered_block_hash, 32) ||
        !node_db_state_set_int(ndb, "snapshot_pending_coins_best_height",
                               svc->offered_height))
        return ZCL_ERR(-6, "activate: failed to set pending coins anchor");

    for (int i = 0; i < 32; i++) {
        if (svc->offered_mmb_root[i]) {
            has_mmb = true;
            break;
        }
    }
    if (has_mmb) {
        if (!node_db_state_set(ndb, "snapshot_mmb_root",
                               svc->offered_mmb_root, 32) ||
            !node_db_state_set(ndb, "snapshot_mmr_height",
                               &svc->offered_height, 4))
            return ZCL_ERR(-7, "activate: failed to set snapshot MMB metadata");
    }
    if (!utxo_commitment_sha3_save(ndb->db, local_root,
                                   svc->offered_height, local_count))
        return ZCL_ERR(-8, "activate: failed to save utxo_sha3");
    if (!zcl_chainwork_is_zero(svc->offered_chain_work) &&
        !zcl_chainwork_is_zero(svc->offered_mmb_root) &&
        verified && chain_evidence_record_has_snapshot_required(verified)) {
        struct chain_evidence_controller authority;
        struct chain_evidence_controller_snapshot_meta meta;
        memset(&meta, 0, sizeof(meta));
        meta.anchor_height = svc->offered_height;
        memcpy(meta.anchor_hash.data, svc->offered_block_hash, 32);
        memcpy(meta.utxo_sha3.data, local_root, 32);
        memcpy(meta.chainwork, svc->offered_chain_work, 32);
        memcpy(meta.mmb_root, svc->offered_mmb_root, 32);
        meta.utxo_count = local_count;
        meta.finality_depth = (uint32_t)zcl_finality_depth();
        meta.schema_version = svc->offered_schema_version;
        meta.producer = "p2p_snapshot";
        meta.verified = *verified;
        chain_evidence_controller_init(&authority, ndb, csr_instance());
        if (chain_evidence_controller_import_snapshot_evidence(&authority, &meta)
            != CEC_OK)
            return ZCL_ERR(-9,
                     "activate: failed to persist snapshot evidence metadata");
    } else if (!zcl_chainwork_is_zero(svc->offered_chain_work) &&
               !zcl_chainwork_is_zero(svc->offered_mmb_root)) {
        event_emitf(EV_SNAPSYNC_VERIFIED, svc->serving_peer_id,
                    "snapshot_evidence=SKIPPED reason=incomplete_verified_inputs");
    }
    if (!snapsync_discard_staging_internal(ndb, "activated").ok)
        return ZCL_ERR(-10, "activate: failed to clear staging after promotion");
    return ZCL_OK;
}

/* ── Tip activation ──────────────────────────────────────── */

static bool snapsync_header_ancestry_linked(const struct block_index *tip)
{
    if (!tip || !tip->phashBlock)
        return false;
    if (tip->nHeight == 0)
        return true;
    return tip->pprev && tip->pprev->phashBlock &&
           tip->pprev->nHeight == tip->nHeight - 1;
}

static bool snapsync_chainwork_recomputed(const struct block_index *tip)
{
    struct arith_uint256 expected;
    struct arith_uint256 proof;

    if (!tip)
        return false;
    proof = GetBlockProof(tip);
    expected = proof;
    if (tip->pprev)
        arith_uint256_add(&expected, &tip->pprev->nChainWork, &proof);
    return arith_uint256_compare(&expected, &tip->nChainWork) == 0 &&
           !zcl_chainwork_is_zero((const uint8_t *)tip->nChainWork.pn);
}

static bool snapsync_tip_is_best_work(const struct main_state *ms,
                                      const struct block_index *tip)
{
    size_t iter = 0;
    struct block_index *candidate = NULL;

    if (!ms || !tip)
        return false;
    if (zcl_chainwork_is_zero((const uint8_t *)tip->nChainWork.pn))
        return false;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &candidate)) {
        if (!candidate || (candidate->nStatus & BLOCK_FAILED_MASK))
            continue;
        if (arith_uint256_compare(&candidate->nChainWork,
                                  &tip->nChainWork) <= 0)
            continue;
        if (block_index_get_ancestor(candidate, tip->nHeight) != tip)
            return false;
    }
    return true;
}

/* Single-writer tip commit for snapshot activation. Routes through
 * the chain_state_repository so block_map, active_chain, coins_tip,
 * and pindex_best_header move together — the exact failure mode that
 * caused the 2026-04-10 UTXO wipe was a snapshot path updating these
 * out of order. Falls back to raw setters when the csr singleton was
 * never wired (unit tests that call this function without boot). */
static bool snapsync_commit_tip(struct main_state *ms,
                                 struct block_index *new_tip,
                                 const char *reason)
{
    if (!new_tip || !new_tip->phashBlock) LOG_FAIL("snapshot_sync", "commit_tip: new_tip=%p phashBlock=%p", (void*)new_tip, new_tip ? (void*)new_tip->phashBlock : NULL);

    {
        struct chain_evidence_controller authority;
        struct chain_evidence_controller_tip_request req = {
            .new_tip = new_tip,
            .utxo_max_height = new_tip->nHeight,
            .update_header_tip = true,
            .reason = reason ? reason : "snapshot.apply_anchor",
        };
        req.verified.header_ancestry_linked =
            snapsync_header_ancestry_linked(new_tip);
        req.verified.chainwork_recomputed =
            snapsync_chainwork_recomputed(new_tip);
        req.verified.nakamoto_selected_best_work =
            snapsync_tip_is_best_work(ms, new_tip);
        req.verified.block_bytes_hash_checked =
            chain_restore_block_is_consensus_backed(new_tip);
        chain_evidence_controller_init(&authority, app_runtime_node_db(),
                            csr_instance());
        if (authority.state == CEC_SNAPSHOT_UTXO_HASH_VERIFIED ||
            authority.state == CEC_TIP_FOLLOWING ||
            authority.state == CEC_BACKGROUND_VALIDATING ||
            authority.state == CEC_FULLY_VALIDATED) {
            enum chain_evidence_controller_result ar =
                chain_evidence_controller_promote_tip(&authority, &req);
            if (ar == CEC_OK)
                return true;
            event_emitf(EV_CHAIN_TIP_REJECTED, 0,
                        "source=snapshot authority=%s reason=%s h=%d",
                        chain_evidence_controller_result_name(ar),
                        reason ? reason : "", new_tip->nHeight);
            return false;
        }
    }

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_SNAPSHOT,
        .decision = POLICY_ALLOW,
        .from_height = ms ? active_chain_height(&ms->chain_active) : -1,
        .to_height = new_tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "snapshot_utxo_sha3_verified",
        .reason = reason ? reason : "snapshot.apply_anchor",
    };
    struct chain_state_commit commit = {
        .new_tip             = new_tip,
        .new_coins_best      = *new_tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip   = true,
        .rollback_auth       = &rollback_auth,
        .wallet_scan_height  = -1,
        .reason              = reason,
    };

    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
    if (rc == CSR_OK) return true;

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        /* Test harness path: singleton was never wired. Fall back to
         * the canonical chain_set_active_tip so existing unit tests
         * still exercise the snapshot activation logic end-to-end. */
        (void)chain_set_active_tip(ms, new_tip, TIP_FROM_SNAPSHOT,
                             reason ? reason : "csr_uninit_fallback");
        ms->pindex_best_header = new_tip;
        return true;
    }
#endif

    event_emitf(EV_CHAIN_TIP_REJECTED, 0,
                "source=snapshot csr=%s reason=%s h=%d",
                csr_result_name(rc), reason, new_tip->nHeight);
    return false;
}

int snapsync_activate_verified_tip(const struct snapshot_sync_service *svc,
                                   struct main_state *ms)
{
    struct uint256 snap_hash;
    struct block_index *snap_bi;

    if (!svc || !ms)
        LOG_ERR("snapshot_sync", "activate_verified_tip: svc=%p ms=%p", (void*)svc, (void*)ms);

    if (!zcl_is_snapshot_anchor_acceptable(svc->offered_height,
                                           svc->offered_peer_tip_height)) {
        LOG_ERR("snapshot_sync",
                "activate_verified_tip: refusing unacceptable snapshot anchor h=%d peer_tip=%d finality=%d",
                svc->offered_height, svc->offered_peer_tip_height,
                zcl_finality_depth());
    }

    memcpy(snap_hash.data, svc->offered_block_hash, 32);
    snap_bi = block_map_find(&ms->map_block_index, &snap_hash);
    if (!snap_bi) {
        /* Snapshot block hash not in local block index — expected for fresh
         * nodes that received a UTXO snapshot via fast sync. FlyClient has
         * verified the chain of work and SHA3 has verified the UTXO set
         * integrity, but this still is not local immutable block storage.
         *
         * Insert metadata only for locator/recovery. It must not have
         * BLOCK_HAVE_DATA, synthetic tx counts, synthetic chainwork, or
         * active-chain status until real block bytes arrive. */
        snap_bi = chain_restore_create_anchor(ms, &snap_hash,
                                              svc->offered_height);
        if (!snap_bi)
            LOG_ERR("snapshot_sync", "activate_verified_tip: metadata anchor failed");
        *snapsync_anchor_slot_internal() = snap_bi;

        /* Set deferred proof validation to snapshot height — all blocks at or below
         * this height skip expensive script/proof verification since the
         * UTXO set at this point is cryptographically verified. */
        g_deferred_proof_validation_below_height = svc->offered_height;

        printf("[snapshot] Metadata anchor at height %d recorded "
               "(FlyClient+SHA3 verified; awaiting block data)\n",
               svc->offered_height);
        return svc->offered_height;
    }

    /* g_snapshot_anchor is intentionally NOT set here: when snap_bi
     * comes from block_map_find, ownership stays with the block map.
     * The "not in map" path above already allocated and registered a
     * heap-owned anchor — that is the only object reset/free is allowed
     * to drop. */
    g_deferred_proof_validation_below_height = snap_bi->nHeight;

    if (!chain_restore_block_is_consensus_backed(snap_bi)) {
        /* Block index entry exists but on-disk bytes are not present.
         * After FlyClient PoW-sampling + SHA3 UTXO-commitment verification
         * the snapshot tip is consensus-immutable at this height even
         * without local block bytes; activating the tip lets headers and
         * subsequent blocks build on it. The block-fetch path will
         * back-fill the bytes later. */
        printf("[snapshot] Activating verified tip at h=%d "
               "(FlyClient+SHA3 verified; awaiting block bytes)\n",
               snap_bi->nHeight);
    }

    if (!snapsync_commit_tip(ms, snap_bi, "snapshot.apply_anchor")) {
        LOG_ERR("snapshot_sync",
                "activate_verified_tip: commit_tip failed h=%d",
                snap_bi->nHeight);
    }
    return snap_bi->nHeight;
}
