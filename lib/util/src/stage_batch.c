/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Outer transaction lifecycle for batched stage drains. */

#include "stage_batch_internal.h"

#include "core/utiltime.h"
#include "util/stage.h"
#include "util/stage_lcc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic bool g_open;
static _Atomic bool g_dirty;
static _Atomic uint64_t g_generation;
static _Atomic int64_t g_commit_last_us;
static _Atomic int64_t g_commit_us_ewma;
static stage_batch_precommit_fn g_precommit;

/* The outer transaction makes intermediate cursor values unobservable, so one
 * exact initial..final LCC proof at COMMIT is equivalent to every prefix. */
static bool g_lcc_seen;
static char g_lcc_name[STAGE_NAME_MAX];
static uint64_t g_lcc_initial;
static uint64_t g_lcc_final;

void stage_batch_set_precommit_hook(stage_batch_precommit_fn fn)
{
    g_precommit = fn;
}

static void record_commit_timing(int64_t elapsed_us)
{
    if (elapsed_us <= 0) elapsed_us = 1;
    atomic_store(&g_commit_last_us, elapsed_us);
    int64_t prev = atomic_load(&g_commit_us_ewma);
    atomic_store(&g_commit_us_ewma,
                 prev ? prev + (elapsed_us - prev) / 16 : elapsed_us);
}

int64_t stage_batch_commit_us_ewma(void)
{
    return atomic_load(&g_commit_us_ewma);
}

bool stage_batch_active(void) { return atomic_load(&g_open); }
uint64_t stage_batch_generation(void) { return atomic_load(&g_generation); }
void stage_batch_mark_dirty(void) { atomic_store(&g_dirty, true); }
bool stage_batch_dirty(void) { return atomic_load(&g_dirty); }

bool stage_batch_defer_lcc(const char *name, uint64_t old_cursor,
                           uint64_t new_cursor)
{
    if (!atomic_load(&g_open) || !name || !name[0]) return false;
    if (!g_lcc_seen) {
        snprintf(g_lcc_name, sizeof(g_lcc_name), "%s", name);
        g_lcc_initial = old_cursor;
        g_lcc_seen = true;
    } else if (strcmp(g_lcc_name, name) != 0) {
        return false;
    }
    g_lcc_final = new_cursor;
    return true;
}

bool stage_batch_begin(sqlite3 *db)
{
    if (!db) return false;
    if (atomic_load(&g_open)) {
        fprintf(stderr, "[stage] batch_begin: a batch is already open\n");  // obs-ok:stage-begin-failure
        return false;
    }
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] batch BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    atomic_store(&g_open, true);
    atomic_store(&g_dirty, false);
    g_lcc_seen = false;
    g_lcc_name[0] = '\0';
    g_lcc_initial = g_lcc_final = 0;
    atomic_fetch_add(&g_generation, 1u);
    return true;
}

static bool lcc_allows_commit(sqlite3 *db)
{
    if (!g_lcc_seen) return true;
    char err[192];
    if (stage_lcc_check_raise(db, g_lcc_name, g_lcc_initial, g_lcc_final,
                              err, sizeof(err)))
        return true;
    bool enforce = stage_lcc_enforcement_enabled(db);
    fprintf(stderr,  // obs-ok:stage-lcc-refuse
            "[stage] LCC %s batched raise %s %llu->%llu: %s\n",
            enforce ? "REFUSE" : "warn(allow)", g_lcc_name,
            (unsigned long long)g_lcc_initial,
            (unsigned long long)g_lcc_final, err);
    return !enforce;
}

bool stage_batch_end(sqlite3 *db, bool commit)
{
    if (!db) return false;
    if (!atomic_load(&g_open)) return true;
    if (commit && (!lcc_allows_commit(db) || (g_precommit && !g_precommit()))) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        atomic_store(&g_open, false);
        return false;
    }
    char *err = NULL;
    const char *finish = commit ? "COMMIT" : "ROLLBACK";
    int64_t started = commit ? GetTimeMicros() : 0;
    int rc = sqlite3_exec(db, finish, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[stage] batch %s: %s\n",  // obs-ok:stage-commit-failure
                finish, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        atomic_store(&g_open, false);
        return false;
    }
    if (err) sqlite3_free(err);
    if (commit) record_commit_timing(GetTimeMicros() - started);
    atomic_store(&g_open, false);
    return true;
}
