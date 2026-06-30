/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * proof_validate_stage_dump — the zcl_state JSON dump for the proof_validate
 * Job. Reads the stage-owned state through sibling-private accessors so the
 * reducer step logic stays focused on validation and cursor movement.
 */

#include "platform/time_compat.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/stage_helpers.h"
#include "proof_validate_stage_internal.h"

#include "core/uint256.h"
#include "json/json.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

#define STAGE_NAME "proof_validate"

/* Surface the lowest ok=0 row (status/proof_type/txid) into `out`, mirroring
 * the validate_headers_report failure-summary query convention. No-op if the
 * db is unavailable or there is no failing row. Takes its own tx lock since
 * dump_state runs outside any stage txn. */
static void dump_first_failure(struct json_value *out, sqlite3 *db)
{
    if (!db)
        return;
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT height, COALESCE(status,''), "
        "       COALESCE(first_failure_proof_type,''), first_failure_txid "
        "  FROM proof_validate_log WHERE ok=0 "
        " ORDER BY height ASC LIMIT 1",
        -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        json_push_kv_int(out, "first_failure_height",
                         sqlite3_column_int64(st, 0));
        const unsigned char *status = sqlite3_column_text(st, 1);
        const unsigned char *ptype = sqlite3_column_text(st, 2);
        json_push_kv_str(out, "first_failure_status",
                         status ? (const char *)status : "");
        json_push_kv_str(out, "first_failure_proof_type",
                         ptype ? (const char *)ptype : "");
        const void *blob = sqlite3_column_blob(st, 3);
        char hex[65] = {0};
        if (blob && sqlite3_column_bytes(st, 3) == 32) {
            struct uint256 t;
            memcpy(t.data, blob, 32);
            uint256_get_hex(&t, hex);
        }
        json_push_kv_str(out, "first_failure_txid", hex);
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();
}

bool proof_validate_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        LOG_FAIL("proof_validate", "dump_state: output is NULL");
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = proof_validate_stage_last_step_unix();
    stage_t *stage = proof_validate_stage_handle();

    stage_dump_header(out, STAGE_NAME, stage);
    json_push_kv_int(out, "verified_total",
                     (int64_t)proof_validate_stage_verified_total());
    json_push_kv_int(out, "proof_invalid_total",
                     (int64_t)proof_validate_stage_proof_invalid_total());
    json_push_kv_int(out, "internal_error_total",
                     (int64_t)proof_validate_stage_internal_error_total());
    json_push_kv_int(out, "upstream_failed_total",
                     (int64_t)proof_validate_stage_upstream_failed_total());
    json_push_kv_int(out, "sapling_spends_verified_total",
                     (int64_t)proof_validate_stage_sapling_spends_verified_total());
    json_push_kv_int(out, "sapling_spends_failed_total",
                     (int64_t)proof_validate_stage_sapling_spends_failed_total());
    json_push_kv_int(out, "sapling_outputs_verified_total",
                     (int64_t)proof_validate_stage_sapling_outputs_verified_total());
    json_push_kv_int(out, "sapling_outputs_failed_total",
                     (int64_t)proof_validate_stage_sapling_outputs_failed_total());
    json_push_kv_int(out, "sprout_groth16_verified_total",
                     (int64_t)proof_validate_stage_sprout_groth16_verified_total());
    json_push_kv_int(out, "sprout_groth16_failed_total",
                     (int64_t)proof_validate_stage_sprout_groth16_failed_total());
    json_push_kv_int(out, "sprout_phgr13_verified_total",
                     (int64_t)proof_validate_stage_sprout_phgr13_verified_total());
    json_push_kv_int(out, "sprout_phgr13_failed_total",
                     (int64_t)proof_validate_stage_sprout_phgr13_failed_total());
    json_push_kv_int(out, "binding_sig_verified_total",
                     (int64_t)proof_validate_stage_binding_sig_verified_total());
    json_push_kv_int(out, "binding_sig_failed_total",
                     (int64_t)proof_validate_stage_binding_sig_failed_total());
    json_push_kv_int(out, "last_advance_height",
                     proof_validate_stage_last_advance_height());
    json_push_kv_int(out, "last_step_unix", last);
    json_push_kv_int(out, "last_step_age_seconds",
                     last > 0 ? now - last : -1);
    json_push_kv_int(out, "last_blocked_unix",
                     proof_validate_stage_last_blocked_unix());
    json_push_kv_int(out, "log_rows",
                     db ? stage_log_row_count(db, STAGE_NAME,
                                              "proof_validate_log") : 0);
    dump_first_failure(out, db);
    stage_dump_counters(out, stage);
    return true;
}
