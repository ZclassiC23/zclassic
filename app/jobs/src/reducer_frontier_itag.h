/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_frontier_itag — the fold-read side of the W4-4 per-row integrity tag.
 *
 * Split out of reducer_frontier.c (file-size ceiling) but conceptually part of
 * the L0 fold: the watermark that keeps a normal fold O(delta), the column
 * presence probe, and the typed mismatch error. The per-row verify itself stays
 * inline in the fold (log_ok_at / log_contiguous_prefix) and calls
 * stage_row_itag_verify(); see jobs/stage_row_itag.h for the tag contract. */

#ifndef ZCL_JOBS_REDUCER_FRONTIER_ITAG_H
#define ZCL_JOBS_REDUCER_FRONTIER_ITAG_H

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

/* Per-boot integrity-tag watermark: the height at/below which every *_log row
 * has been tag-verified this boot. The first fold after boot/reset verifies its
 * whole scanned range; later folds trust the prefix at/below the last resolved
 * H* and verify only the delta above it (the fold is hot — this keeps a normal
 * at-tip fold O(delta), not O(chain)). A reboot re-verifies from scratch.
 *
 *   _watermark()          — current watermark (-1 == nothing verified yet).
 *   _watermark_publish(h) — set it to the just-resolved H* (raise OR lower).
 *   _watermark_reset()    — drop to -1 (refold/reorg: re-verify what we refold).
 * Lock-free atomics; the fold path is already serialized on progress_store's
 * tx lock, and a concurrent dump reader stays torn-safe regardless. */
int32_t reducer_frontier_itag_watermark(void);
void    reducer_frontier_itag_watermark_publish(int32_t hstar);
void    reducer_frontier_itag_watermark_reset(void);

/* Does `table` carry an `itag` column on `db`? Old fixtures / a pre-migration
 * datadir may not; when absent the fold skips tag verification (trusting ok,
 * exactly as before this feature). Cheap metadata read; NOT cached because the
 * same table name can back different schemas across the many test dbs in one
 * process. */
bool reducer_frontier_itag_column_present(struct sqlite3 *db,
                                          const char *table);

/* Typed, de-stormed error naming the (table, height) whose integrity tag failed
 * to verify: a WARN + a named recovery event, throttled (first sight / key
 * change / 300 s keep-alive) so a persistent corruption does not spam the log. */
void reducer_frontier_itag_mismatch_warn(const char *table, int64_t height);

#endif /* ZCL_JOBS_REDUCER_FRONTIER_ITAG_H */
