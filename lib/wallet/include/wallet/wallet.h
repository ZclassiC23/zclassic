/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_WALLET_WALLET_H
#define ZCL_WALLET_WALLET_H

#include "wallet/keystore.h"
#include "wallet/hd_keychain.h"
#include "wallet/sapling_keys.h"
#include "primitives/transaction.h"
#include "chain/chain.h"
#include "core/amount.h"
#include "util/result.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stdint.h>

struct tx_mempool;
struct coins_view_cache;
struct main_state;
struct chain_params;

/* Explicit admission context for a locally-authored wallet transaction.
 *
 * Wallet-originated transactions are network inputs too: before they mutate
 * wallet state or reach relay they must pass the SAME accept_to_mempool gate
 * as P2P `tx` and sendrawtransaction. Keeping every dependency explicit
 * prevents unit scaffolding or a new controller from silently falling back to
 * tx_mempool_add_unchecked(). Production callers must provide all fields; a
 * missing field fails closed. */
struct wallet_tx_admission {
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
    struct main_state *main_state;
    const struct chain_params *params;
};

#define MAX_WALLET_TX 65536
#define MAX_KEY_POOL 2000
#define DEFAULT_KEYPOOL_SIZE 100
#define DEFAULT_TX_CONFIRM_TARGET 2
#define WALLET_FEATURE_BASE 10500
#define WALLET_FEATURE_LATEST 60000

struct wallet_key_pool_entry {
    struct key_id keyid;
    int64_t generation;
    bool persisted;
};

struct key_pool {
    int64_t time;
    struct pubkey vchPubKey;
};

enum wallet_tx_status {
    WALLET_TX_UNKNOWN,
    WALLET_TX_INMEMPOOL,
    WALLET_TX_CONFLICTED,
    WALLET_TX_CONFIRMED
};

struct wallet_tx {
    struct transaction tx;
    struct uint256 hash_block;
    int64_t time_received;
    unsigned int time_received_is_tx_time;
    bool from_me;
    bool is_trusted;
    int64_t debit_cached;
    int64_t credit_cached;
    int64_t immature_credit_cached;
    int64_t available_credit_cached;
    bool debit_cached_valid;
    bool credit_cached_valid;
    bool immature_credit_cached_valid;
    bool available_credit_cached_valid;
    int confirms;
    bool used;
};

struct sapling_received_note {
    struct uint256 txid;
    uint32_t output_index;
    uint8_t diversifier[11];
    uint8_t pk_d[32];
    uint64_t value;
    uint8_t rcm[32];
    uint8_t memo[512];
    uint8_t ivk[32];       /* which ivk decrypted this note */
    uint8_t cm[32];        /* note commitment (for merkle tree) */
    uint8_t nf[32];        /* nullifier (to detect spends) */
    int confirms;
    bool spent;
    bool used;
};

#define MAX_SAPLING_NOTES 16384
#define SPENT_SET_BUCKETS 8192

struct spent_outpoint {
    struct uint256 txid;
    uint32_t vout;
    bool occupied;
};

struct coin_entry {
    const struct wallet_tx *wtx;
    unsigned int i;
    int depth;
    bool spendable;
    bool solvable;
};

struct wallet {
    zcl_mutex_t cs;
    struct basic_keystore keystore;

    struct wallet_tx map_wallet[MAX_WALLET_TX];
    size_t num_wallet_tx;

    struct wallet_key_pool_entry key_pool[MAX_KEY_POOL];
    size_t key_pool_size;
    int64_t next_key_pool_index;

    int64_t oldest_key_pool_time;
    int64_t time_first_key;

    struct ext_key master_key;
    bool has_master_key;
    uint32_t hd_external_counter;  /* next external (receiving) address index */
    uint32_t hd_internal_counter;  /* next internal (change) address index */
    uint32_t hd_account;           /* BIP44 account number (default 0) */

    int64_t default_fee;
    int64_t min_fee;
    bool spend_zero_conf_change;

    struct block_index *best_block;
    int best_block_height;

    struct sapling_keystore sapling_keys;

    struct sapling_received_note *sapling_notes;
    size_t num_sapling_notes;
    size_t sapling_notes_cap;

    struct spent_outpoint spent_set[SPENT_SET_BUCKETS];
    size_t num_spent;
};

void wallet_init(struct wallet *w);
void wallet_rebuild_spent_set(struct wallet *w);
struct coins_view_cache;
void wallet_verify_utxos(struct wallet *w, struct coins_view_cache *coins_tip);
void wallet_free(struct wallet *w);

bool wallet_generate_new_key(struct wallet *w, struct pubkey *pk_out);
bool wallet_get_new_address(struct wallet *w, char *addr_out, size_t addr_size);
/* Same operation, also returning the exact generated/consumed key id. The
 * token lets a durability failure remove its own HD key rather than guessing
 * from the mutable keystore tail. */
bool wallet_get_new_address_with_key_id(struct wallet *w, char *addr_out,
                               size_t addr_size, struct key_id *key_id_out);
bool wallet_top_up_key_pool(struct wallet *w, unsigned int target_size);
/* A top-up publishes new entries as unpersisted. They cannot be handed to a
 * caller until a successful wallet flush marks the pool durable. */
/* Capture after top-up, then mark only that generation after the flush. This
 * watermark prevents a concurrent post-snapshot top-up from being mislabeled
 * durable. */
int64_t wallet_key_pool_generation_ceiling(const struct wallet *w);
void wallet_key_pool_mark_persisted_through(struct wallet *w,
                                            int64_t generation);
size_t wallet_key_pool_persisted_size(const struct wallet *w);
bool wallet_get_key_from_pool(struct wallet *w, struct pubkey *pk_out);

bool wallet_add_to_wallet(struct wallet *w, const struct wallet_tx *wtx);
const struct wallet_tx *wallet_get_tx(const struct wallet *w,
                                       const struct uint256 *hash);

void wallet_mark_dirty(struct wallet_tx *wtx);
bool wallet_is_mine(const struct wallet *w, const struct tx_out *txout);
bool wallet_is_watch_only(const struct wallet *w, const struct tx_out *txout);
bool wallet_is_change(const struct wallet *w, const struct tx_out *txout);

int64_t wallet_get_debit(const struct wallet *w, const struct transaction *tx);
int64_t wallet_get_balance(const struct wallet *w);
int64_t wallet_get_unconfirmed_balance(const struct wallet *w);
int64_t wallet_get_immature_balance(const struct wallet *w);

void wallet_available_coins(const struct wallet *w,
                             struct coin_entry *coins_out,
                             size_t *num_coins, size_t max_coins,
                             bool only_confirmed, bool include_zero_value);

bool wallet_select_coins(const struct wallet *w,
                          const struct coin_entry *available, size_t num_available,
                          int64_t target_value,
                          struct coin_entry *selected, size_t *num_selected,
                          size_t max_selected, int64_t *value_out);

bool wallet_create_transaction(struct wallet *w,
                                const struct tx_destination *dest,
                                int64_t value,
                                struct wallet_tx *wtx_out,
                                int64_t *fee_out,
                                const char **error);

bool wallet_create_transaction_multi(struct wallet *w,
                                      const struct tx_destination *dests,
                                      const int64_t *values,
                                      size_t num_outputs,
                                      struct wallet_tx *wtx_out,
                                      int64_t *fee_out,
                                      const char **error);

/* Validate -> admit to mempool -> record in wallet -> mark inputs spent.
 * On any validation/admission failure the wallet is left unchanged. If the
 * wallet record cannot be installed after admission, the mempool insertion is
 * rolled back. */
struct zcl_result wallet_commit_transaction(
    struct wallet *w, struct wallet_tx *wtx,
    const struct wallet_tx_admission *admission);

/* Undo an unrelayed commit after a durability step fails. Removes the
 * transaction from the mempool and wallet map and restores transparent and
 * shielded spent markers. This is only safe before relay. */
struct zcl_result wallet_rollback_transaction(
    struct wallet *w, const struct wallet_tx *wtx,
    struct tx_mempool *mempool);

void wallet_sync_transaction(struct wallet *w, const struct transaction *tx,
                              const struct block_index *pindex);

/* Advance every stored confirmation depth after the chain tip moved FORWARD,
 * and republish w->best_block_height as `new_best_height`.
 *
 * wallet_sync_transaction() stamps wtx->confirms once, when the transaction's
 * block is connected, and nothing ever raises it again — so a coinbase mined
 * at the tip keeps confirms=1 forever and
 * wallet_tx_get_blocks_to_maturity() never reaches 0: the coins can never be
 * spent by the in-RAM coin selector no matter how far the chain advances.
 * Call this on each forward tip advance, BEFORE syncing the new block's
 * transactions, so the freshly-connected ones are stamped against the correct
 * best height.
 *
 * Only a strictly forward move does anything: a lower or equal
 * `new_best_height` (a reorg, or a replayed height) leaves every depth
 * untouched, so a depth can never be inflated past the real chain. Returns the
 * number of transactions whose depth was raised, 0 when there was nothing to
 * do, or -1 on a NULL wallet. */
int wallet_advance_confirmations(struct wallet *w, int new_best_height);

/* HD wallet initialization */
bool wallet_init_hd(struct wallet *w, const unsigned char *seed, size_t seed_len);
bool wallet_init_hd_from_mnemonic(struct wallet *w, const char *mnemonic,
                                   const char *passphrase);
bool wallet_has_hd(const struct wallet *w);

/* ── Recovery phrase ──────────────────────────────────────────────
 *
 * One phrase, one seed, both key trees. `phrase` is a BIP39 mnemonic;
 * mnemonic_to_wallet_seed() turns it into the single 32-byte seed this
 * wallet persists, and that seed is installed into BOTH the Sapling
 * keystore (ZIP32 master) and the transparent HD chain (BIP32 master).
 * Nothing stores the phrase — the seed is the only thing that lands on
 * disk, and the phrase cannot be recovered from it.
 *
 * Call on a freshly wallet_init()'d wallet, BEFORE any key is minted, so
 * every key the wallet grows descends from the phrase. Returns false on a
 * phrase that fails BIP39 validation; the phrase is never logged, never
 * echoed into an error, and the derived seed is cleansed before return. */
bool wallet_init_from_recovery_phrase(struct wallet *w, const char *phrase);

/* Adopt a seed that was just loaded from disk as this wallet's transparent
 * HD master — but ONLY when this seed provably governs the keys already in
 * the keystore.
 *
 * The test is derivation, not a stored flag: a wallet whose external key 0
 * is the key this seed derives at m/44'/147'/0'/0/0 was grown from this
 * seed, so its later keys must keep coming from it. A wallet created before
 * recovery phrases has independently random transparent keys; index 0 will
 * not be among them, and this function then leaves the wallet exactly as it
 * found it — legacy random key generation, unchanged derivation, no
 * migration. That is deliberate: silently re-pointing an existing wallet's
 * key derivation is how money disappears.
 *
 * An empty keystore adopts unconditionally (a restore in progress).
 *
 * On adoption the external/internal counters are recovered by scanning
 * forward from 0 for the first index whose key is NOT in the keystore, so
 * the next address minted is the next unused one rather than a duplicate of
 * an existing address. The counters are DERIVED from the keystore on every
 * load; they are never a second stored copy of the same fact.
 *
 * Returns true when the seed was adopted, false when it was declined or on
 * a NULL/derivation error. */
bool wallet_hd_adopt_seed(struct wallet *w,
                          const unsigned char seed[32]);

/* Encode the transparent address at m/44'/147'/account'/change/index for
 * `seed`, without touching any wallet state. `change` is BIP44_EXTERNAL
 * (receiving) or BIP44_INTERNAL (change). This is how a caller shows the
 * user which address a recovery phrase brings back, and how a test proves
 * that a restored wallet's first address equals the created one's. */
bool wallet_seed_address_at(const unsigned char seed[32], uint32_t account,
                            uint32_t change, uint32_t index,
                            char *addr_out, size_t addr_size);
bool wallet_get_new_change_address(struct wallet *w, char *addr_out,
                                    size_t addr_size);

/* Add a private key to the wallet keystore. Returns true on success,
 * false if the key is invalid or the keystore add fails. */
bool wallet_import_key(struct wallet *w, const struct privkey *key);
/* Rollback a prior wallet_import_key(). Returns true if the key was
 * found and removed. Used by the controller when persistence fails
 * after keystore add, to keep in-memory and on-disk in sync. */
bool wallet_remove_key(struct wallet *w, const struct key_id *keyid);
/* Look up a private key by key id. Returns true and fills key_out if
 * the key is present in the keystore, false if it is not found. */
bool wallet_dump_key(const struct wallet *w, const struct key_id *keyid,
                      struct privkey *key_out);

struct active_chain;

/* Minimum share of the INDEXED heights in a rescan range whose bodies must
 * actually be read off disk before the scan's "found N" is allowed to mean
 * anything. Justification, from the three non-test sites that ever clear
 * BLOCK_HAVE_DATA — this codebase has no block-file pruning (no fPruneMode /
 * nPruneTarget anywhere), so a cleared flag is never a routine, expected
 * state:
 *   1. snapshot_controller_import.c (header_only) strips it across the WHOLE
 *      imported range — the fast-sync bootstrap, i.e. no bodies at all;
 *   2. boot.c clears it only ABOVE the tip (outside any rescan range);
 *   3. boot_services.c clears it for entries pointing at empty/missing
 *      block files — again a genuine "the body is not here".
 * All three mean the node cannot see those transactions. The one benign
 * cause of a small shortfall is a ragged tail of blocks still in flight
 * during body download, which is bounded and transient; 1% leaves room for
 * that while still catching a snapshot-bootstrapped node (~100% missing). */
#define WALLET_RESCAN_MIN_COVERAGE_PCT 99

/* Coverage + yield accounting for one rescan.
 *
 * This exists because wallet_scan_block() returns a bare 0 for BOTH "this
 * block holds nothing of yours" and "this node has no body for that block",
 * which let a rescan over a body-less range report "0 wallet outputs found"
 * and look like a definitive answer. Every counter below is filled by
 * wallet_rescan_report(); `blocker` is set to a typed, stable name whenever
 * the scan's result must NOT be read as "you have no funds". */
struct wallet_rescan_report {
    int      start_height;
    int      stop_height;
    int64_t  blocks_in_range;        /* stop - start + 1 */
    int64_t  blocks_indexed;         /* active_chain_at() returned an entry */
    int64_t  blocks_scanned;         /* body read off disk AND folded in */
    int64_t  blocks_no_index;        /* active_chain_at() returned NULL */
    int64_t  blocks_missing_data;    /* !(nStatus & BLOCK_HAVE_DATA) */
    int64_t  blocks_read_failed;     /* read_block_from_disk_index() failed */
    int64_t  outputs_found;          /* transparent outputs that are ours */
    int64_t  shielded_notes_found;   /* Sapling notes trial-decrypted */
    int64_t  shielded_txs_unscanned; /* txs with shielded outputs we could
                                      * not even try (no Sapling keys) */
    size_t   sapling_key_count;      /* Sapling keys held at scan time */
    bool     shielded_scan_skipped;  /* advisory: shielded outputs went
                                      * untried because the wallet holds no
                                      * Sapling key — the seed-only-restore
                                      * state. Never on its own a blocker: a
                                      * transparent-only wallet is a normal,
                                      * legitimate configuration. */
    bool     coverage_ok;            /* false whenever blocker[0] is set */
    char     blocker[48];            /* "" when coverage_ok */
};

/* Typed blocker names published in `blocker`. Stable strings — an operator
 * and an agent both key off these. */
#define WALLET_RESCAN_BLOCKER_NO_BLOCK_DATA   "RESCAN_NO_BLOCK_DATA"
#define WALLET_RESCAN_BLOCKER_INCOMPLETE      "RESCAN_INCOMPLETE_COVERAGE"
#define WALLET_RESCAN_BLOCKER_INCONCLUSIVE    "RESCAN_INCONCLUSIVE_ZERO"

int wallet_scan_block(struct wallet *w, const struct block_index *pindex,
                      const char *datadir);
/* Rescan [start_height, stop_height] and fill `out` with the coverage and
 * yield accounting. Returns the number of wallet outputs + shielded notes
 * found, identical to wallet_rescan(). `out` may be NULL. Inspect
 * out->coverage_ok / out->blocker before treating a 0 return as "no funds". */
int wallet_rescan_report(struct wallet *w, const struct active_chain *chain,
                         int start_height, int stop_height,
                         const char *datadir,
                         struct wallet_rescan_report *out);
/* Thin wrapper over wallet_rescan_report() that discards the report. */
int wallet_rescan(struct wallet *w, const struct active_chain *chain,
                  int start_height, int stop_height, const char *datadir);
int wallet_scan_blockfiles(struct wallet *w, const char *datadir);

int wallet_tx_get_blocks_to_maturity(const struct wallet_tx *wtx);

/* Spent outpoint tracking */
void wallet_mark_outpoint_spent(struct wallet *w,
                                 const struct uint256 *txid, uint32_t vout);
void wallet_unmark_outpoint_spent(struct wallet *w,
                                   const struct uint256 *txid, uint32_t vout);
bool wallet_is_outpoint_spent(const struct wallet *w,
                               const struct uint256 *txid, uint32_t vout);

/* Sapling note trial decryption and balance */
struct output_description;
int wallet_try_sapling_decrypt(struct wallet *w,
                                const struct transaction *tx,
                                const struct uint256 *txid);
bool wallet_sapling_nullifier_is_spent(const struct wallet *w,
                                        const uint8_t nf[32]);
void wallet_mark_sapling_nullifiers_spent(struct wallet *w,
                                           const struct transaction *tx);
int64_t wallet_get_sapling_balance(const struct wallet *w);

/* Return a heap-allocated point-in-time snapshot of all sapling notes,
 * copied under w->cs (caller owns the buffer and must free() it). Sets
 * *count to the number copied. Returns NULL when there are no notes or on
 * OOM (with *count == 0). This lets readers iterate the notes without
 * holding the wallet lock or racing a concurrent note-append realloc. */
struct sapling_received_note *wallet_copy_sapling_notes(const struct wallet *w,
                                                         size_t *count);

#endif
