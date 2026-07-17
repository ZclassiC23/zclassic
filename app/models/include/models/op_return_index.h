/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * OP_RETURN catalog — a PROJECTION recording every OP_RETURN output ever
 * seen on the chain (ZNAM, ZSLP, ZANC, and unknown/future lokad tags
 * alike), independent of what any individual protocol parser does with
 * it. Derived purely from confirmed transaction script bytes; rebuildable
 * from scratch (op_return_index_truncate); never consulted by consensus.
 *
 * Field semantics:
 *   tag        the content of the FIRST script PUSH after the OP_RETURN
 *              opcode (up to OP_RETURN_INDEX_TAG_MAX bytes) — the lokad-id
 *              convention shared by ZNAM/ZSLP/ZANC (script/op_return_push.h
 *              read_push). Falls back to the first min(8,payload_len) raw
 *              bytes when the script does not start with a well-formed
 *              push, so malformed/"unknown lokad" scripts are still
 *              cataloged rather than skipped.
 *   tag_text   a printable rendering of `tag` for indexed lookup/GROUP BY:
 *              ASCII when every (trailing-NUL-trimmed) byte is printable
 *              (e.g. ZSLP's "SLP\0" lokad renders as "SLP"), else lowercase
 *              hex of the raw tag bytes.
 *   payload    every byte of the script AFTER the single OP_RETURN opcode
 *              (push framing included) — payload_len/payload_sha3 describe
 *              exactly this byte range, so the digest is well-defined even
 *              for malformed/non-push scripts.
 *
 * Threading contract (see app/services/src/op_return_backfill_service.c
 * for the full rationale): op_return_index_apply_block_rows /
 * db_op_return_index_save are idempotent (INSERT OR IGNORE, PK=(txid,
 * vout_n)) via AR_ADHOC_SAVE (a locally-prepared statement each call) and
 * safe to call from ANY thread. The digest chain
 * (op_return_index_fold_block_digest) and the cursor
 * (op_return_index_get_cursor / op_return_index_set_cursor) are owned
 * EXCLUSIVELY by the backfill service (a single supervisor-driven thread)
 * so the chain stays deterministic — the tip-finalize hook in
 * explorer_index.c only ever inserts rows, it never folds the digest. */

#ifndef ZCL_DB_MODEL_OP_RETURN_INDEX_H
#define ZCL_DB_MODEL_OP_RETURN_INDEX_H

#include "models/database.h"
#include "models/activerecord.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OP_RETURN_INDEX_TAG_MAX      8
#define OP_RETURN_INDEX_TAG_TEXT_MAX 17  /* 16 hex chars + NUL */

struct op_return_index_row {
    uint8_t  txid[32];
    uint32_t vout_n;
    int32_t  height;
    uint8_t  tag[OP_RETURN_INDEX_TAG_MAX];
    uint8_t  tag_len;
    char     tag_text[OP_RETURN_INDEX_TAG_TEXT_MAX];
    uint32_t payload_len;
    uint8_t  payload_sha3[32];
};

struct block; /* fwd — primitives/block.h */

/* Pure: fills tag/tag_len/tag_text/payload_len/payload_sha3 from a raw
 * scriptPubKey. Caller fills txid/vout_n/height. Returns false when
 * `script` does not start with OP_RETURN (0x6a) — nothing to catalog. */
bool op_return_index_extract(const uint8_t *script, size_t script_len,
                             struct op_return_index_row *out);

struct ar_callbacks *db_op_return_index_callbacks(void);
bool db_op_return_index_validate(const struct op_return_index_row *r,
                                 struct ar_errors *errors);

/* Idempotent (INSERT OR IGNORE). Safe from any thread — see file header. */
bool db_op_return_index_save(struct node_db *ndb,
                             const struct op_return_index_row *row);

/* Extract + save a row for every OP_RETURN output in blk. Row-only — never
 * touches the digest/cursor. `rows_out`/`rows_cap` are optional: when
 * rows_out is non-NULL, every extracted row is ALSO copied there (bounded
 * by rows_cap; *rows_count_out is the true count, which may exceed
 * rows_cap on a pathological block — the caller must check for that
 * before trusting rows_out as a complete set) so a caller that also folds
 * the digest (the backfill service) does not have to re-scan the block.
 * Returns true (best-effort; a single row-save failure is logged and
 * skipped, not fatal — mirrors explorer_index.c's own op_returns write). */
bool op_return_index_apply_block_rows(struct node_db *ndb,
                                      const struct block *blk, int32_t height,
                                      struct op_return_index_row *rows_out,
                                      size_t rows_cap,
                                      size_t *rows_count_out);

/* Pure: chain one block's extracted rows into the running catalog digest.
 * `rows` must be in the order op_return_index_apply_block_rows produced
 * them (ascending tx index, then ascending vout). Folds EVERY processed
 * height, including n_rows==0, so the digest also proves no height was
 * skipped, not just which tags were found. Two independently-indexing
 * nodes that walked the same blocks in the same order reach the same
 * digest. */
void op_return_index_fold_block_digest(const uint8_t prev_digest[32],
                                       int32_t height,
                                       const uint8_t block_hash[32],
                                       const struct op_return_index_row *rows,
                                       size_t n_rows, uint8_t out_digest[32]);

/* Cursor = highest height whose digest has been folded, contiguous from
 * -1 (nothing folded yet). Owned exclusively by the backfill service.
 * Always returns true (the "nothing folded yet" state is not a failure)
 * unless ndb itself is unusable. */
bool op_return_index_get_cursor(struct node_db *ndb, int32_t *out_height,
                                uint8_t out_digest[32]);
bool op_return_index_set_cursor(struct node_db *ndb, int32_t height,
                                const uint8_t digest[32]);

/* Drop-and-rederive: delete every catalog row and reset the cursor/digest
 * to the empty state. The backfill service re-derives the whole catalog
 * from block bodies, from height 0, on its next ticks. */
bool op_return_index_truncate(struct node_db *ndb);

int64_t op_return_index_count(struct node_db *ndb);
int64_t op_return_index_count_by_tag_text(struct node_db *ndb,
                                          const char *tag_text);

/* Bounded range/tag query, newest-height-first then txid/vout_n for a
 * stable order. tag_text_filter may be NULL/empty for no filter. Returns
 * the row count written to out (<=max). */
int op_return_index_query(struct node_db *ndb, int32_t h_min, int32_t h_max,
                          const char *tag_text_filter,
                          struct op_return_index_row *out, size_t max);

#endif /* ZCL_DB_MODEL_OP_RETURN_INDEX_H */
