/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot_fast_restart.h"

#include "config/boot_shutdown_marker.h"
#include "models/database.h"
#include "util/thread_registry.h"
#include "util/safe_alloc.h"
#include "event/event.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void boot_fast_restart_arm_quick_check_skip_probe(void)
{
    /* Let node_db_open skip PRAGMA quick_check when the previous shutdown wrote
     * a matching content binding (consumed from the cache detect_unclean
     * populated). Must run BEFORE node.db opens. */
    node_db_set_quick_check_skip_probe(boot_shutdown_marker_quick_check_probe);
}

/* Runs one quick_check on a fresh read-only connection (no contention with the
 * live write handle). A failure is raised LOUDLY via EV_DB_ERROR +
 * EV_OPERATOR_NEEDED (the latter latches DEGRADED in the health surface until
 * an operator/MCP acts) — never silent. */
static void *boot_bg_quick_check_entry(void *arg)
{
    char *path = (char *)arg;
    if (!path)
        return NULL;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        /* Could not even open read-only — surface it, but do not escalate to
         * OPERATOR_NEEDED (the live write handle owns the authoritative view). */
        event_emitf(EV_DB_ERROR, 0,
                    "bg_quick_check open failed rc=%d path=%s", rc, path);
        free(path);
        return NULL;
    }

    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &st, NULL) ==
            SQLITE_OK &&
        st && sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:read-only-introspection
        const unsigned char *txt = sqlite3_column_text(st, 0);
        ok = txt && strcmp((const char *)txt, "ok") == 0;
        if (!ok) {
            fprintf(stderr,  // obs-ok:operator-surface-is-the-alert
                    "[ALERT] bg_quick_check: node.db integrity FAILED after a "
                    "verified-clean quick_check skip: %s\n",
                    txt ? (const char *)txt : "(no detail)");
            event_emitf(EV_DB_ERROR, 0,
                        "bg_quick_check failed result=%s",
                        txt ? (const char *)txt : "unknown");
            event_emitf(EV_OPERATOR_NEEDED, 0,
                        "condition=bg_quick_check_failed detail=node_db_integrity");
        }
    } else {
        event_emitf(EV_DB_ERROR, 0,
                    "bg_quick_check step failed: %s", sqlite3_errmsg(db));
    }
    if (st)
        sqlite3_finalize(st);
    sqlite3_close(db);

    if (ok)
        printf("[boot] bg_quick_check ok (verified-clean skip confirmed)\n");
    free(path);
    return NULL;
}

void boot_fast_restart_start_bg_quick_check(const char *datadir)
{
    if (!boot_shutdown_marker_quick_check_was_skipped() || !datadir)
        return;

    char *path = zcl_malloc(1088, "bg_quick_check_path");
    if (!path)
        return;
    int n = snprintf(path, 1088, "%s/node.db", datadir);
    if (n < 0 || n >= 1088) {
        free(path);
        return;
    }
    if (thread_registry_spawn("bg_quick_check",
                              boot_bg_quick_check_entry, path) != 0) {
        fprintf(stderr,
                "WARNING: failed to spawn background quick_check thread\n");
        free(path);
    }
}
