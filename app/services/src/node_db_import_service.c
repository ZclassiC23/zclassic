/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* node_db_import_service: parallel LevelDB → SQLite UTXO import.
 *
 * Reader thread feeds a ring buffer; N decoder threads deserialize
 * coins-format entries; a single writer thread bulk-inserts into the
 * utxos table.
 *
 * Relocated from node_db_sync_import_utxos's orchestration (formerly
 * app/controllers/src/sync_controller_import.c). It is consensus-adjacent
 * (§3 coins-wedge recovery surface): the import-write path — what rows land
 * in the UTXO set — is byte-identical to the controller body it came from.
 * Only the failure SIGNAL changed: this Service returns struct zcl_result
 * (Law 2) instead of a bare int; the thin controller wrapper maps it back to
 * the legacy `rows-or--1` int so every caller sees the same value as before.
 * // supervisor-ok:bounded-import-pipeline — the decoder/writer threads are a
 * one-shot pool pthread_join'd before return (import_job_join_*), so the join
 * is the liveness; a supervisor contract would be meaningless here. */

#include "services/node_db_import_service.h"

#include "platform/time_compat.h"
#include "services/recovery_policy.h"
#include "services/utxo_import_pipeline.h"
#include "models/database.h"
#include "models/db_txn.h"
#include "models/utxo.h"
#include "storage/dbwrapper.h"
#include "storage/coins_db.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

extern void db_iter_check_error(struct db_iterator *it);

extern volatile sig_atomic_t g_shutdown_requested;

/* ── Controller-private symbols this Service forward-declares ──────────
 * The job-status setters + turbo-mode scope are external-linkage functions
 * defined in app/controllers/src/sync_controller.c, declared only in the
 * controller-private sync_controller_internal.h (not includable from
 * app/services/src/). Forward-declared here to match their definitions, so
 * the relocation stays self-contained without injection. */
struct sync_db_turbo_scope {
    struct node_db *ndb;
    bool entered;
};

bool sync_db_turbo_scope_begin(struct sync_db_turbo_scope *scope,
                               struct node_db *ndb,
                               bool enabled);
bool sync_db_turbo_scope_end(struct sync_db_turbo_scope *scope);

void sync_job_import_begin(void);
void sync_job_import_progress(int total_rows);
void sync_job_import_finish(int total_rows);

/* A chunk of raw LevelDB entries for decode workers */
#define IMPORT_CHUNK_ENTRIES 2048
/* Max outputs per chunk. Must be large enough to hold all outputs from
 * 2048 entries. Worst case: 2048 entries * 468 outputs = 958,464.
 * In practice ~5500 rows per chunk. Use 32768 for 4x safety margin. */
#define IMPORT_MAX_ROWS_PER_CHUNK 32768

struct import_chunk {
    struct utxo_import_raw_entry entries[IMPORT_CHUNK_ENTRIES];
    int num_entries;
    struct utxo_import_row rows[IMPORT_MAX_ROWS_PER_CHUNK];
    int num_rows;
    _Atomic int state; /* 0=free, 1=filled, 2=decoded */
};

#define IMPORT_NUM_CHUNKS 64
/* Auto-detect decoder count: use all cores minus 2 (reader + writer).
 * Minimum 4, maximum 32. More decoders = faster LevelDB deserialization. */

struct import_context {
    struct import_chunk chunks[IMPORT_NUM_CHUNKS];
    _Atomic int total_txids;
    _Atomic int total_rows;
    _Atomic int decode_failures;
    _Atomic int skipped_outputs;
    _Atomic bool cancel_requested;
    _Atomic bool reader_done;
    _Atomic bool decoders_done;
    _Atomic int chunks_produced;  /* total chunks filled by reader */
    _Atomic int chunks_consumed;  /* total chunks written by writer */
    /* LevelDB reader state (single-threaded) */
    struct coins_view_db *cvdb;
    /* Writer state */
    struct node_db *ndb;
    int write_next; /* next chunk index to write (in-order) */
};

struct import_job {
    struct import_context *ctx;
    pthread_t decoders[UTXO_IMPORT_NUM_DECODERS_MAX];
    int num_decoders;
    int decoder_threads_started;
    pthread_t writer_thread;
    bool writer_thread_started;
};

static void *import_decoder_thread(void *arg);
static void *import_writer_thread(void *arg);
static void import_job_join_decoders(struct import_job *job);

static bool import_ctx_should_stop(const struct import_context *ctx)
{
    return (ctx && atomic_load(&ctx->cancel_requested)) || g_shutdown_requested;
}

static void import_ctx_request_stop(struct import_context *ctx)
{
    if (!ctx)
        return;
    atomic_store(&ctx->cancel_requested, true);
}

static void import_chunk_reset(struct import_chunk *chunk)
{
    if (!chunk)
        return;
    for (int ei = 0; ei < chunk->num_entries; ei++)
        free(chunk->entries[ei].value);
    for (int ri = 0; ri < chunk->num_rows; ri++)
        free(chunk->rows[ri].script_overflow);
    chunk->num_entries = 0;
    chunk->num_rows = 0;
    atomic_store(&chunk->state, 0);
}

static void import_context_release_chunks(struct import_context *ctx)
{
    if (!ctx)
        return;
    for (int i = 0; i < IMPORT_NUM_CHUNKS; i++)
        import_chunk_reset(&ctx->chunks[i]);
}

static void import_job_init(struct import_job *job,
                            struct import_context *ctx,
                            int num_decoders)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->ctx = ctx;
    job->num_decoders = num_decoders;
}

static bool import_job_start_decoders(struct import_job *job)
{
    if (!job || !job->ctx)
        LOG_FAIL("sync", "import_start_decoders: invalid args (job=%p)", (void *)job);

    for (int i = 0; i < job->num_decoders; i++) {
        int rc = thread_registry_spawn_ex("zcl_utxo_dec",
                                           import_decoder_thread, job->ctx,
                                           &job->decoders[i]);
        if (rc != 0) {
            LOG_WARN("sync", "UTXO import: thread_registry_spawn_ex decoder[%d] failed: %d", i, rc);
            import_ctx_request_stop(job->ctx);
            import_job_join_decoders(job);
            return false;
        }
        job->decoder_threads_started++;
    }
    return job->decoder_threads_started > 0;
}

static bool import_job_start_writer(struct import_job *job)
{
    if (!job || !job->ctx)
        LOG_FAIL("sync", "import_start_writer: invalid args (job=%p)", (void *)job);
    if (thread_registry_spawn_ex("zcl_utxo_wr",
                                  import_writer_thread, job->ctx,
                                  &job->writer_thread) != 0) {
        fprintf(stderr, "UTXO import: FATAL — writer thread failed to start\n");
        import_ctx_request_stop(job->ctx);
        return false;
    }
    job->writer_thread_started = true;
    return true;
}

static void import_job_join_decoders(struct import_job *job)
{
    if (!job)
        return;
    for (int i = 0; i < job->decoder_threads_started; i++)
        pthread_join(job->decoders[i], NULL);
    job->decoder_threads_started = 0;
}

static void import_job_join_writer(struct import_job *job)
{
    if (!job || !job->writer_thread_started)
        return;
    pthread_join(job->writer_thread, NULL);
    job->writer_thread_started = false;
}

static bool import_job_start(struct import_job *job)
{
    if (!import_job_start_decoders(job))
        LOG_FAIL("sync", "import_job_start: decoders failed to start");
    if (!import_job_start_writer(job)) {
        if (job && job->ctx)
            atomic_store(&job->ctx->reader_done, true);
        import_job_join_decoders(job);
        LOG_FAIL("sync", "import_job_start: writer failed to start");
    }
    return true;
}

/* Decoder worker thread — picks filled chunks, decodes, marks decoded */
static void *import_decoder_thread(void *arg)
{
    struct import_context *ctx = (struct import_context *)arg;

    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Find a filled chunk to decode */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int expected = 1; /* filled */
            if (atomic_compare_exchange_strong(&ctx->chunks[i].state,
                                              &expected, -1)) {
                atomic_thread_fence(memory_order_acquire);
                chunk = &ctx->chunks[i];
                break;
            }
        }
        if (!chunk) {
            if (atomic_load(&ctx->reader_done)) {
                /* Check once more for any remaining chunks */
                bool found = false;
                for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                    if (atomic_load(&ctx->chunks[i].state) == 1) {
                        found = true;
                        break;
                    }
                }
                if (!found) break;
            }
            /* Yield briefly — spin is fine on 32 cores */
            struct timespec ts = {0, 100000}; /* 100μs */
            nanosleep(&ts, NULL);
            continue;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Decode all entries in this chunk */
        chunk->num_rows = 0;
        int skipped_in_chunk = 0;
        for (int i = 0; i < chunk->num_entries; i++) {
            if (import_ctx_should_stop(ctx))
                break;
            int space = IMPORT_MAX_ROWS_PER_CHUNK - chunk->num_rows;
            if (space <= 0) { skipped_in_chunk += chunk->num_entries - i; break; }
            int n = utxo_import_decode_entry(&chunk->entries[i],
                                             &chunk->rows[chunk->num_rows],
                                             space);
            if (n == 0) {
                atomic_fetch_add(&ctx->decode_failures, 1);
            }
            chunk->num_rows += n;
        }
        if (skipped_in_chunk > 0) {
            atomic_fetch_add(&ctx->skipped_outputs, skipped_in_chunk);
            LOG_WARN("sync", "UTXO import: chunk overflow! %d entries skipped " "(rows=%d, max=%d)", skipped_in_chunk, chunk->num_rows, IMPORT_MAX_ROWS_PER_CHUNK);
        }

        if (import_ctx_should_stop(ctx))
            import_chunk_reset(chunk);
        else
            atomic_store(&chunk->state, 2); /* decoded */
    }
    return NULL;
}

/* Writer thread — consumes decoded chunks, inserts into SQLite */
static void *import_writer_thread(void *arg)
{
    struct import_context *ctx = (struct import_context *)arg;
    if (!ctx) LOG_NULL("sync", "import_writer_thread: ctx is NULL");
    struct node_db *ndb = ctx->ndb;
    if (!ndb || !ndb->open || !ndb->stmt_utxo_insert) {
        fprintf(stderr, "UTXO import writer: invalid node_db statement/db state\n");
        import_ctx_request_stop(ctx);
        return NULL;
    }
    sqlite3_stmt *ins = ndb->stmt_utxo_insert;
    int total_rows = 0;
    int next_chunk = 0;

    if (!node_db_begin(ndb)) {
        LOG_WARN("sync", "UTXO import writer: BEGIN failed");
        import_ctx_request_stop(ctx);
    }

    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Look for any decoded chunk to write */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int idx = (next_chunk + i) % IMPORT_NUM_CHUNKS;
            int expected = 2; /* decoded */
            if (atomic_compare_exchange_strong(&ctx->chunks[idx].state,
                                              &expected, -1)) {
                chunk = &ctx->chunks[idx];
                next_chunk = (idx + 1) % IMPORT_NUM_CHUNKS;
                break;
            }
        }
        if (!chunk) {
            if (atomic_load(&ctx->decoders_done)) {
                /* Decoders are done. Use definitive chunk count to
                 * know when we're truly finished — no race possible. */
                int produced = atomic_load(&ctx->chunks_produced);
                int consumed = atomic_load(&ctx->chunks_consumed);
                if (consumed >= produced) break;
                /* Still have chunks to consume — scan harder */
                atomic_thread_fence(memory_order_seq_cst);
            }
            struct timespec ts = {0, 100000}; /* 100μs */
            nanosleep(&ts, NULL);
            continue;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Insert all rows from this chunk */
        for (int ri = 0; ri < chunk->num_rows; ri++) {
            if (import_ctx_should_stop(ctx))
                break;
            struct utxo_import_row *r = &chunk->rows[ri];
            const uint8_t *sc = r->script_overflow ?
                                r->script_overflow : r->script;
            bool row_ok = true;

            row_ok &= utxo_import_writer_bind_checked(ins, "sqlite3_reset",
                                                      sqlite3_reset(ins),
                                                      ndb, total_rows).ok;
            row_ok &= utxo_import_writer_bind_checked(ins,
                "sqlite3_bind_blob(txid)",
                sqlite3_bind_blob(ins, 1, r->txid, 32, SQLITE_STATIC),
                ndb, total_rows).ok;
            row_ok &= utxo_import_writer_bind_checked(ins,
                "sqlite3_bind_int(vout)",
                sqlite3_bind_int(ins, 2, (int)r->vout), ndb, total_rows).ok;
            row_ok &= utxo_import_writer_bind_checked(ins,
                "sqlite3_bind_int64(value)",
                sqlite3_bind_int64(ins, 3, r->value), ndb, total_rows).ok;
            row_ok &= utxo_import_writer_bind_checked(ins,
                "sqlite3_bind_blob(script)",
                sqlite3_bind_blob(ins, 4, sc, (int)r->script_len,
                                  SQLITE_STATIC), ndb, total_rows).ok;
            row_ok &= utxo_import_writer_bind_checked(ins,
                "sqlite3_bind_int(script_type)",
                sqlite3_bind_int(ins, 5, r->script_type), ndb,
                total_rows).ok;
            if (r->has_address)
                row_ok &= utxo_import_writer_bind_checked(ins,
                    "sqlite3_bind_blob(address_hash)",
                    sqlite3_bind_blob(ins, 6, r->address_hash, 20,
                                      SQLITE_STATIC), ndb, total_rows).ok;
            else
                row_ok &= utxo_import_writer_bind_checked(ins,
                    "sqlite3_bind_null(address_hash)",
                    sqlite3_bind_null(ins, 6), ndb, total_rows).ok;
            row_ok &= utxo_import_writer_bind_checked(ins,
                "sqlite3_bind_int(height)",
                sqlite3_bind_int(ins, 7, r->height), ndb, total_rows).ok;
            row_ok &= utxo_import_writer_bind_checked(ins,
                "sqlite3_bind_int(is_coinbase)",
                sqlite3_bind_int(ins, 8, r->is_coinbase), ndb,
                total_rows).ok;
            row_ok &= utxo_import_writer_step_checked(ins, ndb,
                                                      total_rows).ok;
            if (!row_ok) {
                import_ctx_request_stop(ctx);
                break;
            }
            total_rows++;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Commit every ~100K rows */
        if (total_rows % 100000 < chunk->num_rows) {
            if (!node_db_commit(ndb)) {
                LOG_WARN("sync", "UTXO import writer: COMMIT failed");
                if (!node_db_rollback(ndb))
                    LOG_WARN("sync", "UTXO import writer: ROLLBACK failed after commit failure");
                import_ctx_request_stop(ctx);
                import_chunk_reset(chunk);
                break;
            }
            sync_job_import_progress(total_rows);
            printf("UTXO import: %d rows written...\n", total_rows);
            fflush(stdout);
            if (!node_db_begin(ndb)) {
                LOG_WARN("sync", "UTXO import writer: BEGIN restart failed");
                if (!node_db_rollback(ndb))
                    LOG_WARN("sync", "UTXO import writer: rollback after BEGIN restart failure failed");
                import_ctx_request_stop(ctx);
                import_chunk_reset(chunk);
                break;
            }
        }

        /* Free buffers and release chunk */
        for (int ei = 0; ei < chunk->num_entries; ei++) {
            free(chunk->entries[ei].value);
            chunk->entries[ei].value = NULL;
        }
        for (int ri = 0; ri < chunk->num_rows; ri++) {
            free(chunk->rows[ri].script_overflow);
            chunk->rows[ri].script_overflow = NULL;
        }
        chunk->num_entries = 0;
        chunk->num_rows = 0;
        atomic_fetch_add(&ctx->chunks_consumed, 1);
        atomic_store(&chunk->state, 0); /* free for reuse */
    }

    if (!import_ctx_should_stop(ctx)) {
        if (!node_db_commit(ndb))
            LOG_WARN("sync", "UTXO import writer: final COMMIT failed");
    } else {
        if (!node_db_rollback(ndb))
            LOG_WARN("sync", "UTXO import writer: rollback requested by stop flag failed");
    }
    sync_job_import_progress(total_rows);
    atomic_store(&ctx->total_rows, total_rows);
    return NULL;
}

struct zcl_result node_db_import_service_run(struct node_db *ndb,
                                             struct coins_view_db *cvdb,
                                             int *out_rows)
{
    struct import_job job;
    struct sync_db_turbo_scope turbo_mode = {0};
    bool restore_ok = true;

    if (!ndb || !ndb->open || !cvdb)
        return ZCL_ERR(-10, "import_utxos: invalid args (ndb=%p, cvdb=%p)",
                       (void *)ndb, (void *)cvdb);
    sync_job_import_begin();

    int num_decoders = utxo_import_num_decoders();
    printf("UTXO import: parallel pipeline (%d decoders, %d chunks, %ld cores)...\n",
           num_decoders, IMPORT_NUM_CHUNKS, sysconf(_SC_NPROCESSORS_ONLN));
    fflush(stdout);

    struct timespec ts_start;
    platform_time_monotonic_timespec(&ts_start);

    /* ── SQLite turbo mode — delegate to node_db layer ────────────── */
    if (!sync_db_turbo_scope_begin(&turbo_mode, ndb, true)) {
        fprintf(stderr, "UTXO import: failed to enter turbo mode\n");
        sync_job_import_finish(0);
        return ZCL_ERR(-1, "import: failed to enter SQLite turbo mode");
    }

    /* ── Recovery policy gate + scoped wipe ────────────────────────
     * The wipe below is a reimport prelude, not a reorg rollback — in
     * normal operation the table is empty or nearly empty. Historically
     * this call site is *not* the one that caused 2026-04-10, but it
     * shares a primitive with the paths that did, so we gate it the
     * same way: ask the policy, refuse if over cap, abort cleanly.
     * The cap is deliberately generous here (reimport is legitimate)
     * but an operator can still raise ZCL_MAX_UTXO_WIPE_ROWS if a
     * partial import is being resumed.
     *
     * The DELETE + initial state are wrapped in a DB_TXN_SCOPE so a
     * mid-wipe crash or early-return rolls back cleanly: the pipeline
     * below starts from a fully-wiped-and-committed table, not a
     * half-deleted one. */
    /* Skip the wipe if the table is already empty — the caller
     * (utxo_recovery_import_ldb) already wiped via the
     * "boot.ldb_import_prepare" wipe in utxo_recovery_restore.c.
     * This was the third redundant wipe that destroyed data. */
    int64_t existing = node_db_utxo_count(ndb);
    if (existing < 0) existing = 0;

    if (existing > 0) {
        struct recovery_policy rp;
        policy_load_from_env(&rp);
        enum policy_decision pd = policy_check_utxo_wipe(
            &rp, existing, "sync_controller.import_utxos_reimport");
        if (pd != POLICY_ALLOW) {
            LOG_INFO("sync", "UTXO import: recovery_policy refused wipe (code=%s, rows=%lld)", policy_decision_name(pd), (long long)existing);
            if (!sync_db_turbo_scope_end(&turbo_mode))
                fprintf(stderr, "UTXO import: failed to restore normal mode after policy refusal\n");
            sync_job_import_finish(0);
            return ZCL_ERR(-2, "import: recovery_policy refused utxo wipe "
                           "(existing_rows=%lld)", (long long)existing);
        }

        {
            DB_TXN_SCOPE(txn, ndb, "sync_controller.import_utxos_reimport");
            if (!txn) {
                LOG_WARN("sync", "UTXO import: failed to open db_txn for wipe");
                if (!sync_db_turbo_scope_end(&turbo_mode))
                    fprintf(stderr, "UTXO import: failed to restore normal mode after db_txn failure\n");
                sync_job_import_finish(0);
                return ZCL_ERR(-3, "import: failed to open db_txn for "
                               "utxo wipe");
            }
            if (!node_db_wipe_utxos(ndb)) {
                LOG_WARN("sync", "UTXO import: failed to wipe utxos table");
                /* leave scope → auto-rollback */
                if (!sync_db_turbo_scope_end(&turbo_mode))
                    fprintf(stderr, "UTXO import: failed to restore normal mode after wipe failure\n");
                sync_job_import_finish(0);
                return ZCL_ERR(-4, "import: failed to wipe utxos table");
            }
            if (!db_txn_commit(txn)) {
                LOG_WARN("sync", "UTXO import: commit of wipe failed");
                if (!sync_db_turbo_scope_end(&turbo_mode))
                    fprintf(stderr, "UTXO import: failed to restore normal mode after commit failure\n");
                sync_job_import_finish(0);
                return ZCL_ERR(-5, "import: commit of utxo wipe failed");
            }
        }
    }

    /* ── Initialize pipeline context ───────────────────────────────── */
    struct import_context *ctx = zcl_calloc(1, sizeof(struct import_context), "import context");
    if (!ctx) {
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after alloc failure\n");
        sync_job_import_finish(0);
        return ZCL_ERR(-6, "import: failed to allocate import context");
    }
    ctx->cvdb = cvdb;
    ctx->ndb = ndb;
    atomic_store(&ctx->cancel_requested, false);
    atomic_store(&ctx->reader_done, false);
    atomic_store(&ctx->decoders_done, false);
    for (int i = 0; i < IMPORT_NUM_CHUNKS; i++)
        atomic_store(&ctx->chunks[i].state, 0);
    import_job_init(&job, ctx, num_decoders);

    /* ── Start decoder + writer threads ────────────────────────────── */
    if (!import_job_start(&job)) {
        LOG_WARN("sync", "UTXO import: FATAL — worker pipeline failed to start");
        import_context_release_chunks(ctx);
        if (!sync_db_turbo_scope_end(&turbo_mode))
            LOG_WARN("sync", "UTXO import: failed to restore normal mode after worker startup failure");
        free(ctx);
        sync_job_import_finish(0);
        return ZCL_ERR(-7, "import: worker pipeline failed to start");
    }

    /* ── Reader (main thread): iterate LevelDB, fill chunks ────────── */
    /* Take a LevelDB snapshot so the iterator sees a consistent,
     * frozen view even if zclassicd is writing concurrently.
     * This prevents the random UTXO gaps caused by non-atomic reads. */
    db_wrapper_snapshot_begin(&cvdb->db);

    struct db_iterator it;
    db_iter_init(&it, &cvdb->db);
    char seek_key[33];
    seek_key[0] = 'c';
    memset(seek_key + 1, 0, 32);
    db_iter_seek(&it, seek_key, 33);

    int chunk_idx = 0;
    int total_entries = 0;
    int skipped_entries = 0;

    while (db_iter_valid(&it)) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Find a free chunk */
        struct import_chunk *chunk = NULL;
        while (!chunk) {
            if (import_ctx_should_stop(ctx))
                goto reader_done;
            for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                int idx = (chunk_idx + i) % IMPORT_NUM_CHUNKS;
                int expected = 0;
                if (atomic_compare_exchange_strong(&ctx->chunks[idx].state,
                                                  &expected, -1)) {
                    chunk = &ctx->chunks[idx];
                    chunk_idx = (idx + 1) % IMPORT_NUM_CHUNKS;
                    break;
                }
            }
            if (!chunk) {
                struct timespec ts = {0, 50000}; /* 50μs */
                nanosleep(&ts, NULL);
            }
        }

        /* Fill chunk with raw entries from LevelDB */
        chunk->num_entries = 0;
        chunk->num_rows = 0;

        bool end_of_range = false;
        while (chunk->num_entries < IMPORT_CHUNK_ENTRIES &&
               db_iter_valid(&it)) {
            if (import_ctx_should_stop(ctx))
                goto reader_done;
            size_t key_len;
            const char *key_data = db_iter_key(&it, &key_len);
            if (key_len < 1 || key_data[0] != 'c') {
                /* Coins range ended: publish the in-fill chunk first —
                 * a goto here dropped the last ~1500 records (tail). */
                end_of_range = true;
                break;
            }
            if (key_len < 33) { db_iter_next(&it); continue; }

            struct utxo_import_raw_entry *e =
                &chunk->entries[chunk->num_entries];
            memcpy(e->txid, key_data + 1, 32);

            size_t val_len;
            const char *val_data = db_iter_value(&it, &val_len);
            if (val_len > 65535) val_len = 65535;
            e->value = zcl_malloc(val_len, "import entry value");
            if (e->value) {
                memcpy(e->value, val_data, val_len);
                /* db_iter_value() already deobfuscates values using the
                 * obfuscation key (dbwrapper.c:370-372). Do NOT XOR
                 * again here — that would undo the deobfuscation. */
                e->value_len = (uint16_t)val_len;
                chunk->num_entries++;
                total_entries++;
            } else {
                LOG_WARN("sync", "malloc failed for chunk entry value (%zu bytes), skipping entry", val_len);
                skipped_entries++;
            }
            db_iter_next(&it);
        }

        if (chunk->num_entries > 0) {
            atomic_fetch_add(&ctx->chunks_produced, 1);
            atomic_thread_fence(memory_order_release);
            atomic_store(&chunk->state, 1); /* filled → decoders */
        } else {
            atomic_store(&chunk->state, 0); /* empty, release */
        }
        if (end_of_range)
            break;
    }
reader_done:
    /* Checksum failures can end iteration early, dropping entries. */
    db_iter_check_error(&it);
    db_iter_free(&it);
    db_wrapper_snapshot_end(&cvdb->db);
    atomic_store(&ctx->reader_done, true);

    printf("UTXO import: read %d txids from LevelDB (%d skipped)\n",
           total_entries, skipped_entries);
    fflush(stdout);

    /* ── Wait for decoders ────────────────────────────────────────── */
    import_job_join_decoders(&job);

    /* ── Wait for ALL chunks to be consumed by writer ──────────── */
    /* After decoders finish, remaining chunks are in state 2 (decoded).
     * We MUST wait for the writer to consume them ALL before signaling
     * decoders_done. Otherwise the writer sees decoders_done=true, does
     * a quick scan, misses state=2 chunks due to timing, and exits
     * early — dropping the last ~219 txids / ~520 UTXOs. */
    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        bool any_pending = false;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int s = atomic_load_explicit(&ctx->chunks[i].state,
                                          memory_order_acquire);
            if (s == 1 || s == 2) { any_pending = true; break; }
        }
        if (!any_pending) break;
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }
    atomic_store(&ctx->decoders_done, true);

    /* ── Wait for writer ───────────────────────────────────────────── */
    import_job_join_writer(&job);
    int total_rows = atomic_load(&ctx->total_rows);
    sync_job_import_progress(total_rows);
    int decode_fail = atomic_load(&ctx->decode_failures);
    int skip_out = atomic_load(&ctx->skipped_outputs);
    if (decode_fail > 0 || skip_out > 0)
        printf("UTXO import: %d decode failures, %d skipped outputs\n",
               decode_fail, skip_out);

    /* Validation: verify all txids made it to SQLite */
    {
        int64_t sql_rows = 0;
        int64_t sql_txids = 0;
        if (db_utxo_count_rows_and_distinct_txids(ndb, &sql_rows,
                                                  &sql_txids)) {
            if (sql_rows != total_rows) {
                /* Row count mismatch = real data loss — pipeline bug */
                LOG_WARN("sync", "UTXO IMPORT ERROR: wrote %d rows but " "SQLite has %lld rows — data loss!", total_rows, (long long)sql_rows);
            } else if (sql_txids < total_entries) {
                /* Fewer distinct txids is expected: fully-pruned CCoins
                 * (all outputs spent) produce zero rows per txid.
                 * These exist in LevelDB as tombstones until compaction. */
                int pruned = total_entries - (int)sql_txids;
                printf("UTXO import: %d/%d LevelDB entries were "
                       "fully-pruned (all outputs spent)\n",
                       pruned, total_entries);
            }
        }
    }
    fflush(stdout);

    struct timespec ts_write;
    platform_time_monotonic_timespec(&ts_write);
    double pipe_ms = (ts_write.tv_sec - ts_start.tv_sec) * 1000.0 +
                     (ts_write.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import: %d rows written in %.0fms\n", total_rows, pipe_ms);
    fflush(stdout);

    if (import_ctx_should_stop(ctx)) {
        LOG_WARN("sync", "UTXO import: aborted%s", g_shutdown_requested ? " on shutdown" : "");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            LOG_WARN("sync", "UTXO import: failed to restore normal mode after abort");
        restore_ok = false;
        import_context_release_chunks(ctx);
        free(ctx);
        sync_job_import_finish(total_rows);
        return ZCL_ERR(-8, "import: aborted%s",
                       g_shutdown_requested ? " on shutdown" : "");
    }

    /* ── Rebuild all indexes for power-node queries ────────────────── */
    printf("UTXO import: building indexes for fast queries...\n");
    fflush(stdout);

    struct timespec ts_idx;
    platform_time_monotonic_timespec(&ts_idx);

    /* Rebuild indexes and restore safe pragmas */
    if (!sync_db_turbo_scope_end(&turbo_mode)) {
        restore_ok = false;
        LOG_WARN("sync", "UTXO import: failed to restore normal mode");
    }
    if (!restore_ok) {
        import_context_release_chunks(ctx);
        free(ctx);
        sync_job_import_finish(total_rows);
        return ZCL_ERR(-9, "import: failed to restore normal db mode "
                       "after index build");
    }

    struct timespec ts_idx_done;
    platform_time_monotonic_timespec(&ts_idx_done);
    double idx_ms = (ts_idx_done.tv_sec - ts_idx.tv_sec) * 1000.0 +
                    (ts_idx_done.tv_nsec - ts_idx.tv_nsec) / 1e6;
    printf("UTXO import: indexes built in %.0fms\n", idx_ms);

    double total_ms = (ts_idx_done.tv_sec - ts_start.tv_sec) * 1000.0 +
                      (ts_idx_done.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import complete: %d outputs from %d txids in %.1fs "
           "(pipeline %.0fms + index %.0fms)\n",
           total_rows, total_entries, total_ms / 1000.0,
           pipe_ms, idx_ms);
    fflush(stdout);

    import_context_release_chunks(ctx);
    free(ctx);
    sync_job_import_finish(total_rows);
    if (out_rows)
        *out_rows = total_rows;
    return ZCL_OK;
}
