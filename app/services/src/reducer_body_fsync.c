// one-result-type-ok:reducer-body-fsync-scoping-is-best-effort-void
/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_body_fsync — batched block-body fdatasync scoping for the reducer
 * drive. See services/reducer_ingest_service.h for the enter/exit contract.
 *
 * The reducer body-persist path (write_block_to_disk in
 * reducer_persist_ingested_body_locked) fdatasync()s each block on write — a
 * per-block ext4 journal-commit barrier that dominates the fold / catch-up
 * wait (the drive thread parks in jbd2_log_wait_commit while the CPU is idle).
 * This TU defers that fdatasync to the stage drain-batch boundary:
 *
 *   - enter() turns on disk_block_io deferred mode, so block writes made
 *     inside the drive only record themselves as pending, and registers (once)
 *     the stage_batch_end pre-commit hook.
 *   - the pre-commit hook fdatasync()s every pending block file BEFORE the
 *     stage cursor / *_log rows that reference it commit — a false return
 *     VETOES the commit, so no durable marker ever outlives an unsynced body.
 *   - exit() does a final flush (for a body written but not yet covered by a
 *     stage COMMIT) and leaves deferred mode, so unrelated write_block_to_disk
 *     callers (import, tests) keep their immediate per-block fdatasync.
 *
 * Durability is unchanged — only the fsync cadence drops from once-per-block to
 * once-per-batch. At tip the batch is a single block, so the body is synced at
 * that block's own drain COMMIT: identical durability, one extra deferred hop. */

#include "services/reducer_ingest_service.h"

#include "storage/disk_block_io.h"
#include "util/stage.h"

#include <stdatomic.h>

static _Atomic bool g_body_fsync_hook_registered = false;

static bool reducer_body_fsync_precommit(void)
{
    /* Fired by stage_batch_end() immediately before COMMIT. Flush every
     * deferred block body so no committed stage marker references unsynced
     * bytes. A false return VETOES the commit (stage_batch_end rolls back). */
    return disk_block_io_sync_pending();
}

void reducer_enter_batched_body_sync(void)
{
    bool was = atomic_exchange_explicit(&g_body_fsync_hook_registered, true,
                                        memory_order_relaxed);
    if (!was)
        stage_batch_set_precommit_hook(reducer_body_fsync_precommit);
    disk_block_io_set_deferred_sync(true);
}

void reducer_exit_batched_body_sync(void)
{
    /* Final flush for any body written but not yet covered by a stage COMMIT
     * (e.g. a drive that persisted a body then advanced nothing this pass),
     * then leave deferred mode. Best-effort: a sync failure keeps those files
     * pending, and the still-registered hook will retry (and veto) at the next
     * stage COMMIT, so no marker commits ahead of the bytes. */
    (void)disk_block_io_sync_pending();
    disk_block_io_set_deferred_sync(false);
}
