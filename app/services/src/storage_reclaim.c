/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * storage_reclaim — see header for rationale. This is the public reclaim
 * entry point the disk_full_pause condition calls so a near-full disk can
 * actually free derived bytes (and the condition clears) instead of latching.
 */

// one-result-type-ok:reclaim-counts-out-struct — E2 (one way out): the public
// surface is best-effort and reports a COUNT SUMMARY, not a fallible op whose
// reason must travel. storage_reclaim_derived() always runs every source it can
// (a failed/absent source is recorded in sources_ok/sources_total, never an
// error return) and returns a struct storage_reclaim_result; the underlying
// fallible checkpoints (progress_store_checkpoint / db_maintenance_checkpoint_now)
// already carry their own reasons. storage_reclaim_run_count() returns a counter.

#include "services/storage_reclaim.h"

#include "platform/time_compat.h"
#include "services/db_maintenance.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static _Atomic int64_t g_reclaim_runs;

int64_t storage_reclaim_run_count(void)
{
    return atomic_load(&g_reclaim_runs);
}

/* Unlink "<x>.tmp" crash orphans older than the min-age guard. An atomic
 * write-then-rename creates "<path>.tmp" and renames it within well under a
 * second, so a .tmp older than the guard cannot be an in-flight write — it is
 * a leftover from a process that died mid-write. Top level only (the datadir
 * root), regular files only, never a symlink or directory. Returns the count
 * removed and accumulates freed bytes into *bytes_out. */
static int sweep_stale_tmp(const char *datadir, int64_t *bytes_out)
{
    if (!datadir || !*datadir)
        return 0;
    DIR *d = opendir(datadir);
    if (!d) {
        LOG_WARN("storage_reclaim",
                 "[reclaim] opendir(%s) failed: %s", datadir, strerror(errno));
        return 0;
    }
    int removed = 0;
    time_t now = platform_time_wall_time_t();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 4, ".tmp") != 0)
            continue;
        char path[2048];
        int pn = snprintf(path, sizeof(path), "%s/%s", datadir, e->d_name);
        if (pn <= 0 || pn >= (int)sizeof(path))
            continue;
        struct stat st;
        if (lstat(path, &st) != 0)
            continue; /* vanished between readdir and lstat — nothing to do */
        if (!S_ISREG(st.st_mode))
            continue; /* never unlink a directory or follow a symlink */
        if ((int64_t)(now - st.st_mtime) < STORAGE_RECLAIM_TMP_MIN_AGE_SECS)
            continue; /* young: may be an in-flight atomic write — leave it */
        if (unlink(path) == 0) {
            removed++;
            if (bytes_out) *bytes_out += (int64_t)st.st_size;
        } else {
            LOG_WARN("storage_reclaim",
                     "[reclaim] unlink(%s) failed: %s", path, strerror(errno));
        }
    }
    closedir(d);
    return removed;
}

struct storage_reclaim_result storage_reclaim_derived(const char *datadir)
{
    struct storage_reclaim_result r = {0};

    /* 1. progress.kv WAL → checkpoint+truncate (the cursor log). */
    r.sources_total++;
    if (progress_store_checkpoint())
        r.sources_ok++;

    /* 2. node.db WAL → checkpoint+truncate (UTXO set + explorer tables). */
    r.sources_total++;
    if (db_maintenance_checkpoint_now().ok)
        r.sources_ok++;

    /* 3. Sweep stale *.tmp crash orphans under the datadir. */
    r.tmp_files_removed = sweep_stale_tmp(datadir, &r.tmp_bytes_removed);

    atomic_fetch_add(&g_reclaim_runs, 1);

    LOG_INFO("storage_reclaim",
             "[reclaim] derived bytes freed: sources_ok=%d/%d tmp_removed=%d "
             "tmp_bytes=%lld datadir=%s",
             r.sources_ok, r.sources_total, r.tmp_files_removed,
             (long long)r.tmp_bytes_removed, datadir ? datadir : "");
    return r;
}
