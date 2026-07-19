// one-result-type-ok:gate-predicate — this file owns no orchestration result.
// Its only bool export (index_fold_disk_ok) is a simple go/no-go gate; the
// other exports are void blocker-raisers/clearers and a test setter. There is
// no fallible service lifecycle whose failure reason must travel via
// struct zcl_result — every failure surfaces as a NAMED util/blocker.h record.

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * index_fold_guard — see services/index_fold_guard.h. Two safety rails shared
 * by the address_index and txindex index folds; each names a typed blocker
 * rather than failing silently or spinning. NOT a repair rung: it never
 * re-derives or heals state — it REFUSES to start the fold (disk headroom) or
 * NAMES a structural coverage floor (snapshot seed), so the writer never
 * produces bad state in the first place. */

#include "services/index_fold_guard.h"

#include "jobs/reducer_frontier.h"          /* REDUCER_TRUSTED_BASE_HEIGHT_KEY */
#include "services/disk_monitor.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Test-overridable free-space floor. <0 means "use the compiled default". */
static _Atomic int64_t g_min_free_override = -1;

void index_fold_set_min_free_for_test(int64_t bytes)
{
    atomic_store(&g_min_free_override, bytes);
}

static int64_t min_free_bytes(void)
{
    int64_t o = atomic_load(&g_min_free_override);
    return o >= 0 ? o : INDEX_FOLD_MIN_FREE_BYTES;
}

/* Build "<index_id>.<suffix>" into out (bounded). */
static void mk_blocker_id(char *out, size_t cap, const char *index_id,
                          const char *suffix)
{
    snprintf(out, cap, "%s.%s", index_id, suffix);
}

bool index_fold_disk_ok(const char *index_id, const char *subsys,
                            const char *datadir)
{
    if (!index_id || !subsys || !datadir || !datadir[0])
        return true;                         /* nothing to measure — fail open */

    char id[BLOCKER_ID_MAX];
    /* blocker-id: *.disk_low */
    mk_blocker_id(id, sizeof(id), index_id, "disk_low");

    /* The running disk_monitor already owns the hard CRITICAL refuse; a backfill
     * must not add bytes while it is tripped. */
    bool critical = disk_monitor_is_critical();

    /* Conservative backfill-specific floor: a fresh statvfs of the datadir FS.
     * A statvfs error returns <0 and we fail OPEN (the monitor is the authority
     * on a genuinely full disk; we don't want a transient stat error to wedge
     * the catalog). */
    int64_t free_bytes = disk_monitor_free_bytes(datadir);
    int64_t floor = min_free_bytes();
    bool low = free_bytes >= 0 && free_bytes < floor;

    if (critical || low) {
        struct blocker_record r;
        char reason[BLOCKER_REASON_MAX];
        snprintf(reason, sizeof(reason),
                 "disk headroom too low to start/continue the %s backfill: "
                 "free=%lld bytes, floor=%lld bytes, disk_monitor_critical=%d "
                 "on %s — holding the index fold so consensus writes keep their "
                 "last bytes. Auto-clears when free space returns above the "
                 "floor; free space (or -%s=0 to disable this index).",
                 index_id, (long long)free_bytes, (long long)floor,
                 (int)critical, datadir, index_id);
        if (blocker_init(&r, id, subsys, BLOCKER_RESOURCE, reason))
            (void)blocker_set(&r);
        return false;
    }

    blocker_clear(id);                       /* healthy — no-op if never set */
    return true;
}

/* Read REDUCER_TRUSTED_BASE_HEIGHT_KEY (8-byte LE) from progress_meta.
 * Returns true on a clean read; *found=false when the key is absent (a
 * from-genesis datadir with no snapshot seed). */
static bool read_seed_floor(sqlite3 *db, int64_t *floor_out, bool *found)
{
    *floor_out = -1;
    *found = false;
    if (!db)
        return false;
    uint8_t blob[8] = {0};
    size_t n = 0;
    bool present = false;
    if (!progress_meta_get_blob_exact(db, REDUCER_TRUSTED_BASE_HEIGHT_KEY,
                                      blob, sizeof(blob), &n, &present)) {
        LOG_WARN("index_backfill",
                 "trusted_base_height read failed — leaving seed blocker as-is");
        return false;
    }
    if (!present)
        return true;                         /* clean read, no seed floor */
    if (n != sizeof(blob)) {
        LOG_WARN("index_backfill",
                 "trusted_base blob malformed (len=%zu) — treating as no floor",
                 n);
        return true;
    }
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | blob[i];              /* little-endian */
    *floor_out = (int64_t)v;
    *found = true;
    return true;
}

void index_fold_note_absent_body(const char *index_id, const char *subsys,
                                     sqlite3 *db, int64_t absent_height)
{
    if (!index_id || !subsys)
        return;

    char id[BLOCKER_ID_MAX];
    /* blocker-id: *.below_snapshot_seed */
    mk_blocker_id(id, sizeof(id), index_id, "below_snapshot_seed");

    /* A4: the snapshot-seed floor (REDUCER_TRUSTED_BASE_HEIGHT_KEY) is a KERNEL
     * fact written by the reducer to consensus.db. Read it from the kernel
     * authority, NOT the projection fold handle `db` the caller passes
     * (progress.kv holds only address_index/txindex and has no progress_meta). */
    (void)db;
    int64_t seed_floor = -1;
    bool have_seed = false;
    if (!read_seed_floor(progress_store_db(), &seed_floor, &have_seed))
        return;                              /* DB read error — leave as-is */

    if (!have_seed || absent_height > seed_floor) {
        /* No snapshot seed, or the hole is ABOVE the seed floor: this is a
         * transient/genuine gap the service's own coverage_blocked flag already
         * surfaces. Not a structural below-seed floor. */
        blocker_clear(id);
        return;
    }

    /* absent_height <= seed_floor: bodies below the snapshot seed are
     * structurally absent; the forward-only fold can never cross the floor.
     * Name it so the operator sees a DEPENDENCY on the historical body backfill
     * rather than a silent spin. */
    struct blocker_record r;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "%s backfill cannot fold at height %lld: the block body is absent "
             "at/below the snapshot-seed floor (reducer_trusted_base_height=%lld). "
             "Bodies below the seed were never downloaded on this snapshot-seeded "
             "datadir, so this rebuildable index has no source to fold across the "
             "floor. Not an error and not a consensus stall — it clears when the "
             "historical bodies are backfilled below the seed, or stays a named "
             "coverage floor. Opt out with -%s=0.",
             index_id, (long long)absent_height, (long long)seed_floor,
             index_id);
    if (blocker_init(&r, id, subsys, BLOCKER_DEPENDENCY, reason))
        (void)blocker_set(&r);
}

void index_fold_clear_seed_blocker(const char *index_id)
{
    if (!index_id)
        return;
    char id[BLOCKER_ID_MAX];
    mk_blocker_id(id, sizeof(id), index_id, "below_snapshot_seed");
    blocker_clear(id);
}
