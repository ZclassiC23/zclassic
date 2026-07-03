/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONFIG_MINT_ANCHOR_PROGRESS_H
#define ZCL_CONFIG_MINT_ANCHOR_PROGRESS_H

#include <stdbool.h>
#include <stdint.h>

#include <sqlite3.h>

struct sha3_utxo_checkpoint;

/* Durable checkpoint-bound marker for the offline -mint-anchor producer. */
#define MINT_ANCHOR_IN_PROGRESS_KEY "mint_anchor_in_progress_v1"

/* Mark that this progress.kv is in the middle of minting the compiled anchor.
 * The blob binds the resume permission to the checkpoint height/count/SHA3, so
 * a future binary with a different checkpoint resets instead of inheriting an
 * unrelated fold. */
bool mint_anchor_progress_mark(sqlite3 *db,
                               const struct sha3_utxo_checkpoint *cp);

/* Clear the marker after the snapshot has been written and hard-verified. */
bool mint_anchor_progress_clear(sqlite3 *db);

/* True iff -mint-anchor may resume without resetting to genesis. A current
 * marker must match `cp`; older interrupted mints that predate the marker may
 * be adopted only when refold_in_progress is set and coins_applied_height is a
 * sane genesis..anchor frontier. `applied_through_out` receives
 * coins_applied_height - 1 when known. */
bool mint_anchor_progress_can_resume(sqlite3 *db,
                                     const struct sha3_utxo_checkpoint *cp,
                                     int32_t *applied_through_out,
                                     bool *legacy_adopted_out);

#endif /* ZCL_CONFIG_MINT_ANCHOR_PROGRESS_H */
