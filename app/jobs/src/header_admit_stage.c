/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * header_admit_stage — implementation. See jobs/header_admit_stage.h.
 *
 * Single-process singleton. The F-2 stage primitive does all the
 * cursor / replay heavy lifting; this module is just the step body and
 * the schema-bootstrap glue for the `header_admit_log` table that lives
 * in progress.kv alongside `stage_cursor`. */

#include "jobs/header_admit_stage.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "jobs/stage_helpers.h"
#include "jobs/mint_fold_ceiling.h"
#include "models/header_admit_log.h"
#include "platform/time_compat.h"
#include "services/header_admit_inbox.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/event_log_singleton.h"
#include "jobs/block_header_emit.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/accept_block_header.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "primitives/block.h"

#include <pthread.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAGE_NAME       "header_admit"

/* Cap the reorg-rewind backward scan. Normal reorgs are 1-6 blocks; a
 * deep stale divergence must not pin the CPU walking back across 3.1M
 * heights every supervisor tick. */
#define HEADER_ADMIT_REORG_REWIND_MAX_DEPTH  10000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static _Atomic uint64_t g_admitted_total = 0;
static _Atomic uint64_t g_inbox_drained_total = 0;
static _Atomic uint64_t g_inbox_logged_total = 0;
static _Atomic uint64_t g_reorg_rewind_total = 0;
static _Atomic uint64_t g_header_event_emit_total = 0;
static _Atomic uint64_t g_header_event_emit_fail_total = 0;
/* Reducer producer path: count of block_index entries CREATED by the
 * stage from raw header bytes (add_to_block_index). */
static _Atomic uint64_t g_produced_total = 0;
static _Atomic int64_t  g_last_admit_height = -1;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
#ifdef ZCL_TESTING
static header_admit_authoritative_hook g_authoritative_hook = NULL;
static void *g_authoritative_hook_user = NULL;
#endif

MAILBOX_DEFINE(header_admit, struct header_admit_msg,
               HEADER_ADMIT_INBOX_CAPACITY)

/* ── Pending raw-header staging (reducer producer path) ──────────────
 *
 * The inbox drain copies any message carrying raw header bytes
 * (m->has_header) into this small bounded ring, keyed by hash. step_admit
 * consults it when the active chain has no block at the height it needs
 * and then CREATES the block_index entry via add_to_block_index — letting
 * the reducer extend the chain without the legacy accept_block_header.
 *
 * Touched only on the stage's single step thread (drain + step both run
 * inside header_admit_stage_step_once), so no lock is needed. Hash-hint
 * pushers never set has_header, so they do not enter the producer path. */
#define HEADER_ADMIT_PENDING_CAP 64

struct pending_header {
    struct uint256       hash;
    struct block_header  header;
    bool                 occupied;
};

static struct pending_header g_pending[HEADER_ADMIT_PENDING_CAP];

/* Stage a raw header for later production. Overwrites the oldest slot on
 * collision (simple ring); idempotent on identical hash. */
static void pending_header_stage(const struct block_header *h)
{
    struct uint256 hash;
    block_header_get_hash(h, &hash);

    int free_slot = -1;
    for (int i = 0; i < HEADER_ADMIT_PENDING_CAP; i++) {
        if (g_pending[i].occupied) {
            if (memcmp(g_pending[i].hash.data, hash.data, 32) == 0)
                return;  /* already staged */
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    static int rr = 0;
    int slot = (free_slot >= 0) ? free_slot
                                : (rr = (rr + 1) % HEADER_ADMIT_PENDING_CAP);
    g_pending[slot].hash     = hash;
    g_pending[slot].header   = *h;
    g_pending[slot].occupied = true;
}

/* Find a staged raw header whose parent is already in the block_index and
 * would land at `want_height`. Returns the slot's header pointer, or NULL.
 * Consumes (frees) the slot on a hit. */
static const struct block_header *pending_header_take(struct main_state *ms,
                                                      int want_height)
{
    for (int i = 0; i < HEADER_ADMIT_PENDING_CAP; i++) {
        if (!g_pending[i].occupied)
            continue;
        const struct block_header *h = &g_pending[i].header;
        int produced_height;
        if (want_height == 0) {
            produced_height = 0;  /* genesis: no parent */
        } else {
            struct block_index *pprev = block_map_find(&ms->map_block_index,
                                                       &h->hashPrevBlock);
            if (!pprev)
                continue;
            produced_height = pprev->nHeight + 1;
        }
        if (produced_height != want_height)
            continue;
        g_pending[i].occupied = false;
        return h;
    }
    return NULL;
}

/* ── Step body ─────────────────────────────────────────────────────── */

/* Write one header_admit_log row through the AR lifecycle (Law 2: the
 * HeaderAdmitLog model is the only writer of its table). `db` is the
 * progress.kv handle. */
static bool log_insert(sqlite3 *db, int height,
                        const struct uint256 *hash,
                        const struct uint256 *parent_hash)
{
    struct db_header_admit_log row = {
        .height      = (int64_t)height,
        .has_parent  = (parent_hash != NULL),
        .admitted_at = platform_time_wall_unix(),
    };
    memcpy(row.hash, hash->data, 32);
    if (parent_hash)
        memcpy(row.parent_hash, parent_hash->data, 32);

    if (!db_header_admit_log_save(db, &row)) {
        LOG_WARN("header_admit", "[header_admit] log save height=%d failed", height);
        return false;
    }
    return true;
}

static void handle_header_admit_msg(const struct header_admit_msg *m)
{
    if (!m) return;

    atomic_fetch_add(&g_inbox_drained_total, 1);

    /* L1: stage carried header BEFORE the height<0 guard. Self-mined /
     * submitblock / rebuild pushes carry height=-1 (no peer height hint);
     * dropping them here starved the producer path so a regtest-mined block
     * never created a block_index. Network-safe: step_admit only produces when
     * active_chain_at(next_h) is NULL, never true for an existing network
     * block. */
    if (m->has_header)
        pending_header_stage(&m->header);

    struct main_state *ms = g_ms;
    sqlite3 *db = progress_store_db();
    if (!ms || !db || m->height < 0 || m->height > INT32_MAX)
        return;

    struct block_index *bi = active_chain_at(&ms->chain_active,
                                             (int)m->height);
    if (!bi || !bi->phashBlock)
        return;

    if (memcmp(bi->phashBlock->data, m->hash.data, 32) != 0) {
        LOG_WARN("header_admit", "[header_admit] inbox hash mismatch height=%lld peer=%u", (long long)m->height, m->peer_id);
        return;
    }

    const struct uint256 *parent_hash = NULL;
    if (m->height > 0) {
        if (!bi->pprev || !bi->pprev->phashBlock)
            return;
        parent_hash = bi->pprev->phashBlock;
    }

    progress_store_tx_lock();
    bool logged = log_insert(db, (int)m->height, bi->phashBlock, parent_hash);
    progress_store_tx_unlock();
    if (logged)
        atomic_fetch_add(&g_inbox_logged_total, 1);
}

static bool authoritative_admit(struct main_state *ms, struct block_index *bi)
{
#ifdef ZCL_TESTING
    if (g_authoritative_hook)
        return g_authoritative_hook(ms, bi, g_authoritative_hook_user);
#endif

    if (!ms || !bi || !bi->phashBlock)
        return false;

    struct block_index *mapped =
        block_map_find(&ms->map_block_index, bi->phashBlock);
    if (mapped && mapped != bi)
        return false;

    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
        bi->nStatus = (bi->nStatus & ~BLOCK_VALID_MASK) |
                      BLOCK_VALID_TREE;
    return true;
}


/* ── Reorg-rewind (mirrors tip_finalize_stage.c rewind) ─────────────── */

/* Read the header_admit_log row hash at `height` and compare it to the
 * active chain's hash at that height. `out_known` is false (a no-op) when
 * the log has no row there OR the chain has no block there; otherwise
 * `out_matches` reflects whether the bytes are equal. Returns false only
 * on a SQL-prepare failure (treated as fatal by the caller). Reuses the
 * SELECT-hash pattern of header_admit_stage_has_record (above). */
static bool log_row_active_match(sqlite3 *db, int height,
                                 bool *out_known, bool *out_matches)
{
    *out_known   = false;
    *out_matches = false;
    if (!db || height < 0)
        return true;

    struct main_state *ms = g_ms;
    struct block_index *bi =
        ms ? active_chain_at(&ms->chain_active, height) : NULL;
    if (!bi || !bi->phashBlock)
        return true;  /* chain has no block here → no-op (out_known=false) */

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hash FROM header_admit_log WHERE height=?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("header_admit", "[header_admit] rewind prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        const void *blob = sqlite3_column_blob(st, 0);
        int nb = sqlite3_column_bytes(st, 0);
        if (blob && nb == 32) {
            *out_known   = true;
            *out_matches = (memcmp(blob, bi->phashBlock->data, 32) == 0);
        }
    }
    sqlite3_finalize(st);
    return true;
}

/* Detect a reorg below the cursor and rewind to the fork point so the
 * stale rows get re-admitted (INSERT OR REPLACE) with the canonical
 * hashes on the forward re-walk.
 *
 * Unlike tip_finalize — whose reorg always touches the tip — a
 * header_admit divergence can sit BELOW a matching tip (the live case:
 * cursor=3129674, tip rows at 3129672/3129673 match, but 3129671 holds a
 * stale pre-reorg hash). A tip-only `cursor-1` check would miss it. So we
 * scan the recent window [cursor-2 .. floor] (capped) and rewind to the
 * DEEPEST divergent height found — re-admitting it and everything above.
 *
 * The scan is bounded by HEADER_ADMIT_REORG_REWIND_MAX_DEPTH so a deep
 * stale fork can't pin the CPU. A height whose log row matches, or whose
 * chain has no block (out_known=false — the LOG_AHEAD shrink case), is a
 * no-op and never triggers a rewind.
 *
 * This touches only the stage cursor and the header_admit_log. Returns false
 * (→ JOB_FATAL) only on a SQL/persist failure. */
static bool rewind_cursor_if_active_chain_reorged(sqlite3 *db)
{
    if (!g_stage || !g_ms)
        return true;

    uint64_t cursor = stage_cursor_persisted(db, STAGE_NAME, STAGE_NAME);
    if (cursor == 0)
        return true;
    if (cursor > (uint64_t)INT32_MAX) {
        LOG_WARN("header_admit", "[header_admit] reorg rewind cursor too large: %llu", (unsigned long long)cursor);
        return false;
    }

    /* Scan the recent window below the cursor for the deepest height
     * whose logged hash no longer matches the active chain. */
    int floor_h = (int)cursor - HEADER_ADMIT_REORG_REWIND_MAX_DEPTH;
    if (floor_h < 0) floor_h = 0;
    int deepest_divergent = -1;
    for (int h = (int)cursor - 1; h >= floor_h; h--) {
        bool known = false, matches = false;
        if (!log_row_active_match(db, h, &known, &matches))
            return false;
        if (known && !matches)
            deepest_divergent = h;  /* keep going: find the lowest one */
    }
    if (deepest_divergent < 0)
        return true;  /* recent window is consistent → no rewind */

    if (deepest_divergent == floor_h && floor_h > 0)
        LOG_WARN("header_admit", "[header_admit] reorg rewind cap hit (depth=%d): divergence may extend below floor=%d", HEADER_ADMIT_REORG_REWIND_MAX_DEPTH, floor_h);

    uint64_t rewind_to = (uint64_t)deepest_divergent;
    if (rewind_to >= cursor)
        return true;

    if (!stage_set_cursor(g_stage, db, rewind_to)) {
        LOG_WARN("header_admit", "[header_admit] reorg rewind failed from=%llu to=%llu", (unsigned long long)cursor, (unsigned long long)rewind_to);
        return false;
    }

    atomic_fetch_add(&g_reorg_rewind_total, 1);
    atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
    event_emitf(EV_REORG_START, 0,
                "header_admit reorg_cursor_rewind from=%llu to=%llu",
                (unsigned long long)cursor,
                (unsigned long long)rewind_to);
    return true;
}

/* Reducer producer: CREATE the block_index entry for `want_height` from a
 * staged raw header (add_to_block_index). Returns the
 * created (or already-mapped) entry, or NULL when no staged header lands
 * at this height (caller IDLEs).
 * Does not touch coins.db, disk, or the active-chain tip — it only adds
 * to ms->map_block_index, exactly as legacy accept_block_header would. */
static struct block_index *produce_block_index(struct main_state *ms,
                                               int want_height)
{
    const struct block_header *h = pending_header_take(ms, want_height);
    if (!h)
        return NULL;

    struct block_index *bi = add_to_block_index(ms, h);
    if (!bi || !bi->phashBlock) {
        LOG_WARN("header_admit", "[header_admit] produce add_to_block_index failed height=%d", want_height);
        return NULL;
    }
    atomic_fetch_add(&g_produced_total, 1);
    return bi;
}

static job_result_t step_admit(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    reducer_extend_window_to_candidate(ms, true);

    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    /* ANCHOR-SET MINT bound: the `-mint-anchor` driver caps the fold at the
     * SHA3 checkpoint anchor so the resulting coins_kv set IS the anchor set.
     * Default ceiling is MINT_FOLD_NO_CEILING (INT32_MAX) → never fires on a
     * normal boot (no clamp applied). Refusing to admit above the ceiling makes
     * the whole pipeline converge AT it (every downstream stage gates on this
     * cursor). */
    if (next_h > mint_fold_ceiling_get())
        return JOB_IDLE;

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !bi->phashBlock) {
        /* Reducer producer path: the active chain has no block here yet.
         * If a raw header for this height was staged via the inbox, CREATE
         * the block_index entry so the reducer can extend the chain without
         * the legacy accept_block_header. */
        bi = produce_block_index(ms, next_h);
        if (!bi || !bi->phashBlock) return JOB_IDLE;
    }

    const struct uint256 *parent_hash = NULL;
    if (next_h > 0) {
        if (!bi->pprev || !bi->pprev->phashBlock) {
            blocker_init(&c->blocker, "header_admit",
                          "missing_parent",
                          BLOCKER_PERMANENT,
                          "block_index entry has no pprev linkage");
            atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
            return JOB_BLOCKED;
        }
        parent_hash = bi->pprev->phashBlock;
    }

    if (!authoritative_admit(ms, bi)) {
        LOG_WARN("header_admit",
                 "[header_admit] authoritative admit failed height=%d",
                 next_h);
        return JOB_FATAL;
    }

    /* L2.5: advance the best-header frontier to this admitted block when it is
     * the most-WORK header. The network path does this in msg_headers.c; the
     * reducer PRODUCER path (self-mined / submitblock / rebuild) has no other
     * writer of pindex_best_header, and the downstream stages resolve a block
     * above the finalized window ONLY via pindex_best_header (vh_resolve_bi),
     * so without this the pipeline stalls. Chainwork-ranked, advance-only;
     * idempotent for the network path (best_header already at/past bi). */
    {
        struct block_index *cb = ms->pindex_best_header;
        bool adv = (cb == NULL);
        if (cb && bi != cb) {
            if (!arith_uint256_is_zero(&bi->nChainWork) &&
                !arith_uint256_is_zero(&cb->nChainWork))
                adv = arith_uint256_compare(&bi->nChainWork, &cb->nChainWork) > 0;
            else
                adv = bi->nHeight > cb->nHeight;
        }
        if (adv) ms->pindex_best_header = bi;
    }

    /* Idempotent block-index projection update; best-effort, never fatal.
     * Emit after the VALID_TREE promotion so the persisted nStatus reflects
     * it. */
    block_index_emit_header_event(bi, "header_admit",
                                  &g_header_event_emit_total,
                                  &g_header_event_emit_fail_total);

    if (!log_insert(db, next_h, bi->phashBlock, parent_hash))
        return JOB_FATAL;

    c->cursor_out = c->cursor_in + 1;
    atomic_fetch_add(&g_admitted_total, 1);
    atomic_store(&g_last_admit_height, (int64_t)next_h);
    return JOB_ADVANCED;
}

/* ── Public API ────────────────────────────────────────────────────── */

#ifdef ZCL_TESTING
void header_admit_stage_set_authoritative_hook(
    header_admit_authoritative_hook hook,
    void *user)
{
    g_authoritative_hook = hook;
    g_authoritative_hook_user = user;
}
#endif

bool header_admit_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("header_admit", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("header_admit",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);

    /* Idempotent: same ms, already initialised → success. */
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("header_admit",
                "init: already bound to a different main_state");
        return true;
    }

    if (!db_header_admit_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_admit, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("header_admit", "init: stage_create failed");
    }
    uint64_t persisted_cursor = stage_cursor_persisted(db, STAGE_NAME,
                                                       STAGE_NAME);
    if (!stage_set_cursor(s, db, persisted_cursor)) {
        stage_destroy(s);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("header_admit",
             "[header_admit] stage initialised (authoritative)");
    return true;
}

job_result_t header_admit_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    progress_store_tx_lock();
    bool rewind_ok = rewind_cursor_if_active_chain_reorged(db);
    if (!rewind_ok) {
        progress_store_tx_unlock();
        LOG_RETURN(JOB_FATAL, "header_admit",
                   "FATAL: active-chain reorg rewind failed");
    }
    (void)mailbox_header_admit_drain(handle_header_admit_msg);
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    return r;
}

STAGE_DRAIN_IMPL(header_admit)

void header_admit_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    /* Reset per-init observability state. The persisted cursor in
     * progress.kv is preserved (that's the whole point of the saga);
     * what we reset is what would be misleading across re-inits —
     * lifetime counters that should restart with each bind. */
    atomic_store(&g_admitted_total, (uint64_t)0);
    atomic_store(&g_inbox_drained_total, (uint64_t)0);
    atomic_store(&g_inbox_logged_total, (uint64_t)0);
    atomic_store(&g_reorg_rewind_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_fail_total, (uint64_t)0);
    atomic_store(&g_produced_total, (uint64_t)0);
    atomic_store(&g_last_admit_height, (int64_t)-1);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    memset(g_pending, 0, sizeof(g_pending));
#ifdef ZCL_TESTING
    g_authoritative_hook = NULL;
    g_authoritative_hook_user = NULL;
#endif
    pthread_mutex_unlock(&g_lock);
}

uint64_t header_admit_stage_cursor(void)
{
    if (!g_stage)
        return 0;
    return stage_cursor(g_stage);
}

uint64_t header_admit_stage_admitted_total(void)
{
    return atomic_load(&g_admitted_total);
}

uint64_t header_admit_stage_reorg_rewind_total(void)
{
    return atomic_load(&g_reorg_rewind_total);
}

uint64_t header_admit_stage_produced_total(void)
{
    return atomic_load(&g_produced_total);
}

bool header_admit_stage_has_record(int32_t height,
                                   const struct uint256 *hash)
{
    if (height < 0 || !hash)
        return false;

    sqlite3 *db = progress_store_db();
    if (!db)
        return false;

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM header_admit_log WHERE height=?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        progress_store_tx_unlock();
        return false;
    }

    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        const void *blob = sqlite3_column_blob(st, 0);
        int nb = sqlite3_column_bytes(st, 0);
        found = (blob && nb == 32 &&
                 memcmp(blob, hash->data, 32) == 0);
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return found;
}

bool header_admit_stage_dump_state_json(struct json_value *out,
                                         const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    stage_dump_header(out, STAGE_NAME, g_stage);
    json_push_kv_int (out, "admitted_total",
                      (int64_t)atomic_load(&g_admitted_total));
    json_push_kv_int (out, "inbox_drained_total",
                      (int64_t)atomic_load(&g_inbox_drained_total));
    json_push_kv_int (out, "inbox_logged_total",
                      (int64_t)atomic_load(&g_inbox_logged_total));
    json_push_kv_int (out, "reorg_rewind_total",
                      (int64_t)atomic_load(&g_reorg_rewind_total));
    json_push_kv_int (out, "header_event_emit_total",
                      (int64_t)atomic_load(&g_header_event_emit_total));
    json_push_kv_int (out, "header_event_emit_fail_total",
                      (int64_t)atomic_load(&g_header_event_emit_fail_total));
    json_push_kv_int (out, "produced_total",
                      (int64_t)atomic_load(&g_produced_total));
    json_push_kv_int (out, "last_admit_height",
                      atomic_load(&g_last_admit_height));
    json_push_kv_int (out, "last_step_unix",
                      atomic_load(&g_last_step_unix));
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_str(out, "authority", "authoritative");
    stage_dump_counters(out, g_stage);
    return true;
}
