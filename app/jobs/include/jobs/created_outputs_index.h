#ifndef ZCL_JOBS_CREATED_OUTPUTS_INDEX_H
#define ZCL_JOBS_CREATED_OUTPUTS_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward creation index: (txid, vout) -> {value, scriptPubKey, height}.
 *
 * Populated by body_persist (pipeline stage 4) as each verified block body
 * lands on disk. Because body_persist runs strictly UPSTREAM of script_validate
 * (stage 5) — body_persist.cursor > script_validate.cursor at all times — every
 * output created at or below the script_validate frontier is already indexed
 * BEFORE script_validate needs it, independent of the trailing utxo_apply cursor
 * (stage 7). This is the production prevout source for transparent script
 * verification when the node runs without -txindex.
 *
 * It is a CREATION index, not a UTXO set: rows are never deleted on spend, so a
 * coin spent earlier in the same lookahead window is still resolvable. Pre-anchor
 * (snapshot-seeded) coins are NOT here — the resolver falls back to the seeded
 * utxo_projection for those. See the P0 §2.1 design (docs/work/FINISH_CHECKLIST.md). */

struct block;
typedef struct sqlite3 sqlite3;

/* Create the created_outputs table + height index if absent. Idempotent.
 * Returns false on a real SQLite error. Call from body_persist_stage_init. */
bool created_outputs_index_ensure_schema(sqlite3 *db);

/* Insert every output of every tx in `blk`, created at `height`. Idempotent
 * (INSERT OR REPLACE keyed on (txid,vout)) so a post-rewind re-persist rewrites
 * identical rows. Returns false on a real SQLite error (caller -> JOB_FATAL). */
bool created_outputs_index_put_block(sqlite3 *db, const struct block *blk,
                                     int height);

/* Resolve one outpoint. Returns true and fills *value_out + script (up to
 * script_cap bytes, with the true length in *script_len_out) when found;
 * false when absent or on error. Tolerates a not-yet-created table. */
bool created_outputs_index_get(sqlite3 *db, const uint8_t txid[32],
                               uint32_t vout, int64_t *value_out,
                               uint8_t *script_out, size_t script_cap,
                               size_t *script_len_out);

/* Resolve one outpoint only when its creator height is inside
 * [min_height, max_height]. This is used by repair replay to build a bounded
 * parent-height view above the durable coins frontier without accidentally
 * authorizing outputs from future or unrelated sparse-log islands. */
bool created_outputs_index_get_bounded(sqlite3 *db, const uint8_t txid[32],
                                       uint32_t vout, int min_height,
                                       int max_height, int64_t *value_out,
                                       uint8_t *script_out, size_t script_cap,
                                       size_t *script_len_out,
                                       int *height_out);

/* Prune all rows with height < floor (finality pruning). UNWIRED in v1 —
 * exported for a follow-up once on-disk storage is measured. Returns false
 * on a real SQLite error. */
bool created_outputs_index_prune_below(sqlite3 *db, int floor);

#endif /* ZCL_JOBS_CREATED_OUTPUTS_INDEX_H */
