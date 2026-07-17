/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_frontier_itag — implementation. See reducer_frontier_itag.h. */

#include "reducer_frontier_itag.h"

#include "event/event.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/log_throttle.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int_least32_t g_itag_verify_watermark = -1;  /* -1: nothing yet */

int32_t reducer_frontier_itag_watermark(void)
{
    return (int32_t)atomic_load(&g_itag_verify_watermark);
}

void reducer_frontier_itag_watermark_publish(int32_t hstar)
{
    atomic_store(&g_itag_verify_watermark, (int_least32_t)hstar);
}

void reducer_frontier_itag_watermark_reset(void)
{
    atomic_store(&g_itag_verify_watermark, -1);
}

/* FNV-1a/16 of the (short, fixed) table name — only used to key the throttle so
 * a persistent per-(table,height) mismatch de-storms instead of spamming every
 * fold. Collisions merely merge two throttles, which is harmless. */
static uint32_t itag_table_key16(const char *t)
{
    uint32_t h = 2166136261u;
    for (; t && *t; t++)
        h = (h ^ (uint8_t)*t) * 16777619u;
    return h & 0xffffu;
}

void reducer_frontier_itag_mismatch_warn(const char *table, int64_t height)
{
    static struct log_throttle t = LOG_THROTTLE_INIT;
    uint64_t key = ((uint64_t)itag_table_key16(table) << 32) |
                   (uint32_t)(int32_t)height;
    int64_t now = platform_time_wall_unix();
    uint64_t reps = 0;
    if (log_throttle_should_emit(&t, key, now, 300, &reps)) {
        LOG_WARN("reducer",
                 "stage-row integrity tag MISMATCH: table=%s height=%lld — row "
                 "verdict bytes changed under its tag; treating as NOT ok and "
                 "capping H* below this height (corruption LOWERS the frontier) "
                 "repeated=%llu",
                 table, (long long)height, (unsigned long long)reps);
        event_emitf(EV_RECOVERY_ACTION, 0,
                    "action=stage_row_itag_mismatch table=%s height=%lld",
                    table, (long long)height);
    }
}

bool reducer_frontier_itag_column_present(sqlite3 *db, const char *table)
{
    char sql[96];
    int n = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (n <= 0 || n >= (int)sizeof(sql))
        return false;  // raw-return-ok:bounded-name-cannot-overflow
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;  // raw-return-ok:absent-column-means-skip-verify
    bool present = false;
    while (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        const unsigned char *name = sqlite3_column_text(st, 1);  /* col 1 = name */
        if (name && strcmp((const char *)name, "itag") == 0) {
            present = true;
            break;
        }
    }
    sqlite3_finalize(st);
    return present;
}
