/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZSLP per-(token, outpoint) ledger — the PROJECTION that makes token
 * balances fully chain-derived.
 *
 * WHY this exists: `zslp_balances` (app/models/src/zslp.c) is a credit-only
 * app ledger and is left EMPTY by the chain-scan path on purpose — see
 * explorer_index_zslp.c: "a credit-only ledger cannot debit SEND inputs
 * and would over-count holders; deferred to a future per-(token,outpoint)
 * ledger". THIS is that ledger. Each row is ONE token-bearing transaction
 * output (an SLP UTXO): it is created when a GENESIS/MINT/SEND assigns a
 * token amount to a real output, and marked spent when a later transaction
 * spends that outpoint (the always-on tx_inputs spend graph). A holder's
 * balance is therefore SUM(amount) over that holder's UNSPENT rows — a
 * debit-correct ledger, unlike the credit-only zslp_balances.
 *
 * Chain-derived + rebuildable: every row is derived from confirmed,
 * already-persisted node.db projections — zslp_transfers (the SLP
 * amount/vout/token_id interpretation, produced by explorer_index_apply_slp
 * so we do NOT re-implement the SLP field mapping), tx_outputs (the
 * authoritative recipient address + proof the output really exists), and
 * tx_inputs (the spend graph). Never consulted by consensus; rebuildable
 * from scratch via zslp_ledger_truncate + a fresh backfill.
 *
 * SLP-validity divergence (documented on purpose): strict SLP consensus
 * BURNS a SEND whose declared output sum exceeds the sum of its VALID token
 * inputs (the whole tx is invalid, no outputs get tokens). Our explorer
 * path (explorer_index_apply_slp / explorer_index_zslp.c) does NOT validate
 * input sums — it credits every declared SEND output unconditionally. This
 * ledger MATCHES that behavior (parity with our own projection matters more
 * than strict SLP here). A SEND that over-declares therefore mints ledger
 * rows for the extra amount; this is a projection, not consensus.
 *
 * Threading contract (mirrors op_return_index.h): row inserts + spend marks
 * are idempotent (INSERT OR IGNORE + deterministic UPDATE) and safe from
 * ANY thread, so the live tip hook (zslp_ledger_apply_slp_live, called from
 * explorer_index.c's index_op_return) and the backfill service can write
 * the SAME rows without coordination. The cursor + running digest
 * (zslp_ledger_get_cursor/set_cursor, folded by zslp_ledger_apply_height)
 * are owned EXCLUSIVELY by the single supervisor-driven backfill service so
 * two independently-indexing nodes converge on the same digest. */

#ifndef ZCL_DB_MODEL_ZSLP_LEDGER_H
#define ZCL_DB_MODEL_ZSLP_LEDGER_H

#include "models/database.h"

#include <stdbool.h>
#include <stdint.h>

struct transaction; /* fwd — primitives/transaction.h */
struct slp_message; /* fwd — zslp/slp.h */

/* Live per-block hook: apply a parsed SLP message for the tx in hand.
 * Creates a ledger row for every token-bearing output the message assigns
 * (GENESIS/MINT -> vout 1, SEND -> vouts 1..N; same interpretation as
 * explorer_index_apply_slp) that actually exists on the tx, and marks spent
 * every ledger outpoint this tx's inputs consume. Idempotent; never touches
 * the cursor/digest. Address is resolved from the tx's own output script
 * (tx_outputs for those vouts may not be written yet at this call site).
 * Best-effort: a single row failure is logged, not fatal. */
bool zslp_ledger_apply_slp_live(struct node_db *ndb,
                                const struct transaction *tx,
                                const struct slp_message *m, int height);

/* Backfill one height from node.db projections and fold its running digest.
 * CREATE rows: zslp_transfers JOIN tx_outputs at `height` (token amount +
 * authoritative address + existence proof). SPENDS: tx_inputs at `height`
 * whose consumed outpoint has a ledger row. Rows are (re-)inserted
 * idempotently and spends marked; then a streaming SHA3 digest folds
 * prev_digest, the height, every CREATE (token_id/txid/vout/amount/address/
 * created_height) in (txid,vout) order, and every SPEND (outpoint + spender
 * + height) in (txid,vin_index) order. Folds EVERY height (including ones
 * with no SLP activity) so the digest also proves no height was skipped.
 * Owned exclusively by the backfill service. Returns false on a DB error
 * (caller must stop the batch — advance-cursor-or-named-blocker). */
bool zslp_ledger_apply_height(struct node_db *ndb, int32_t height,
                              const uint8_t prev_digest[32],
                              uint8_t out_digest[32]);

/* Cursor = highest height whose digest has been folded, contiguous from -1
 * (nothing folded yet). Always returns true unless ndb is unusable. */
bool zslp_ledger_get_cursor(struct node_db *ndb, int32_t *out_height,
                            uint8_t out_digest[32]);
bool zslp_ledger_set_cursor(struct node_db *ndb, int32_t height,
                            const uint8_t digest[32]);

/* Drop-and-rederive: delete every ledger row and reset cursor/digest. */
bool zslp_ledger_truncate(struct node_db *ndb);

/* Reconciliation surface: a holder's spendable token balance =
 * SUM(amount) over that (token_id,address)'s UNSPENT rows. token_id is
 * internal (node) byte order; address is the 20-byte hash160. */
int64_t zslp_ledger_balance(struct node_db *ndb, const uint8_t token_id[32],
                            const uint8_t address[20]);

/* Row counts for diagnostics. */
int64_t zslp_ledger_count(struct node_db *ndb);
int64_t zslp_ledger_unspent_count(struct node_db *ndb);

#endif /* ZCL_DB_MODEL_ZSLP_LEDGER_H */
