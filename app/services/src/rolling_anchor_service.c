/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Rolling SHA3 anchor extension — see services/rolling_anchor_service.h
 * for the contract.
 *
 * Lifecycle:
 *   1. boot:  rolling_anchor_init(datadir)
 *   2. supervisor tick: rolling_anchor_extend_if_due(ms, datadir)
 *      every 60s under the chain domain. */

#include "platform/time_compat.h"
#include "services/rolling_anchor_service.h"
#include "services/oracle_policy.h"
#include "services/quorum_oracle_service.h"

#include "supervisors/domains.h"
#include "chain/chain.h"
#include "chain/sha3_windows.h"
#include "core/serialize.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "event/event.h"
#include "json/json.h"
#include "primitives/block.h"
#include "storage/disk_block_io.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/main_constants.h"
#include "validation/sync_evidence_policy.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RA_MAGIC           "ZCLRAW1"     /* 7 bytes + NUL = 8 */
#define RA_MAGIC_LEN       8
#define RA_SCHEMA          1u
#define RA_DEFAULT_DEPTH   ZCL_FINALITY_DEPTH
#define RA_DEFAULT_CAP     10
#define RA_FILE_NAME       "sha3_windows_runtime.dat"
#define RA_RECORD_SIZE     36u            /* i32 + 32 bytes hash */
#define RA_TICK_SECS       60              /* periodic extend cadence */

struct ra_record {
    int32_t start_height;
    uint8_t hash[32];
};

static struct {
    pthread_mutex_t lock;
    bool   initialized;
    int    confirmation_depth;
    int    max_extend_per_call;
    struct ra_record *windows;
    int    count;
    int    capacity;
    _Atomic int64_t total_extended;
    _Atomic int64_t total_skipped_policy;
    _Atomic int64_t total_skipped_depth;
    _Atomic int64_t total_read_failures;
    _Atomic int64_t last_extend_unix;
    char   file_path[1024];
    /* Periodic-tick context, populated by rolling_anchor_start. */
    struct main_state    *tick_ms;
    char                  tick_datadir[1024];
} g_ra = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct liveness_contract g_ra_contract;
static _Atomic supervisor_child_id g_ra_supervisor_id = SUPERVISOR_INVALID_ID;

static int ra_compile_time_end(void)
{
    if (g_sha3_windows_count == 0) return -1; // raw-return-ok:sentinel-no-compile-time-windows
    return (int)(g_sha3_windows_count * SHA3_WINDOW_SIZE) - 1;
}

/* Caller holds g_ra.lock. Last committed runtime window's end height
 * (or compile-time end when no runtime windows are present). */
static int ra_runtime_end_locked(void)
{
    int compile_end = ra_compile_time_end();
    if (g_ra.count == 0) return compile_end;
    return g_ra.windows[g_ra.count - 1].start_height +
           (int)SHA3_WINDOW_SIZE - 1;
}

/* Compute the file-level SHA3 over the on-disk body (everything before
 * the trailing 32-byte digest). Caller passes the body bytes + length. */
static void ra_file_digest(const uint8_t *body, size_t body_len,
                            uint8_t out[32])
{
    sha3_256(body, body_len, out);
}

/* Write the in-memory ring to disk atomically. Caller holds lock. */
static bool ra_persist_locked(void)
{
    if (g_ra.file_path[0] == '\0') return false;
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", g_ra.file_path)
            >= (int)sizeof(tmp))
        return false;
    size_t body_len = (size_t)RA_MAGIC_LEN + 4 + 4 +
                      (size_t)g_ra.count * RA_RECORD_SIZE;
    uint8_t *body = zcl_malloc(body_len, "ra_persist.body");
    if (!body) return false;
    uint8_t *p = body;
    memcpy(p, RA_MAGIC, RA_MAGIC_LEN); p += RA_MAGIC_LEN;
    uint32_t schema = RA_SCHEMA;
    memcpy(p, &schema, 4); p += 4;
    uint32_t count = (uint32_t)g_ra.count;
    memcpy(p, &count, 4); p += 4;
    for (int i = 0; i < g_ra.count; i++) {
        int32_t h = g_ra.windows[i].start_height;
        memcpy(p, &h, 4); p += 4;
        memcpy(p, g_ra.windows[i].hash, 32); p += 32;
    }
    uint8_t digest[32];
    ra_file_digest(body, body_len, digest);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(body);
        fprintf(stderr,
                "[rolling_anchor] persist: fopen(%s) failed: %s\n",
                tmp, strerror(errno));
        return false;
    }
    bool ok = (fwrite(body, 1, body_len, f) == body_len) &&
              (fwrite(digest, 1, 32, f) == 32) &&
              (fflush(f) == 0) &&
              (fsync(fileno(f)) == 0);
    fclose(f);
    free(body);
    if (!ok) {
        unlink(tmp);
        fprintf(stderr,
                "[rolling_anchor] persist: write/sync failed: %s\n",
                strerror(errno));
        return false;
    }
    if (rename(tmp, g_ra.file_path) != 0) {
        unlink(tmp);
        fprintf(stderr,
                "[rolling_anchor] persist: rename failed: %s\n",
                strerror(errno));
        return false;
    }
    return true;
}

/* Read + verify file. Returns true on success (file absent counts as
 * success — caller leaves g_ra empty). On corruption returns false;
 * caller decides whether to delete the file. */
static bool ra_load_locked(void)
{
    struct stat st;
    if (stat(g_ra.file_path, &st) != 0) {
        return errno == ENOENT;  /* absent file is fine */
    }
    if (st.st_size < (off_t)(RA_MAGIC_LEN + 4 + 4 + 32)) {
        fprintf(stderr,
                "[rolling_anchor] load: file too small (%lld bytes)\n",
                (long long)st.st_size);
        return false;
    }
    size_t fsz = (size_t)st.st_size;
    FILE *f = fopen(g_ra.file_path, "rb");
    if (!f) {
        fprintf(stderr,
                "[rolling_anchor] load: fopen failed: %s\n",
                strerror(errno));
        return false;
    }
    uint8_t *buf = zcl_malloc(fsz, "ra_load.buf");
    if (!buf) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, fsz, f) != fsz) {
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    /* Layout: body (fsz - 32) || digest (32). */
    size_t body_len = fsz - 32;
    uint8_t expected[32];
    memcpy(expected, buf + body_len, 32);
    uint8_t computed[32];
    ra_file_digest(buf, body_len, computed);
    if (memcmp(expected, computed, 32) != 0) {
        LOG_WARN("rolling_anchor", "[rolling_anchor] load: file SHA3 mismatch — " "discarding runtime anchors");
        free(buf);
        return false;
    }

    if (memcmp(buf, RA_MAGIC, RA_MAGIC_LEN) != 0) {
        fprintf(stderr,
                "[rolling_anchor] load: bad magic — discarding\n");
        free(buf);
        return false;
    }
    uint32_t schema = 0;
    memcpy(&schema, buf + RA_MAGIC_LEN, 4);
    if (schema != RA_SCHEMA) {
        LOG_INFO("rolling_anchor", "[rolling_anchor] load: schema %u != %u — discarding", schema, RA_SCHEMA);
        free(buf);
        return false;
    }
    uint32_t count = 0;
    memcpy(&count, buf + RA_MAGIC_LEN + 4, 4);
    size_t expected_size = (size_t)RA_MAGIC_LEN + 4 + 4 +
                           (size_t)count * RA_RECORD_SIZE;
    if (expected_size != body_len) {
        LOG_WARN("rolling_anchor", "[rolling_anchor] load: size mismatch (count=%u body=%zu) " "— discarding", count, body_len);
        free(buf);
        return false;
    }

    /* Validate monotonic + alignment + contiguity with compile-time
     * prefix. */
    int compile_end = ra_compile_time_end();
    int expected_start =
        (compile_end >= 0) ? compile_end + 1 : 0;

    struct ra_record *recs = NULL;
    if (count > 0) {
        recs = zcl_calloc(count, sizeof(*recs), "ra.load.recs");
        if (!recs) { free(buf); return false; }
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = buf + RA_MAGIC_LEN + 4 + 4 +
                            (size_t)i * RA_RECORD_SIZE;
        int32_t h = 0;
        memcpy(&h, p, 4);
        if (h != expected_start || h % SHA3_WINDOW_SIZE != 0) {
            LOG_WARN("rolling_anchor", "[rolling_anchor] load: bad start_height at idx %u " "(got %d expected %d) — discarding", i, h, expected_start);
            free(recs);
            free(buf);
            return false;
        }
        recs[i].start_height = h;
        memcpy(recs[i].hash, p + 4, 32);
        expected_start += (int)SHA3_WINDOW_SIZE;
    }
    free(buf);

    /* Adopt. */
    free(g_ra.windows);
    g_ra.windows  = recs;
    g_ra.count    = (int)count;
    g_ra.capacity = (int)count;
    if (count > 0) {
        LOG_INFO("rolling_anchor", "[rolling_anchor] load: %u windows loaded " "(coverage h=%d..%d)", count, recs[0].start_height, recs[count - 1].start_height + (int)SHA3_WINDOW_SIZE - 1);
    }
    return true;
}

struct zcl_result rolling_anchor_init(const char *datadir,
                          const struct rolling_anchor_config *cfg)
{
    if (!datadir || !datadir[0])
        return ZCL_ERR(-1, "rolling_anchor_init: NULL/empty datadir");
    pthread_mutex_lock(&g_ra.lock);
    if (g_ra.initialized) {
        pthread_mutex_unlock(&g_ra.lock);
        return ZCL_OK;
    }
    g_ra.confirmation_depth =
        (cfg && cfg->confirmation_depth > 0)
            ? cfg->confirmation_depth : RA_DEFAULT_DEPTH;
    g_ra.max_extend_per_call =
        (cfg && cfg->max_extend_per_call > 0)
            ? cfg->max_extend_per_call : RA_DEFAULT_CAP;
    snprintf(g_ra.file_path, sizeof(g_ra.file_path), "%s/%s",
             datadir, RA_FILE_NAME);

    bool ok = ra_load_locked();
    if (!ok) {
        /* Corrupt — wipe so we don't keep refusing to load. */
        unlink(g_ra.file_path);
        free(g_ra.windows);
        g_ra.windows = NULL;
        g_ra.count = 0;
        g_ra.capacity = 0;
    }
    g_ra.initialized = true;
    pthread_mutex_unlock(&g_ra.lock);
    if (!ok)
        return ZCL_ERR(-2, "rolling_anchor_init: load failed (corrupt file wiped) datadir=%s",
                       datadir);
    return ZCL_OK;
}

/* Caller holds lock. Compute SHA3 over heights [start_h..start_h+999]
 * by reading each block from disk via active_chain. Returns true if
 * every block was read; false on any I/O failure. */
static bool ra_compute_window_hash(struct main_state *ms,
                                    const char *datadir,
                                    int start_h,
                                    uint8_t out_hash[32],
                                    int *out_failure_height)
{
    *out_failure_height = -1;
    if (!ms || !datadir || !datadir[0])
        return false;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    struct byte_stream s;
    stream_init(&s, 4096);

    bool ok = true;
    for (int h = start_h; h < start_h + (int)SHA3_WINDOW_SIZE; h++) {
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (!bi || bi->nFile < 0 ||
            !(bi->nStatus & BLOCK_HAVE_DATA)) {
            *out_failure_height = h;
            ok = false;
            break;
        }
        struct block blk;
        block_init(&blk);
        if (!read_block_from_disk_index_pread(&blk, bi, datadir)) {
            block_free(&blk);
            *out_failure_height = h;
            ok = false;
            break;
        }
        s.size = 0;
        s.read_pos = 0;
        s.error = false;
        if (!block_serialize(&blk, &s)) {
            block_free(&blk);
            *out_failure_height = h;
            ok = false;
            break;
        }
        sha3_256_write(&ctx, s.data, s.size);
        block_free(&blk);
    }
    stream_free(&s);
    if (ok) sha3_256_finalize(&ctx, out_hash);
    return ok;
}

static bool ra_quorum_allows_commit(int height)
{
    struct quorum_oracle_result qr;
    int present = 0;

    if (!quorum_oracle_probe(height, &qr).ok)
        return true;
    for (int i = 0; i < QO_SRC_NUM; i++) {
        if (qr.by_source[i].present && !qr.by_source[i].error &&
            qr.by_source[i].hash_hex[0] != '\0')
            present++;
    }
    if (present <= 1)
        return true;
    return qr.verdict == QO_VERDICT_QUORUM_MATCH &&
           qr.agreeing_sources >= 2;
}

int rolling_anchor_extend_if_due(struct main_state *ms,
                                  const char *datadir)
{
    if (!ms || !datadir || !datadir[0]) return 0;
    if (!g_ra.initialized) return 0;

    if (!oracle_policy_chain_extension_allowed()) {
        atomic_fetch_add(&g_ra.total_skipped_policy, 1);
        return 0;
    }

    int tip = active_chain_height(&ms->chain_active);
    if (tip < 0) return 0;

    pthread_mutex_lock(&g_ra.lock);
    int current_end = ra_runtime_end_locked();
    int cap = g_ra.max_extend_per_call;
    pthread_mutex_unlock(&g_ra.lock);

    if (current_end < 0) return 0;  /* no compile-time anchors */

    int next_start = ((current_end + 1) / (int)SHA3_WINDOW_SIZE) *
                     (int)SHA3_WINDOW_SIZE;
    if (next_start <= current_end) {
        /* Should be exactly current_end + 1, but be defensive. */
        next_start = current_end + 1;
        if (next_start % SHA3_WINDOW_SIZE != 0) return 0;
    }

    int extended = 0;
    while (extended < cap) {
        int last_h_in_win = next_start + (int)SHA3_WINDOW_SIZE - 1;
        if (last_h_in_win > zcl_immutable_height(tip)) {
            atomic_fetch_add(&g_ra.total_skipped_depth, 1);
            break;
        }
        if (!oracle_policy_chain_extension_allowed()) {
            atomic_fetch_add(&g_ra.total_skipped_policy, 1);
            break;
        }
        if (!ra_quorum_allows_commit(last_h_in_win)) {
            atomic_fetch_add(&g_ra.total_skipped_policy, 1);
            event_emitf(EV_SYNC_STATE_CHANGE, 0,
                        "rolling_anchor quorum split h=%d", last_h_in_win);
            fprintf(stderr,
                    "[rolling_anchor] extend: quorum refused window "
                    "start=%d end=%d\n", next_start, last_h_in_win);
            break;
        }

        uint8_t hash[32];
        int fail_h = -1;
        if (!ra_compute_window_hash(ms, datadir, next_start, hash, &fail_h)) {
            atomic_fetch_add(&g_ra.total_read_failures, 1);
            LOG_WARN("rolling_anchor", "[rolling_anchor] extend: read failed at h=%d " "(window start=%d) — stopping", fail_h, next_start);
            break;
        }

        pthread_mutex_lock(&g_ra.lock);
        if (g_ra.count == g_ra.capacity) {
            int new_cap = g_ra.capacity ? g_ra.capacity * 2 : 16;
            struct ra_record *r =
                zcl_realloc(g_ra.windows,
                            (size_t)new_cap * sizeof(*r),
                            "rolling_anchor.windows");
            if (!r) {
                pthread_mutex_unlock(&g_ra.lock);
                break;
            }
            g_ra.windows = r;
            g_ra.capacity = new_cap;
        }
        g_ra.windows[g_ra.count].start_height = next_start;
        memcpy(g_ra.windows[g_ra.count].hash, hash, 32);
        g_ra.count++;
        bool persisted = ra_persist_locked();
        pthread_mutex_unlock(&g_ra.lock);

        if (!persisted) {
            /* Disk write failure - roll back the in-memory entry so
             * we don't claim evidence that is not on disk. */
            pthread_mutex_lock(&g_ra.lock);
            if (g_ra.count > 0) g_ra.count--;
            pthread_mutex_unlock(&g_ra.lock);
            LOG_WARN("rolling_anchor", "[rolling_anchor] extend: persist failed at start=%d " "— in-memory rollback", next_start);
            break;
        }

        char hex[65];
        HexStr(hash, 32, false, hex, sizeof(hex));
        LOG_INFO("rolling_anchor", "[rolling_anchor] extended: start=%d end=%d sha3=%s", next_start, last_h_in_win, hex);
        atomic_fetch_add(&g_ra.total_extended, 1);
        atomic_store(&g_ra.last_extend_unix, (int64_t)platform_time_wall_time_t());

        next_start += (int)SHA3_WINDOW_SIZE;
        extended++;
    }
    return extended;
}

/* ── Periodic-tick wrapper ─────────────────────────────────────── */

static int rolling_anchor_effective_prefix_end(void)
{
    pthread_mutex_lock(&g_ra.lock);
    int runtime_end = ra_runtime_end_locked();
    pthread_mutex_unlock(&g_ra.lock);
    return runtime_end;
}

static void rolling_anchor_on_stall(struct liveness_contract *c)
{
    int reason = c ? atomic_load(&c->stall_reason) : SUPERVISOR_STALL_NONE;
    int runtime_end = rolling_anchor_effective_prefix_end();
    int64_t read_failures = atomic_load(&g_ra.total_read_failures);
    const char *reason_name = supervisor_stall_reason_name(
        (enum supervisor_stall_reason)reason);

    LOG_WARN("supervisor", "[supervisor] chain.rolling_anchor stalled reason=%s effective_prefix_end=%d read_failures=%lld", reason_name, runtime_end, (long long)read_failures);
    event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
                "chain.rolling_anchor stalled reason=%s effective_prefix_end=%d read_failures=%lld",
                reason_name, runtime_end, (long long)read_failures);
}

static void rolling_anchor_on_tick(struct liveness_contract *c)
{
    (void)c;
    pthread_mutex_lock(&g_ra.lock);
    struct main_state *ms = g_ra.tick_ms;
    char datadir[1024];
    memcpy(datadir, g_ra.tick_datadir, sizeof(datadir));
    pthread_mutex_unlock(&g_ra.lock);

    supervisor_child_id id = atomic_load(&g_ra_supervisor_id);
    if (!ms) {
        supervisor_tick(id);
        return;
    }

    int64_t before_failures = atomic_load(&g_ra.total_read_failures);
    (void)rolling_anchor_extend_if_due(ms, datadir);
    supervisor_progress(id, (int64_t)rolling_anchor_effective_prefix_end());
    if (atomic_load(&g_ra.total_read_failures) > before_failures)
        supervisor_report_stall(id, SUPERVISOR_STALL_CHILD_REPORTED);
    supervisor_tick(id);
}

struct zcl_result rolling_anchor_start(struct main_state *ms, const char *datadir)
{
    if (!ms || !datadir || !datadir[0])
        return ZCL_ERR(-1, "rolling_anchor_start: bad args ms=%p datadir=%s",
                       (void*)ms, datadir ? datadir : "(null)");

    /* Idempotent — init() returns ZCL_OK if file absent. */
    (void)rolling_anchor_init(datadir, NULL);

    pthread_mutex_lock(&g_ra.lock);
    g_ra.tick_ms = ms;
    snprintf(g_ra.tick_datadir, sizeof(g_ra.tick_datadir), "%s", datadir);
    pthread_mutex_unlock(&g_ra.lock);

    supervisor_child_id id = atomic_load(&g_ra_supervisor_id);
    if (id != SUPERVISOR_INVALID_ID) {
        supervisor_set_period(id, RA_TICK_SECS);
        supervisor_progress(id, (int64_t)rolling_anchor_effective_prefix_end());
        supervisor_tick(id);
        return ZCL_OK;
    }

    liveness_contract_init(&g_ra_contract, "chain.rolling_anchor");
    atomic_store(&g_ra_contract.period_secs, (int64_t)RA_TICK_SECS);
    atomic_store(&g_ra_contract.deadline_secs, (int64_t)0);
    atomic_store(&g_ra_contract.progress_max_quiet_us, (int64_t)0);
    g_ra_contract.on_tick = rolling_anchor_on_tick;
    g_ra_contract.on_stall = rolling_anchor_on_stall;

    supervisor_domains_init();
    id = supervisor_register_in_domain(g_chain_sup, &g_ra_contract);
    atomic_store(&g_ra_supervisor_id, id);
    if (id == SUPERVISOR_INVALID_ID)
        return ZCL_ERR(-2, "rolling_anchor_start: supervisor register failed");
    supervisor_progress(id, (int64_t)rolling_anchor_effective_prefix_end());
    supervisor_tick(id);
    return ZCL_OK;
}

void rolling_anchor_stop(void)
{
    supervisor_child_id id = atomic_load(&g_ra_supervisor_id);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_set_period(id, 0);
    pthread_mutex_lock(&g_ra.lock);
    g_ra.tick_ms = NULL;
    g_ra.tick_datadir[0] = '\0';
    pthread_mutex_unlock(&g_ra.lock);
}

bool rolling_anchor_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    pthread_mutex_lock(&g_ra.lock);
    int count = g_ra.count;
    int compile_end = ra_compile_time_end();
    int runtime_end = ra_runtime_end_locked();
    int depth = g_ra.confirmation_depth;
    int cap = g_ra.max_extend_per_call;
    char path_copy[sizeof(g_ra.file_path)];
    memcpy(path_copy, g_ra.file_path, sizeof(path_copy));
    pthread_mutex_unlock(&g_ra.lock);

    json_push_kv_int (out, "runtime_window_count", count);
    json_push_kv_int (out, "compile_time_window_count",
                      (int64_t)g_sha3_windows_count);
    json_push_kv_int (out, "compile_time_prefix_end_height", compile_end);
    json_push_kv_int (out, "effective_prefix_end_height", runtime_end);
    json_push_kv_int (out, "confirmation_depth", depth);
    json_push_kv_int (out, "max_extend_per_call", cap);
    json_push_kv_str (out, "file_path", path_copy);
    json_push_kv_int (out, "total_extended",
                      atomic_load(&g_ra.total_extended));
    json_push_kv_int (out, "total_skipped_policy",
                      atomic_load(&g_ra.total_skipped_policy));
    json_push_kv_int (out, "total_skipped_depth",
                      atomic_load(&g_ra.total_skipped_depth));
    json_push_kv_int (out, "total_read_failures",
                      atomic_load(&g_ra.total_read_failures));
    json_push_kv_int (out, "last_extend_unix",
                      atomic_load(&g_ra.last_extend_unix));
    return true;
}

void rolling_anchor_reset_for_test(void)
{
    rolling_anchor_stop();
#ifdef ZCL_TESTING
    supervisor_child_id id = atomic_exchange(&g_ra_supervisor_id,
                                             SUPERVISOR_INVALID_ID);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_unregister(id);
#endif
    pthread_mutex_lock(&g_ra.lock);
    free(g_ra.windows);
    g_ra.windows = NULL;
    g_ra.count = 0;
    g_ra.capacity = 0;
    g_ra.initialized = false;
    g_ra.file_path[0] = '\0';
    g_ra.tick_ms = NULL;
    g_ra.tick_datadir[0] = '\0';
    pthread_mutex_unlock(&g_ra.lock);
    atomic_store(&g_ra.total_extended, 0);
    atomic_store(&g_ra.total_skipped_policy, 0);
    atomic_store(&g_ra.total_skipped_depth, 0);
    atomic_store(&g_ra.total_read_failures, 0);
    atomic_store(&g_ra.last_extend_unix, 0);
}
