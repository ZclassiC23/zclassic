/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Reducer-ingest service — the synchronous block-intake path that drives the
 * staged Job pipeline as the authoritative chain-advance engine.
 *
 * This is the single responsibility split out of chain_activation_service.c
 * (which keeps the activation state machine). The reducer wrapper drives the
 * eight staged-Job step bodies and the stateless check_block gate, then reads
 * back the verdict from the freshly-written stage log rows. Public entry
 * points (reducer_is_authoritative / reducer_kick / reducer_ingest_block) are
 * declared in services/chain_activation_service.h and keep identical
 * names/signatures; the activation FSM shares only reducer_drain_to_convergence
 * via the private services/reducer_ingest_service.h seam. */

// one-result-type-ok:reducer-drive-counts
/* The reducer entry points return advance-counts / authority bools; a
 * failure surfaces via the stage FATAL latch + EV_OPERATOR_NEEDED, not a
 * return-value reason (same rationale as the parent chain_activation_service.c). */

#include "services/chain_activation_service.h"
#include "services/reducer_ingest_service.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "event/event.h"
#include "core/utiltime.h"
#include <stdio.h>
#include <string.h>

#include "util/log_macros.h"
#include "util/reducer_drive_guard.h"
#include "util/util.h"  /* GetDataDir */

/* ── Reducer-as-ingest includes ─────────────────────────────────────
 * The synchronous reducer wrapper drives the staged Job pipeline and the
 * stateless check_block gate, then reads back the verdict from the
 * freshly-written stage log rows. */
#include "consensus/validation.h"
#include "validation/check_block.h"
#include "primitives/block.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "storage/progress_store.h"
#include <sqlite3.h>
#include "storage/disk_block_io.h"
#include "services/header_admit_inbox.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/block_header_emit.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_repair.h"  /* header-solution repair-table backfill */

/* ── Reducer-as-ingest: synchronous wrapper driving the staged Job pipeline.
 * Drain the eight stage step bodies once, in pipeline order — the SAME
 * *_stage_drain functions the per-stage supervisor children tick
 * (staged_sync_supervisor.c). One pass; caller loops to convergence. */
static int reducer_drain_all_stages(int max_steps_per_stage)
{
    int advanced = 0;
    advanced += header_admit_stage_drain(max_steps_per_stage);
    advanced += validate_headers_stage_drain(max_steps_per_stage);
    advanced += body_fetch_stage_drain(max_steps_per_stage);
    advanced += body_persist_stage_drain(max_steps_per_stage);
    advanced += script_validate_stage_drain(max_steps_per_stage);
    advanced += proof_validate_stage_drain(max_steps_per_stage);
    advanced += utxo_apply_stage_drain(max_steps_per_stage);
    advanced += tip_finalize_stage_drain(max_steps_per_stage);
    return advanced;
}

/* Loop reducer_drain_all_stages to convergence within a bounded latency
 * budget. A no-advance pass is convergence UNLESS a stage went FATAL this
 * pass (fatal_generation moved) — a wedged stage masquerading as idle, so
 * we page EV_OPERATOR_NEEDED before breaking. */
int reducer_drain_to_convergence(void)
{
    const int64_t drain_budget_us = 2000 * 1000; /* 2s, same as legacy */
    const int     drain_hard_cap  = 4096;
    const int     per_stage_batch = 100;
    int64_t       start_us        = GetTimeMicros();
    uint64_t      fatal_gen0      = stage_fatal_generation();
    int           total           = 0;
    for (int round = 0; round < drain_hard_cap; round++) {
        int adv = reducer_drain_all_stages(per_stage_batch);
        total += adv;
        if (adv == 0) {
            char st[STAGE_NAME_MAX] = {0}, why[128] = {0};
            if (stage_fatal_generation() != fatal_gen0 &&
                stage_last_fatal(st, sizeof(st), why, sizeof(why)))
                event_emitf(EV_OPERATOR_NEEDED, 0,
                            "condition=reducer_stage_fatal stage=%s reason=%s",
                            st, why);
            break;
        }
        if (GetTimeMicros() - start_us > drain_budget_us)
            break;
    }
    return total;
}

bool reducer_is_authoritative(void)
{
    return true;
}

int reducer_kick(struct chain_activation_controller *ctl)
{
    if (!ctl)
        return 0;
    zcl_mutex_lock(&ctl->mutex);
    int advanced = reducer_drain_to_convergence();
    zcl_mutex_unlock(&ctl->mutex);
    return advanced;
}

/* Map freshly-written stage log rows for `height`/`hash` into `out`. */
static bool reducer_read_back_verdict(int height,
                                      const struct uint256 *hash,
                                      struct validation_state *out)
{
    sqlite3 *pdb = progress_store_db();

    struct validate_headers_window_report rep;
    if (validate_headers_stage_window_report(height, height, &rep) &&
        rep.failed_count > 0) {
        validation_state_dos(out, 100, false, REJECT_INVALID,
                             rep.first_fail_reason[0]
                                 ? rep.first_fail_reason
                                 : "header-validation-failed",
                             false, NULL);
        return false;
    }

    /* Convention-aware: "the hash of the block AT height" is witnessed by
     * the finalized row at height-1 OR an anchor row at height
     * (tip_finalize_stage.h). The raw finalized_tip_at(height) read only
     * matched anchor rows here (a finalized row at height carries the
     * LOOKAHEAD hash(height+1)), so a freshly-finalized block was invisible
     * to read-back until the trusted-tip anchor landed seconds later. */
    uint8_t finalized[32];
    if (pdb &&
        tip_finalize_stage_block_hash_at(pdb, height, finalized) &&
        memcmp(finalized, hash->data, 32) == 0) {
        return true; /* out left MODE_VALID by the caller's init */
    }

    validation_state_invalid(out, false, REJECT_INVALID,
                             "block-not-finalized-by-reducer", NULL);
    return false;
}

static bool reducer_header_rejected_at(int height, struct validation_state *out)
{
    struct validate_headers_window_report rep;
    if (!validate_headers_stage_window_report(height, height, &rep) ||
        rep.failed_count == 0)
        return false;

    validation_state_dos(out, 100, false, REJECT_INVALID,
                         rep.first_fail_reason[0]
                             ? rep.first_fail_reason
                             : "header-validation-failed",
                         false, NULL);
    return true;
}

static bool reducer_pending_body_is_accepted(
        const struct block_index *bi,
        struct validation_state *out)
{
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA) ||
        (bi->nStatus & BLOCK_FAILED_MASK))
        return false;

    if (reducer_header_rejected_at(bi->nHeight, out))
        return false;

    /* Consensus gate: the live tip is accepted ONLY if it cleared utxo_apply
     * (HAVE_DATA && !FAILED is no witness — stage fails record ok=0, never
     * BLOCK_FAILED_MASK). Caller already confirmed bi IS the active tip. */
    if (!utxo_apply_stage_succeeded_at(bi->nHeight))
        return false;

    validation_state_init(out);
    return true;
}

static bool reducer_persist_ingested_body_locked(
        struct chain_activation_controller *ctl,
        const struct uint256 *block_hash,
        struct block *pblock,
        struct validation_state *out)
{
    if (!ctl || !ctl->ms || !block_hash || !pblock) {
        LOG_WARN("reducer", "body persist invalid args ctl=%p ms=%p hash=%p block=%p",
                 (void *)ctl, ctl ? (void *)ctl->ms : NULL,
                 (const void *)block_hash, (void *)pblock);
        return validation_state_error(out, "reducer-body-null-arg");
    }

    struct block_index *bi = block_map_find(&ctl->ms->map_block_index,
                                            block_hash);
    if (!bi)
        return true;

    /* Stamp nTx from the body in hand (defect #10): without it the
     * emitted EV_BLOCK_HEADER carries n_tx=0, the projection persists
     * that, and the next boot's nChainTx propagation breaks right at
     * this block — the restart-loses-the-connected-extent class. Set
     * even on the HAVE_DATA early-return below so a later stage's emit
     * carries it. */
    if (bi->nTx == 0 && pblock->num_vtx > 0)
        bi->nTx = (unsigned int)pblock->num_vtx;

    if (bi->nStatus & BLOCK_HAVE_DATA)
        return true;

    if (reducer_header_rejected_at(bi->nHeight, out))
        return false;

    if (!ctl->datadir || !ctl->datadir[0] || !ctl->params) {
        LOG_WARN("reducer", "body persist missing runtime wiring h=%d datadir=%p params=%p",
                 bi->nHeight, (const void *)ctl->datadir,
                 (const void *)ctl->params);
        return validation_state_error(out, "reducer-body-runtime-unwired");
    }

    /* Write the body to the SAME directory the stage readers read from. Every
     * reader (body_persist/script_validate/proof_validate/utxo_apply) resolves
     * the block file under GetDataDir(true) — the NET-SPECIFIC datadir (e.g.
     * <base>/regtest). ctl->datadir is the BASE datadir (boot passes
     * ctx->datadir), so on a net with a subdir (regtest/testnet) writing to
     * ctl->datadir/blocks lands the body where the readers never look —
     * body_persist then fails read_failed and the whole drive cascades to
     * "block-not-finalized-by-reducer". On mainnet GetDataDir(true)==base, so
     * this is byte-identical there. This is exactly why mainnet sync works but
     * regtest `generate` mined nothing. */
    char persist_dir[2048];
    GetDataDir(true, persist_dir, sizeof(persist_dir));
    const char *bdir = persist_dir[0] ? persist_dir : ctl->datadir;

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    if (!write_block_to_disk(pblock, &pos, bdir,
                             ctl->params->pchMessageStart)) {
        LOG_WARN("reducer", "body persist write failed h=%d", bi->nHeight);
        return validation_state_error(out, "reducer-body-write-failed");
    }

    if (!block_index_set_have_data_verified(bi, &pos, bdir)) {
        LOG_WARN("reducer", "body persist verify failed h=%d file=%d pos=%u",
                 bi->nHeight, pos.nFile, pos.nPos);
        return validation_state_error(out, "reducer-body-verify-failed");
    }

    block_index_emit_header_event(bi, "reducer_ingest", NULL, NULL);
    LOG_INFO("reducer", "persisted ingested block body h=%d file=%d pos=%u",
             bi->nHeight, pos.nFile, pos.nPos);
    return true;
}

bool reducer_ingest_block(struct chain_activation_controller *ctl,
                          struct block *pblock,
                          enum reducer_source source,
                          bool force,
                          struct validation_state *out)
{
    (void)source; /* informational; `force` carries the live semantics */

    if (!out)
        return false;
    validation_state_init(out);

    if (!ctl || !pblock)
        return validation_state_error(out, "reducer-null-arg");

    /* (1) Stateless gate FIRST, inline, BEFORE any log/stage mutation. A
     * garbage block is rejected with the verdict already in `out`; nothing is
     * admitted to the inbox. The `force`/requested flag does not relax the
     * stateless checks; it gates the relay pre-filters inside the admit
     * producer path, not this gate. */
    if (!check_block(pblock, out, ctl->params, true, true, true)) {
        LOG_FAIL("reducer", "check_block failed: %s",
                 out->reject_reason[0] ? out->reject_reason : "unknown");
        return false;
    }

    /* (2) Push the header + raw bytes into the header_admit_inbox so the
     * producer path (step 2) can CREATE the block_index entry
     * without legacy accept_block_header. Hash-hint is the block hash. */
    struct uint256 block_hash;
    block_get_hash(pblock, &block_hash);

    /* Backfill the header's Equihash solution into the durable repair
     * side-table BEFORE the validate_headers drain below. check_block (above)
     * already stateless-verified this solution, so it is real. validate_headers
     * resolves any height ABOVE the persisted node.db tip from this table and
     * INDEPENDENTLY re-verifies PoW + Equihash, so this is a verified cache,
     * never a trust shortcut. Without it, a full block ingested for a height
     * whose in-index entry lost its nSolution (loaders drop it to save RAM)
     * fails validate_headers with "no-header-solution-backfill-required" even
     * though the trusted full block in hand carries the real solution — the
     * live wedge. Fixing it here makes EVERY full-block ingest (rebuild_recent
     * from the co-located zclassicd, submitblock, P2P block) self-supply its
     * solution. The save is hash-bound (recomputes hash(header)==block_hash)
     * and keyed by canonical height, so it cannot poison a different block. */
    if (pblock->header.nSolutionSize > 0 && ctl->ms) {
        int sol_h = -1;
        struct block_index *self =
            block_map_find(&ctl->ms->map_block_index, &block_hash);
        if (self) {
            sol_h = self->nHeight;
        } else {
            struct block_index *prev =
                block_map_find(&ctl->ms->map_block_index,
                               &pblock->header.hashPrevBlock);
            if (prev) sol_h = prev->nHeight + 1;
        }
        sqlite3 *rdb = progress_store_db();
        if (sol_h >= 0 && rdb)
            (void)stage_repair_header_solution_save(rdb, sol_h, &block_hash,
                                                    &pblock->header);
    }

    struct header_admit_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.hash = block_hash;
    msg.observed_unix = (int64_t)GetTime();
    msg.has_header = true;
    msg.header = pblock->header;
    /* height hint: one past the prev block's height when known, else -1
     * (admit verifies against the active chain regardless of the hint). */
    msg.height = -1;
    if (!mailbox_header_admit_push(&msg)) {
        /* Inbox full: cannot admit this block right now. Not a consensus
         * reject — report a transient error so the caller can retry. */
        return validation_state_error(out, "header-admit-inbox-full");
    }

    /* (3) Drain the eight stage step bodies synchronously under the SAME
     * mutex reducer activation serializes on, ahead of the 2s supervisor
     * tickers, so a single at-tip block reaches tip_finalize within the
     * call. The reorg disconnect (step 4) is driven from inside utxo_apply
     * when a better fork is selected. */
    zcl_mutex_lock(&ctl->mutex);
    /* Mark a synchronous drive so the staged_sync_supervisor yields its stage
     * ticks for the duration — they share the active-chain window, which is not
     * under the per-stage lock, and a concurrent supervisor drain races this
     * drive and can lock in a permanent failure row for the block being
     * ingested. Cleared before every return below (mutex unlock points). */
    reducer_drive_enter();
    (void)force; /* relay pre-filter gating lives in the admit producer */
    /* Runtime re-seed ONLY when the finalize cursor is genuinely BEHIND the
     * served tip's own transition (cursor < T, e.g. after a repair clamp).
     * cursor == T is the healthy steady state — the T→T+1 transition is the
     * stage's own pending work — and the old `< T+1` gate re-seeded it on
     * EVERY at-tip ingest, stamping the cursor to T+1 and skipping the
     * pending transition: each new block could only publish when ITS
     * successor arrived (the served-tip-trails-by-one defect, task #30;
     * live-reproduced 2026-06-12 post-anchor-fix: block 3144895 arrived
     * 11:50, published 11:53 with 3144896). */
    struct block_index *anchor_tip = active_chain_tip(&ctl->ms->chain_active);
    if (anchor_tip && anchor_tip->phashBlock &&
        tip_finalize_stage_cursor() < (uint64_t)anchor_tip->nHeight)
        (void)tip_finalize_stage_seed_anchor(anchor_tip->nHeight,
                                             anchor_tip->phashBlock->data,
                                             false /* runtime re-seed */);

    /* Body-INDEPENDENT prefix drain: run ONLY header_admit + validate_headers
     * to convergence so the block_index is created and the header is validated
     * BEFORE the body is persisted below. Draining the full pipeline here
     * (while the body is still absent) made the body-dependent stages record a
     * permanent failure row for this height — body_fetch turns a transient
     * validate_headers verdict into "skipped_invalid" and advances its cursor,
     * and the forward-only cursor never re-processes the height in the second
     * drain (where the body IS present), so the stale ok=0 propagated downstream
     * as upstream_failed and the block was rejected. Bounds match
     * reducer_drain_to_convergence (4096 rounds, 100/stage). */
    for (int _r = 0; _r < 4096; _r++) {
        int _adv = header_admit_stage_drain(100) +
                   validate_headers_stage_drain(100);
        if (_adv == 0)
            break;
    }
    if (!reducer_persist_ingested_body_locked(ctl, &block_hash, pblock, out)) {
        reducer_drive_exit();
        zcl_mutex_unlock(&ctl->mutex);
        return false;
    }
    /* Full drain now that BLOCK_HAVE_DATA is set: body_fetch .. tip_finalize
     * each process this height exactly once, with the body present. */
    (void)reducer_drain_to_convergence();

    struct block_index *ingested =
        block_map_find(&ctl->ms->map_block_index, &block_hash);

    /* Regtest on-demand mining (fMineBlocksOnDemand) has no successor header to
     * drive tip_finalize's one-block lookahead, so a self-mined block would
     * never finalize and `generate` would loop forever re-mining the same block
     * (the tip never leaves genesis). Publish the just-mined, fully-validated
     * at-tip block as the authoritative tip via the documented trusted-tip
     * primitive. set_authoritative_tip routes through anchor_cursor_to_authority
     * (monotonic guard: cannot lower the finalize cursor) and writes the own-hash
     * anchor row reducer_read_back_verdict reads. Gated on fMineBlocksOnDemand
     * (true ONLY for regtest; false on main/testnet, lib/chain/src/chainparams.c
     * — so byte-identical on a real network) AND on the same consensus witness
     * reducer_pending_body_is_accepted trusts (HAVE_DATA && !FAILED && utxo_apply
     * succeeded). Inlined, not the helper, so it never validation_state_init's
     * the caller's `out`. Under the held mutex. */
    if (ctl->params && ctl->params->fMineBlocksOnDemand && ingested &&
        (ingested->nStatus & BLOCK_HAVE_DATA) &&
        !(ingested->nStatus & BLOCK_FAILED_MASK) &&
        utxo_apply_stage_succeeded_at(ingested->nHeight)) {
        tip_finalize_stage_set_authoritative_tip(ingested->nHeight,
                                                 block_hash.data);
    }

    /* Prefer the just-ingested height for the read-back. The active tip may
     * still be one block behind while tip_finalize waits for lookahead, but
     * header/stateful rejects are recorded at the ingested height. */
    struct block_index *tip = active_chain_tip(&ctl->ms->chain_active);
    int target_h = ingested ? ingested->nHeight : (tip ? tip->nHeight : 0);
    reducer_drive_exit();
    zcl_mutex_unlock(&ctl->mutex);

    /* (4) Read back the verdict from the freshly-written log rows. */
    if (reducer_read_back_verdict(target_h, &block_hash, out))
        return true;

    /* Pending fallback: accept ONLY the live active tip (ingested == tip,
     * snapshotted under the lock) — a fork can't borrow another block's row. */
    if (ingested && ingested == tip &&
        reducer_pending_body_is_accepted(ingested, out))
        return true;

    /* On a regtest on-demand (generate/submitblock) REJECT, dump each stage's
     * log row + cursor for the height so an operator can see exactly which
     * stage in the synchronous drive recorded the failure. Gated on
     * fMineBlocksOnDemand — regtest only, zero cost on a live network. */
    if (ctl->params && ctl->params->fMineBlocksOnDemand) {
        sqlite3 *pdb = progress_store_db();
        static const char *const tbl[7] = {
            "validate_headers_log", "body_fetch_log", "body_persist_log",
            "script_validate_log", "proof_validate_log", "utxo_apply_log",
            "tip_finalize_log" };
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "[ondemand-reject] h=%d:", target_h);
        for (int i = 0; i < 7 && pdb && n < (int)sizeof(line); i++) {
            sqlite3_stmt *st = NULL;
            char q[96];
            snprintf(q, sizeof(q), "SELECT ok FROM %s WHERE height=?", tbl[i]);
            int ok = -1;
            if (sqlite3_prepare_v2(pdb, q, -1, &st, NULL) == SQLITE_OK) {  // raw-sql-ok:regtest-diag
                sqlite3_bind_int(st, 1, target_h);
                if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:regtest-diag
                    ok = sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %.3s=%d",
                          tbl[i], ok);
        }
        LOG_INFO("reducer", "%s", line);
    }

    return false;
}
