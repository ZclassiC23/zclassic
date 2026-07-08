/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage_fallback — hash-bound apply candidate recovery.
 *
 * The normal apply path reads active_chain[height]. During bootstrap/refold
 * the visible active-chain window can lag durable reducer rows even though the
 * block map already holds the body. This helper allows exactly that narrow
 * case without trusting height alone. */

#include "utxo_apply_stage_internal.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "script_validate_log_store.h"
#include "jobs/tip_finalize_stage.h"
#include "storage/disk_block_io.h"
#include "util/log_macros.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdbool.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

static struct { int64_t h; int64_t reason; uint64_t reps; }
    g_select_idle_warn = { .h = -1, .reason = UA_SELECT_IDLE_NONE };

const char *utxo_apply_select_idle_reason_name(
        enum utxo_apply_select_idle_reason reason)
{
    switch (reason) {
    case UA_SELECT_IDLE_NONE:
        return "none";
    case UA_SELECT_IDLE_NO_MAIN_STATE:
        return "no_main_state";
    case UA_SELECT_IDLE_ACTIVE_CHAIN_MISSING:
        return "active_chain_missing";
    case UA_SELECT_IDLE_ACTIVE_CHAIN_BODILESS:
        return "active_chain_bodiless";
    case UA_SELECT_IDLE_NO_SCRIPT_HASH:
        return "no_script_hash";
    case UA_SELECT_IDLE_BLOCK_MAP_MISS:
        return "block_map_miss";
    case UA_SELECT_IDLE_HEIGHT_MISMATCH:
        return "height_mismatch";
    case UA_SELECT_IDLE_INDEXED_BODY_MISSING:
        return "indexed_body_missing";
    case UA_SELECT_IDLE_INDEXED_BODY_READ_FAILED:
        return "indexed_body_read_failed";
    case UA_SELECT_IDLE_INDEXED_BODY_HASH_MISMATCH:
        return "indexed_body_hash_mismatch";
    case UA_SELECT_IDLE_PARENT_MISMATCH:
        return "parent_mismatch";
    case UA_SELECT_IDLE_STAGE_READ_FAILED:
        return "stage_read_failed";
    }
    return "unknown";
}

void utxo_apply_select_idle_note(int height,
        enum utxo_apply_select_idle_reason reason,
        const struct block_index *bi)
{
    atomic_fetch_add(&g_ua_select_idle_total, 1);
    atomic_store(&g_ua_select_idle_height, (int64_t)height);
    atomic_store(&g_ua_select_idle_reason, (int64_t)reason);

    bool changed = g_select_idle_warn.h != (int64_t)height ||
                   g_select_idle_warn.reason != (int64_t)reason;
    uint64_t suppressed = 0;
    if (changed) {
        suppressed = g_select_idle_warn.reps;
        g_select_idle_warn.h = (int64_t)height;
        g_select_idle_warn.reason = (int64_t)reason;
        g_select_idle_warn.reps = 0;
    } else {
        g_select_idle_warn.reps++;
        return;
    }

    unsigned int status = block_index_status_load(bi);
    int file = block_index_file_load(bi);
    unsigned int data_pos = block_index_data_pos_load(bi);
    LOG_WARN("utxo_apply",
             "[utxo_apply] select idle height=%d reason=%s "
             "bi=%p bi_height=%d status=0x%x file=%d data_pos=%u "
             "(suppressed=%llu)",
             height, utxo_apply_select_idle_reason_name(reason),
             (const void *)bi, bi ? bi->nHeight : -1, status, file, data_pos,
             (unsigned long long)suppressed);
}

static bool indexed_body_hash_verifies(const struct block_index *bi,
                                       const char *datadir,
                                       enum utxo_apply_select_idle_reason *why)
{
    if (!bi || !bi->phashBlock || !datadir || !datadir[0]) {
        if (why) *why = UA_SELECT_IDLE_INDEXED_BODY_MISSING;
        return false;
    }
    int file = block_index_file_load(bi);
    unsigned int data_pos = block_index_data_pos_load(bi);
    if (file < 0 || data_pos == 0) {
        if (why) *why = UA_SELECT_IDLE_INDEXED_BODY_MISSING;
        return false;
    }

    struct disk_block_pos pos = { .nFile = file, .nPos = data_pos };
    struct block blk;
    block_init(&blk);
    if (!read_block_from_disk_pread(&blk, &pos, datadir)) {
        block_free(&blk);
        if (why) *why = UA_SELECT_IDLE_INDEXED_BODY_READ_FAILED;
        return false;
    }
    struct uint256 got;
    block_get_hash(&blk, &got);
    bool ok = uint256_eq(&got, bi->phashBlock);
    block_free(&blk);
    if (!ok && why) *why = UA_SELECT_IDLE_INDEXED_BODY_HASH_MISMATCH;
    return ok;
}

static bool candidate_extends_visible_parent(struct main_state *ms,
                                             const struct block_index *bi,
                                             int height)
{
    if (!ms || !bi)
        return false;
    if (height == 0)
        return bi->pprev == NULL;

    struct block_index *parent =
        active_chain_at(&ms->chain_active, height - 1);
    if (!parent || !parent->phashBlock || !bi->pprev ||
        !bi->pprev->phashBlock)
        return false;
    return uint256_eq(bi->pprev->phashBlock, parent->phashBlock);
}

static bool candidate_extends_durable_parent(sqlite3 *db,
                                             const struct block_index *bi,
                                             int height)
{
    if (!db || !bi)
        return false;
    if (height == 0)
        return bi->pprev == NULL;
    if (!bi->pprev || !bi->pprev->phashBlock)
        return false;

    uint8_t parent_hash[32];
    if (!tip_finalize_stage_block_hash_at(db, height - 1, parent_hash))
        return false;  // raw-return-ok:optional-durable-parent-witness
    return memcmp(parent_hash, bi->pprev->phashBlock->data,
                  sizeof(parent_hash)) == 0;
}

static struct block_index *hash_bound_fallback(
        sqlite3 *db, struct main_state *ms, int height,
        const struct script_validate_verdict_row *sv_row,
        enum utxo_apply_select_idle_reason *why)
{
    if (!ms) {
        if (why) *why = UA_SELECT_IDLE_NO_MAIN_STATE;
        return NULL;
    }
    if (!sv_row || sv_row->ok != 1 || !sv_row->has_block_hash) {
        if (why) *why = UA_SELECT_IDLE_NO_SCRIPT_HASH;
        return NULL;
    }

    struct block_index *bi = block_map_find(&ms->map_block_index,
                                            &sv_row->block_hash);
    if (!bi) {
        if (why) *why = UA_SELECT_IDLE_BLOCK_MAP_MISS;
        return NULL;
    }
    if (bi->nHeight != height) {
        if (why) *why = UA_SELECT_IDLE_HEIGHT_MISMATCH;
        return NULL;
    }
    if ((bi->nStatus & BLOCK_HAVE_DATA) == 0) {
        char datadir[2048];
        GetDataDir(true, datadir, sizeof(datadir));
        enum utxo_apply_select_idle_reason body_why =
            UA_SELECT_IDLE_INDEXED_BODY_MISSING;
        if (!indexed_body_hash_verifies(bi, datadir, &body_why)) {
            if (why) *why = body_why;
            return NULL;
        }
        block_index_status_fetch_or(bi, BLOCK_HAVE_DATA);
    }
    if (!candidate_extends_visible_parent(ms, bi, height) &&
        !candidate_extends_durable_parent(db, bi, height)) {
        if (why) *why = UA_SELECT_IDLE_PARENT_MISMATCH;
        return NULL;
    }
    return bi;
}

struct block_index *utxo_apply_select_apply_block(
        sqlite3 *db, struct main_state *ms, int height,
        const struct script_validate_verdict_row *sv_row)
{
    if (!ms) {
        utxo_apply_select_idle_note(height, UA_SELECT_IDLE_NO_MAIN_STATE,
                                    NULL);
        return NULL;
    }

    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (bi && (bi->nStatus & BLOCK_HAVE_DATA))
        return bi;

    atomic_fetch_add(&g_ua_window_miss_total, 1);
    atomic_store(&g_ua_window_miss_height, (int64_t)height);

    enum utxo_apply_select_idle_reason why =
        bi ? UA_SELECT_IDLE_ACTIVE_CHAIN_BODILESS
           : UA_SELECT_IDLE_ACTIVE_CHAIN_MISSING;
    struct block_index *fallback = hash_bound_fallback(db, ms, height, sv_row,
                                                       &why);
    if (!fallback) {
        utxo_apply_select_idle_note(height, why, bi);
        return NULL;
    }

    atomic_fetch_add(&g_ua_hash_bound_fallback_total, 1);
    atomic_store(&g_ua_hash_bound_fallback_height, (int64_t)height);
    return fallback;
}
