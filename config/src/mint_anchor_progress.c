/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/mint_anchor_progress.h"

#include "chain/checkpoints.h"
#include "jobs/refold_progress.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MINT_ANCHOR_MARKER_LEN 48

static void map_wle32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t map_rle32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void map_wle64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t map_rle64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

static void marker_encode(uint8_t out[MINT_ANCHOR_MARKER_LEN],
                          const struct sha3_utxo_checkpoint *cp)
{
    memcpy(out, "ZAM1", 4);
    map_wle32(out + 4, (uint32_t)cp->height);
    map_wle64(out + 8, cp->utxo_count);
    memcpy(out + 16, cp->sha3_hash, 32);
}

static bool marker_matches(const uint8_t in[MINT_ANCHOR_MARKER_LEN],
                           const struct sha3_utxo_checkpoint *cp)
{
    if (memcmp(in, "ZAM1", 4) != 0)
        return false;
    if ((int32_t)map_rle32(in + 4) != cp->height)
        return false;
    if (map_rle64(in + 8) != cp->utxo_count)
        return false;
    return memcmp(in + 16, cp->sha3_hash, 32) == 0;
}

static bool read_matching_marker(sqlite3 *db,
                                 const struct sha3_utxo_checkpoint *cp,
                                 bool *found_out, bool *matches_out)
{
    if (found_out)
        *found_out = false;
    if (matches_out)
        *matches_out = false;
    if (!db || !cp)
        return false;

    uint8_t buf[MINT_ANCHOR_MARKER_LEN] = {0};
    size_t n = 0;
    bool found = false;
    if (!progress_meta_get(db, MINT_ANCHOR_IN_PROGRESS_KEY,
                           buf, sizeof(buf), &n, &found))
        LOG_FAIL("mint_anchor", "progress marker read failed");

    if (found_out)
        *found_out = found;
    if (!found)
        return true;

    bool matches = n == sizeof(buf) && marker_matches(buf, cp);
    if (matches_out)
        *matches_out = matches;
    return true;
}

static bool applied_through(sqlite3 *db, int32_t *out, bool *found_out)
{
    if (out)
        *out = -1;
    if (found_out)
        *found_out = false;
    int32_t frontier = 0;
    bool found = false;
    if (!coins_kv_get_applied_height(db, &frontier, &found))
        LOG_FAIL("mint_anchor", "coins_applied_height read failed");
    if (found_out)
        *found_out = found;
    if (out)
        *out = found ? frontier - 1 : -1;
    return true;
}

bool mint_anchor_progress_mark(sqlite3 *db,
                               const struct sha3_utxo_checkpoint *cp)
{
    if (!db || !cp)
        LOG_FAIL("mint_anchor", "mark: invalid args db=%p cp=%p",
                 (void *)db, (const void *)cp);
    uint8_t marker[MINT_ANCHOR_MARKER_LEN] = {0};
    marker_encode(marker, cp);
    if (!progress_meta_set(db, MINT_ANCHOR_IN_PROGRESS_KEY,
                           marker, sizeof(marker)))
        LOG_FAIL("mint_anchor", "mark: progress_meta_set failed");
    return true;
}

bool mint_anchor_progress_clear(sqlite3 *db)
{
    if (!db)
        LOG_FAIL("mint_anchor", "clear: NULL db");
    if (!progress_meta_delete(db, MINT_ANCHOR_IN_PROGRESS_KEY))
        LOG_FAIL("mint_anchor", "clear: progress_meta_delete failed");
    return true;
}

bool mint_anchor_progress_can_resume(sqlite3 *db,
                                     const struct sha3_utxo_checkpoint *cp,
                                     int32_t *applied_through_out,
                                     bool *legacy_adopted_out)
{
    if (applied_through_out)
        *applied_through_out = -1;
    if (legacy_adopted_out)
        *legacy_adopted_out = false;
    if (!db || !cp)
        LOG_FAIL("mint_anchor", "can_resume: invalid args db=%p cp=%p",
                 (void *)db, (const void *)cp);

    bool found = false;
    bool matches = false;
    if (!read_matching_marker(db, cp, &found, &matches))
        return false;  /* logged */

    int32_t through = -1;
    bool have_frontier = false;
    if (!applied_through(db, &through, &have_frontier))
        return false;  /* logged */

    if (found && !matches) {
        LOG_WARN("mint_anchor",
                 "[mint-anchor] existing progress marker targets a different "
                 "checkpoint — resetting the offline mint from genesis");
        return false;
    }

    if (found && matches) {
        if (have_frontier && through > cp->height) {
            LOG_WARN("mint_anchor",
                     "[mint-anchor] progress marker found but frontier "
                     "applied-through=%d is past anchor=%d — resetting",
                     through, cp->height);
            return false;
        }
        if (applied_through_out)
            *applied_through_out = through;
        return true;
    }

    /* Legacy adoption: the 2026-07-01 producer predates this marker and may
     * have been killed after durable stage/coins commits. Only adopt when the
     * durable refold signal is active and the frontier is within the
     * genesis..anchor mint span. A contaminated or wrong fold cannot publish:
     * boot_mint_anchor_run still hard-asserts SHA3/count before writing. */
    if (!refold_in_progress() || !have_frontier || through < -1 ||
        through > cp->height)
        return false;

    if (!mint_anchor_progress_mark(db, cp))
        return false;  /* logged */
    if (legacy_adopted_out)
        *legacy_adopted_out = true;
    if (applied_through_out)
        *applied_through_out = through;
    LOG_INFO("mint_anchor",
             "[mint-anchor] adopted legacy interrupted mint at "
             "applied-through=%d for checkpoint h=%d; future restarts resume "
             "without a genesis reset",
             through, cp->height);
    return true;
}
