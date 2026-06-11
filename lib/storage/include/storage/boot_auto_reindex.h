/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_auto_reindex — durable, bounded "self-rebuild" request for crash-only
 * recovery. When boot's post-restore integrity gate detects the
 * reindex-recoverable shape (a derived tip ABOVE the on-disk validated index
 * extent: zero_nbits==0, holes only above the index, no structural nBits
 * corruption), it records a request that the NEXT boot consumes to set
 * -reindex-chainstate — which rewinds to the consistent reindex target and
 * rebuilds the UTXO set from blocks/. This is the crash-only primitive: the
 * node throws away inconsistent derived state and re-derives it, instead of
 * FATAL-ing or surgically repairing.
 *
 * The request is bounded per anchor-height episode so a genuinely corrupt
 * blocks/ pages the operator instead of looping, and is fsync-durable so the
 * budget survives a crash mid-rebuild. File: <datadir>/auto_reindex_request —
 * a top-level sentinel that is NEVER part of any derived-state wipe set.
 */

#ifndef ZCL_STORAGE_BOOT_AUTO_REINDEX_H
#define ZCL_STORAGE_BOOT_AUTO_REINDEX_H

#include <stdbool.h>
#include <stdint.h>

/* Max reindex attempts per anchor-height episode before pausing for the
 * operator (a genuinely corrupt blocks/ must page, not loop). */
#define BOOT_AUTO_REINDEX_MAX 3

/* Record a self-rebuild request keyed on `anchor` (the wedged tip height).
 * If the on-disk request already names this anchor its count is incremented,
 * else the count resets to 1. fsync-durable. Returns the new attempt count
 * (>=1), or 0 on a write error. */
int boot_auto_reindex_request(const char *datadir, int32_t anchor);

/* True iff a self-rebuild request is on disk — boot consumes it to set
 * -reindex-chainstate before the coins-integrity gate runs. */
bool boot_auto_reindex_pending(const char *datadir);

/* Clear the request once the node boots to a clean post-restore integrity
 * state (the rebuild converged), or when the attempt budget is exhausted. */
void boot_auto_reindex_clear(const char *datadir);

#endif /* ZCL_STORAGE_BOOT_AUTO_REINDEX_H */
