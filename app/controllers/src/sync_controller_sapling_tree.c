/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Sapling tree rebuild support for the sync controller.
 *
 * This file owns the long-running replay that rebuilds the persisted
 * Sapling commitment tree from block files. */

#include "controllers/sync_controller.h"
#include "sync_controller_internal.h"

#include "chain/chain.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "core/utiltime.h"
#include "jobs/reducer_frontier.h"
#include "models/database.h"
#include "primitives/block.h"
#include "sapling/incremental_merkle_tree.h"
#include "services/block_index_loader.h"
#include "supervisors/domains.h"
#include "util/blocker.h"
#include "util/boot_progress.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* Live incident (2026-07): the deferred/live rebuild shares the reducer's
 * node_db connection from another thread, so a persist's BEGIN nested into
 * the reducer's open batch tx (SQLITE_ERROR, un-retryable) and spammed on
 * every checkpoint while the rebuild never completed. The primary cure is the
 * autocommit guard in sapling_tree_persist_pair_status; this bounded retry is
 * the residual defense for a genuine cross-connection BUSY/LOCKED. */
#define SAPLING_TREE_BEGIN_MAX_ATTEMPTS 5
#define SAPLING_TREE_BEGIN_BACKOFF_MS   60

/* The FINAL persist bounded-retries a DEFERRED result, yielding between
 * attempts so the reducer can commit its batch and return to autocommit. The
 * per-100k checkpoint persists never wait — a deferred checkpoint is skipped
 * and re-attempted at the next checkpoint / the final persist. */
#define SAPLING_TREE_FINAL_PERSIST_ATTEMPTS  8
#define SAPLING_TREE_FINAL_PERSIST_BACKOFF_MS 125

/* Heartbeat cadence for the supervised rebuild (task spec: every 60s). */
#define SAPLING_TREE_REBUILD_HEARTBEAT_MS 60000

static bool sapling_header_root_known(const struct block_index *bi)
{
    static const uint8_t zeros32[32] = {0};

    return bi && memcmp(bi->hashFinalSaplingRoot.data, zeros32, 32) != 0;
}

#ifdef ZCL_TESTING
/* See controllers/sync_controller.h. 0 = disabled (real node_db_begin runs);
 * >0 = decrementing count of forced-busy attempts; <0 = forced busy forever
 * (stays negative — never reaches 0). */
static _Atomic int g_sapling_begin_force_busy = 0;

void sapling_tree_rebuild_test_force_begin_busy(int attempts)
{
    atomic_store(&g_sapling_begin_force_busy, attempts);
}

/* Counts persists DEFERRED because the shared node_db connection already held
 * a foreign open tx (the real production failure). Lets a test assert the
 * deferral path actually FIRED, not just "no crash". */
static _Atomic int g_sapling_persist_deferrals = 0;

int sapling_tree_rebuild_test_persist_deferrals(void)
{
    return atomic_load(&g_sapling_persist_deferrals);
}

void sapling_tree_rebuild_test_reset_persist_deferrals(void)
{
    atomic_store(&g_sapling_persist_deferrals, 0);
}
#endif

/* Single BEGIN attempt. Returns true + rc=SQLITE_OK on success, false + the
 * sqlite result code on failure. Under ZCL_TESTING the fault-injection counter
 * above can substitute a simulated SQLITE_BUSY without touching the real
 * connection, so the retry/backoff/blocker logic is deterministically tested. */
static bool sapling_tree_do_begin(struct node_db *ndb, int *rc_out)
{
#ifdef ZCL_TESTING
    int remaining = atomic_load(&g_sapling_begin_force_busy);
    if (remaining != 0) {
        if (remaining > 0)
            atomic_fetch_sub(&g_sapling_begin_force_busy, 1);
        *rc_out = SQLITE_BUSY;
        return false;
    }
#endif
    if (node_db_begin(ndb)) {
        *rc_out = SQLITE_OK;
        return true;
    }
    struct node_db_status st;
    node_db_get_status(ndb, &st);
    *rc_out = st.last_sqlite_rc;
    return false;
}

/* Bounded retry + linear backoff around node_db_begin() for a GENUINE
 * SQLITE_BUSY/LOCKED — a lock lost to another CONNECTION (e.g. the periodic
 * WAL-checkpoint thread), the one contention class that clears on its own.
 * The nested-BEGIN case (a foreign tx open on THIS connection → SQLITE_ERROR
 * "cannot start a transaction within a transaction", which can NEVER clear by
 * retrying) is filtered out upstream by the sqlite3_get_autocommit() guard in
 * sapling_tree_persist_pair_status, so this loop only ever sees the retryable
 * class. Correct classification depends on node_db_begin now preserving the
 * real rc (app/models/src/database.c). Bounded attempts + name-a-blocker on
 * exhaustion (no log spam); waits only on the CALLING thread, so it cannot
 * block the reducer drive (LOCK-ORDER LAW). */
static bool sapling_tree_begin_with_retry(struct node_db *ndb, int64_t height)
{
    for (int attempt = 1; attempt <= SAPLING_TREE_BEGIN_MAX_ATTEMPTS;
         attempt++) {
        int rc = SQLITE_OK;
        if (sapling_tree_do_begin(ndb, &rc))
            return true;

        bool busy = (rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
        bool exhausted = (attempt == SAPLING_TREE_BEGIN_MAX_ATTEMPTS);

        if (!busy || exhausted) {
            char reason[BLOCKER_REASON_MAX];
            snprintf(reason, sizeof(reason),
                    "persist_pair BEGIN failed height=%lld attempt=%d/%d "
                    "rc=%d (%s)", (long long)height, attempt,
                    SAPLING_TREE_BEGIN_MAX_ATTEMPTS, rc,
                    sqlite3_errstr(rc));
            struct blocker_record rec;
            if (blocker_init(&rec, "sapling_tree_rebuild.persist_busy",
                             "sync.sapling_tree_rebuild",
                             BLOCKER_TRANSIENT, reason))
                blocker_set(&rec);
            LOG_WARN("sapling_tree_rebuild", "%s", reason);
            return false;
        }

        int64_t backoff_ms = (int64_t)SAPLING_TREE_BEGIN_BACKOFF_MS * attempt;
        struct timespec ts = {
            .tv_sec = backoff_ms / 1000,
            .tv_nsec = (backoff_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }
    return false;
}

/* Tri-state outcome of a persist attempt. DEFERRED is distinct from FAILED:
 * it means "wrote nothing on purpose, retry later" (a foreign open tx owned
 * the connection), NOT a derived-state error — the rebuild loop must not
 * fail-close on it. */
enum sapling_persist_status {
    SAPLING_PERSIST_OK = 0,
    SAPLING_PERSIST_DEFERRED,
    SAPLING_PERSIST_FAILED,
};

/* lane/sapling-tree-persist: persist node_state["sapling_tree"] and
 * node_state["sapling_tree_rebuild_height"] as ONE atomic write so a crash
 * can never pair the tree blob with a stale/absent height (the boot-time
 * loader, config/src/boot.c sapling_tree_verify_at_saved_height, trusts that
 * pairing). When the reducer already holds the batch (ndb->sync_in_batch) the
 * pair folds into that outer tx instead of nesting a BEGIN.
 *
 * lane/e3-sapling-rebuild-robust: NEVER nest a BEGIN on a connection that
 * already holds an open tx. The deferred/live rebuild runs on a thread OTHER
 * than the reducer while SHARING the reducer's FULLMUTEX node_db handle; the
 * reducer's batch (sync_controller_blocks.c) keeps a tx open across many
 * blocks. A BEGIN into that live tx returns SQLITE_ERROR "cannot start a
 * transaction within a transaction" — un-retryable while the outer tx lives,
 * the real production failure the old BUSY/LOCKED retry could never clear.
 * ndb->sync_in_batch only tracks the reducer's OWN bookkeeping (and lags a
 * live tx in the begin→flag-set window), so gate the own-BEGIN decision on the
 * ACTUAL connection state via sqlite3_get_autocommit(): a foreign open tx is a
 * transient timing condition, not a state error — DEFER (write nothing) and
 * let the caller retry once the connection returns to autocommit. */
static enum sapling_persist_status
sapling_tree_persist_pair_status(struct node_db *ndb,
                                 const void *blob, size_t blob_len,
                                 int64_t height)
{
    bool own_tx = ndb && !ndb->sync_in_batch;
    if (own_tx) {
        if (ndb->db && sqlite3_get_autocommit(ndb->db) == 0) {
#ifdef ZCL_TESTING
            atomic_fetch_add(&g_sapling_persist_deferrals, 1);
#endif
            LOG_INFO("sapling_tree_rebuild",
                    "persist_pair: deferring height=%lld — connection holds a "
                    "foreign open transaction (reducer batch); will retry when "
                    "it returns to autocommit", (long long)height);
            return SAPLING_PERSIST_DEFERRED;
        }
        if (!sapling_tree_begin_with_retry(ndb, height))
            return SAPLING_PERSIST_FAILED;
    }

    bool ok = node_db_state_set(ndb, "sapling_tree", blob, blob_len) &&
              node_db_state_set_int(ndb, "sapling_tree_rebuild_height",
                                    height);

    if (own_tx) {
        if (ok) {
            ok = node_db_commit(ndb);
        } else {
            node_db_rollback(ndb);
        }
    }
    return ok ? SAPLING_PERSIST_OK : SAPLING_PERSIST_FAILED;
}

/* Public bool wrapper (unchanged signature — external callers in
 * sync_controller_blocks.c, wallet_rescan_controller_witness.c, and
 * boot_refold_staged.c always persist under their OWN tx / sync_in_batch, so
 * they never hit the DEFERRED branch and OK-vs-not-OK is all they need). */
bool sapling_tree_persist_pair(struct node_db *ndb,
                               const void *blob, size_t blob_len,
                               int64_t height)
{
    return sapling_tree_persist_pair_status(ndb, blob, blob_len, height)
           == SAPLING_PERSIST_OK;
}

/* ── Supervision (lib/util/include/util/supervisor.h contract) ───────────
 * The rebuild runs synchronously on WHATEVER thread invokes it (boot's own
 * thread, an RPC thread for `rebuildsaplingtree`, or a dedicated deferred
 * worker — see config/src/boot.c). Registering a liveness contract makes a
 * hung invocation a NAMED stall (independent supervisor thread notices
 * last_tick_us going stale) instead of a silent multi-hour black hole —
 * exactly the failure the live incident hit. One contract is shared across
 * invocations (registered once, re-armed at the top of every call). */
static struct liveness_contract g_sapling_rebuild_contract;
static _Atomic supervisor_child_id g_sapling_rebuild_sup_id =
    SUPERVISOR_INVALID_ID;

static supervisor_child_id sapling_tree_rebuild_supervisor_ensure(void)
{
    supervisor_child_id id = atomic_load(&g_sapling_rebuild_sup_id);
    if (id != SUPERVISOR_INVALID_ID)
        return id;
    if (!supervisor_start())
        return SUPERVISOR_INVALID_ID;

    liveness_contract_init(&g_sapling_rebuild_contract,
                           "sync.sapling_tree_rebuild");
    /* Self-driven (period_secs=0): the replay loop ticks itself every
     * ~60s. deadline_secs is generous headroom over the worst-case
     * persist-retry stall (6 attempts * up to ~1.2s backoff, each attempt
     * behind a 10s sqlite busy_timeout) so a genuinely stuck loop — not
     * just one slow checkpoint — is what fires the stall. */
    atomic_store(&g_sapling_rebuild_contract.deadline_secs, 180);
    atomic_store(&g_sapling_rebuild_contract.progress_max_quiet_us, 0);

    supervisor_domains_init();
    supervisor_child_id new_id = supervisor_register_in_domain(
        g_chain_sup, &g_sapling_rebuild_contract);
    if (new_id == SUPERVISOR_INVALID_ID)
        return SUPERVISOR_INVALID_ID;

    supervisor_child_id expected = SUPERVISOR_INVALID_ID;
    if (!atomic_compare_exchange_strong(&g_sapling_rebuild_sup_id, &expected,
                                        new_id)) {
        /* Lost a registration race against a concurrent invocation —
         * unregister the duplicate and use the winner's id. */
        supervisor_unregister(new_id);
        return atomic_load(&g_sapling_rebuild_sup_id);
    }
    return new_id;
}

/* Raise (or clear) the fail-closed blocker family the boot-time "Sapling
 * tree root MISMATCH" path drives sapling_tree_rebuild() through — every
 * fail-closed reason from this function (root mismatch included) shares
 * one blocker id so operators see a single named signal instead of raw
 * log lines. Root-mismatch reasons are PERMANENT (a real derived-state
 * disagreement, not a lock race); every other reason (serialize/persist
 * plumbing) is TRANSIENT. */
static void sapling_tree_rebuild_raise_fail_blocker(const char *fail_reason,
                                                     int fail_height,
                                                     int total_commitments,
                                                     int mismatches)
{
    bool is_root_mismatch = fail_reason &&
        (strstr(fail_reason, "sapling_root_mismatch") != NULL ||
         strcmp(fail_reason, "tip_missing_sapling_root") == 0);
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
            "sapling_tree_rebuild fail-closed reason=%s height=%d "
            "commitments=%d mismatches=%d",
            fail_reason ? fail_reason : "unknown", fail_height,
            total_commitments, mismatches);
    struct blocker_record rec;
    if (blocker_init(&rec, "sapling_tree_rebuild.fail_closed",
                     "sync.sapling_tree_rebuild",
                     is_root_mismatch ? BLOCKER_PERMANENT
                                       : BLOCKER_TRANSIENT,
                     reason))
        blocker_set(&rec);
}

int sapling_tree_rebuild(struct node_db *ndb,
                         const struct active_chain *chain,
                         const char *datadir)
{
    if (!ndb || !chain || !datadir)
        LOG_ERR("sync", "sapling_tree_rebuild: invalid args (ndb=%p, chain=%p, datadir=%p)",
                (void *)ndb, (void *)chain, (void *)datadir);

    int header_tip = active_chain_height(chain);
    int sapling_height = 476969; /* ZClassic Sapling activation */

    /* Resolve the rebuild endpoint from coins-applied state, NOT the
     * pre-fold header tip. On a wedged node the active/header tip can run
     * far ahead of the durable coins frontier (active tip << header tip);
     * the persisted hashFinalSaplingRoot above the applied frontier may be
     * absent, so verifying the rebuilt tree against the HEADER tip would
     * FATAL on `tip_missing_sapling_root` before the forward fold even runs.
     * Cap the endpoint to coins-best (coins_applied_height - 1, the durable
     * applied frontier) when that is present and lower than the header tip:
     * coins-best is by construction a block the node has APPLIED, so its body
     * is on disk (BLOCK_HAVE_DATA) and its hashFinalSaplingRoot is known, so
     * the final tip-root check has a real block to verify against. When the
     * coins frontier is ABSENT (a fresh/legacy datadir, or a unit test with no
     * progress store) the header tip is kept unchanged — the per-block replay
     * already skips header-only (non-HAVE_DATA) blocks (see :BLOCK_HAVE_DATA
     * check below), so the existing legacy-resume behavior is preserved. */
    int chain_tip = header_tip;
    {
        int32_t coins_best = -1;
        if (reducer_frontier_derive_coins_best_now(&coins_best, NULL, NULL)
            && coins_best >= 0 && coins_best < chain_tip) {
            LOG_INFO("sapling_tree_rebuild",
                     "sapling_tree_rebuild: capping endpoint to coins-applied "
                     "height %d (header tip %d)", coins_best, header_tip);
            chain_tip = coins_best;
        }
    }

    if (chain_tip < sapling_height) return 0;

    supervisor_child_id sup_id = sapling_tree_rebuild_supervisor_ensure();
    if (sup_id != SUPERVISOR_INVALID_ID) {
        atomic_store(&g_sapling_rebuild_contract.completed, false);
        atomic_store(&g_sapling_rebuild_contract.deadline_secs, 180);
        supervisor_progress(sup_id, 0);
        supervisor_tick(sup_id);
    }
    int64_t last_heartbeat_ms = GetTimeMillis();

    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);
    int total_commitments = 0;
    int mismatches = 0;
    int start_height = sapling_height;
    const char *fail_reason = NULL;
    int fail_height = -1;

    /* Try to resume from a persisted checkpoint to avoid replaying
     * 2.6M blocks on every crash recovery. Two candidates, most
     * authoritative first:
     *   (1) Flat-file checkpoint at <datadir>/sapling_tree_ckpt.dat
     *       flushed every 10K blocks, SHA3-verified.
     *   (2) node_state["sapling_tree"] - flushed every 100K blocks,
     *       SQLite-backed, legacy path.
     * The flat-file path short-circuits the node_state path on a hit;
     * a miss falls through to the original node_state probe. */
    int64_t ckpt_h = 0;
    {
        char ckpt_path[512];
        int n = snprintf(ckpt_path, sizeof(ckpt_path),
                         "%s/sapling_tree_ckpt.dat", datadir);
        if (n > 0 && (size_t)n < sizeof(ckpt_path)) {
            int64_t flat_h = 0;
            uint8_t flat_hash[32] = {0};
            struct incremental_merkle_tree ff_tree;
            sapling_tree_init(&ff_tree);
            if (sapling_tree_load_checkpoint(&ff_tree, &flat_h, flat_hash,
                                             ckpt_path)
                && flat_h > sapling_height) {
                /* Fail-closed bind to the header chain at flat_h: height <=
                 * endpoint, same block hash, and root == hashFinalSaplingRoot.
                 * A stale/reorged/mismatched checkpoint is refused and the
                 * replay restarts from Sapling activation. */
                const struct block_index *ckpt_bi =
                    active_chain_at(chain, (int)flat_h);
                struct uint256 ffr;
                incremental_tree_root(&ff_tree, &ffr);
                bool exp_hash_known = ckpt_bi && ckpt_bi->phashBlock;
                enum sapling_ckpt_verdict v = sapling_ckpt_verify_binding(
                    flat_h, &ffr, flat_hash, chain_tip,
                    exp_hash_known ? ckpt_bi->phashBlock->data : NULL,
                    exp_hash_known,
                    sapling_header_root_known(ckpt_bi)
                        ? &ckpt_bi->hashFinalSaplingRoot : NULL,
                    sapling_header_root_known(ckpt_bi));
                if (v == SAPLING_CKPT_OK) {
                    tree = ff_tree;
                    start_height = (int)flat_h + 1;
                    total_commitments =
                        (int)incremental_tree_size(&tree);
                    ckpt_h = flat_h;
                    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: resuming " "from flat-file checkpoint h=%lld " "(%d commitments,)", (long long)flat_h, total_commitments);
                    fflush(stderr);
                } else {
                    LOG_WARN("sapling_tree_rebuild",
                             "sapling_tree_rebuild: refusing flat-file "
                             "checkpoint h=%lld (%s)",
                             (long long)flat_h, sapling_ckpt_verdict_str(v));
                }
            }
        }
    }

    if (ckpt_h == 0
        && node_db_state_get_int(ndb, "sapling_tree_rebuild_height", &ckpt_h)
        && ckpt_h > sapling_height && ckpt_h <= chain_tip) {
        uint8_t tbuf[8192];
        size_t tlen = 0;
        if (node_db_state_get(ndb, "sapling_tree", tbuf, sizeof(tbuf), &tlen)
            && tlen > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tbuf, tlen);
            if (incremental_tree_deserialize(&tree, &ts)) {
                const struct block_index *ckpt_bi =
                    active_chain_at(chain, (int)ckpt_h);
                if (sapling_header_root_known(ckpt_bi)) {
                    struct uint256 ckpt_root;
                    incremental_tree_root(&tree, &ckpt_root);
                    if (memcmp(ckpt_root.data,
                               ckpt_bi->hashFinalSaplingRoot.data,
                               32) == 0) {
                        start_height = (int)ckpt_h + 1;
                        total_commitments =
                            (int)incremental_tree_size(&tree);
                        LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: resuming " "from checkpoint h=%d (%d commitments)", (int)ckpt_h, total_commitments);
                        fflush(stderr);
                    } else {
                        LOG_WARN("sapling_tree_rebuild",
                                 "sapling_tree_rebuild: checkpoint h=%d "
                                 "root mismatch; replaying from activation",
                                 (int)ckpt_h);
                        sapling_tree_init(&tree);
                    }
                } else {
                    LOG_WARN("sapling_tree_rebuild",
                             "sapling_tree_rebuild: refusing unverified "
                             "node_state checkpoint h=%d (missing "
                             "hashFinalSaplingRoot)",
                             (int)ckpt_h);
                    sapling_tree_init(&tree);
                }
            }
        }
    }

    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: replaying h=%d..%d", start_height, chain_tip);
    fflush(stderr);

    int64_t t_replay_start = GetTimeMillis();
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    for (int h = start_height; h <= chain_tip; h++) {
        if ((h % 100) == 0)
            boot_progress_tick("sapling_tree_rebuild");
        if (sup_id != SUPERVISOR_INVALID_ID && (h % 1000) == 0) {
            int64_t now_ms = GetTimeMillis();
            if (now_ms - last_heartbeat_ms >=
                SAPLING_TREE_REBUILD_HEARTBEAT_MS) {
                last_heartbeat_ms = now_ms;
                supervisor_tick(sup_id);
                supervisor_progress(sup_id, total_commitments);
                LOG_INFO("sapling_tree_rebuild",
                        "sapling_tree_rebuild: heartbeat h=%d/%d "
                        "commitments_folded=%d", h, chain_tip,
                        total_commitments);
            }
        }
        const struct block_index *bi = active_chain_at(chain, h);
        if (!bi) continue;
        if (!(bi->nStatus & BLOCK_HAVE_DATA)) continue;

        if (bi->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            cached_data = sync_controller_mmap_block_file(datadir, bi->nFile,
                                                          &cached_size);
            cached_file = cached_data ? bi->nFile : -1;
            if (!cached_data) continue;
        }

        if (bi->nDataPos >= cached_size) continue;

        struct block blk;
        block_init(&blk);
        size_t remaining = cached_size - bi->nDataPos;
        struct byte_stream s;
        stream_init_from_data(&s, cached_data + bi->nDataPos, remaining);
        if (!block_deserialize(&blk, &s)) {
            block_free(&blk);
            continue;
        }

        for (size_t i = 0; i < blk.num_vtx; i++) {
            const struct transaction *tx = &blk.vtx[i];
            for (size_t j = 0; j < tx->num_shielded_output; j++) {
                incremental_tree_append(&tree,
                    &tx->v_shielded_output[j].cm);
                total_commitments++;
            }
        }

        bool is_checkpoint = ((h - sapling_height) % 100000 == 0 &&
                              h > sapling_height);
        if (is_checkpoint) {
            struct uint256 computed;
            incremental_tree_root(&tree, &computed);
            if (!sapling_header_root_known(bi)) {
                fail_reason = "checkpoint_missing_sapling_root";
                fail_height = h;
            } else if (memcmp(computed.data,
                       blk.header.hashFinalSaplingRoot.data, 32) != 0) {
                mismatches++;
                fail_reason = "checkpoint_sapling_root_mismatch";
                fail_height = h;
            }
        }

        block_free(&blk);

        if (fail_reason)
            goto fail;

        if (is_checkpoint) {
            LOG_WARN("sapling_tree_rebuild", "  sapling_tree_rebuild: h=%d/%d " "commitments=%d mismatches=%d", h, chain_tip, total_commitments, mismatches);
            fflush(stderr);

            struct byte_stream ts;
            stream_init(&ts, 4096);
            if (!incremental_tree_serialize(&tree, &ts)) {
                stream_free(&ts);
                fail_reason = "serialize_checkpoint_tree_failed";
                fail_height = h;
                goto fail;
            }
            enum sapling_persist_status ps =
                sapling_tree_persist_pair_status(ndb, ts.data, ts.size,
                                                 (int64_t)h);
            stream_free(&ts);
            if (ps == SAPLING_PERSIST_FAILED) {
                fail_reason = "persist_checkpoint_pair_failed";
                fail_height = h;
                goto fail;
            }
            /* DEFERRED: a foreign tx transiently owned the connection. Skip
             * persisting THIS checkpoint — the tree stays in memory and the
             * next checkpoint (or the final persist) records it. Not a
             * failure; the fold keeps making progress. */
            if (ps == SAPLING_PERSIST_DEFERRED)
                LOG_INFO("sapling_tree_rebuild",
                        "checkpoint persist deferred at h=%d (busy)", h);
        }
    }

    if (cached_data) {
        munmap(cached_data, cached_size);
        cached_data = NULL;
        cached_size = 0;
    }

    /* Verify against the RESOLVED endpoint (the coins-applied frontier, or the
     * header tip when no coins frontier exists), not active_chain_tip() which
     * is always the header tip — using the header tip on a wedged node would
     * compare the rebuilt tree against a block above the applied frontier whose
     * hashFinalSaplingRoot may be absent and FATAL on `tip_missing_sapling_root`
     * before the fold runs. When the frontier is absent, active_chain_at(chain,
     * chain_tip) == active_chain_tip(chain), so the legacy path is unchanged. */
    const struct block_index *tip = active_chain_at(chain, chain_tip);
    struct uint256 final_root;
    incremental_tree_root(&tree, &final_root);
    bool tip_root_known = sapling_header_root_known(tip);
    bool match = tip_root_known && memcmp(final_root.data,
                               tip->hashFinalSaplingRoot.data, 32) == 0;

    char root_hex[65];
    uint256_get_hex(&final_root, root_hex);
    int64_t replay_ms = GetTimeMillis() - t_replay_start;
    int replayed_blocks = (chain_tip >= start_height)
                          ? (chain_tip - start_height + 1)
                          : 0;
    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: replayed %d blocks in %lld ms", replayed_blocks, (long long)replay_ms);
    LOG_WARN("sapling_tree_rebuild", "sapling_tree_rebuild: DONE commitments=%d " "mismatches=%d root=%s match=%s", total_commitments, mismatches, root_hex, match ? "YES" : "NO");
    fflush(stderr);

    if (!match) {
        fail_reason = tip_root_known ? "tip_sapling_root_mismatch"
                                     : "tip_missing_sapling_root";
        fail_height = tip ? tip->nHeight : -1;
        goto fail;
    }

    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        if (!incremental_tree_serialize(&tree, &ts)) {
            stream_free(&ts);
            fail_reason = "serialize_final_tree_failed";
            fail_height = chain_tip;
            goto fail;
        }
        /* The final persist MUST durably land the rebuilt tree. A DEFERRED
         * result means the reducer's batch tx transiently owns the connection;
         * the reducer commits on a seconds timescale, so a bounded wait-then-
         * retry (yielding this background thread) lets the persist land without
         * ever nesting a BEGIN. Exhaustion is handled below. */
        enum sapling_persist_status ps = SAPLING_PERSIST_DEFERRED;
        for (int a = 1; a <= SAPLING_TREE_FINAL_PERSIST_ATTEMPTS &&
                        ps == SAPLING_PERSIST_DEFERRED; a++) {
            ps = sapling_tree_persist_pair_status(ndb, ts.data, ts.size,
                                                  (int64_t)chain_tip);
            if (ps == SAPLING_PERSIST_DEFERRED &&
                a < SAPLING_TREE_FINAL_PERSIST_ATTEMPTS) {
                int64_t backoff_ms =
                    (int64_t)SAPLING_TREE_FINAL_PERSIST_BACKOFF_MS * a;
                struct timespec bt = {
                    .tv_sec = backoff_ms / 1000,
                    .tv_nsec = (backoff_ms % 1000) * 1000000L,
                };
                nanosleep(&bt, NULL);
            }
        }
        stream_free(&ts);
        if (ps == SAPLING_PERSIST_DEFERRED) {
            /* Stayed busy past the budget — a transient lock/timing condition,
             * NOT a derived-state disagreement: name the persist_busy TRANSIENT
             * blocker (self-clears on the next successful rebuild) and return
             * the soft-failure code (rc<0) the deferred worker reads as "leave
             * the tree stale, retry next pass" — deliberately NOT the
             * fail_closed path (reserved for real root/state mismatches).
             * Complete the child first so a clean give-up is not a hung stall. */
            char reason[BLOCKER_REASON_MAX];
            snprintf(reason, sizeof(reason),
                    "final persist deferred height=%lld: connection stayed in "
                    "a foreign open transaction past %d attempts",
                    (long long)chain_tip,
                    SAPLING_TREE_FINAL_PERSIST_ATTEMPTS);
            struct blocker_record rec;
            if (blocker_init(&rec, "sapling_tree_rebuild.persist_busy",
                             "sync.sapling_tree_rebuild",
                             BLOCKER_TRANSIENT, reason))
                blocker_set(&rec);
            LOG_ERROR("sapling_tree_rebuild", "%s", reason);
            if (sup_id != SUPERVISOR_INVALID_ID)
                supervisor_child_complete(sup_id);
            return -1; // raw-return-ok:logged_above_and_persist_busy_blocker_set
        }
        if (ps == SAPLING_PERSIST_FAILED) {
            fail_reason = "persist_final_pair_failed";
            fail_height = chain_tip;
            goto fail;
        }
    }

    (void)tree;
    if (sup_id != SUPERVISOR_INVALID_ID) {
        supervisor_progress(sup_id, total_commitments);
        supervisor_tick(sup_id);
        supervisor_child_complete(sup_id);
    }
    blocker_clear("sapling_tree_rebuild.fail_closed");
    blocker_clear("sapling_tree_rebuild.persist_busy");
    return total_commitments;

fail:
    if (cached_data) munmap(cached_data, cached_size);
    if (sup_id != SUPERVISOR_INVALID_ID)
        supervisor_child_complete(sup_id);
    sapling_tree_rebuild_raise_fail_blocker(fail_reason, fail_height,
                                            total_commitments, mismatches);
    LOG_ERR("sapling_tree_rebuild",
            "sapling_tree_rebuild: fail-closed reason=%s height=%d "
            "commitments=%d mismatches=%d",
            fail_reason ? fail_reason : "unknown", fail_height,
            total_commitments, mismatches);
}

/* ── Deferred/live background rebuild ─────────────────────────────────
 * config/src/boot.c's "Sapling tree root MISMATCH ... deferring live
 * rebuild until after boot" branch used to only set
 * g_sapling_tree_rebuilding=true and fall through into a normal boot —
 * nothing ever actually ran the rebuild afterward (a dead deferral: the
 * flag stayed true forever, so every future root-mismatch check silently
 * self-suppressed instead of ever curing the tree). This runs the SAME
 * rebuild-then-reload sequence the synchronous boot-time path runs, off
 * a background thread so a multi-million-block replay never blocks node
 * startup. Living in this TU means the thread_registry_spawn call site
 * below is automatically covered by this file's supervisor_register_
 * in_domain() call above (Gate #23) — sapling_tree_rebuild() registers
 * "sync.sapling_tree_rebuild" on entry, so a stuck deferred run is a
 * named supervisor stall, never a silent one. */
struct sapling_tree_deferred_args {
    struct node_db *ndb;
    struct active_chain *chain;
    const char *datadir;
    struct main_state *ms;
};

static void *sapling_tree_rebuild_deferred_thread(void *arg)
{
    struct sapling_tree_deferred_args *a = arg;
    struct node_db *ndb = a->ndb;
    struct active_chain *chain = a->chain;
    const char *datadir = a->datadir;
    struct main_state *ms = a->ms;
    free(a);

    size_t old_size = incremental_tree_size(&ms->sapling_tree);
    LOG_INFO("sapling_tree_rebuild",
            "deferred rebuild: starting background replay "
            "(pre-rebuild size=%zu)", old_size);
    atomic_store(&g_sapling_tree_rebuilding, true);
    int n = sapling_tree_rebuild(ndb, chain, datadir);
    if (n >= 0) {
        uint8_t tbuf[8192];
        size_t tlen = 0;
        if (node_db_state_get(ndb, "sapling_tree", tbuf, sizeof(tbuf),
                              &tlen) && tlen > 0) {
            struct byte_stream ts2;
            stream_init_from_data(&ts2, tbuf, tlen);
            sapling_tree_init(&ms->sapling_tree);
            incremental_tree_deserialize(&ms->sapling_tree, &ts2);
            set_sapling_tree_for_flush(&ms->sapling_tree);
            LOG_WARN("sapling_tree_rebuild",
                    "deferred rebuild: DONE %d commitments (was %zu)",
                    n, old_size);
        }
    } else {
        LOG_WARN("sapling_tree_rebuild",
                "deferred rebuild: FAILED (rc=%d) — tree left at "
                "pre-rebuild size=%zu; sapling_tree_rebuild already "
                "raised a named blocker (see `zclassic23 dumpstate "
                "blocker`)", n, old_size);
    }
    atomic_store(&g_sapling_tree_rebuilding, false);
    node_db_wal_checkpoint(ndb);
    if (datadir)
        save_block_index_flat(datadir, ms);
    thread_registry_unregister_self();
    return NULL;
}

/* Kick off the deferred/live rebuild as a background thread. Called from
 * boot when the synchronous inline rebuild would be too slow to run
 * before P2P/RPC starts. On spawn failure the tree simply stays stale
 * (as it already was) until an operator runs `rebuildsaplingtree`. */
void sapling_tree_rebuild_start_deferred(struct node_db *ndb,
                                         struct active_chain *chain,
                                         const char *datadir,
                                         struct main_state *ms)
{
    atomic_store(&g_sapling_tree_rebuilding, true);
    struct sapling_tree_deferred_args *args =
        zcl_malloc(sizeof(*args), "sapling_tree_deferred_args");
    if (!args) {
        LOG_WARN("sapling_tree_rebuild",
                "start_deferred: alloc failed — tree stays stale until "
                "an operator runs `rebuildsaplingtree`");
        atomic_store(&g_sapling_tree_rebuilding, false);
        return;
    }
    args->ndb = ndb;
    args->chain = chain;
    args->datadir = datadir;
    args->ms = ms;
    if (thread_registry_spawn("zcl_sapling_defer",
            sapling_tree_rebuild_deferred_thread, args, NULL) != 0) {
        LOG_WARN("sapling_tree_rebuild",
                "start_deferred: thread spawn failed — tree stays stale "
                "until an operator runs `rebuildsaplingtree`");
        free(args);
        atomic_store(&g_sapling_tree_rebuilding, false);
    }
}
