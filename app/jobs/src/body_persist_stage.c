/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_persist_stage — implementation. See jobs/body_persist_stage.h.
 *
 * Consumes body_fetch_log and verifies that bodies observed on disk are
 * readable, hash to the active-chain header, and merkle-reconstruct to the
 * admitted header's root. It writes body_persist_log plus its stage cursor in
 * progress.kv, and emits verified bodies into the append-only event log.
 * READ-class failures are never persisted as verdicts: the stage clears
 * BLOCK_HAVE_DATA and holds the cursor until the body is re-fetched (see
 * requeue_body_for_refetch). */

#include "platform/time_compat.h"
#include "jobs/body_persist_stage.h"
#include "jobs/created_outputs_index.h"
#include "jobs/stage_helpers.h"
#include "body_persist_log_store.h"

#include "bloom/merkle.h"
#include "chain/chain.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "json/json.h"
#include "primitives/block.h"
#include "storage/disk_block_io.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/event_log_singleton.h"
#include "jobs/block_header_emit.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE_NAME "body_persist"

/* struct body_fetch_row + the body_persist_log schema/read/write helpers
 * live in body_persist_log_store.c (pure sqlite kernel helpers below the AR
 * layer). */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static body_persist_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;

static _Atomic uint64_t g_verified_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_read_failed_total = 0;
static _Atomic uint64_t g_header_mismatch_total = 0;
static _Atomic uint64_t g_merkle_mismatch_total = 0;
static _Atomic uint64_t g_body_emit_total = 0;
static _Atomic uint64_t g_body_emit_fail_total = 0;
static _Atomic uint64_t g_header_event_emit_total = 0;
static _Atomic uint64_t g_header_event_emit_fail_total = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;

static bool verify_merkle_root(const struct block *blk)
{
    if (!blk) return false;
    if (blk->num_vtx == 0)
        return uint256_is_null(&blk->header.hashMerkleRoot);

    struct uint256 *txids = malloc(blk->num_vtx * sizeof(struct uint256)); // raw-alloc-ok:bounded-temporary
    if (!txids) return false;
    for (size_t i = 0; i < blk->num_vtx; i++)
        txids[i] = blk->vtx[i].hash;
    struct uint256 root = compute_merkle_root(txids, blk->num_vtx);
    free(txids);
    return uint256_eq(&root, &blk->header.hashMerkleRoot);
}

/* Emit the validated block body into the append-only event log.
 * Best-effort event emission: any failure (log not wired, serialize/append
 * error) is counted and logged, never propagated to the stage result. The body
 * bytes are the canonical block_serialize() wire form, so a replay consumer
 * round-trips them via block_deserialize(). */
static void emit_block_body_event(const struct block *blk,
                                  const struct uint256 *hash,
                                  int height)
{
    event_log_t *log = event_log_singleton();
    if (!log) {
        /* Not wired yet (early boot, or unit tests that don't open the
         * singleton). Not a hard failure — the projection catches up. */
        return;
    }

    struct byte_stream s;
    stream_init(&s, 1024);
    if (!block_serialize(blk, &s)) {
        stream_free(&s);
        LOG_WARN("body_persist",
                 "[body_persist] event emit: block_serialize failed h=%d",
                 height);
        atomic_fetch_add_explicit(&g_body_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }

    if (s.size > EV_BLOCK_BODY_MAX_BODY) {
        LOG_WARN("body_persist",
                 "[body_persist] event emit: body %zu > max %u h=%d",
                 s.size, (unsigned)EV_BLOCK_BODY_MAX_BODY, height);
        stream_free(&s);
        atomic_fetch_add_explicit(&g_body_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }

    struct ev_block_body b;
    memset(&b, 0, sizeof(b));
    memcpy(b.hash, hash->data, 32);
    b.height   = height;
    b.body_len = (uint32_t)s.size;

    size_t cap = ev_block_body_wire_size(b.body_len);
    uint8_t *buf = zcl_malloc(cap, "body_persist/emit_body");
    if (!buf) {
        stream_free(&s);
        atomic_fetch_add_explicit(&g_body_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }

    size_t written = 0;
    bool ok = ev_block_body_serialize(&b, s.data, buf, cap, &written);
    stream_free(&s);
    if (!ok) {
        free(buf);
        LOG_WARN("body_persist",
                 "[body_persist] event emit: serialize failed h=%d",
                 height);
        atomic_fetch_add_explicit(&g_body_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }

    uint64_t off = event_log_append(log, EV_BLOCK_BODY, buf, written);
    free(buf);
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_body_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&g_body_emit_total, 1, memory_order_relaxed);
}

/* READ-class failure discipline (mirrors proof_validate's reader handling
 * and chain_restore_repair's HAVE_DATA drop): the stored body failed to
 * read, or read fine but does not belong to the hash-bound active-chain
 * header (wrong/corrupt/torn block on disk). A re-fetched body can change
 * every one of these verdicts, so never persist them as permanent ok=0
 * rows — those statuses sit outside every repair's status set and would
 * pin H*, utxo_apply and tip_finalize forever. Clear BLOCK_HAVE_DATA and
 * re-emit the header event (the projection persists the cleared bit across
 * restarts, same discipline as the success path), hold the cursor, and let
 * the normal !HAVE_DATA sync path re-download the body; this stage then
 * retries. While cleared, the HAVE_DATA gate above idles without re-reading,
 * so the counter advances once per requeue, not per step. Only
 * upstream_failed (a deterministic header-consensus verdict re-fetch cannot
 * change) remains a permanent row. */
static job_result_t requeue_body_for_refetch(struct block_index *bi,
                                             int height, const char *what,
                                             _Atomic uint64_t *counter)
{
    bi->nStatus &= ~BLOCK_HAVE_DATA;
    block_index_emit_header_event(bi, STAGE_NAME, &g_header_event_emit_total,
                                  &g_header_event_emit_fail_total);
    atomic_fetch_add(counter, 1);
    atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
    LOG_WARN(STAGE_NAME,
             "[body_persist] %s height=%d: cleared HAVE_DATA, holding cursor "
             "for body re-fetch", what, height);
    return JOB_IDLE;
}

static job_result_t step_persist(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t bf_cursor = stage_cursor_persisted(db, "body_fetch", STAGE_NAME);
    if ((uint64_t)next_h >= bf_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct body_fetch_row upstream;
    int found = body_persist_body_fetch_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        if (!body_persist_log_insert(db, next_h, "upstream_failed", false))
            return JOB_FATAL;
        atomic_fetch_add(&g_upstream_failed_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !bi->phashBlock || !(bi->nStatus & BLOCK_HAVE_DATA)) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct block blk;
    block_init(&blk);
    body_persist_reader_fn reader = g_reader ? g_reader
                                             : stage_default_block_reader;
    if (!reader(&blk, bi, g_datadir, g_reader_user)) {
        block_free(&blk);
        return requeue_body_for_refetch(bi, next_h, "read_failed",
                                        &g_read_failed_total);
    }

    struct uint256 disk_hash;
    block_get_hash(&blk, &disk_hash);
    if (uint256_cmp(&disk_hash, bi->phashBlock) != 0) {
        block_free(&blk);
        return requeue_body_for_refetch(bi, next_h, "header_mismatch",
                                        &g_header_mismatch_total);
    }

    if (!verify_merkle_root(&blk)) {
        block_free(&blk);
        return requeue_body_for_refetch(bi, next_h, "merkle_mismatch",
                                        &g_merkle_mismatch_total);
    }

    /* The body is read, hashes to its header, and merkle-checks; emit it into
     * the append-only log before freeing. */
    emit_block_body_event(&blk, &disk_hash, next_h);

    /* Index every output this block creates so script_validate (stage 5) can
     * resolve transparent prevouts without -txindex. body_persist (stage 4) is
     * strictly upstream, so the index is complete at/below the script_validate
     * frontier before it is needed (P0 §2.1). Load-bearing for validation:
     * a write failure is fatal, not silently skipped. */
    if (!created_outputs_index_put_block(db, &blk, next_h)) {
        block_free(&blk);
        return JOB_FATAL;
    }

    /* The body has landed on disk and round-tripped (hash + merkle verified),
     * so mark BLOCK_HAVE_DATA on the in-memory block_index entry. Then re-emit
     * EV_BLOCK_HEADER with updated nStatus so the projection persists the
     * HAVE_DATA bit (idempotent INSERT OR REPLACE keyed on hash). nTx rides
     * the same emit: an n_tx=0 row breaks the next boot's nChainTx propagation
     * exactly at this block. */
    bi->nStatus |= BLOCK_HAVE_DATA;
    if (bi->nTx == 0 && blk.num_vtx > 0)
        bi->nTx = (unsigned int)blk.num_vtx;
    block_index_emit_header_event(bi, "body_persist", &g_header_event_emit_total, &g_header_event_emit_fail_total);

    block_free(&blk);
    if (!body_persist_log_insert(db, next_h, "verified", true))
        return JOB_FATAL;
    atomic_fetch_add(&g_verified_total, 1);
    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

bool body_persist_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("body_persist", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("body_persist",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("body_persist",
                "init: already bound to a different main_state");
        return true;
    }

    if (!body_persist_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    /* The forward creation index this stage maintains for script_validate. */
    if (!created_outputs_index_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    GetDataDir(true, g_datadir, sizeof(g_datadir));

    stage_t *s = stage_create(STAGE_NAME, step_persist, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("body_persist", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("body_persist", "[body_persist] stage initialised");
    return true;
}

STAGE_STEP_ONCE_SIMPLE(body_persist)

STAGE_DRAIN_IMPL(body_persist)

void body_persist_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_datadir[0] = '\0';
    g_reader = NULL;
    g_reader_user = NULL;
    atomic_store(&g_verified_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_read_failed_total, (uint64_t)0);
    atomic_store(&g_header_mismatch_total, (uint64_t)0);
    atomic_store(&g_merkle_mismatch_total, (uint64_t)0);
    atomic_store(&g_body_emit_total, (uint64_t)0);
    atomic_store(&g_body_emit_fail_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_fail_total, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    pthread_mutex_unlock(&g_lock);
}

void body_persist_stage_set_reader(body_persist_reader_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_reader = fn;
    g_reader_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t body_persist_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t body_persist_stage_verified_total(void)
{
    return atomic_load(&g_verified_total);
}

uint64_t body_persist_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t body_persist_stage_read_failed_total(void)
{
    return atomic_load(&g_read_failed_total);
}

uint64_t body_persist_stage_header_mismatch_total(void)
{
    return atomic_load(&g_header_mismatch_total);
}

uint64_t body_persist_stage_merkle_mismatch_total(void)
{
    return atomic_load(&g_merkle_mismatch_total);
}

uint64_t body_persist_stage_body_emit_total(void)
{
    return atomic_load(&g_body_emit_total);
}

uint64_t body_persist_stage_body_emit_fail_total(void)
{
    return atomic_load(&g_body_emit_fail_total);
}

bool body_persist_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = atomic_load(&g_last_step_unix);

    stage_dump_header(out, STAGE_NAME, g_stage);
    json_push_kv_int (out, "verified_total",
                      (int64_t)atomic_load(&g_verified_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "read_failed_total",
                      (int64_t)atomic_load(&g_read_failed_total));
    json_push_kv_int (out, "header_mismatch_total",
                      (int64_t)atomic_load(&g_header_mismatch_total));
    json_push_kv_int (out, "merkle_mismatch_total",
                      (int64_t)atomic_load(&g_merkle_mismatch_total));
    json_push_kv_int (out, "body_emit_total",
                      (int64_t)atomic_load(&g_body_emit_total));
    json_push_kv_int (out, "body_emit_fail_total",
                      (int64_t)atomic_load(&g_body_emit_fail_total));
    json_push_kv_int (out, "header_event_emit_total",
                      (int64_t)atomic_load(&g_header_event_emit_total));
    json_push_kv_int (out, "header_event_emit_fail_total",
                      (int64_t)atomic_load(&g_header_event_emit_fail_total));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds",
                      last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "log_rows",
                      db ? stage_log_row_count(db, STAGE_NAME,
                                               "body_persist_log") : 0);
    stage_dump_counters(out, g_stage);
    return true;
}
