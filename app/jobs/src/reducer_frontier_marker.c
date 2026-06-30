/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared one-shot progress_meta markers for reducer-frontier helpers. */

#include "stage_repair_reducer_frontier_internal.h"

#include "core/uint256.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

bool stage_reducer_frontier_repair_marker_key(
    char key[192],
    const char *repair_name,
    int height,
    const struct uint256 *block_hash)
{
    if (!key || !repair_name || !block_hash)
        LOG_FAIL("stage_repair",
                 "[stage_repair] repair marker key NULL input repair=%s h=%d",
                 repair_name ? repair_name : "(null)", height);

    char hex[65];
    uint256_get_hex(block_hash, hex);
    int n = snprintf(key, 192, "reducer_frontier.%s_repair.%d.%s",
                     repair_name, height, hex);
    if (n <= 0 || n >= 192)
        LOG_FAIL("stage_repair",
                 "[stage_repair] repair marker key overflow repair=%s h=%d",
                 repair_name, height);
    return true;
}

bool stage_reducer_frontier_repair_marker_seen(
    sqlite3 *db,
    const char *key,
    const char *label,
    bool *seen)
{
    if (!db || !key || !seen)
        LOG_FAIL("stage_repair",
                 "[stage_repair] repair marker read NULL input label=%s key=%s",
                 label ? label : "(null)", key ? key : "(null)");

    *seen = false;
    uint8_t blob[8] = {0};
    size_t n = 0;
    if (!progress_meta_get(db, key, blob, sizeof(blob), &n, seen))
        LOG_FAIL("stage_repair",
                 "[stage_repair] %s marker read failed key=%s",
                 label ? label : "repair", key);
    return true;
}

bool stage_reducer_frontier_repair_marker_record_in_tx(
    sqlite3 *db,
    const char *key,
    const char *label)
{
    if (!db || !key)
        LOG_FAIL("stage_repair",
                 "[stage_repair] repair marker write NULL input label=%s key=%s",
                 label ? label : "(null)", key ? key : "(null)");

    uint8_t one = 1;
    if (!progress_meta_set_in_tx(db, key, &one, sizeof(one)))
        LOG_FAIL("stage_repair",
                 "[stage_repair] %s marker write failed key=%s",
                 label ? label : "repair", key);
    return true;
}
