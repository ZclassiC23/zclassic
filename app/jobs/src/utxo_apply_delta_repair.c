/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_delta_repair — one-shot stale verdict repairs.
 *
 * These repairs reuse the inverse-delta machinery from
 * utxo_apply_delta_reorg.c, but they are not fork reorg handling. Each repair
 * is consensus-critical and gated by current-binary dry-runs plus a
 * (height,block_hash) one-shot marker. */

#include "jobs/utxo_apply_delta.h"
#include "utxo_apply_delta_internal.h"

#include "coins/coins.h"
#include "primitives/block.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VALUE_OVERFLOW_REPAIR_ACK_ENV \
    "ZCL_REDUCER_VALUE_OVERFLOW_REPAIR_ACK"

static bool owner_ack_value_overflow_repair(void)
{
    const char *v = getenv(VALUE_OVERFLOW_REPAIR_ACK_ENV);
    return v && strcmp(v, "1") == 0;
}

static int repair_row_still_present(sqlite3 *db, int height,
                                    const char *want_status)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok, status FROM utxo_apply_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair row prepare failed: %s",
                 sqlite3_errmsg(db));
        return -1;  // raw-return-ok:tri-state-error-logged
    }
    sqlite3_bind_int(st, 1, height);

    bool ok = false;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const unsigned char *status = sqlite3_column_text(st, 1);
        ok = sqlite3_column_int(st, 0) == 0 &&
             status && strcmp((const char *)status, want_status) == 0;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair row step failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;  // raw-return-ok:tri-state-error-logged
    }
    sqlite3_finalize(st);
    return ok ? 1 : 0;
}

static bool repair_marker_key(char key[160], const char *kind, int height,
                              const struct uint256 *block_hash)
{
    if (!key || !kind || !block_hash)
        return false;
    char hex[65];
    uint256_get_hex(block_hash, hex);
    int n = snprintf(key, 160, "utxo_apply.%s_repair.%d.%s",
                     kind, height, hex);
    return n > 0 && n < 160;
}

static bool repair_marker_seen(sqlite3 *db, const char *key, bool *seen)
{
    *seen = false;
    uint8_t blob[8] = {0};
    size_t n = 0;
    if (!progress_meta_get(db, key, blob, sizeof(blob), &n, seen)) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair marker read failed key=%s",
                 key ? key : "(null)");
        return false;
    }
    return true;
}

static bool repair_marker_record_in_tx(sqlite3 *db, const char *key)
{
    uint8_t one = 1;
    if (!progress_meta_set_in_tx(db, key, &one, sizeof(one))) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair marker write failed key=%s",
                 key ? key : "(null)");
        return false;
    }
    return true;
}

static bool repair_live_lookup(const struct uint256 *txid, uint32_t vout,
                               struct utxo_apply_lookup *out, void *user)
{
    sqlite3 *db = user;
    if (!txid || !out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!db)
        return true;

    struct coins c;
    coins_init(&c);
    if (!coins_kv_get_coins(db, txid->data, &c)) {
        coins_free(&c);
        return true;
    }

    bool ok = true;
    if (vout < c.num_vout && !tx_out_is_null(&c.vout[vout])) {
        const struct tx_out *o = &c.vout[vout];
        size_t slen = o->script_pub_key.size;
        if (slen > UTXO_APPLY_SCRIPT_MAX) {
            ok = false;
        } else {
            out->found = true;
            out->value = o->value;
            out->height = (uint32_t)(c.height < 0 ? 0 : c.height);
            out->is_coinbase = c.is_coinbase;
            out->script_len = (uint32_t)slen;
            if (slen)
                memcpy(out->script, o->script_pub_key.data, slen);
        }
    }
    coins_free(&c);
    return ok;
}

static bool dry_run_after_inverse(sqlite3 *db, int height, int cursor,
                                  const struct block *blk,
                                  struct delta_summary *dry)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair dry-run BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    for (int h = cursor - 1; h >= height; h--) {
        if (!utxo_apply_emit_inverse_delta(db, h)) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }

    utxo_apply_compute_block_delta(blk, (uint32_t)height,
                                   repair_live_lookup, db, dry);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return true;
}

bool utxo_apply_repair_value_overflow_hole(
    sqlite3 *db,
    int height,
    uint64_t cursor,
    const struct uint256 *block_hash,
    const struct block *blk,
    struct utxo_apply_value_overflow_repair_result *out)
{
    struct utxo_apply_value_overflow_repair_result local;
    memset(&local, 0, sizeof(local));
    local.height = height;
    local.cursor_before = cursor;
    local.cursor_after = cursor;
    if (out)
        *out = local;

    if (!db || !block_hash || !blk) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair refused: bad input "
                 "db=%p block_hash=%p blk=%p",
                 (void *)db, (const void *)block_hash, (const void *)blk);
        return false;
    }
    if (height < 0 || cursor == 0 || (uint64_t)height >= cursor) {
        if (out)
            *out = local;
        return true;
    }

    local.attempted = true;

    if (utxo_projection_get_author() != UTXO_AUTHOR_STAGE) {
        local.author_refused = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair refused h=%d: "
                 "utxo author is not stage",
                 height);
        if (out)
            *out = local;
        return true;
    }

    if (!owner_ack_value_overflow_repair()) {
        local.owner_refused = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair owner-gated h=%d: "
                 "set %s=1 only on an operator-approved datadir copy",
                 height, VALUE_OVERFLOW_REPAIR_ACK_ENV);
        if (out)
            *out = local;
        return true;
    }

    progress_store_tx_lock();

    int row_present = repair_row_still_present(db, height, "value_overflow");
    if (row_present < 0) {
        progress_store_tx_unlock();
        return false;
    }
    if (row_present == 0) {
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }

    struct delta_summary dry;
    if (!dry_run_after_inverse(db, height, (int)cursor, blk, &dry)) {
        progress_store_tx_unlock();
        return false;
    }
    local.dry_run_ok = dry.ok;
    if (!dry.ok) {
        local.genuinely_invalid = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair: H genuinely invalid "
                 "height=%d status=%s kind=%s",
                 height, dry.status ? dry.status : "(null)",
                 dry.failure_kind ? dry.failure_kind : "(null)");
        free_delta(&dry);
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }
    free_delta(&dry);

    char marker[160];
    if (!repair_marker_key(marker, "value_overflow", height, block_hash)) {
        progress_store_tx_unlock();
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair marker key overflow h=%d",
                 height);
        return false;
    }

    bool marker_seen = false;
    if (!repair_marker_seen(db, marker, &marker_seen)) {
        progress_store_tx_unlock();
        return false;
    }
    if (marker_seen) {
        local.marker_seen = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair skipped h=%d: "
                 "one-shot marker already present",
                 height);
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }

    int C = (int)cursor;
    if ((uint64_t)C != cursor || C <= height) {
        progress_store_tx_unlock();
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair cursor invalid h=%d "
                 "cursor=%llu",
                 height, (unsigned long long)cursor);
        return false;
    }

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    for (int h = C - 1; h >= height; h--) {
        if (!utxo_apply_emit_inverse_delta(db, h)) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            progress_store_tx_unlock();
            return false;
        }
    }
    if (!utxo_apply_delete_rows_above(db, height, C - 1)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    if (!utxo_apply_unwind_write_cursor(db, (uint64_t)height)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    if (!coins_kv_set_applied_height_in_tx(db, height)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    if (!repair_marker_record_in_tx(db, marker)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair COMMIT failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    progress_store_tx_unlock();

    local.repaired = true;
    local.cursor_after = (uint64_t)height;
    LOG_WARN("utxo_apply",
             "[utxo_apply] value_overflow repair rewound cursor %llu -> %d "
             "for stale hole h=%d",
             (unsigned long long)cursor, height, height);

    utxo_projection_t *proj = utxo_projection_get_global();
    if (proj) {
        progress_store_tx_lock();
        bool reseeded = utxo_projection_reseed_from_coins_kv(proj, db);
        progress_store_tx_unlock();
        if (!reseeded) {
            LOG_WARN("utxo_apply",
                     "[utxo_apply] value_overflow repair: projection reseed "
                     "from coins_kv failed after consensus rewind "
                     "(non-blocking)");
        }
    }
    if (out)
        *out = local;
    return true;
}
