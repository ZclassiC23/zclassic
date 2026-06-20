/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * refold_progress — see refold_progress.h. The durable key lives in
 * progress.kv (progress_meta); a cached atomic answers the hot-path reader. */

#include "jobs/refold_progress.h"

#include "jobs/reducer_frontier.h"   /* REDUCER_FRONTIER_TRUSTED_ANCHOR */
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <stdatomic.h>
#include <string.h>

/* Cached answer for the hot path. Refreshed at boot + on every mark/clear.
 * Conservative default: false (== normal boot, floor stays at the anchor). */
static _Atomic bool g_refold_in_progress = false;

/* B2 — the from-ANCHOR sub-mode cache. A from-anchor refold sets BOTH this and
 * g_refold_in_progress; this one keeps the L0 floor at the compiled anchor (the
 * fold starts AT the anchor) instead of letting refold_in_progress() drop it to
 * 0 as the from-genesis refold does. Conservative default false (normal boot). */
static _Atomic bool g_refold_from_anchor = false;

bool refold_in_progress(void)
{
    return atomic_load(&g_refold_in_progress);
}

bool refold_from_anchor_active(void)
{
    return atomic_load(&g_refold_from_anchor);
}

bool refold_progress_refresh(sqlite3 *db)
{
    if (!db)
        LOG_FAIL("refold", "refresh: NULL db");

    uint8_t blob[1] = {0};
    size_t n = 0;
    bool found = false;
    uint8_t ablob[1] = {0};
    size_t an = 0;
    bool afound = false;
    progress_store_tx_lock();
    bool ok = progress_meta_get(db, REFOLD_IN_PROGRESS_KEY,
                                blob, sizeof(blob), &n, &found);
    bool aok = progress_meta_get(db, REFOLD_FROM_ANCHOR_KEY,
                                 ablob, sizeof(ablob), &an, &afound);
    progress_store_tx_unlock();
    if (!ok || !aok) {
        /* On a read error keep both caches conservatively false: a stuck-true
         * cache would silently suspend the self-repair on a normal boot. */
        atomic_store(&g_refold_in_progress, false);
        atomic_store(&g_refold_from_anchor, false);
        LOG_FAIL("refold", "refresh: progress_meta_get failed");
    }

    bool on = found && n == 1 && blob[0] == 0x01;
    atomic_store(&g_refold_in_progress, on);
    bool aon = afound && an == 1 && ablob[0] == 0x01;
    atomic_store(&g_refold_from_anchor, aon);
    return true;
}

bool refold_progress_mark_started(sqlite3 *db)
{
    if (!db)
        LOG_FAIL("refold", "mark_started: NULL db");

    const uint8_t one = 0x01;
    progress_store_tx_lock();
    bool ok = progress_meta_set(db, REFOLD_IN_PROGRESS_KEY, &one, sizeof(one));
    progress_store_tx_unlock();
    if (!ok)
        LOG_FAIL("refold", "mark_started: progress_meta_set failed");

    atomic_store(&g_refold_in_progress, true);
    LOG_INFO("refold",
             "from-genesis refold marked in progress — H* floor lowered to 0 "
             "and below-anchor self-repair suspended until the fold's "
             "utxo_apply cursor passes the trusted anchor (%d)",
             (int)REDUCER_FRONTIER_TRUSTED_ANCHOR);
    return true;
}

void refold_progress_boot_init(sqlite3 *db, bool mark_started)
{
    if (!db) {
        LOG_WARN("refold", "boot_init: NULL db — cache stays false");
        return;
    }
    (void)refold_progress_refresh(db);
    if (mark_started)
        (void)refold_progress_mark_started(db);
}

bool refold_progress_clear_if_crossed(sqlite3 *db, int32_t utxo_apply_cursor)
{
    if (!db)
        LOG_FAIL("refold", "clear_if_crossed: NULL db");

    /* Cheap pre-check off the cache: nothing to clear when not refolding. */
    if (!atomic_load(&g_refold_in_progress))
        return true;
    if (utxo_apply_cursor < REDUCER_FRONTIER_TRUSTED_ANCHOR)
        return true;  /* still below the floor — keep refolding */

    progress_store_tx_lock();
    /* Confirm against the durable key (the cache could be a stale true from a
     * crash window). Absent key => already cleared, nothing to do. */
    uint8_t blob[1] = {0};
    size_t n = 0;
    bool found = false;
    bool ok = progress_meta_get(db, REFOLD_IN_PROGRESS_KEY,
                                blob, sizeof(blob), &n, &found);
    if (ok && found)
        ok = progress_meta_delete(db, REFOLD_IN_PROGRESS_KEY);
    progress_store_tx_unlock();
    if (!ok)
        LOG_FAIL("refold", "clear_if_crossed: progress_meta read/delete failed");

    atomic_store(&g_refold_in_progress, false);
    LOG_INFO("refold",
             "from-genesis refold crossed the trusted anchor (%d) at "
             "utxo_apply cursor=%d — cleared refold_in_progress; H* floor "
             "and self-repair restored to normal",
             (int)REDUCER_FRONTIER_TRUSTED_ANCHOR, (int)utxo_apply_cursor);
    return true;
}

/* ── from-ANCHOR refold ─────────────────────────────────────────────────── */

bool refold_progress_mark_started_from_anchor(sqlite3 *db, int32_t resume_target)
{
    if (!db)
        LOG_FAIL("refold", "mark_started_from_anchor: NULL db");

    /* Store the resume target little-endian (a stable, byte-order-independent
     * int32 blob, matching the coins_applied_height encoding convention). */
    uint8_t tbuf[4] = {
        (uint8_t)(resume_target & 0xff),
        (uint8_t)((resume_target >> 8) & 0xff),
        (uint8_t)((resume_target >> 16) & 0xff),
        (uint8_t)((resume_target >> 24) & 0xff),
    };
    const uint8_t one = 0x01;
    progress_store_tx_lock();
    /* Set BOTH the shared in-progress key (so refold_in_progress() + the
     * mirror-sync guard hold) AND the from-anchor sub-mode key + target. */
    bool ok = progress_meta_set(db, REFOLD_IN_PROGRESS_KEY, &one, sizeof(one));
    if (ok)
        ok = progress_meta_set(db, REFOLD_FROM_ANCHOR_KEY, &one, sizeof(one));
    if (ok)
        ok = progress_meta_set(db, REFOLD_FROM_ANCHOR_TARGET_KEY,
                               tbuf, sizeof(tbuf));
    progress_store_tx_unlock();
    if (!ok)
        LOG_FAIL("refold", "mark_started_from_anchor: progress_meta_set failed");

    atomic_store(&g_refold_in_progress, true);
    atomic_store(&g_refold_from_anchor, true);
    LOG_INFO("refold",
             "from-ANCHOR refold marked in progress — H* floor HELD at the "
             "compiled anchor (%d) and below-anchor self-repair suspended until "
             "the fold's utxo_apply cursor reaches the resume target (%d)",
             (int)REDUCER_FRONTIER_TRUSTED_ANCHOR, (int)resume_target);
    return true;
}

bool refold_progress_clear_if_reached(sqlite3 *db, int32_t utxo_apply_cursor,
                                      int32_t target)
{
    if (!db)
        LOG_FAIL("refold", "clear_if_reached: NULL db");

    /* Cheap pre-check off the cache: nothing to clear when not from-anchor. */
    if (!atomic_load(&g_refold_from_anchor))
        return true;

    progress_store_tx_lock();
    /* When the caller passes target < 0, read the durable resume target written
     * by mark_started_from_anchor (the single place the LE int32 is decoded). */
    if (target < 0) {
        uint8_t tbuf[4] = {0};
        size_t tn = 0;
        bool tfound = false;
        if (progress_meta_get(db, REFOLD_FROM_ANCHOR_TARGET_KEY,
                              tbuf, sizeof(tbuf), &tn, &tfound) &&
            tfound && tn == 4)
            target = (int32_t)((uint32_t)tbuf[0] | ((uint32_t)tbuf[1] << 8) |
                               ((uint32_t)tbuf[2] << 16) |
                               ((uint32_t)tbuf[3] << 24));
    }
    if (target > 0 && utxo_apply_cursor < target) {
        progress_store_tx_unlock();
        return true;  /* still below the target — keep refolding */
    }

    /* Confirm against the durable key (the cache could be a stale true from a
     * crash window). Absent key => already cleared, nothing to do. */
    uint8_t blob[1] = {0};
    size_t n = 0;
    bool found = false;
    bool ok = progress_meta_get(db, REFOLD_FROM_ANCHOR_KEY,
                                blob, sizeof(blob), &n, &found);
    if (ok && found) {
        ok = progress_meta_delete(db, REFOLD_FROM_ANCHOR_KEY);
        if (ok)
            ok = progress_meta_delete(db, REFOLD_FROM_ANCHOR_TARGET_KEY);
        if (ok)
            ok = progress_meta_delete(db, REFOLD_IN_PROGRESS_KEY);
    }
    progress_store_tx_unlock();
    if (!ok)
        LOG_FAIL("refold", "clear_if_reached: progress_meta read/delete failed");

    atomic_store(&g_refold_from_anchor, false);
    atomic_store(&g_refold_in_progress, false);
    LOG_INFO("refold",
             "from-anchor refold reached the resume target (%d) at utxo_apply "
             "cursor=%d — cleared refold_from_anchor + refold_in_progress; H* "
             "floor and self-repair restored to normal",
             (int)target, (int)utxo_apply_cursor);
    return true;
}

#ifdef ZCL_TESTING
void refold_progress_test_set_cached(bool in_progress)
{
    atomic_store(&g_refold_in_progress, in_progress);
}
#endif
