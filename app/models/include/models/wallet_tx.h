/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_WALLET_TX_H
#define ZCL_DB_MODEL_WALLET_TX_H

#include "models/database.h"
#include "models/activerecord.h"
#include <stdbool.h>
#include <stdint.h>

struct db_wallet_tx {
    uint8_t txid[32];
    uint8_t *raw_tx;
    size_t raw_tx_len;
    uint8_t block_hash[32];
    bool has_block;
    int block_height;
    int64_t time_received;
    bool from_me;
    int64_t fee;
};

struct db_wallet_projection_summary {
    int chain_tip_height;
    int effective_tip_height;
    int utxo_count;
    int note_count;
    int64_t transparent_balance;
    int64_t shielded_balance;
    int64_t speed_balance;
};

struct db_wallet_txid_ref {
    uint8_t txid[32];
};

struct db_wallet_tx_raw_view {
    uint8_t *raw_tx;
    size_t raw_tx_len;
    int block_height;
};

/* Callbacks and validation */
struct ar_callbacks *db_wallet_tx_callbacks(void);
bool db_wallet_tx_validate(const struct db_wallet_tx *t, struct ar_errors *errors);

bool db_wallet_tx_save(struct node_db *ndb, const struct db_wallet_tx *t);
bool db_wallet_tx_find(struct node_db *ndb, const uint8_t txid[32],
                       struct db_wallet_tx *out);
bool db_wallet_tx_delete(struct node_db *ndb, const uint8_t txid[32]);
int db_wallet_tx_count(struct node_db *ndb);
void db_wallet_tx_free(struct db_wallet_tx *t);

/* Sum of fees over from-me transactions with a positive fee, and the
 * count of such fee-paying transactions (via fee_paying_count, if non-NULL).
 * Returns 0 / sets count 0 on a closed db. */
int64_t db_wallet_tx_total_fees(struct node_db *ndb, int *fee_paying_count);

/* Read-only projection for raw wallet transactions ordered by height desc.
 * Intended for controller/service scans that need deserializable tx blobs
 * without owning SQL directly. Caller must free rows with
 * db_wallet_tx_raw_view_free(). */
int db_wallet_tx_recent_raw(struct node_db *ndb,
                            struct db_wallet_tx_raw_view *out,
                            size_t max);
void db_wallet_tx_raw_view_free(struct db_wallet_tx_raw_view *row);
int db_wallet_tx_list_unconfirmed(struct node_db *ndb,
                                  struct db_wallet_txid_ref *out,
                                  size_t max);
bool db_wallet_tx_update_block_height(struct node_db *ndb,
                                      const uint8_t txid[32],
                                      int block_height);

/* List wallet transactions in descending time order with offset paging. */
int db_wallet_tx_list(struct node_db *ndb, struct db_wallet_tx *out,
                      size_t max, size_t offset);

/* List transactions at a given height. */
int db_wallet_tx_at_height(struct node_db *ndb, int height,
                           struct db_wallet_tx *out, size_t max);

/* Wallet UTXOs (transparent outputs belonging to the wallet) */
struct db_wallet_utxo {
    uint8_t txid[32];
    uint32_t vout;
    int64_t value;
    uint8_t address_hash[20];
    uint8_t *script;
    size_t script_len;
    int height;
    uint8_t spent_txid[32];
    int spent_vin;
    bool is_spent;
    bool is_coinbase;
};

/* Validation */
bool db_wallet_utxo_validate(const struct db_wallet_utxo *u,
                              struct ar_errors *errors);

bool db_wallet_utxo_save(struct node_db *ndb, const struct db_wallet_utxo *u);
bool db_wallet_utxo_mark_spent(struct node_db *ndb,
                               const uint8_t txid[32], uint32_t vout,
                               const uint8_t spent_by[32], int vin);
bool db_wallet_utxo_find(struct node_db *ndb,
                         const uint8_t txid[32], uint32_t vout,
                         struct db_wallet_utxo *out);
int64_t db_wallet_utxo_balance(struct node_db *ndb);
int64_t db_wallet_utxo_balance_with_count(struct node_db *ndb, int *utxo_count);
int db_wallet_chain_tip_height(struct node_db *ndb);
int db_wallet_effective_tip_height(struct node_db *ndb);
bool db_wallet_projection_summary(struct node_db *ndb,
                                  struct db_wallet_projection_summary *out);

/* List unspent wallet UTXOs. Returns count. */
int db_wallet_utxo_list_unspent(struct node_db *ndb,
                                struct db_wallet_utxo *out, size_t max);

/* List all wallet UTXOs (spent + unspent). Returns count. */
int db_wallet_utxo_list_all(struct node_db *ndb,
                            struct db_wallet_utxo *out, size_t max);

/* Recent wallet activity row: the value/height of an unspent wallet UTXO
 * joined with its block time (0 when the block row is absent). Powers the
 * API wallet panel's "activity" list. */
struct db_wallet_activity {
    int64_t value;
    int height;
    int64_t time;
};

/* List the most-recent unspent wallet UTXOs (height DESC) with block time,
 * up to max rows. Returns count written to out. */
int db_wallet_utxo_recent_activity(struct node_db *ndb,
                                   struct db_wallet_activity *out, size_t max);

/* Coin selection: unspent, non-coinbase (or mature coinbase). */
int db_wallet_utxo_select_coins(struct node_db *ndb, int64_t target,
                                int current_height,
                                struct db_wallet_utxo *out, size_t max);

/* Delete a single wallet UTXO by outpoint. */
bool db_wallet_utxo_delete(struct node_db *ndb,
                            const uint8_t txid[32], uint32_t vout);

/* Count wallet UTXOs for a given txid. */
int db_wallet_utxo_count_for_tx(struct node_db *ndb,
                                 const uint8_t txid[32]);

/* Free malloc'd fields (script) after db_wallet_utxo_find(). */
void db_wallet_utxo_free(struct db_wallet_utxo *u);

/* Delete all wallet UTXOs. */
bool db_wallet_utxo_delete_all(struct node_db *ndb);
bool db_wallet_utxo_replace_all(struct node_db *ndb,
                                const struct db_wallet_utxo *rows,
                                size_t count);

/* Delete all wallet transactions. */
bool db_wallet_tx_delete_all(struct node_db *ndb);

/* Sapling notes */
struct db_sapling_note {
    uint8_t txid[32];
    uint32_t output_index;
    int64_t value;
    uint8_t rcm[32];
    uint8_t memo[512];
    size_t memo_len;
    uint8_t ivk[32];
    uint8_t diversifier[11];
    uint8_t pk_d[32];
    uint8_t cm[32];
    uint8_t nullifier[32];
    int block_height;
    uint8_t spent_txid[32];
    bool is_spent;
    uint8_t *witness_data;
    size_t witness_data_len;
    int witness_height;
    char address[128]; /* bech32 z-address derived from diversifier+pk_d */
};

bool db_sapling_note_validate(const struct db_sapling_note *n,
                               struct ar_errors *errors);
bool db_sapling_note_save(struct node_db *ndb, const struct db_sapling_note *n);

/* Tri-state result for marking a sapling note spent.
 *
 * The node.db index only tracks wallet/indexed sapling notes, NOT every
 * note on the chain. During projection catchup we replay EVERY on-chain
 * sapling spend; the vast majority reference notes we never indexed. That
 * is a BENIGN miss, not an error — the caller must keep going. Only a real
 * SQLite write failure (busy/error/corrupt) is fatal. The legacy bool API
 * conflated the two (returned false for both), which wedged the whole
 * backfill on the first not-our-note spend. */
enum db_mark_spent_result {
    DB_MARK_SPENT_OK = 0,        /* matched an indexed note and updated it   */
    DB_MARK_SPENT_NOT_FOUND = 1, /* nullifier not in our index (benign skip) */
    DB_MARK_SPENT_ERROR = 2,     /* real DB write error (fatal)              */
};

/* Tri-state variant. Distinguishes benign-miss from real write error so the
 * projection catchup can skip not-our-note spends without aborting. */
enum db_mark_spent_result db_sapling_note_mark_spent_ex(
                                struct node_db *ndb,
                                const uint8_t nullifier[32],
                                const uint8_t spent_by[32]);

/* Legacy bool wrapper: true only when an indexed note was updated.
 * Treats both NOT_FOUND and ERROR as false — do NOT use this in catchup. */
bool db_sapling_note_mark_spent(struct node_db *ndb,
                                const uint8_t nullifier[32],
                                const uint8_t spent_by[32]);
bool db_sapling_note_is_nullifier_spent(struct node_db *ndb,
                                        const uint8_t nullifier[32]);
int64_t db_sapling_note_balance(struct node_db *ndb);
int64_t db_sapling_note_balance_for_ivk(struct node_db *ndb,
                                        const uint8_t ivk[32]);
int64_t db_sapling_note_balance_with_count(struct node_db *ndb, int *note_count);
int64_t db_sapling_note_balance_for_address(struct node_db *ndb,
                                            const char *address);
int64_t db_sapling_note_balance_for_exact_value(struct node_db *ndb,
                                                int64_t value);

/* List unspent notes. Returns count. */
int db_sapling_note_list_unspent(struct node_db *ndb,
                                 struct db_sapling_note *out, size_t max);

/* List unspent notes for a specific ivk. Returns count. */
int db_sapling_note_list_unspent_for_ivk(struct node_db *ndb,
                                          const uint8_t ivk[32],
                                          struct db_sapling_note *out, size_t max);

/* List all notes (spent + unspent). Returns count. */
int db_sapling_note_list_all(struct node_db *ndb,
                              struct db_sapling_note *out, size_t max);

/* List all notes for the coinanalysis RPC: a narrower projection
 * (txid, output_index, value, block_height, spent_txid, diversifier,
 * pk_d, witness_height) ordered by block_height ASC, with the z-address
 * derived (HRP "zs") only when diversifier+pk_d are present. The other
 * struct fields (rcm/memo/ivk/cm/nullifier) are left zeroed. Returns
 * count written to out. */
int db_sapling_note_list_all_analysis(struct node_db *ndb,
                                      struct db_sapling_note *out, size_t max);

/* Save/load witness data for a Sapling note */
bool db_sapling_note_save_witness(struct node_db *ndb,
                                   const uint8_t txid[32], uint32_t output_index,
                                   const uint8_t *witness_blob, size_t blob_len,
                                   int height);
bool db_sapling_note_load_witness(struct node_db *ndb,
                                   const uint8_t txid[32], uint32_t output_index,
                                   uint8_t **witness_blob_out, size_t *blob_len_out,
                                   int *height_out);
bool db_sapling_note_delete_all(struct node_db *ndb);
bool db_sapling_note_replace_all(struct node_db *ndb,
                                 const struct db_sapling_note *rows,
                                 size_t count);

/* Free malloc'd fields (witness_data) after loading a sapling note. */
void db_sapling_note_free(struct db_sapling_note *n);

/* ── Relationships ─────────────────────────────────────────────── */

/* WalletTx has_many :wallet_utxos */
int db_wallet_tx_utxos(struct node_db *ndb, const uint8_t txid[32],
                        struct db_wallet_utxo *out, size_t max);

/* WalletTx has_many :sapling_notes */
int db_wallet_tx_notes(struct node_db *ndb, const uint8_t txid[32],
                        struct db_sapling_note *out, size_t max);

/* WalletTx belongs_to :block */
struct db_block;
bool db_wallet_tx_block(struct node_db *ndb, const struct db_wallet_tx *t,
                        struct db_block *out);

/* WalletUTXO belongs_to :wallet_key */
struct db_wallet_key;
bool db_wallet_utxo_key(struct node_db *ndb, const struct db_wallet_utxo *u,
                        struct db_wallet_key *out);

/* SaplingNote belongs_to :sapling_key */
struct db_sapling_key;
bool db_sapling_note_key(struct node_db *ndb, const struct db_sapling_note *n,
                         struct db_sapling_key *out);

/* Callbacks for wallet UTXO and sapling note */
struct ar_callbacks *db_wallet_utxo_callbacks(void);
/* Test-only: re-arm the wallet_tx + wallet_utxo before/after_save hooks (see .c). */
void wallet_tx_reset_hooks_for_testing(void);
struct ar_callbacks *db_sapling_note_callbacks(void);

#endif
