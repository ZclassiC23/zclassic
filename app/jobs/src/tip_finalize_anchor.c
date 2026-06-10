/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_anchor — trusted anchor/seed cursor alignment for the
 * tip_finalize stage, extracted from tip_finalize_stage.c (E1 file-size
 * ceiling). See tip_finalize_anchor_internal.h for the cross-TU seam.
 *
 * Both jump sites here are FIX-3 guarded: a trusted re-anchor or seed may
 * only advance the finalize cursor across heights its own tip_finalize_log
 * covers (stage_anchor_cap_target_at_log_frontier) — cursor jumps past
 * rowless heights are the manufacturing site of the log-hole wedge class.
 * The seed exemption is ALWAYS a pre-insert verdict: both sites write the
 * anchor row before the cursor write, so a post-insert "log empty" probe
 * can never be true (see stage_anchor.h, prong 2). */

#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_anchor.h"
#include "jobs/stage_helpers.h"
#include "tip_finalize_anchor_internal.h"
#include "tip_finalize_log_store.h"

#include "core/uint256.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

#define STAGE_NAME "tip_finalize"

static bool ensure_authority_anchor_row(sqlite3 *db, int height,
                                        const uint8_t hash[32])
{
    struct finalized_tip_row row;
    if (!finalized_tip_row_at(db, height, &row))
        return false;
    if (row.found && row.ok && row.has_tip_hash &&
        memcmp(row.tip_hash.data, hash, 32) == 0)
        return true;

    struct uint256 tip_hash;
    memcpy(tip_hash.data, hash, 32);
    return log_insert(db, height, "anchor", true, NULL, 0, 0, &tip_hash);
}

bool tip_finalize_anchor_cursor_to_authority(sqlite3 *db, int height,
                                             const uint8_t hash[32],
                                             bool anchor_upstream,
                                             bool require_prior_progress,
                                             const char *reason)
{
    stage_t *stage = tip_finalize_stage_handle();
    if (!db || !stage || height < 0 || !hash)
        return true;

    uint64_t target = (uint64_t)height + 1u;
    uint64_t cursor = stage_cursor_persisted(db, STAGE_NAME, STAGE_NAME);
    /* PRE-INSERT row count: the FIX-3 seed-exemption discriminator for the
     * frontier cap below. Must be read BEFORE ensure_authority_anchor_row —
     * after the insert, "log empty" can never be true (stage_anchor.h). */
    int64_t rows = stage_log_row_count(db, STAGE_NAME, "tip_finalize_log");
    if (require_prior_progress && cursor == 0 && rows <= 0)
        return true;
    if (!ensure_authority_anchor_row(db, height, hash))
        return false;
    if (anchor_upstream &&
        !stage_anchor_upstream_cursors_to(db, target, STAGE_NAME, reason,
                                          false /* runtime re-anchor */))
        return false;
    /* FIX-3 jump site 2: never advance the finalize cursor past a rowless
     * height. A healthy restart is unchanged — the scan over
     * [cursor, height+1) is covered by the just-ensured anchor row at
     * `height`; a held/rowless span caps the jump so the stage
     * re-finalizes forward (slower, never wrong). */
    if (!stage_anchor_cap_target_at_log_frontier(db, "tip_finalize_log",
                                                 cursor, target,
                                                 rows <= 0, &target))
        return false;
    if (cursor >= target)
        return true;
    if (!stage_set_cursor(stage, db, target)) {
        LOG_WARN("tip_finalize",
                 "[tip_finalize] authority anchor cursor failed from=%llu to=%llu reason=%s",
                 (unsigned long long)cursor, (unsigned long long)target, reason ? reason : "");
        return false;
    }
    LOG_INFO("tip_finalize",
             "[tip_finalize] authority anchor cursor from=%llu to=%llu reason=%s",
             (unsigned long long)cursor, (unsigned long long)target, reason ? reason : "");
    return true;
}

void tip_finalize_stage_set_authoritative_tip(int height,
                                              const uint8_t hash[32])
{
    tip_finalize_publish_last_advance(height, hash);
    sqlite3 *db = progress_store_db();
    if (db && tip_finalize_stage_handle()) {
        progress_store_tx_lock();
        (void)tip_finalize_anchor_cursor_to_authority(db, height, hash, false,
                                                      false, "trusted_tip");
        progress_store_tx_unlock();
    }
}

bool tip_finalize_stage_seed_anchor(int height, const uint8_t hash[32],
                                    bool trusted_seed)
{
    if (height < 0 || !hash)
        return false;

    sqlite3 *db = progress_store_db();
    if (!db) {
        /* Not wired (very early boot, or unit tests without a progress
         * store). The cold-start seed is best-effort until the stage is
         * available. */
        return false;
    }
    progress_store_tx_lock();
    if (!ensure_log_schema(db)) {
        progress_store_tx_unlock();
        return false;
    }

    /* PRE-INSERT state for the FIX-3 seed-exemption verdict: a trusted
     * (SHA3-verified snapshot) seed is caller-declared; a fresh datadir is
     * recognised by the log being empty BEFORE the anchor row below is
     * written (stage_anchor.h, prong 2). */
    uint64_t cursor = stage_cursor_persisted(db, STAGE_NAME, STAGE_NAME);
    int64_t pre_insert_rows =
        stage_log_row_count(db, STAGE_NAME, "tip_finalize_log");
    bool seed_exempt = trusted_seed || pre_insert_rows == 0;

    struct uint256 tip_hash;
    memcpy(tip_hash.data, hash, 32);

    /* Snapshot/trusted anchors have no per-block work or UTXO delta. */
    if (!log_insert(db, height, "anchor", true, NULL, 0, 0, &tip_hash)) {
        progress_store_tx_unlock();
        return false;
    }

    if (!stage_anchor_upstream_cursors_to(db, (uint64_t)height + 1u,
                                          STAGE_NAME, "seed_anchor",
                                          trusted_seed)) {
        progress_store_tx_unlock();
        return false;
    }

    /* FIX-3 jump site 3: cap the self-stamp at the tip_finalize_log
     * frontier. The just-written anchor row covers `height`, so an at-tip
     * ingest re-seed stays a benign no-op-or-contiguous stamp; a stamp
     * across a rowless span is capped so the reducer re-finalizes forward
     * instead of manufacturing a hole behind the cursor. */
    uint64_t stamp = (uint64_t)height + 1u;
    if (!stage_anchor_cap_target_at_log_frontier(db, "tip_finalize_log",
                                                 cursor, stamp, seed_exempt,
                                                 &stamp)) {
        progress_store_tx_unlock();
        return false;
    }

    /* Resume after the anchor; rebuild reads cursor-1 == anchor. A capped
     * re-anchor lands BELOW height+1: boot paths that assume the anchor is
     * at cursor-1 must tolerate it (the reducer re-finalizes forward from
     * the frontier — slower, never wrong). */
    stage_t *stage = tip_finalize_stage_handle();
    if (stage && !stage_set_cursor(stage, db, stamp)) {
        LOG_WARN("tip_finalize",
                 "[tip_finalize] anchor seed: cursor stamp to %llu failed",
                 (unsigned long long)stamp);
        progress_store_tx_unlock();
        return false;
    }

    /* Publish immediately, not only after the next boot. */
    tip_finalize_publish_last_advance(height, hash);
    progress_store_tx_unlock();
    return true;
}
