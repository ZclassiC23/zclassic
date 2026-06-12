/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair — public entry/coordinator for the reducer-stage repair
 * helpers used by Conditions. The focused concerns live in sibling TUs:
 *   - stage_repair_header_solution.c — header-solution backfill (save/load),
 *   - stage_repair_body_fetch.c      — body-fetch candidacy detection,
 *   - stage_repair_rewind.c          — the destructive poison rewind.
 * This TU owns the boot-time tip-finalize cursor reconcile (the SAFE,
 * non-destructive repair that never writes the cursor below durable
 * tip_finalize_log finality). It reuses the shared progress.kv accessors from
 * jobs/stage_repair_internal.h. */

#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"

#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static bool tip_finalize_served_floor_unlocked(sqlite3 *db, int *out)
{
    *out = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(MAX(height), -1) FROM tip_finalize_log WHERE ok=1",
            -1, &st, NULL) != SQLITE_OK) {
        const char *msg = sqlite3_errmsg(db);
        if (msg && strstr(msg, "no such table") != NULL)
            return true;
        LOG_WARN("stage_repair",
                 "[stage_repair] served floor prepare failed: %s",
                 msg ? msg : "(no message)");
        return false;
    }

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] served floor step failed: %s",
                 sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

bool stage_reconcile_clamp_tip_finalize_to_floor(
    sqlite3 *db, int coins_best, struct stage_reconcile_result *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db)
        return false;

    /* No durable applied anchor → nothing to floor on; leave the chain alone. */
    if (coins_best < 0)
        return true;

    if (!stage_table_ensure(db))
        return false;

    progress_store_tx_lock();

    int served_floor = -1;
    if (!tip_finalize_served_floor_unlocked(db, &served_floor)) {
        progress_store_tx_unlock();
        return false;
    }

    /* The tip_finalize cursor floor is the served/applied tip's OWN height —
     * never +1 (task #31, unifying the last two +1 conventions with the
     * task-#30 authority-anchor fix). `coins_best` here IS the served tip
     * height: boot_derive_coins_best returns coins_applied_height-1, and the
     * utxo_apply stage co-commits coins_applied_height == (highest applied
     * height)+1, so coins_best == highest applied block height. Cursor C means
     * "served tip at C; the C→C+1 transition is pending"; clamping to
     * coins_best+1 claims the coins_best→coins_best+1 transition that nothing
     * finalized and SKIPS it forever (the cursor is monotonic) — one late
     * block per restart through this repair path (block coins_best+1 could
     * only publish when coins_best+2 arrived). served_floor (the deepest
     * finalized ok=1 height) is itself a served tip height under the same
     * convention, so the stronger floor is served_floor, not served_floor+1. */
    int floor = coins_best;
    if (served_floor >= 0 && served_floor > floor)
        floor = served_floor;
    if (out)
        out->floor = floor;

    int cur = -1;
    if (!stage_repair_cursor_at_unlocked(db, "tip_finalize", &cur)) {
        progress_store_tx_unlock();
        return false;
    }

    /* Cursor target is the stronger of the coins frontier and already-served
     * finality (both served-tip heights). Never lowering below served_floor
     * prevents a boot-time public tip regression when stale coins_best metadata
     * lags durable finalized rows. If the cursor is behind served finality,
     * advancing it to the target only skips rows that tip_finalize_log already
     * proved ok=1. */
    if (cur == floor) {
        progress_store_tx_unlock();
        return true;   /* no-op, clamped=false */
    }

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tip_finalize clamp BEGIN failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    /* Reconcile ONLY the tip_finalize cursor. No log deletions, no upstream
     * cursor changes — the upstream evidence stays intact, and the surviving
     * tip_finalize_log rows keep the public-tip floor at served_floor. */
    if (!stage_repair_force_stage_cursor(db, "tip_finalize", floor)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tip_finalize clamp COMMIT failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();

    if (out)
        out->clamped = true;
    LOG_WARN("stage_repair",
             "[stage_repair] reducer cursor reconcile: tip_finalize cursor "
             "%d -> %d (coins_best=%d served_floor=%d); no logs deleted, "
             "public tip remains floored at served finality",
             cur, floor, coins_best, served_floor);
    return true;
}
