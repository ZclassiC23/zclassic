// one-result-type-ok:reducer-body-fsync-scoping-is-best-effort-void
/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_body_fsync — batched fdatasync scoping for the reducer drive's two
 * deferrable on-disk artifacts: the block bodies (blk*.dat) AND the append-only
 * event_log. See services/reducer_ingest_service.h for the enter/exit contract.
 *
 * The reducer fold defers two per-block fsync sources. (1) The body-persist
 * path (write_block_to_disk in reducer_persist_ingested_body_locked)
 * fdatasync()s each block on write. (2) event_log_append() fsync()s TWICE per
 * event, and the fold emits ~2 events/block (EV_BLOCK_BODY + EV_BLOCK_HEADER)
 * ON the drive thread — ~4 event fsyncs/block. Every one is an ext4
 * journal-commit barrier that dominates the fold / catch-up wait (the drive
 * thread parks in jbd2_log_wait_commit while the CPU is idle). This TU defers
 * both to the stage drain-batch boundary:
 *
 *   - enter() turns on disk_block_io deferred mode + event_log deferred mode,
 *     so block writes / event appends made inside the drive only buffer (page
 *     cache) instead of fdatasync()ing, and registers (once) the stage_batch_end
 *     pre-commit hook.
 *   - the pre-commit hook fdatasync()s every pending block file AND the
 *     event_log ONCE BEFORE the stage cursor / *_log rows that reference them
 *     commit — a false return from either VETOES the commit, so no durable
 *     marker ever outlives an unsynced body or event.
 *   - exit() does a final flush (for work written but not yet covered by a
 *     stage COMMIT) and leaves deferred mode, so unrelated write_block_to_disk
 *     / event_log callers (import, tests, at-tip) keep their immediate per-op
 *     fdatasync.
 *
 * Durability is unchanged — only the fsync cadence drops from ~5/block to
 * ~1/batch. At tip the batch is a single block, so the artifacts are synced at
 * that block's own drain COMMIT: identical durability, one extra deferred hop. */

#include "services/reducer_ingest_service.h"

#include "storage/disk_block_io.h"
#include "storage/event_log.h"
#include "storage/event_log_singleton.h"
#include "util/stage.h"

#include <stdatomic.h>

static _Atomic bool g_body_fsync_hook_registered = false;

static bool reducer_batched_durability_precommit(void)
{
    /* Fired by stage_batch_end() immediately before COMMIT. Flush BOTH
     * deferred on-disk artifacts the fold defers — the block bodies
     * (blk*.dat, disk_block_io) AND the append-only event_log — so no
     * committed stage marker (cursor, *_log row) references unsynced bytes.
     * A false return from EITHER flush VETOES the commit (stage_batch_end
     * rolls back). Both are attempted (no short-circuit) so a transient
     * failure in one still fdatasyncs the other. */
    bool bodies = disk_block_io_sync_pending();
    event_log_t *log = event_log_singleton();
    bool events = log ? event_log_flush(log) : true;
    return bodies && events;
}

void reducer_enter_batched_body_sync(void)
{
    bool was = atomic_exchange_explicit(&g_body_fsync_hook_registered, true,
                                        memory_order_relaxed);
    if (!was)
        stage_batch_set_precommit_hook(reducer_batched_durability_precommit);
    disk_block_io_set_deferred_sync(true);
    /* event_log is a per-handle flag; NULL singleton (early boot, offline
     * mint with emission suppressed) simply means nothing to defer/flush. */
    event_log_t *log = event_log_singleton();
    if (log)
        event_log_set_deferred_sync(log, true);
}

void reducer_exit_batched_body_sync(void)
{
    /* Final flush for any body / event written but not yet covered by a stage
     * COMMIT (e.g. a drive that persisted work then advanced nothing this
     * pass), then leave deferred mode. Best-effort: a sync failure keeps the
     * artifact pending/dirty, and the still-registered hook retries (and
     * vetoes) at the next stage COMMIT, so no marker commits ahead of bytes. */
    (void)disk_block_io_sync_pending();
    disk_block_io_set_deferred_sync(false);
    event_log_t *log = event_log_singleton();
    if (log) {
        (void)event_log_flush(log);
        event_log_set_deferred_sync(log, false);
    }
}
