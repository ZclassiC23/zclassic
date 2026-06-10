/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_post_step — reducer post-finalize side effects.
 *
 * The post-finalize side-effect step: once tip_finalize has moved the
 * in-memory active-chain window, this runs the derived effects that belong to
 * tip connection but are not reducer cursor authority:
 * wallet transaction sync + Sapling trial-decrypt/note-persist, nullifier
 * spend marking, mempool removal of confirmed txs, and the MMR/MMB appends.
 * Internal to app/jobs/src — not a public jobs/ API. */

#ifndef ZCL_JOBS_TIP_FINALIZE_POST_STEP_H
#define ZCL_JOBS_TIP_FINALIZE_POST_STEP_H

struct block_index;

/* Run the post-finalize side effects for the just-connected tip block.
 *
 * `pindex_new` is the block_index of the newly finalized tip (already set
 * as chain[] tip by the caller). The block body is read back from disk via
 * GetDataDir() (resolved here, not threaded from the stage). NULL
 * pindex_new is a no-op; a missing on-disk body (HAVE_DATA absent / read
 * failure) is a benign skip.
 *
 * Every subsystem handle (wallet, mempool, node_db) is fetched via the
 * public app_runtime_* accessors and individually NULL-guarded. */
void tip_finalize_run_post_finalize(struct block_index *pindex_new);

#endif /* ZCL_JOBS_TIP_FINALIZE_POST_STEP_H */
