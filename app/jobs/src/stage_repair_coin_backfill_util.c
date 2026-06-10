/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_coin_backfill_util — read-only progress.kv helpers, key
 * builders, direct refusal paging, and the zcl_state snapshot for the
 * frontier coin backfill (docs/work/coin-backfill-repair.md §2). The
 * orchestration + the single write transaction live in
 * stage_repair_coin_backfill.c; this TU never writes consensus state. */

#include "stage_repair_coin_backfill_util.h"

#include "event/event.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_LATCH_SLOTS 8

struct page_latch {
    bool used;
    int height;
    int status;
    uint8_t hash[32];
};

/* Snapshot + page latch state. Written on the self_heal supervisor thread,
 * read by the zcl_state dumper on the MCP thread — guarded by g_state_lock;
 * monotonic counters are lock-free atomics. */
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct coin_backfill_result g_last_result;
static int64_t g_last_call_unix;
static struct page_latch g_latch[PAGE_LATCH_SLOTS];
static unsigned g_latch_next;

static _Atomic uint64_t g_calls_total;
static _Atomic uint64_t g_repaired_total;
static _Atomic uint64_t g_inserted_total;
static _Atomic uint64_t g_rebind_total;
static _Atomic uint64_t g_pages_emitted_total;
static _Atomic uint64_t g_pages_suppressed_total;

bool coin_backfill_owner_ack(void)
{
    const char *v = getenv(COIN_BACKFILL_ACK_ENV);
    return v && strcmp(v, "1") == 0;
}

const char *coin_backfill_status_name(enum coin_backfill_status st)
{
    switch (st) {
    case COIN_BACKFILL_NOT_APPLICABLE:     return "not_applicable";
    case COIN_BACKFILL_SCANNING:           return "scanning";
    case COIN_BACKFILL_REPAIRED:           return "repaired";
    case COIN_BACKFILL_OWNER_REFUSED:      return "owner_refused";
    case COIN_BACKFILL_REFUSED_SPENT:      return "refused_spent";
    case COIN_BACKFILL_REFUSED_UNPROVABLE: return "refused_unprovable";
    case COIN_BACKFILL_MARKER_SEEN:        return "marker_seen";
    }
    return "unknown";
}

void coin_backfill_txid_hex(const uint8_t txid[32], char out[65])
{
    struct uint256 u;
    memcpy(u.data, txid, 32);
    uint256_get_hex(&u, out);
}

bool coin_backfill_key_h_hash(char out[192], const char *prefix, int height,
                              const struct uint256 *hash)
{
    if (!out || !prefix || !hash)
        LOG_FAIL("coin_backfill", "[coin_backfill] key_h_hash NULL input");
    char hex[65];
    uint256_get_hex(hash, hex);
    int n = snprintf(out, 192, "%s.%d.%s", prefix, height, hex);
    if (n <= 0 || n >= 192)
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] key overflow prefix=%s h=%d", prefix,
                 height);
    return true;
}

bool coin_backfill_outpoint_marker_key(char out[160], const uint8_t txid[32],
                                       uint32_t vout)
{
    char hex[65];
    coin_backfill_txid_hex(txid, hex);
    int n = snprintf(out, 160, "utxo_apply.coin_backfill.outpoint.%s:%u",
                     hex, vout);
    if (n <= 0 || n >= 160)
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] outpoint marker key overflow vout=%u",
                 vout);
    return true;
}

bool coin_backfill_meta_present(struct sqlite3 *db, const char *key,
                                bool *present)
{
    uint8_t blob[8];
    size_t n = 0;
    *present = false;
    if (!progress_meta_get(db, key, blob, sizeof(blob), &n, present))
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] meta read failed key=%s", key);
    return true;
}

bool coin_backfill_rounds_read(struct sqlite3 *db, const char *key,
                               int32_t *out)
{
    uint8_t blob[8];
    size_t n = 0;
    bool found = false;
    *out = 0;
    if (!progress_meta_get(db, key, blob, sizeof(blob), &n, &found))
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] rounds read failed key=%s", key);
    if (found && n >= 4)
        *out = coin_backfill_le32_get(blob);
    return true;
}

bool find_lowest_prevout_unresolved_hole_unlocked(
    struct sqlite3 *db, int cursor, int *out_height, char status_out[32],
    struct uint256 *hash_out, bool *hash_found)
{
    *out_height = -1;
    status_out[0] = '\0';
    *hash_found = false;
    if (cursor <= 0)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height, status, block_hash FROM script_validate_log "
            "WHERE ok = 0 "
            "  AND status IN ('internal_error', 'prevout_unresolved', "
            "                 'block_decode_failed') "
            "  AND height < ? "
            "ORDER BY height LIMIT 1",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] hole prepare failed: %s",
                 sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, cursor);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
        const unsigned char *s = sqlite3_column_text(st, 1);
        snprintf(status_out, 32, "%s", s ? (const char *)s : "");
        if (sqlite3_column_bytes(st, 2) == 32 && sqlite3_column_blob(st, 2)) {
            memcpy(hash_out->data, sqlite3_column_blob(st, 2), 32);
            *hash_found = true;
        }
    } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(st);
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] hole step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return true;
}

/* delta-horizon walk bound: the contiguous covered window on a wedged
 * datadir is a few thousand heights; if the walk has not found a gap after
 * this many steps the horizon is reported as unbounded (-1) and the repair
 * REFUSES rather than guessing a permissive floor. */
#define COIN_BACKFILL_HORIZON_WALK_MAX 262144

bool utxo_apply_log_contiguous_floor(struct sqlite3 *db, int cursor,
                                     int *out_floor)
{
    *out_floor = cursor;
    if (cursor <= 0)
        return true;

    sqlite3_stmt *log_st = NULL, *delta_st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM utxo_apply_log WHERE height = ?",
            -1, &log_st, NULL) != SQLITE_OK)
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] horizon log prepare failed: %s",
                 sqlite3_errmsg(db));
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM utxo_apply_delta WHERE height = ?",
            -1, &delta_st, NULL) != SQLITE_OK) {
        sqlite3_finalize(log_st);
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] horizon delta prepare failed: %s",
                 sqlite3_errmsg(db));
    }

    int floor = cursor;
    bool sql_ok = true;
    for (int h = cursor - 1, steps = 0; h >= 0; h--, steps++) {
        if (steps >= COIN_BACKFILL_HORIZON_WALK_MAX) {
            floor = -1; /* unbounded walk: refuse rather than guess */
            break;
        }
        sqlite3_reset(log_st);
        sqlite3_bind_int(log_st, 1, h);
        int rc = sqlite3_step(log_st);  // raw-sql-ok:progress-kv-kernel-store
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            sql_ok = false;
            break;
        }
        bool have = rc == SQLITE_ROW;
        if (have) {
            sqlite3_reset(delta_st);
            sqlite3_bind_int(delta_st, 1, h);
            rc = sqlite3_step(delta_st);  // raw-sql-ok:progress-kv-kernel-store
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
                sql_ok = false;
                break;
            }
            have = rc == SQLITE_ROW;
        }
        if (!have)
            break;
        floor = h;
    }
    sqlite3_finalize(log_st);
    sqlite3_finalize(delta_st);
    if (!sql_ok)
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] horizon walk step failed: %s",
                 sqlite3_errmsg(db));
    *out_floor = floor;
    return true;
}

bool coin_backfill_hole_row_matches_unlocked(struct sqlite3 *db, int height,
                                             const struct uint256 *hash,
                                             bool *match)
{
    *match = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT status, block_hash FROM script_validate_log "
            "WHERE height = ? AND ok = 0",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] hole recheck prepare failed: %s",
                 sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        *match = s && strcmp((const char *)s, "prevout_unresolved") == 0 &&
                 sqlite3_column_bytes(st, 1) == 32 &&
                 sqlite3_column_blob(st, 1) &&
                 memcmp(sqlite3_column_blob(st, 1), hash->data, 32) == 0;
    } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(st);
        LOG_FAIL("coin_backfill",
                 "[coin_backfill] hole recheck step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return true;
}

/* Once-latched per (H,holehash,status); blocker_set's token bucket
 * additionally dedups the blocker itself. Direct-emit precedent:
 * app/services/src/chain_tip_watchdog.c wd_decide_restart. */
void coin_backfill_page_refusal(enum coin_backfill_status st, int h,
                                const struct uint256 *hole_hash,
                                const char *reason)
{
    uint8_t hash[32] = {0};
    if (hole_hash)
        memcpy(hash, hole_hash->data, 32);

    pthread_mutex_lock(&g_state_lock);
    for (size_t i = 0; i < PAGE_LATCH_SLOTS; i++) {
        if (g_latch[i].used && g_latch[i].height == h &&
            g_latch[i].status == (int)st &&
            memcmp(g_latch[i].hash, hash, 32) == 0) {
            pthread_mutex_unlock(&g_state_lock);
            atomic_fetch_add(&g_pages_suppressed_total, 1u);
            return;
        }
    }
    struct page_latch *slot = &g_latch[g_latch_next++ % PAGE_LATCH_SLOTS];
    slot->used = true;
    slot->height = h;
    slot->status = (int)st;
    memcpy(slot->hash, hash, 32);
    pthread_mutex_unlock(&g_state_lock);

    char id[BLOCKER_ID_MAX];
    enum blocker_class cls;
    const char *escape;
    switch (st) {
    case COIN_BACKFILL_OWNER_REFUSED:
        snprintf(id, sizeof(id), "coin_backfill.owner_gate");
        cls = BLOCKER_DEPENDENCY;
        escape = "export " COIN_BACKFILL_ACK_ENV "=1 in the unit env, restart";
        break;
    case COIN_BACKFILL_REFUSED_UNPROVABLE:
        snprintf(id, sizeof(id), "coin_backfill.unprovable.%d", h);
        cls = BLOCKER_DEPENDENCY;
        escape = "fetch deep body via rebuild_recent / -cold-import";
        break;
    default: /* REFUSED_SPENT / MARKER_SEEN: refuse, never guess */
        snprintf(id, sizeof(id), "coin_backfill.%d", h);
        cls = BLOCKER_PERMANENT;
        escape = "operator: investigate lost-coin class / consensus divergence";
        break;
    }
    char btext[BLOCKER_REASON_MAX];
    snprintf(btext, sizeof(btext),
             "coin_backfill h=%d status=%s reason=%s; escape: %s",
             h, coin_backfill_status_name(st), reason, escape);
    struct blocker_record rec;
    if (blocker_init(&rec, id, "coin_backfill", cls, btext))
        blocker_set(&rec); /* dup/cap outcomes logged by blocker_set */
    event_emitf(EV_OPERATOR_NEEDED, 0,
                "coin_backfill h=%d status=%s reason=%s",
                h, coin_backfill_status_name(st), reason);
    atomic_fetch_add(&g_pages_emitted_total, 1u);
    LOG_WARN("coin_backfill",
             "[coin_backfill] OPERATOR NEEDED h=%d status=%s reason=%s "
             "escape=%s", h, coin_backfill_status_name(st), reason, escape);
}

void coin_backfill_stats_note_call(void)
{
    atomic_fetch_add(&g_calls_total, 1u);
}

void coin_backfill_stats_note_rebind(void)
{
    atomic_fetch_add(&g_rebind_total, 1u);
}

void coin_backfill_stats_note_repaired(int inserted)
{
    atomic_fetch_add(&g_repaired_total, 1u);
    atomic_fetch_add(&g_inserted_total, (uint64_t)(inserted > 0 ? inserted
                                                                : 0));
}

void coin_backfill_publish_result(const struct coin_backfill_result *r)
{
    if (!r)
        return; /* nothing to publish; dump keeps the previous snapshot */
    pthread_mutex_lock(&g_state_lock);
    g_last_result = *r;
    g_last_call_unix = platform_time_wall_unix();
    pthread_mutex_unlock(&g_state_lock);
}

void coin_backfill_reset_latches_for_testing(void)
{
    pthread_mutex_lock(&g_state_lock);
    memset(g_latch, 0, sizeof(g_latch));
    g_latch_next = 0;
    pthread_mutex_unlock(&g_state_lock);
}

bool coin_backfill_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;  // raw-return-ok:dumper-null-out-no-context
    json_set_object(out);

    pthread_mutex_lock(&g_state_lock);
    struct coin_backfill_result r = g_last_result;
    int64_t last = g_last_call_unix;
    pthread_mutex_unlock(&g_state_lock);

    json_push_kv_str(out, "last_status", coin_backfill_status_name(r.status));
    json_push_kv_int(out, "hole_height", r.hole_height);
    json_push_kv_int(out, "unresolved_count", r.unresolved_count);
    json_push_kv_int(out, "inserted_count", r.inserted_count);
    json_push_kv_int(out, "scan_next_height", r.scan_next_height);
    json_push_kv_int(out, "scan_top_height", r.scan_top_height);
    json_push_kv_int(out, "creator_floor", r.creator_floor);
    json_push_kv_int(out, "delta_horizon", r.delta_horizon);
    json_push_kv_str(out, "refuse_reason", r.refuse_reason);
    json_push_kv_int(out, "last_call_unix", last);
    json_push_kv_int(out, "calls_total",
                     (int64_t)atomic_load(&g_calls_total));
    json_push_kv_int(out, "repaired_total",
                     (int64_t)atomic_load(&g_repaired_total));
    json_push_kv_int(out, "inserted_outpoints_total",
                     (int64_t)atomic_load(&g_inserted_total));
    json_push_kv_int(out, "scan_rebind_total",
                     (int64_t)atomic_load(&g_rebind_total));
    json_push_kv_int(out, "pages_emitted_total",
                     (int64_t)atomic_load(&g_pages_emitted_total));
    json_push_kv_int(out, "pages_suppressed_total",
                     (int64_t)atomic_load(&g_pages_suppressed_total));
    json_push_kv_bool(out, "owner_ack", coin_backfill_owner_ack());
    return true;
}
