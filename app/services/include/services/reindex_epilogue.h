/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reindex_epilogue — derive ALL durable post-reindex state from the
 * just-replayed UTXO set in ONE ordered commit discipline so the recovery
 * path can never manufacture the coins_applied > hstar wedge.
 *
 * tenacity-roadmap item 3: -reindex-chainstate (the crash-only auto-recovery
 * verb) used to end torn — boot_index_clear_coins_state DELETES the SHA3
 * commitment + coins_best cache but nothing recomputes them, coins_kv is never
 * reseeded, and the 8 reducer stage cursors + coins_applied_height keep their
 * STALE pre-reindex values. Stale cursors over a freshly-rebuilt coin set
 * manufacture the coins_applied > hstar coin-tear shape; with the never-give-up
 * unit that degrades into an infinite reindex loop. This epilogue closes it by
 * DERIVING every value from the replayed authoritative set. */

#ifndef ZCL_SERVICES_REINDEX_EPILOGUE_H
#define ZCL_SERVICES_REINDEX_EPILOGUE_H

#include <stdbool.h>

struct main_state;
struct node_db;

/* Run after a SUCCESSFUL full reindex replay (errors==0, coins flushed to the
 * `utxos` mirror, g_utxo_commitment_skip CLEARED by the caller). Derives, in
 * order: (1) reset+reseed coins_kv from the mirror + migration stamp;
 * (2) recompute the SHA3 commitment + count, stamp utxo_sha3 + coins_best_block
 * cache; (3) raise coins_applied_height to tip+1 FIRST, then clamp the
 * tip_finalize anchor + 8 stage cursors to the replayed tip via the
 * trusted-seed convention; (4) self-check H* == replayed tip.
 *
 * Returns true iff all derivations land AND H* == tip. Best-effort-but-LOUD:
 * any failure logs, PAGES (EV_OPERATOR_NEEDED + typed reason), and returns
 * false so the caller leaves the reindex sentinel pending for a next-boot
 * retry — bounded by the existing auto-reindex attempt budget, never FATAL,
 * never a silent infinite loop.
 *
 * `datadir` locates <datadir>/node.db for the coins_kv reseed copy. */
bool reindex_epilogue_derive(struct main_state *ms, struct node_db *ndb,
                             const char *datadir);

#endif /* ZCL_SERVICES_REINDEX_EPILOGUE_H */
