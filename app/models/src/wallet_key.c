/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: WalletKey, SaplingKey, WalletSeed, WalletScript
 *
 * validates :pubkey_hash, presence: true
 * validates :pubkey, presence: true
 * validates :privkey, presence: true
 * validates :pubkey_len, custom: "must be 33 (compressed)"
 *
 * SaplingKey:
 * validates :ivk, presence: true
 * validates :xsk, :xfvk, :diversifier, :pk_d, presence: true
 * validates :address, string_present: true */

#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "encoding/utilstrencodings.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "keys/key.h"
#include "keys/key_io.h"
#include "keys/pubkey.h"
#include "chain/chainparams.h"
#include "script/standard.h"
#include "storage/event_log_payloads.h"
#include "storage/wallet_projection.h"
#include "support/cleanse.h"
#include "util/result.h"
#include "event/event.h"
#include "util/log_json.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "util/safe_alloc.h"

/* WSQL_* error codes used by the rich-error save. */
#include "wallet/wallet_sqlite.h"

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(wallet_key)
DEFINE_MODEL_CALLBACKS(sapling_key)
DEFINE_MODEL_CALLBACKS(wallet_script)

static bool wallet_key_before_save(void *record, void *ctx)
{
    (void)record;
    (void)ctx;
    /* If the operator set a passphrase, the keystore-at-rest path is
     * the right place to wrap the privkey before persistence.  This
     * model does not own that crypto; signal that wrapping is pending
     * via a structured log line and let the save proceed unencrypted
     * (legacy behaviour) until the keystore wiring lands. */
    if (getenv("ZCL_WALLET_PASSPHRASE"))
        log_jsonf(LOG_JSON_WARN, "wallet_key.passphrase_set_pending_encryption",
                  "\"model\":\"wallet_key\"");
    return true;
}

static void wallet_key_after_save(void *record, void *ctx)
{
    (void)ctx;
    const struct db_wallet_key *k = record;
    char addr_hex[41];
    char address[EV_WALLET_ADDRESS_MAX + 1];
    HexStr(k->pubkey_hash, 20, false, addr_hex, sizeof(addr_hex));
    event_emitf(EV_WALLET_KEY_SAVED, 0,
                "kind=transparent addr_hash=%s", addr_hex);

    memset(address, 0, sizeof(address));
    const struct chain_params *cp = chain_params_get();
    if (cp) {
        size_t pk_pfx_len = 0;
        size_t sc_pfx_len = 0;
        const unsigned char *pk_pfx = chain_params_base58_prefix(
            cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
        const unsigned char *sc_pfx = chain_params_base58_prefix(
            cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);
        struct tx_destination dest;
        memset(&dest, 0, sizeof(dest));
        dest.type = DEST_KEY_ID;
        memcpy(dest.id.key.id.data, k->pubkey_hash, 20);
        (void)encode_destination(&dest, pk_pfx, pk_pfx_len,
                                 sc_pfx, sc_pfx_len,
                                 address, sizeof(address));
    }

    if (!wallet_projection_emit_key_add(k->pubkey_hash, address, "",
            (uint32_t)k->created_at)) {
        LOG_WARN("wallet_projection", "[wallet_projection] key add projection emit failed");
    }
}

static void wallet_key_init_hooks(void)
{
    static bool done = false;
    if (done) return;
    struct ar_callbacks *cbs = db_wallet_key_callbacks();
    ar_register_before_save(cbs, wallet_key_before_save);
    ar_register_after_save(cbs, wallet_key_after_save);
    done = true;
}

static bool sapling_key_before_save(void *record, void *ctx)
{
    (void)record;
    (void)ctx;
    if (getenv("ZCL_WALLET_PASSPHRASE"))
        log_jsonf(LOG_JSON_WARN, "wallet_key.passphrase_set_pending_encryption",
                  "\"model\":\"sapling_key\"");
    return true;
}

static void sapling_key_after_save(void *record, void *ctx)
{
    (void)ctx;
    const struct db_sapling_key *k = record;
    char fvk_hex[33];
    HexStr(k->xfvk, 16, false, fvk_hex, sizeof(fvk_hex));
    char addr_hex[33];
    HexStr(k->ivk, 16, false, addr_hex, sizeof(addr_hex));
    event_emitf(EV_WALLET_KEY_SAVED, 0,
                "kind=sapling addr_hash=%s", addr_hex);
    event_emitf(EV_SAPLING_KEY_SAVED, 0, "fvk_hash=%s", fvk_hex);
}

static void sapling_key_init_hooks(void)
{
    static bool done = false;
    if (done) return;
    struct ar_callbacks *cbs = db_sapling_key_callbacks();
    ar_register_before_save(cbs, sapling_key_before_save);
    ar_register_after_save(cbs, sapling_key_after_save);
    done = true;
}

/* Test-only: re-arm the wallet_key AND sapling_key model hooks after a group
 * cleared the shared callback structs via ar_callbacks_init() (test_activerecord
 * does this to silence emits, leaving them wiped for later groups). Mirrors the
 * init_hooks pair but bypasses their one-shot `done` guards. */
void wallet_key_reset_hooks_for_testing(void)
{
    struct ar_callbacks *wk = db_wallet_key_callbacks();
    ar_callbacks_init(wk);
    ar_register_before_save(wk, wallet_key_before_save);
    ar_register_after_save(wk, wallet_key_after_save);
    struct ar_callbacks *sk = db_sapling_key_callbacks();
    ar_callbacks_init(sk);
    ar_register_before_save(sk, sapling_key_before_save);
    ar_register_after_save(sk, sapling_key_after_save);
}

/* ── Validation ────────────────────────────────────────────────── */

bool db_wallet_key_validate(const struct db_wallet_key *k,
                            struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, k, pubkey_hash);
    validates_presence_of(errors, k, pubkey);
    validates_presence_of(errors, k, privkey);
    validates_custom(errors, k->pubkey_len == 33,
                     "pubkey_len", "must be 33 (compressed)");
    validates_custom(errors, !(k->pubkey_len == 33 && !k->compressed),
                     "compressed", "must be true for 33-byte pubkey");
    validates_non_negative(errors, k, created_at);
    return !ar_errors_any(errors);
}

bool db_sapling_key_validate(const struct db_sapling_key *k,
                             struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, k, ivk);
    validates_presence_of(errors, k, xsk);
    validates_presence_of(errors, k, xfvk);
    validates_presence_of(errors, k, diversifier);
    validates_presence_of(errors, k, pk_d);
    validates_string_present(errors, k->address, "address");
    return !ar_errors_any(errors);
}

bool db_wallet_script_validate(const struct db_wallet_script *sc,
                               struct ar_errors *errors)
{
    static const uint8_t zero[20] = {0};
    ar_errors_clear(errors);
    validates_custom(errors,
        memcmp(sc->script_hash, zero, 20) != 0,
        "script_hash", "can't be blank");
    validates_custom(errors,
        sc->redeem_script != NULL,
        "redeem_script", "can't be null");
    validates_positive(errors, sc, script_len);
    validates_max(errors, sc, script_len, 10000);
    return !ar_errors_any(errors);
}

/* ── WalletKey CRUD ───────────────────────────────────────────── */

bool db_wallet_key_save(struct node_db *ndb, const struct db_wallet_key *k)
{
    if (!ndb->open) return false;
    if (k->created_at == 0)
        ((struct db_wallet_key *)k)->created_at = (int64_t)platform_time_wall_time_t();

    wallet_key_init_hooks();
    struct ar_callbacks *cbs = db_wallet_key_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO wallet_keys"
        "(pubkey_hash,pubkey,privkey,compressed,created_at)"
        " VALUES(?,?,?,?,?)",
        cbs, "wallet_key", k, db_wallet_key_validate,
        AR_BIND_BLOB(s, 1, k->pubkey_hash, 20);
        AR_BIND_BLOB(s, 2, k->pubkey, (int)k->pubkey_len);
        AR_BIND_BLOB(s, 3, k->privkey, 32);
        AR_BIND_INT(s, 4, k->compressed ? 1 : 0);
        AR_BIND_INT(s, 5, k->created_at));
}


bool db_wallet_key_find(struct node_db *ndb, const uint8_t pubkey_hash[20],
                        struct db_wallet_key *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT pubkey,privkey,compressed,created_at"
        " FROM wallet_keys WHERE pubkey_hash=?",
        AR_BIND_BLOB(s, 1, pubkey_hash, 20),
        memset(out, 0, sizeof(*out));
        memcpy(out->pubkey_hash, pubkey_hash, 20);
        int pk_len = AR_COL_BYTES(s, 0);
        const void *pk = sqlite3_column_blob(s, 0);
        if (pk && pk_len <= 33) {
            memcpy(out->pubkey, pk, (size_t)pk_len);
            out->pubkey_len = (size_t)pk_len;
        }
        AR_READ_BLOB(s, 1, out->privkey, 32);
        out->compressed = AR_COL_INT(s, 2) != 0;
        out->created_at = AR_COL_INT(s, 3));
}

bool db_wallet_key_delete(struct node_db *ndb, const uint8_t pubkey_hash[20])
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_wallet_key_callbacks();
    struct db_wallet_key k;
    memset(&k, 0, sizeof(k));
    memcpy(k.pubkey_hash, pubkey_hash, 20);
    sqlite3_stmt *s = NULL;
    AR_ADHOC_DESTROY(ndb, s, "DELETE FROM wallet_keys WHERE pubkey_hash=?",
        cbs, &k, AR_BIND_BLOB(s, 1, pubkey_hash, 20));
}

bool db_wallet_key_exists(struct node_db *ndb, const uint8_t pubkey_hash[20])
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    AR_QUERY_EXISTS(ndb, s,
        "SELECT 1 FROM wallet_keys WHERE pubkey_hash=?",
        AR_BIND_BLOB(s, 1, pubkey_hash, 20));
}

int db_wallet_key_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    AR_QUERY_COUNT_SQL(ndb, "SELECT COUNT(*) FROM wallet_keys");
}

bool db_wallet_key_first_pubkey_hash(struct node_db *ndb, uint8_t out[20])
{
    if (!ndb || !ndb->open || !out)
        return false;
    sqlite3_stmt *s = NULL;
    AR_PREPARE_BOOL(ndb, s,
        "SELECT pubkey_hash FROM wallet_keys LIMIT 1");
    bool ok = false;
    if (AR_STEP_ROW(s)) {
        const void *pkh = sqlite3_column_blob(s, 0);
        if (pkh && AR_COL_BYTES(s, 0) == 20) {
            memcpy(out, pkh, 20);
            ok = true;
        }
    }
    AR_FINALIZE(s);
    return ok;
}

/* ── Row Deserialization ──────────────────────────────────────── */

static void row_to_wallet_key(sqlite3_stmt *s, struct db_wallet_key *k)
{
    memset(k, 0, sizeof(*k));
    AR_READ_BLOB(s, 0, k->pubkey_hash, 20);
    int pk_len = AR_COL_BYTES(s, 1);
    const void *pk = sqlite3_column_blob(s, 1);
    if (pk && pk_len <= 33) {
        memcpy(k->pubkey, pk, (size_t)pk_len);
        k->pubkey_len = (size_t)pk_len;
    }
    AR_READ_BLOB(s, 2, k->privkey, 32);
    k->compressed = AR_COL_INT(s, 3) != 0;
    k->created_at = AR_COL_INT(s, 4);
}

int db_wallet_key_each(struct node_db *ndb, wallet_key_cb cb, void *ctx)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    AR_PREPARE_RET(ndb, s,
        "SELECT pubkey_hash,pubkey,privkey,compressed,created_at"
        " FROM wallet_keys",
        0);
    int count = 0;
    while (AR_STEP_ROW(s)) {
        struct db_wallet_key k;
        row_to_wallet_key(s, &k);
        cb(&k, ctx);
        memory_cleanse(k.privkey, 32);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

/* ── SaplingKey CRUD ──────────────────────────────────────────── */

bool db_sapling_key_save(struct node_db *ndb, const struct db_sapling_key *k)
{
    if (!ndb->open) return false;
    sapling_key_init_hooks();
    struct ar_callbacks *cbs = db_sapling_key_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO wallet_sapling_keys"
        "(ivk,xsk,xfvk,diversifier,pk_d,child_index,address)"
        " VALUES(?,?,?,?,?,?,?)",
        cbs, "sapling_key", k, db_sapling_key_validate,
        AR_BIND_BLOB(s, 1, k->ivk, 32);
        AR_BIND_BLOB(s, 2, k->xsk, 169);
        AR_BIND_BLOB(s, 3, k->xfvk, 169);
        AR_BIND_BLOB(s, 4, k->diversifier, 11);
        AR_BIND_BLOB(s, 5, k->pk_d, 32);
        AR_BIND_INT(s, 6, (int)k->child_index);
        AR_BIND_TEXT(s, 7, k->address));
}

/* ── SaplingKey Row Deserialization ────────────────────────────── */

static void row_to_sapling_key(sqlite3_stmt *s, int col,
                                struct db_sapling_key *out)
{
    memset(out, 0, sizeof(*out));
    AR_READ_BLOB(s, col, out->ivk, 32);
    AR_READ_BLOB(s, col + 1, out->xsk, 169);
    AR_READ_BLOB(s, col + 2, out->xfvk, 169);
    AR_READ_BLOB(s, col + 3, out->diversifier, 11);
    AR_READ_BLOB(s, col + 4, out->pk_d, 32);
    out->child_index = (uint32_t)AR_COL_INT(s, col + 5);
    AR_READ_STR(s, col + 6, out->address, sizeof(out->address));
}

bool db_sapling_key_find_by_ivk(struct node_db *ndb, const uint8_t ivk[32],
                                struct db_sapling_key *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT ivk,xsk,xfvk,diversifier,pk_d,child_index,address"
        " FROM wallet_sapling_keys WHERE ivk=?",
        AR_BIND_BLOB(s, 1, ivk, 32),
        if (out)
            row_to_sapling_key(s, 0, out));
}

bool db_sapling_key_find_by_address(struct node_db *ndb, const char *address,
                                    struct db_sapling_key *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT ivk,xsk,xfvk,diversifier,pk_d,child_index,address"
        " FROM wallet_sapling_keys WHERE address=?",
        AR_BIND_TEXT(s, 1, address),
        row_to_sapling_key(s, 0, out));
}

int db_sapling_key_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    AR_QUERY_COUNT_SQL(ndb, "SELECT COUNT(*) FROM wallet_sapling_keys");
}

/* ── Wallet Seed ──────────────────────────────────────────────── */

bool db_wallet_seed_save(struct node_db *ndb, const uint8_t seed[32],
                         uint32_t next_child)
{
    if (!ndb->open) return false;
    static const uint8_t zero[32] = {0};
    if (memcmp(seed, zero, 32) == 0) return false;

    sqlite3_stmt *s = NULL;
    AR_EXEC_BOOL(ndb, s,
        "INSERT OR REPLACE INTO wallet_seed(id,seed,next_child)"
        " VALUES(1,?,?)",
        AR_BIND_BLOB(s, 1, seed, 32);
        AR_BIND_INT(s, 2, (int)next_child));
}

bool db_wallet_seed_load(struct node_db *ndb, uint8_t seed[32],
                         uint32_t *next_child)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT seed,next_child FROM wallet_seed WHERE id=1",
        (void)0,
        AR_READ_BLOB(s, 0, seed, 32);
        *next_child = (uint32_t)AR_COL_INT(s, 1));
}

/* ── Redeem Scripts ───────────────────────────────────────────── */

bool db_wallet_script_save(struct node_db *ndb, const struct db_wallet_script *sc)
{
    if (!ndb->open) return false;
    struct ar_callbacks *cbs = db_wallet_script_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO wallet_scripts(script_hash,redeem_script)"
        " VALUES(?,?)",
        cbs, "wallet_script", sc, db_wallet_script_validate,
        AR_BIND_BLOB(s, 1, sc->script_hash, 20);
        AR_BIND_BLOB(s, 2, sc->redeem_script, (int)sc->script_len));
}

bool db_wallet_script_find(struct node_db *ndb, const uint8_t script_hash[20],
                           struct db_wallet_script *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT redeem_script FROM wallet_scripts WHERE script_hash=?",
        AR_BIND_BLOB(s, 1, script_hash, 20),
        memset(out, 0, sizeof(*out));
        memcpy(out->script_hash, script_hash, 20);
        out->script_len = (size_t)AR_COL_BYTES(s, 0);
        const void *rs = sqlite3_column_blob(s, 0);
        if (rs && out->script_len > 0) {
            out->redeem_script = zcl_malloc(out->script_len, "wallet_key redeem_script find");
            if (out->redeem_script)
                memcpy(out->redeem_script, rs, out->script_len);
        });
}

/* ── Relationships ─────────────────────────────────────────────── */

/* WalletKey has_many :wallet_utxos */
int db_wallet_key_utxos(struct node_db *ndb, const uint8_t pubkey_hash[20],
                        struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,script,height,is_coinbase"
        " FROM wallet_utxos WHERE address_hash=?"
        " AND spent_txid IS NULL ORDER BY value DESC LIMIT ?",
        -1, &s, NULL);
    if (!s) LOG_RETURN(0, "wallet_key", "prepare failed: %s", sqlite3_errmsg(ndb->db));
    AR_BIND_BLOB(s, 1, pubkey_hash, 20);
    AR_BIND_INT(s, 2, (int)max);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        AR_READ_BLOB(s, 0, out[count].txid, 32);
        out[count].vout = (uint32_t)AR_COL_INT(s, 1);
        out[count].value = AR_COL_INT(s, 2);
        memcpy(out[count].address_hash, pubkey_hash, 20);
        out[count].script_len = (size_t)AR_COL_BYTES(s, 3);
        out[count].script = NULL;
        out[count].height = (int)AR_COL_INT(s, 4);
        out[count].is_coinbase = AR_COL_INT(s, 5) != 0;
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

/* SaplingKey has_many :sapling_notes */
int db_sapling_key_notes(struct node_db *ndb, const uint8_t ivk[32],
                         struct db_sapling_note *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,output_index,value,rcm,memo,diversifier,pk_d,"
        "cm,nullifier,block_height,source"
        " FROM wallet_sapling_notes WHERE ivk=?"
        " AND spent_txid IS NULL ORDER BY value DESC LIMIT ?",
        -1, &s, NULL);
    if (!s) LOG_RETURN(0, "wallet_key", "prepare failed: %s", sqlite3_errmsg(ndb->db));
    AR_BIND_BLOB(s, 1, ivk, 32);
    AR_BIND_INT(s, 2, (int)max);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        AR_READ_BLOB(s, 0, out[count].txid, 32);
        out[count].output_index = (uint32_t)AR_COL_INT(s, 1);
        out[count].value = AR_COL_INT(s, 2);
        AR_READ_BLOB(s, 3, out[count].rcm, 32);
        int memo_len = AR_COL_BYTES(s, 4);
        const void *memo = sqlite3_column_blob(s, 4);
        if (memo && memo_len > 0) {
            size_t ml = (size_t)memo_len < 512 ? (size_t)memo_len : 512;
            memcpy(out[count].memo, memo, ml);
            out[count].memo_len = ml;
        }
        memcpy(out[count].ivk, ivk, 32);
        AR_READ_BLOB(s, 5, out[count].diversifier, 11);
        AR_READ_BLOB(s, 6, out[count].pk_d, 32);
        AR_READ_BLOB(s, 7, out[count].cm, 32);
        AR_READ_BLOB(s, 8, out[count].nullifier, 32);
        if (sqlite3_column_type(s, 9) != SQLITE_NULL)
            out[count].block_height = (int)AR_COL_INT(s, 9);
        AR_READ_STR(s, 10, out[count].source, sizeof(out[count].source));
        if (!out[count].source[0])
            snprintf(out[count].source, sizeof(out[count].source), "%s",
                     DB_SAPLING_NOTE_SOURCE_LOCAL);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}
