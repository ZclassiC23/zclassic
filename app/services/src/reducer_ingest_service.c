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
#include "util/thread_registry.h"  /* thread_registry_shutdown_requested */
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
#include "jobs/stage_helpers.h"
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

/* Shared drain core. `budget_us <= 0` means NO latency budget: keep draining
 * until convergence (a no-advance pass) or the round hard cap, whichever first.
 * The supervisor/FSM path passes a 2s budget so it yields its 2s stage ticks;
 * the dedicated mint driver passes 0 to drain the staged pipeline back-to-back
 * (see reducer_drain_to_convergence_unbudgeted). `per_stage_batch` sets how
 * many blocks each stage folds under ONE batch transaction (one COMMIT, one
 * fsync, one ext4 journal barrier per stage per round — see STAGE_DRAIN_IMPL).
 * A larger batch drops the fsync/journal-commit cadence, which is the genesis
 * fold's dominant wait (the drive thread blocks in jbd2_log_wait_commit). Full
 * validation is identical for any batch size — only the commit cadence and the
 * call's latency differ, never WHAT a stage checks. */
static int reducer_drain_core(int64_t budget_us, int hard_cap, int per_stage_batch)
{
    int64_t   start_us        = GetTimeMicros();
    uint64_t  fatal_gen0      = stage_fatal_generation();
    int       total           = 0;
    if (per_stage_batch <= 0) per_stage_batch = 100;
    for (int round = 0; round < hard_cap; round++) {
        /* On shutdown, return at this round boundary (a safe, committed point —
         * each stage's batch has already COMMITted) so the P2P message thread's
         * reducer activation exits promptly and connman_join succeeds instead of
         * timing out and detaching the thread under the frees that follow. The
         * fold is resumable, so stopping mid-drain loses no state. */
        if (thread_registry_shutdown_requested())
            break;
        int adv = reducer_drain_all_stages(per_stage_batch);
        total += adv;
        if (adv == 0)
            break;
        if (budget_us > 0 && GetTimeMicros() - start_us > budget_us)
            break;
    }
    /* Page the operator on a FATAL latched during this drain regardless of
     * which exit fired — convergence (adv==0) OR the budget timeout. A stage
     * can return JOB_FATAL every pass while another keeps advancing, so
     * total>0 and the loop exits on the budget, not on adv==0; gating the page
     * on the adv==0 break alone let that masked-FATAL recur unpaged. */
    {
        char st[STAGE_NAME_MAX] = {0}, why[128] = {0};
        if (stage_fatal_generation() != fatal_gen0 &&
            stage_last_fatal(st, sizeof(st), why, sizeof(why)))
            event_emitf(EV_OPERATOR_NEEDED, 0,
                        "condition=reducer_stage_fatal stage=%s reason=%s",
                        st, why);
    }
    return total;
}

int reducer_drain_to_convergence(void)
{
    const int64_t drain_budget_us = 2000 * 1000; /* 2s, same as legacy */
    const int     drain_hard_cap  = 4096;
    const int     per_stage_batch = 100;          /* legacy cadence, unchanged */
    return reducer_drain_core(drain_budget_us, drain_hard_cap, per_stage_batch);
}

int reducer_drain_to_convergence_unbudgeted(void)
{
    /* No 2s latency budget: drain back-to-back, not in 2s slices. Each slice
     * under the budgeted path re-acquired the mutex, re-read the frontier, and
     * raced the supervisor's own 2s stage ticks; here the drain just keeps
     * folding. The round cap is generous (folds tens of thousands of blocks per
     * call) yet still returns periodically so the -mint-anchor driver loop can
     * log applied-through progress and run its no-progress stall guard — it is
     * a chunk size, NOT a 2s wall-clock chop. A larger per-stage batch (one
     * COMMIT/fsync per this many blocks per stage) also drops the ext4
     * journal-commit cadence that otherwise pins the drive thread in
     * jbd2_log_wait_commit. */
    const int drain_hard_cap   = 64;      /* up to 64 * batch blocks per call */
    const int per_stage_batch  = 1000;    /* one fsync per ~1k blocks/stage:
                                           * ~10x fewer journal barriers than
                                           * the 100-block legacy cadence, while
                                           * still committing often enough that
                                           * progress stays visible and the open
                                           * transaction (+ WAL) stays bounded. */
    return reducer_drain_core(/*budget_us=*/0, drain_hard_cap, per_stage_batch);
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

int reducer_kick_unbudgeted(struct chain_activation_controller *ctl)
{
    if (!ctl)
        return 0;
    /* The dedicated -mint-anchor driver's tight drain: same locking + drive
     * marking as reducer_kick, but the inner drain has no 2s budget so one
     * call folds the staged pipeline back-to-back until convergence. Held
     * under ctl->mutex for the whole drain — the same serialization point the
     * supervisor takes — so no concurrent supervisor drain races the
     * active-chain window. Full validation is unchanged. */
    zcl_mutex_lock(&ctl->mutex);
    reducer_drive_enter();
    int advanced = reducer_drain_to_convergence_unbudgeted();
    reducer_drive_exit();
    zcl_mutex_unlock(&ctl->mutex);
    return advanced;
}

/* A failed validate_headers row whose reason is a pure header-source HASH
 * disagreement (the on-disk/index header pinned at a height differs from the
 * header source we validated) is NOT proof the peer is malicious — at a
 * CONTESTED height it is exactly the signature of a competing valid-PoW fork.
 * Distinguish it from genuinely-invalid reasons (high-hash, invalid-solution,
 * bad-equihash-solution-size, ...) so the ban classification can be softened
 * for the fork case ONLY. Validity is unaffected: a hash-mismatch row never
 * adopts the block here regardless of this classification. */
static bool reducer_reason_is_hash_mismatch(const char *reason)
{
    return reason && reason[0] && strstr(reason, "hash-mismatch") != NULL;
}

/* CONTESTED height = the block_index holds more than one entry at `height`
 * AND at least one of those entries already carries BLOCK_HAVE_DATA (i.e. a
 * real body for a competitor is on disk). That is the structural signature of
 * a fork at the height, distinct from a single pinned bodiless/failed orphan.
 * Read-only scan of the in-memory index; safe under the reducer drive. */
static bool reducer_height_is_contested(struct main_state *ms, int height)
{
    if (!ms)
        return false;

    int entries = 0;
    bool any_have_data = false;
    size_t iter = 0;
    struct block_index *bi = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (!bi || bi->nHeight != height)
            continue;
        entries++;
        if (bi->nStatus & BLOCK_HAVE_DATA)
            any_have_data = true;
    }
    return entries > 1 && any_have_data;
}

/* Map freshly-written stage log rows for `height`/`hash` into `out`. */
static bool reducer_read_back_verdict(struct main_state *ms,
                                      int height,
                                      const struct uint256 *hash,
                                      struct validation_state *out)
{
    sqlite3 *pdb = progress_store_db();

    struct validate_headers_window_report rep;
    if (validate_headers_stage_window_report(height, height, &rep) &&
        rep.failed_count > 0) {
        /* Part B ban softening: at a CONTESTED height (>1 index entry, one
         * with a real body on disk) a header-source hash-mismatch is a
         * competing valid-PoW FORK, not a malicious block. The body already
         * passed the stateless check_block gate (step 1) before reaching this
         * read-back, so PoW/malformedness is NOT the cause. Drop the DoS to 0
         * so the peer serving the real chain is not banned. The block is still
         * NOT adopted (return false) — only the ban classification changes.
         * Genuinely-invalid reasons (high-hash, invalid-solution, ...) keep
         * dos=100; this is download/peer-ban policy, not a validity predicate. */
        int dos = 100;
        if (reducer_reason_is_hash_mismatch(rep.first_fail_reason) &&
            reducer_height_is_contested(ms, height))
            dos = 0;
        validation_state_dos(out, dos, false, REJECT_INVALID,
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

    uint64_t tf_cursor = pdb ? stage_cursor_persisted(
        pdb, "tip_finalize", "reducer") : tip_finalize_stage_cursor();
    uint64_t ua_cursor = pdb ? stage_cursor_persisted(
        pdb, "utxo_apply", "reducer") : 0;
    int64_t tf_published = tip_finalize_stage_last_height();
    bool ua_ok = utxo_apply_stage_succeeded_at(height);
    uint8_t witness_hash[32];
    bool tf_hash_witness = pdb &&
        tip_finalize_stage_block_hash_at(pdb, height, witness_hash);
    const char *tf_blocked = tip_finalize_stage_last_blocked_reason();
    char debug[MAX_REJECT_REASON];
    snprintf(debug, sizeof(debug),
             "h=%d tf_cursor=%llu tf_published=%lld ua_cursor=%llu "
             "ua_ok=%d tf_hash_witness=%d tf_blocked=%s",
             height, (unsigned long long)tf_cursor,
             (long long)tf_published, (unsigned long long)ua_cursor,
             ua_ok ? 1 : 0, tf_hash_witness ? 1 : 0,
             tf_blocked && tf_blocked[0] ? tf_blocked : "none");
    validation_state_invalid(out, false, REJECT_INVALID,
                             "block-not-finalized-by-reducer", debug);
    return false;
}

static bool reducer_header_rejected_at(struct main_state *ms, int height,
                                       struct validation_state *out)
{
    struct validate_headers_window_report rep;
    if (!validate_headers_stage_window_report(height, height, &rep) ||
        rep.failed_count == 0)
        return false;

    /* Part B ban softening (same rule as reducer_read_back_verdict): a
     * header-source hash-mismatch at a contested height is a fork, not a
     * malicious block — drop DoS to 0 so the real-chain peer is not banned.
     * The header is still REJECTED (return true) — only ban policy changes. */
    int dos = 100;
    if (reducer_reason_is_hash_mismatch(rep.first_fail_reason) &&
        reducer_height_is_contested(ms, height))
        dos = 0;
    validation_state_dos(out, dos, false, REJECT_INVALID,
                         rep.first_fail_reason[0]
                             ? rep.first_fail_reason
                             : "header-validation-failed",
                         false, NULL);
    return true;
}

static bool reducer_pending_body_is_accepted(
        struct main_state *ms,
        const struct block_index *bi,
        struct validation_state *out)
{
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA) ||
        (bi->nStatus & BLOCK_FAILED_MASK))
        return false;

    if (reducer_header_rejected_at(ms, bi->nHeight, out))
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
        struct validation_state *out,
        bool honor_header_reject)
{
    if (!ctl || !ctl->ms || !block_hash || !pblock) {
        LOG_WARN("reducer", "body persist invalid args ctl=%p ms=%p hash=%p block=%p",
                 (void *)ctl, ctl ? (void *)ctl->ms : NULL,
                 (const void *)block_hash, (void *)pblock);
        return validation_state_error(out, "reducer-body-null-arg");
    }

    struct block_index *bi = block_map_find(&ctl->ms->map_block_index,
                                            block_hash);
    if (!bi) {
        char hex[65];
        uint256_get_hex(block_hash, hex);
        LOG_WARN("reducer",
                 "body persist missing block_index entry for hash=%s; "
                 "header admission did not create a selectable candidate",
                 hex);
        return validation_state_error(out, "reducer-body-header-missing");
    }

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

    /* Synchronous ingest needs the immediate prior verdict. Catch-up staging
     * deliberately skips this height-keyed gate after check_block(): stale
     * hash-mismatch rows must not prevent persisting the body that lets the
     * reducer re-fold the actual hash. */
    if (honor_header_reject &&
        reducer_header_rejected_at(ctl->ms, bi->nHeight, out))
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

static void reducer_cache_ingested_solution(
        struct chain_activation_controller *ctl,
        struct block *pblock,
        const struct uint256 *block_hash)
{
    if (!ctl || !pblock || !block_hash ||
        pblock->header.nSolutionSize == 0 || !ctl->ms)
        return;

    int sol_h = -1;
    struct block_index *self =
        block_map_find(&ctl->ms->map_block_index, block_hash);
    if (self) {
        sol_h = self->nHeight;
    } else {
        struct block_index *prev =
            block_map_find(&ctl->ms->map_block_index,
                           &pblock->header.hashPrevBlock);
        if (prev)
            sol_h = prev->nHeight + 1;
    }
    sqlite3 *rdb = progress_store_db();
    if (sol_h >= 0 && rdb)
        (void)stage_repair_header_solution_save(rdb, sol_h, block_hash,
                                                &pblock->header);
}

static bool reducer_push_header_admit(struct block *pblock,
                                      const struct uint256 *block_hash,
                                      struct validation_state *out)
{
    struct header_admit_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.hash = *block_hash;
    msg.observed_unix = (int64_t)GetTime();
    msg.has_header = true;
    msg.header = pblock->header;
    msg.height = -1;
    if (mailbox_header_admit_push(&msg))
        return true;

    /* Inbox full: cannot admit this block right now. Not a consensus
     * reject — report a transient error so the caller can retry. */
    LOG_WARN("reducer", "header admit inbox full while staging block");
    return validation_state_error(out, "header-admit-inbox-full");
}

bool reducer_stage_p2p_block_for_catchup(
    struct chain_activation_controller *ctl,
    struct block *pblock,
    struct validation_state *out)
{
    if (!out)
        return false;
    validation_state_init(out);
    if (!ctl || !pblock)
        return validation_state_error(out, "reducer-null-arg");
    if (!ctl->ms)
        return validation_state_error(out, "reducer-main-state-unwired");

    if (!check_block(pblock, out, ctl->params, true, true, true)) {
        LOG_FAIL("reducer", "check_block failed: %s",
                 out->reject_reason[0] ? out->reject_reason : "unknown");
        return false;
    }

    struct uint256 block_hash;
    block_get_hash(pblock, &block_hash);
    reducer_cache_ingested_solution(ctl, pblock, &block_hash);
    bool header_pushed = reducer_push_header_admit(pblock, &block_hash, out);
    if (!header_pushed)
        return false;

    zcl_mutex_lock(&ctl->mutex);
    reducer_drive_enter();

    struct block_index *bi = block_map_find(&ctl->ms->map_block_index,
                                            &block_hash);
    if (!bi) {
        reducer_drive_exit();
        zcl_mutex_unlock(&ctl->mutex);
        validation_state_invalid(out, false, REJECT_INVALID,
                                 "p2p-block-header-missing",
                                 "header not admitted yet");
        return false;
    }

    bool persisted = reducer_persist_ingested_body_locked(
        ctl, &block_hash, pblock, out, false);
    if (persisted && ctl->ms->pindex_best_header)
        (void)active_chain_extend_window_have_data(
            &ctl->ms->chain_active, &ctl->ms->map_block_index,
            ctl->ms->pindex_best_header, ctl->ms->pindex_best_header->nHeight);

    int staged_height = bi->nHeight;
    reducer_drive_exit();
    zcl_mutex_unlock(&ctl->mutex);

    if (!persisted)
        return false;

    char debug[MAX_REJECT_REASON];
    snprintf(debug, sizeof(debug), "h=%d body persisted for staged reducer",
             staged_height);
    validation_state_invalid(out, false, REJECT_INVALID,
                             "p2p-block-staged-for-reducer", debug);
    return false;
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
    reducer_cache_ingested_solution(ctl, pblock, &block_hash);
    bool header_pushed = reducer_push_header_admit(pblock, &block_hash, out);
    if (!header_pushed)
        return false;

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

    /* Regtest on-demand bootstrap: a FRESH genesis-only node (no import /
     * snapshot / reindex ever ran tip_finalize_stage_seed_anchor) has no
     * genesis anchor row, so the staged cursors are stuck at 0 and the first
     * `generate` records no utxo_apply row (utx=-1) -> tip never finalizes
     * ("block-not-finalized-by-reducer"). Import/snapshot/reindex paths all
     * seed genesis; for a fresh on-demand node nothing does. The runtime
     * re-seed above cannot reach this case (tip at genesis => nHeight 0 =>
     * `cursor < 0` is always false). Seed the genesis anchor ONCE, exactly as
     * the passing `generate 1` model in test_reducer_step_drain_harness.c does.
     * Gated on fMineBlocksOnDemand so it is a no-op on main/testnet (the bool is
     * the FIRST condition; byte-identical there). Doubly inert off-regtest: it
     * only fires for a node whose tip IS genesis with an unseeded cursor, a
     * state a synced node is never in. Idempotent: seed_anchor is INSERT-OR-
     * IGNORE and runs the full seed_integrity_gate; the nHeight==0 && cursor==0
     * guard self-disables after the first block. */
    if (ctl->params && ctl->params->fMineBlocksOnDemand &&
        anchor_tip && anchor_tip->phashBlock &&
        anchor_tip->nHeight == 0 &&
        tip_finalize_stage_cursor() == 0)
        (void)tip_finalize_stage_seed_anchor(0,
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
    if (!reducer_persist_ingested_body_locked(ctl, &block_hash, pblock, out,
                                              true)) {
        reducer_drive_exit();
        zcl_mutex_unlock(&ctl->mutex);
        return false;
    }
    /* Extend the visible active-chain window along the contiguous have-data
     * frontier before draining the body-dependent stages, so body_fetch /
     * body_persist / script_validate can see active_chain_at(cursor+1) as the
     * just-persisted body lands. (See chain_activation_service.c for the full
     * rationale: the blocks-less snapshot boot retracts the window to the seed
     * and nothing else widens it inside this drive.) No-op when there is no gap;
     * takes only the active-chain + block-map rwlocks; inside reducer_drive. */
    if (ctl->ms->pindex_best_header)
        (void)active_chain_extend_window_have_data(
            &ctl->ms->chain_active, &ctl->ms->map_block_index,
            ctl->ms->pindex_best_header, ctl->ms->pindex_best_header->nHeight);
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
    if (reducer_read_back_verdict(ctl->ms, target_h, &block_hash, out))
        return true;

    /* Pending fallback: accept ONLY the live active tip (ingested == tip,
     * snapshotted under the lock) — a fork can't borrow another block's row. */
    if (ingested && ingested == tip &&
        reducer_pending_body_is_accepted(ctl->ms, ingested, out))
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
