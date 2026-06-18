/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_EXPLORER_INDEX_H
#define ZCL_DB_MODEL_EXPLORER_INDEX_H

/* Explorer projection writers — the full per-block indexer.
 *
 * These eight db_*_save functions populate the read-derived explorer
 * projection tables (tx_outputs, tx_inputs, op_returns, sapling_spends,
 * sapling_outputs, joinsplits, sprout_nullifiers, view_integrity) from
 * already-validated on-disk block bodies. They are the only writers for
 * those tables and are driven by index_tx_projections() in
 * node_db_catchup_service.c (the single per-block write unit, on both the
 * forward-sync and -reindex-explorer paths).
 *
 * node.db ONLY. None of these reach coins_kv / progress.kv / the reducer /
 * the block index / any consensus predicate. A save failure degrades an
 * explorer row; it never gates block acceptance.
 *
 * Idempotent: every statement is INSERT OR REPLACE keyed on the table PK,
 * so a reindex / restart re-walk overwrites the existing row rather than
 * double-inserting. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct node_db;
struct block;
struct block_index;
struct transaction;
struct spend_description;
struct output_description;
struct js_description;
struct ar_errors;

/* Index every projection for one already-validated block: per-tx outputs,
 * inputs, op_returns (+ ZSLP flag / ZNAM apply), sapling spends/outputs,
 * joinsplits, sprout nullifiers, then the per-height SHA3 chained
 * view_integrity receipt. This is the single per-block hook called once
 * from sync_block_lean (both the forward-sync and -reindex-explorer paths).
 *
 *   prev_receipt   the previous height's 32-byte receipt (32 zero bytes at
 *                  genesis). Carried across the ascending catchup walk.
 *   out_receipt    receives this height's receipt (chain it forward).
 *   out_sprout     receives Σ(vpub_old - vpub_new) for the block.
 *   out_sapling    receives Σ value_balance for the block.
 *
 * Returns false only on a catastrophic argument error (logged). An
 * individual projection save failure is logged and skipped (a degraded
 * explorer row) — it never aborts the block or gates acceptance. */
bool explorer_index_block(struct node_db *ndb, const struct block *blk,
                          const struct block_index *pindex,
                          const uint8_t prev_receipt[32],
                          uint8_t out_receipt[32],
                          int64_t *out_sprout, int64_t *out_sapling);

/* Transparent output: txid/vout PK, value, script_type (enum script_type),
 * optional 20-byte address hash (NULL → SQL NULL), block height. */
bool db_tx_output_save(struct node_db *ndb, const uint8_t txid[32],
                       uint32_t vout, int64_t value, int script_type,
                       const uint8_t *address_hash /* 20B or NULL */,
                       int block_height);

/* Transparent input: txid/vin_index PK, the spent prevout, block height.
 * Coinbase null prevouts are skipped by the caller, not here. */
bool db_tx_input_save(struct node_db *ndb, const uint8_t txid[32],
                      uint32_t vin_index, const uint8_t prev_txid[32],
                      uint32_t prev_vout, int block_height);

/* OP_RETURN: txid PK (one row per tx), raw scriptPubKey (incl. leading
 * 0x6a), block height, is_slp flag (true when slp_parse matched). */
bool db_op_return_save(struct node_db *ndb, const uint8_t txid[32],
                       int block_height, const uint8_t *script,
                       size_t script_len, bool is_slp);

/* Sapling spend: stores cv/anchor/nullifier/rk (32B each). */
bool db_sapling_spend_save(struct node_db *ndb, const uint8_t txid[32],
                           uint32_t spend_index,
                           const struct spend_description *sd,
                           int block_height);

/* Sapling output: stores cv/cm/ephemeral_key (32B each) only — NOT the
 * enc/out ciphertext or zkproof. */
bool db_sapling_output_save(struct node_db *ndb, const uint8_t txid[32],
                            uint32_t output_index,
                            const struct output_description *od,
                            int block_height);

/* Sprout JoinSplit: vpub_old/vpub_new (i64), anchor (32B). */
bool db_joinsplit_save(struct node_db *ndb, const uint8_t txid[32],
                       uint32_t js_index, const struct js_description *jsd,
                       int block_height);

/* Sprout nullifier: nullifier PK, the spending txid, block height. */
bool db_sprout_nullifier_save(struct node_db *ndb, const uint8_t nullifier[32],
                              const uint8_t txid[32], int block_height);

/* Per-height SHA3 chained integrity receipt: height PK, 32-byte hash. */
bool db_view_integrity_save(struct node_db *ndb, int64_t height,
                            const uint8_t sha3[32]);

/* Read the 20-byte address hash recorded for a spent prevout (used to
 * derive a ZNAM op's owner from its first input). Returns false when the
 * prevout row is absent or carries no address (non-P2PKH). */
bool db_tx_output_addr(struct node_db *ndb, const uint8_t txid[32],
                       uint32_t vout, uint8_t addr20[20]);

/* Truncate every explorer projection + on-chain ZNAM table. Used by the
 * -reindex-explorer driver before the genesis..tip re-walk (ZNAM FCFS /
 * TRANSFER are not partial-replay safe, so they MUST be cleared too).
 * node.db only. Returns false (logged) on the first DELETE failure. */
bool db_explorer_index_truncate(struct node_db *ndb);

#endif
