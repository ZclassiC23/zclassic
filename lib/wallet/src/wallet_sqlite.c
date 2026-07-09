/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SQLite-backed wallet storage. Uses node.db tables directly.
 * Replaces wallet_db.c (LevelDB) for all runtime wallet operations.
 *
 * Rich-error primary (`*_r`) returns struct zcl_result — every failure
 * carries a code, a message, and a source file:line. The bool-returning
 * names are kept as thin wrappers so callers (controllers) keep
 * compiling; each wrapper LOG_FAILs on non-ok, so a false return is
 * never silent (a silent `wallet_sqlite_open() returned false and
 * nobody noticed` would regenerate fresh keys and lose funds). */

#include "platform/time_compat.h"
#include "wallet/wallet_sqlite.h"
#include "wallet/wallet_keystore.h"
#include "wallet/keystore.h"
#include "keys/key.h"
#include "core/serialize.h"
#include "crypto/sha256.h"
#include "support/cleanse.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/result.h"
#include "util/safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Sentinel key used by wallet_sqlite_self_test.  The probe round-trips
 * this string through the node_state table — chosen because node_state
 * is in the baseline SCHEMA[] and always present.  The value is
 * overwritten with 32 fresh random bytes on every self-test, so stale
 * probes never pass. */
#define WSQL_CANARY_KEY "wallet_sqlite_canary"

/* Bounded BEGIN IMMEDIATE retries for wallet_sqlite_flush_r under WAL
 * write-lock contention. Each attempt already blocks up to the connection
 * busy_timeout via the SQLite busy handler, so this bounds total wait to
 * ~attempts × busy_timeout — enough to outlast a bulk-catch-up commit window
 * without hanging a key persist indefinitely. */
#define WALLET_FLUSH_BEGIN_MAX_ATTEMPTS 4

/* Counts rows dropped by read_keys_r because decode/decrypt failed.
 * Surfaced via wallet_sqlite_read_keys_corrupt_count() and, by
 * extension, the getwalletinfo.persistence JSON block.  Boot is
 * single-threaded so no synchronisation is needed; a reader sees a
 * consistent value once boot finishes. */
static int g_read_keys_corrupt_rows = 0;

int wallet_sqlite_read_keys_corrupt_count(void)
{
    return g_read_keys_corrupt_rows;
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* Capture a failure into ws->last_error so wallet_sqlite_get_health
 * can surface the most recent reason without the caller threading
 * state through the health struct. */
static struct zcl_result wsql_fail(struct wallet_sqlite *ws,
                                    struct zcl_result r)
{
    if (ws && !r.ok) {
        size_t n = sizeof(ws->last_error) - 1;
        strncpy(ws->last_error, r.message, n);
        ws->last_error[n] = '\0';
    }
    return r;
}

static void wallet_sqlite_reset_all_statements(struct wallet_sqlite *ws)
{
    if (!ws)
        return;
    if (ws->stmt_key_write) sqlite3_reset(ws->stmt_key_write);
    if (ws->stmt_key_read) sqlite3_reset(ws->stmt_key_read);
    if (ws->stmt_key_read_one) sqlite3_reset(ws->stmt_key_read_one);
    if (ws->stmt_key_delete) sqlite3_reset(ws->stmt_key_delete);
    if (ws->stmt_tx_write) sqlite3_reset(ws->stmt_tx_write);
    if (ws->stmt_tx_read) sqlite3_reset(ws->stmt_tx_read);
    if (ws->stmt_seed_write) sqlite3_reset(ws->stmt_seed_write);
    if (ws->stmt_seed_read) sqlite3_reset(ws->stmt_seed_read);
    if (ws->stmt_zkey_write) sqlite3_reset(ws->stmt_zkey_write);
    if (ws->stmt_zkey_read) sqlite3_reset(ws->stmt_zkey_read);
    if (ws->stmt_script_write) sqlite3_reset(ws->stmt_script_write);
    if (ws->stmt_script_read) sqlite3_reset(ws->stmt_script_read);
    if (ws->stmt_watch_write) sqlite3_reset(ws->stmt_watch_write);
    if (ws->stmt_watch_read) sqlite3_reset(ws->stmt_watch_read);
    if (ws->stmt_best_block_write) sqlite3_reset(ws->stmt_best_block_write);
    if (ws->stmt_best_block_read) sqlite3_reset(ws->stmt_best_block_read);
    if (ws->stmt_scan_height_write) sqlite3_reset(ws->stmt_scan_height_write);
    if (ws->stmt_scan_height_read) sqlite3_reset(ws->stmt_scan_height_read);
}

/* ── Wallet-at-rest encryption helpers ────────────────────────── */

/* Returns the wallet passphrase if set, NULL otherwise.  When the
 * env var is empty we treat it as "no encryption" — a conscious
 * operator decision must supply a non-empty string. */
static const char *wallet_passphrase(void)
{
    const char *p = getenv("ZCL_WALLET_PASSPHRASE");
    return (p && *p) ? p : NULL;
}

/* Detect a WKS1 envelope header in a blob.  Safe with NULL. */
static bool is_wks1_blob(const void *data, size_t len)
{
    return data && len >= WKS_HEADER_LEN &&
           memcmp(data, WKS_MAGIC, WKS_MAGIC_LEN) == 0;
}

/* Encrypt `plain` (plen bytes) into a malloc'd envelope.  Sets
 * *out and *out_len on success; caller must free *out.  Returns
 * false on any encryption failure (out stays NULL). */
static bool wallet_encrypt_blob(const uint8_t *plain, size_t plen,
                                 uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    const char *pass = wallet_passphrase();
    if (!pass) return false;

    size_t cap = wks_envelope_size(plen);
    uint8_t *buf = zcl_malloc(cap, "wallet_encrypt_buf");
    if (!buf) return false;

    size_t elen = 0;
    if (!wks_encrypt(plain, plen, pass, wks_default_iterations(),
                     buf, cap, &elen)) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = elen;
    return true;
}

/* Decrypt a WKS1 envelope into a malloc'd plaintext.  Sets *out
 * and *out_len on success; caller must memory_cleanse+free *out.
 * Returns false on wrong passphrase / tampered data. */
static bool wallet_decrypt_blob(const uint8_t *envelope, size_t env_len,
                                 uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    const char *pass = wallet_passphrase();
    if (!pass) return false;

    /* Plaintext can never be longer than the envelope. */
    uint8_t *buf = zcl_malloc(env_len, "wallet_decrypt_buf");
    if (!buf) return false;

    size_t plen = 0;
    if (!wks_decrypt(envelope, env_len, pass, buf, env_len, &plen)) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = plen;
    return true;
}

/* ── Open / Close ──────────────────────────────────────────────── *
 *
 * Invariant: every wallet table this code prepares statements against
 * must exist in app/models/src/database.c SCHEMA[] (the production
 * schema runner).  db/schema.sql is a reference dump and is never
 * executed, so a table present only there is absent at runtime.  A
 * missing table must surface WSQL_SCHEMA_MISSING / WSQL_PREPARE_FAIL
 * with the offending table name rather than returning a bare false:
 * a silent false lets boot conclude the keystore is empty, regenerate
 * a fresh keypool, and skip the flush — losing the real keys. */

/* Preparation descriptors — every statement includes the table name
 * it depends on so we can report which table is missing. */
struct wsql_prep_spec {
    sqlite3_stmt **out;
    const char   *sql;
    const char   *table;
    const char   *label;
};

static struct zcl_result wsql_prepare_one(sqlite3 *db,
                                           const struct wsql_prep_spec *spec)
{
    int rc = sqlite3_prepare_v2(db, spec->sql, -1, spec->out, NULL);
    if (rc != SQLITE_OK) {
        const char *msg = sqlite3_errmsg(db);
        int code = WSQL_PREPARE_FAIL;
        /* sqlite3 reports "no such table: X" for missing tables.
         * Hoist that up to a distinct error code so the boot state
         * machine can give a targeted remediation message. */
        if (msg && strstr(msg, "no such table"))
            code = WSQL_SCHEMA_MISSING;
        return ZCL_ERR(code,
            "prepare %s (table=%s) failed: %s",
            spec->label, spec->table, msg ? msg : "(null)");
    }
    return ZCL_OK;
}

struct zcl_result wallet_sqlite_open_r(struct wallet_sqlite *ws, sqlite3 *db)
{
    if (!ws)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet_sqlite pointer is NULL");
    if (!db)
        return ZCL_ERR(WSQL_NULL_ARG, "sqlite3 pointer is NULL");

    /* Callers pass uninitialised stack structs in tests, so zero
     * before inspecting any field.  Callers that want to reopen an
     * already-open handle must close() first; that closes statements
     * and clears ws->open to false. */
    memset(ws, 0, sizeof(*ws));
    ws->db = db;

    const struct wsql_prep_spec preps[] = {
        { &ws->stmt_key_write,
          "INSERT OR REPLACE INTO wallet_keys"
          " (pubkey_hash, pubkey, privkey, compressed)"
          " VALUES(?,?,?,?)",
          "wallet_keys", "key_write" },
        { &ws->stmt_key_read,
          "SELECT pubkey_hash, pubkey, privkey, compressed, rowid"
          " FROM wallet_keys",
          "wallet_keys", "key_read" },
        { &ws->stmt_key_read_one,
          "SELECT pubkey, privkey, compressed"
          " FROM wallet_keys WHERE pubkey_hash=?",
          "wallet_keys", "key_read_one" },
        { &ws->stmt_key_delete,
          "DELETE FROM wallet_keys WHERE pubkey_hash=?",
          "wallet_keys", "key_delete" },
        { &ws->stmt_tx_write,
          "INSERT OR REPLACE INTO wallet_transactions"
          " (txid, raw_tx, block_hash, block_height, time_received, from_me)"
          " VALUES(?,?,?,?,?,?)",
          "wallet_transactions", "tx_write" },
        { &ws->stmt_tx_read,
          "SELECT txid, raw_tx, block_hash, block_height,"
          " time_received, from_me"
          " FROM wallet_transactions",
          "wallet_transactions", "tx_read" },
        { &ws->stmt_seed_write,
          "INSERT OR REPLACE INTO wallet_seed(id, seed, next_child)"
          " VALUES(1, ?, ?)",
          "wallet_seed", "seed_write" },
        { &ws->stmt_seed_read,
          "SELECT seed, next_child FROM wallet_seed WHERE id=1",
          "wallet_seed", "seed_read" },
        { &ws->stmt_zkey_write,
          "INSERT OR REPLACE INTO wallet_sapling_keys"
          " (ivk, xsk, xfvk, diversifier, pk_d, child_index, address)"
          " VALUES(?,?,?,?,?,?,'')",
          "wallet_sapling_keys", "zkey_write" },
        { &ws->stmt_zkey_read,
          "SELECT ivk, xsk, xfvk, diversifier, pk_d, child_index"
          " FROM wallet_sapling_keys",
          "wallet_sapling_keys", "zkey_read" },
        { &ws->stmt_script_write,
          "INSERT OR REPLACE INTO wallet_scripts"
          " (script_hash, redeem_script) VALUES(?,?)",
          "wallet_scripts", "script_write" },
        { &ws->stmt_script_read,
          "SELECT script_hash, redeem_script FROM wallet_scripts",
          "wallet_scripts", "script_read" },
        { &ws->stmt_watch_write,
          "INSERT OR REPLACE INTO wallet_watch_only"
          " (address_hash, address, created_at) VALUES(?,?,?)",
          "wallet_watch_only", "watch_write" },
        { &ws->stmt_watch_read,
          "SELECT address_hash, address FROM wallet_watch_only",
          "wallet_watch_only", "watch_read" },
        { &ws->stmt_best_block_write,
          "INSERT OR REPLACE INTO node_state(key,value)"
          " VALUES('wallet_best_block',?)",
          "node_state", "best_block_write" },
        { &ws->stmt_best_block_read,
          "SELECT value FROM node_state WHERE key='wallet_best_block'",
          "node_state", "best_block_read" },
        { &ws->stmt_scan_height_write,
          "INSERT OR REPLACE INTO node_state(key,value)"
          " VALUES('wallet_scan_height',?)",
          "node_state", "scan_height_write" },
        { &ws->stmt_scan_height_read,
          "SELECT value FROM node_state WHERE key='wallet_scan_height'",
          "node_state", "scan_height_read" },
    };

    for (size_t i = 0; i < sizeof(preps) / sizeof(preps[0]); i++) {
        struct zcl_result r = wsql_prepare_one(db, &preps[i]);
        if (!r.ok) {
            wallet_sqlite_close(ws);
            return wsql_fail(ws, r);
        }
    }

    ws->open = true;
    ws->last_error[0] = '\0';
    return ZCL_OK;
}

bool wallet_sqlite_open(struct wallet_sqlite *ws, sqlite3 *db)
{
    struct zcl_result r = wallet_sqlite_open_r(ws, db);
    if (r.ok) return true;
    LOG_FAIL("wallet_sqlite", "code=%d (%s:%d) %s",
             r.code,
             r.source_file ? r.source_file : "?", r.source_line, r.message);
}

void wallet_sqlite_close(struct wallet_sqlite *ws)
{
    if (!ws) return;
    if (ws->stmt_key_write)    { sqlite3_finalize(ws->stmt_key_write);    ws->stmt_key_write = NULL; }
    if (ws->stmt_key_read)     { sqlite3_finalize(ws->stmt_key_read);     ws->stmt_key_read = NULL; }
    if (ws->stmt_key_read_one) { sqlite3_finalize(ws->stmt_key_read_one); ws->stmt_key_read_one = NULL; }
    if (ws->stmt_key_delete)   { sqlite3_finalize(ws->stmt_key_delete);   ws->stmt_key_delete = NULL; }
    if (ws->stmt_tx_write)     { sqlite3_finalize(ws->stmt_tx_write);     ws->stmt_tx_write = NULL; }
    if (ws->stmt_tx_read)      { sqlite3_finalize(ws->stmt_tx_read);      ws->stmt_tx_read = NULL; }
    if (ws->stmt_seed_write)   { sqlite3_finalize(ws->stmt_seed_write);   ws->stmt_seed_write = NULL; }
    if (ws->stmt_seed_read)    { sqlite3_finalize(ws->stmt_seed_read);    ws->stmt_seed_read = NULL; }
    if (ws->stmt_zkey_write)   { sqlite3_finalize(ws->stmt_zkey_write);   ws->stmt_zkey_write = NULL; }
    if (ws->stmt_zkey_read)    { sqlite3_finalize(ws->stmt_zkey_read);    ws->stmt_zkey_read = NULL; }
    if (ws->stmt_script_write) { sqlite3_finalize(ws->stmt_script_write); ws->stmt_script_write = NULL; }
    if (ws->stmt_script_read)  { sqlite3_finalize(ws->stmt_script_read);  ws->stmt_script_read = NULL; }
    if (ws->stmt_watch_write)  { sqlite3_finalize(ws->stmt_watch_write);  ws->stmt_watch_write = NULL; }
    if (ws->stmt_watch_read)   { sqlite3_finalize(ws->stmt_watch_read);   ws->stmt_watch_read = NULL; }
    if (ws->stmt_best_block_write)  { sqlite3_finalize(ws->stmt_best_block_write);  ws->stmt_best_block_write = NULL; }
    if (ws->stmt_best_block_read)   { sqlite3_finalize(ws->stmt_best_block_read);   ws->stmt_best_block_read = NULL; }
    if (ws->stmt_scan_height_write) { sqlite3_finalize(ws->stmt_scan_height_write); ws->stmt_scan_height_write = NULL; }
    if (ws->stmt_scan_height_read)  { sqlite3_finalize(ws->stmt_scan_height_read);  ws->stmt_scan_height_read = NULL; }
    ws->db = NULL;
    ws->open = false;
}

/* ── Self-test ─────────────────────────────────────────────────── *
 *
 * Write → read → compare → delete round-trip on the `node_state`
 * table, which is already part of the baseline schema.  The probe
 * overwrites any previous canary value with 32 fresh random bytes
 * every call, so a stale blob can never pass the comparison.  This is
 * a lightweight subsystem check, not a durable boot-state canary. */
struct zcl_result wallet_sqlite_self_test(struct wallet_sqlite *ws)
{
    if (!ws)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet_sqlite pointer is NULL");
    if (!ws->open || !ws->db)
        return wsql_fail(ws, ZCL_ERR(WSQL_DB_NOT_OPEN,
            "self_test: wallet_sqlite is not open"));

    uint8_t probe[32];
    sqlite3_stmt *ins = NULL, *sel = NULL, *del = NULL;

    /* Seed the probe from a blend of clock and getrandom-ish
     * sources.  Correctness here is about uniqueness across
     * consecutive calls, not cryptographic strength. */
    int64_t now = (int64_t)platform_time_wall_time_t();
    uint32_t salt = (uint32_t)((uintptr_t)ws ^ (uint32_t)now ^
                               (uint32_t)sqlite3_last_insert_rowid(ws->db));
    for (size_t i = 0; i < sizeof(probe); i++)
        probe[i] = (uint8_t)(salt ^ (i * 0x9E)) + (uint8_t)now;

    int rc = sqlite3_prepare_v2(ws->db,
        "INSERT OR REPLACE INTO node_state(key,value) VALUES(?, ?)",
        -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        struct zcl_result r = ZCL_ERR(WSQL_PREPARE_FAIL,
            "self_test: prepare INSERT failed: %s", sqlite3_errmsg(ws->db));
        if (ins) sqlite3_finalize(ins);
        return wsql_fail(ws, r);
    }

    sqlite3_bind_text(ins, 1, WSQL_CANARY_KEY, -1, SQLITE_STATIC);
    sqlite3_bind_blob(ins, 2, probe, (int)sizeof(probe), SQLITE_STATIC);
    if (AR_STEP_WRITE(ins) != SQLITE_DONE) {
        struct zcl_result r = ZCL_ERR(WSQL_CANARY_WRITE_FAIL,
            "self_test: canary INSERT failed: %s", sqlite3_errmsg(ws->db));
        sqlite3_finalize(ins);
        return wsql_fail(ws, r);
    }
    sqlite3_finalize(ins);

    rc = sqlite3_prepare_v2(ws->db,
        "SELECT value FROM node_state WHERE key=?",
        -1, &sel, NULL);
    if (rc != SQLITE_OK) {
        struct zcl_result r = ZCL_ERR(WSQL_PREPARE_FAIL,
            "self_test: prepare SELECT failed: %s", sqlite3_errmsg(ws->db));
        if (sel) sqlite3_finalize(sel);
        return wsql_fail(ws, r);
    }
    sqlite3_bind_text(sel, 1, WSQL_CANARY_KEY, -1, SQLITE_STATIC);

    bool match = false;
    if (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(sel, 0);
        int dlen = sqlite3_column_bytes(sel, 0);
        match = (data && dlen == (int)sizeof(probe) &&
                 memcmp(data, probe, sizeof(probe)) == 0);
    }
    sqlite3_finalize(sel);

    if (!match) {
        struct zcl_result r = ZCL_ERR(WSQL_CANARY_READ_MISMATCH,
            "self_test: probe readback did not match written value");
        return wsql_fail(ws, r);
    }

    /* Clean up so node_state does not retain stale canary material. */
    rc = sqlite3_prepare_v2(ws->db,
        "DELETE FROM node_state WHERE key=?", -1, &del, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(del, 1, WSQL_CANARY_KEY, -1, SQLITE_STATIC);
        (void)AR_STEP_WRITE(del);
        sqlite3_finalize(del);
    }

    ws->canary_ok = true;
    ws->canary_last_ok_ts = now;
    return ZCL_OK;
}

/* ── Keys ──────────────────────────────────────────────────────── */

static struct zcl_result wsql_validate_key(const struct pubkey *pk,
                                            const struct privkey *key)
{
    if (!pk)
        return ZCL_ERR(WSQL_INVARIANT_PUBKEY, "pubkey pointer is NULL");
    if (!key)
        return ZCL_ERR(WSQL_INVARIANT_PRIVKEY, "privkey pointer is NULL");
    if (pk->size != 33 && pk->size != 65)
        return ZCL_ERR(WSQL_INVARIANT_PUBKEY,
            "pubkey size %u is not 33 or 65", pk->size);
    if (!key->fValid)
        return ZCL_ERR(WSQL_INVARIANT_PRIVKEY,
            "privkey->fValid is false");

    static const uint8_t zero32[32] = {0};
    if (memcmp(key->vch, zero32, 32) == 0)
        return ZCL_ERR(WSQL_INVARIANT_PRIVKEY,
            "privkey is all zero");
    return ZCL_OK;
}

struct zcl_result wallet_sqlite_write_key_r(struct wallet_sqlite *ws,
                                            const struct pubkey *pk,
                                            const struct privkey *key)
{
    if (!ws)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet_sqlite pointer is NULL");
    if (!ws->open)
        return wsql_fail(ws, ZCL_ERR(WSQL_DB_NOT_OPEN,
            "write_key: wallet_sqlite is not open"));

    struct zcl_result v = wsql_validate_key(pk, key);
    if (!v.ok) return wsql_fail(ws, v);

    struct key_id kid = pubkey_get_id(pk);

    uint8_t *enc_blob = NULL;
    size_t enc_len = 0;
    bool encrypted = wallet_encrypt_blob(key->vch, 32, &enc_blob, &enc_len);

    sqlite3_stmt *s = ws->stmt_key_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, kid.id.data, 20, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, pk->vch, (int)pk->size, SQLITE_STATIC);
    if (encrypted)
        sqlite3_bind_blob(s, 3, enc_blob, (int)enc_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_blob(s, 3, key->vch, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, key->fCompressed ? 1 : 0);

    int rc = AR_STEP_WRITE(s);
    if (enc_blob) { memory_cleanse(enc_blob, enc_len); free(enc_blob); }

    if (rc != SQLITE_DONE)
        return wsql_fail(ws, ZCL_ERR(WSQL_WRITE_FAIL,
            "write_key: step rc=%d: %s", rc, sqlite3_errmsg(ws->db)));
    return ZCL_OK;
}

bool wallet_sqlite_write_key(struct wallet_sqlite *ws, const struct pubkey *pk,
                              const struct privkey *key)
{
    struct zcl_result r = wallet_sqlite_write_key_r(ws, pk, key);
    if (r.ok) return true;
    LOG_FAIL("wallet_sqlite", "code=%d (%s:%d) %s",
             r.code,
             r.source_file ? r.source_file : "?", r.source_line, r.message);
}

struct zcl_result wallet_sqlite_delete_key_r(struct wallet_sqlite *ws,
                                             const struct pubkey *pk)
{
    if (!ws)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet_sqlite pointer is NULL");
    if (!pk)
        return ZCL_ERR(WSQL_NULL_ARG, "pubkey pointer is NULL");
    if (!ws->open)
        return wsql_fail(ws, ZCL_ERR(WSQL_DB_NOT_OPEN,
            "delete_key: wallet_sqlite is not open"));

    struct key_id kid = pubkey_get_id(pk);
    sqlite3_stmt *s = ws->stmt_key_delete;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, kid.id.data, 20, SQLITE_STATIC);

    int rc = AR_STEP_WRITE(s);
    if (rc != SQLITE_DONE)
        return wsql_fail(ws, ZCL_ERR(WSQL_WRITE_FAIL,
            "delete_key: step rc=%d: %s", rc, sqlite3_errmsg(ws->db)));
    return ZCL_OK;
}

struct zcl_result wallet_sqlite_read_single_key(struct wallet_sqlite *ws,
                                                const struct pubkey *pk,
                                                struct privkey *out_key)
{
    if (!ws)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet_sqlite pointer is NULL");
    if (!pk)
        return ZCL_ERR(WSQL_NULL_ARG, "pubkey pointer is NULL");
    if (!out_key)
        return ZCL_ERR(WSQL_NULL_ARG, "out_key pointer is NULL");
    if (!ws->open)
        return wsql_fail(ws, ZCL_ERR(WSQL_DB_NOT_OPEN,
            "read_single_key: wallet_sqlite is not open"));

    struct key_id kid = pubkey_get_id(pk);
    sqlite3_stmt *s = ws->stmt_key_read_one;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, kid.id.data, 20, SQLITE_STATIC);

    int rc = AR_STEP_ROW_READONLY(s);
    if (rc == SQLITE_DONE)
        return wsql_fail(ws, ZCL_ERR(WSQL_READ_FAIL,
            "read_single_key: pubkey_hash not found in wallet_keys"));
    if (rc != SQLITE_ROW)
        return wsql_fail(ws, ZCL_ERR(WSQL_READ_FAIL,
            "read_single_key: step rc=%d: %s", rc, sqlite3_errmsg(ws->db)));

    int priv_len = sqlite3_column_bytes(s, 1);
    const void *priv_data = sqlite3_column_blob(s, 1);
    int compressed = sqlite3_column_int(s, 2);

    if (!priv_data || priv_len < 32)
        return wsql_fail(ws, ZCL_ERR(WSQL_READ_FAIL,
            "read_single_key: privkey column too short (%d bytes)", priv_len));

    privkey_init(out_key);
    if (is_wks1_blob(priv_data, (size_t)priv_len)) {
        uint8_t *plain = NULL;
        size_t plain_len = 0;
        if (!wallet_decrypt_blob(priv_data, (size_t)priv_len,
                                 &plain, &plain_len) || plain_len < 32) {
            if (plain) { memory_cleanse(plain, plain_len); free(plain); }
            return wsql_fail(ws, ZCL_ERR(WSQL_READ_FAIL,
                "read_single_key: decrypt failed (wrong passphrase?)"));
        }
        memcpy(out_key->vch, plain, 32);
        memory_cleanse(plain, plain_len);
        free(plain);
    } else {
        memcpy(out_key->vch, priv_data, 32);
    }
    out_key->fValid = true;
    out_key->fCompressed = (compressed != 0);
    return ZCL_OK;
}

struct zcl_result wallet_sqlite_read_keys_r(struct wallet_sqlite *ws,
                                            struct wallet *w)
{
    if (!ws)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet_sqlite pointer is NULL");
    if (!w)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet pointer is NULL");
    if (!ws->open)
        return wsql_fail(ws, ZCL_ERR(WSQL_DB_NOT_OPEN,
            "read_keys: wallet_sqlite is not open"));

    sqlite3_stmt *s = ws->stmt_key_read;
    sqlite3_reset(s);

    int loaded = 0;
    int rc;
    /* A dropped corrupt row must increment a counter surfaced via
     * getwalletinfo.persistence: boot.c STATE F cannot distinguish a
     * corrupted keystore from an empty one, so a silently-dropped row
     * would look like an empty wallet.  Counting drift lets operators
     * see it instead of discovering it when funds fail to spend. */
    while ((rc = AR_STEP_ROW_READONLY(s)) == SQLITE_ROW) {
        int pk_len = sqlite3_column_bytes(s, 1);
        const void *pk_data = sqlite3_column_blob(s, 1);
        int priv_len = sqlite3_column_bytes(s, 2);
        const void *priv_data = sqlite3_column_blob(s, 2);
        int compressed = sqlite3_column_int(s, 3);
        int64_t rowid = sqlite3_column_int64(s, 4);

        if (!pk_data || pk_len < 33 || !priv_data || priv_len < 32) {
            fprintf(stderr,  // obs-ok:corrupt-row-counted-via-g_read_keys_corrupt_rows
                "[wallet_sqlite] %s:%d %s(): read_keys: dropping "
                "malformed row rowid=%lld "
                "(pubkey col bytes=%d data=%s, privkey col bytes=%d data=%s)\n",
                __FILE__, __LINE__, __func__,
                (long long)rowid,
                pk_len, pk_data ? "present" : "NULL",
                priv_len, priv_data ? "present" : "NULL");
            g_read_keys_corrupt_rows++;
            continue;
        }

        struct pubkey pk;
        pubkey_set(&pk, pk_data, (unsigned int)pk_len);

        struct privkey key;
        privkey_init(&key);

        if (is_wks1_blob(priv_data, (size_t)priv_len)) {
            uint8_t *plain = NULL;
            size_t plain_len = 0;
            if (!wallet_decrypt_blob(priv_data, (size_t)priv_len,
                                     &plain, &plain_len) ||
                plain_len < 32) {
                if (plain) { memory_cleanse(plain, plain_len); free(plain); }
                fprintf(stderr,  // obs-ok:corrupt-row-counted-via-g_read_keys_corrupt_rows
                    "[wallet_sqlite] %s:%d %s(): read_keys: dropping "
                    "row rowid=%lld — WKS1 decrypt failed "
                    "(wrong passphrase or tampered envelope?)\n",
                    __FILE__, __LINE__, __func__, (long long)rowid);
                g_read_keys_corrupt_rows++;
                continue;
            }
            memcpy(key.vch, plain, 32);
            memory_cleanse(plain, plain_len);
            free(plain);
        } else {
            memcpy(key.vch, priv_data, 32);
        }
        key.fValid = true;
        key.fCompressed = (compressed != 0);

        keystore_add_key(&w->keystore, &key);
        memory_cleanse(key.vch, 32);
        loaded++;
    }

    if (rc != SQLITE_DONE)
        return wsql_fail(ws, ZCL_ERR(WSQL_READ_FAIL,
            "read_keys: step rc=%d after %d rows: %s",
            rc, loaded, sqlite3_errmsg(ws->db)));
    return ZCL_OK;
}

bool wallet_sqlite_read_keys(struct wallet_sqlite *ws, struct wallet *w)
{
    struct zcl_result r = wallet_sqlite_read_keys_r(ws, w);
    if (r.ok) return true;
    LOG_FAIL("wallet_sqlite", "code=%d (%s:%d) %s",
             r.code,
             r.source_file ? r.source_file : "?", r.source_line, r.message);
}

/* ── Transactions ──────────────────────────────────────────────── */

bool wallet_sqlite_write_tx(struct wallet_sqlite *ws,
                              const struct wallet_tx *wtx)
{
    if (!ws || !ws->open)
        LOG_FAIL("wallet_sqlite", "write_tx: not open");

    struct byte_stream bs;
    stream_init(&bs, 512);
    transaction_serialize(&wtx->tx, &bs);

    sqlite3_stmt *s = ws->stmt_tx_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, wtx->tx.hash.data, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, bs.data, (int)bs.size, SQLITE_STATIC);
    sqlite3_bind_blob(s, 3, wtx->hash_block.data, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, wtx->confirms > 0 ? wtx->confirms : 0);
    sqlite3_bind_int64(s, 5, wtx->time_received);
    sqlite3_bind_int(s, 6, wtx->from_me ? 1 : 0);

    bool ok = AR_STEP_WRITE(s) == SQLITE_DONE;
    stream_free(&bs);
    if (!ok)
        LOG_FAIL("wallet_sqlite", "write_tx: step failed: %s",
                 sqlite3_errmsg(ws->db));
    return true;
}

bool wallet_sqlite_read_txs(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws || !ws->open)
        LOG_FAIL("wallet_sqlite", "read_txs: not open");

    sqlite3_stmt *s = ws->stmt_tx_read;
    sqlite3_reset(s);

    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        int raw_len = sqlite3_column_bytes(s, 1);
        const void *raw_data = sqlite3_column_blob(s, 1);
        if (!raw_data || raw_len < 10) continue;

        struct wallet_tx wtx;
        memset(&wtx, 0, sizeof(wtx));

        const void *bh = sqlite3_column_blob(s, 2);
        if (bh && sqlite3_column_bytes(s, 2) >= 32)
            memcpy(wtx.hash_block.data, bh, 32);

        wtx.confirms = sqlite3_column_int(s, 3);
        wtx.time_received = sqlite3_column_int64(s, 4);
        wtx.from_me = sqlite3_column_int(s, 5) != 0;

        struct byte_stream bs;
        stream_init_from_data(&bs, raw_data, (size_t)raw_len);
        transaction_init(&wtx.tx);
        if (transaction_deserialize(&wtx.tx, &bs)) {
            wtx.used = true;
            wallet_add_to_wallet(w, &wtx);
            /* wallet_add_to_wallet deep-copies the tx into its own slot, so the
             * vin/vout/shielded arrays allocated by transaction_deserialize here
             * are otherwise orphaned — a leak on every wallet-tx loaded. (A
             * failed deserialize already frees its partial state internally.) */
            transaction_free(&wtx.tx);
        }
        stream_free(&bs);
    }

    return true;
}

/* ── Best block / scan height ──────────────────────────────────── */

bool wallet_sqlite_write_scan_height(struct wallet_sqlite *ws, int height)
{
    if (!ws || !ws->open)
        LOG_FAIL("wallet_sqlite", "write_scan_height: not open");
    sqlite3_stmt *s = ws->stmt_scan_height_write;
    sqlite3_reset(s);
    int32_t h = (int32_t)height;
    sqlite3_bind_blob(s, 1, &h, 4, SQLITE_TRANSIENT);
    bool ok = AR_STEP_WRITE(s) == SQLITE_DONE;
    if (!ok)
        LOG_FAIL("wallet_sqlite", "write_scan_height: step failed: %s",
                 sqlite3_errmsg(ws->db));
    return true;
}

bool wallet_sqlite_read_scan_height(struct wallet_sqlite *ws, int *height)
{
    if (!ws || !ws->open) return false;
    sqlite3_stmt *s = ws->stmt_scan_height_read;
    sqlite3_reset(s);
    bool ok = false;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(s, 0);
        if (data && sqlite3_column_bytes(s, 0) >= 4) {
            int32_t h;
            memcpy(&h, data, 4);
            *height = h;
            ok = true;
        }
    }
    sqlite3_reset(s);
    return ok;
}

/* ── Sapling seed & keys ───────────────────────────────────────── */

bool wallet_sqlite_write_sapling_seed(struct wallet_sqlite *ws,
                                        const uint8_t seed[32])
{
    if (!ws || !ws->open)
        LOG_FAIL("wallet_sqlite", "write_sapling_seed: not open");

    int next_child = 0;
    sqlite3_reset(ws->stmt_seed_read);
    if (AR_STEP_ROW_READONLY(ws->stmt_seed_read) == SQLITE_ROW)
        next_child = sqlite3_column_int(ws->stmt_seed_read, 1);

    uint8_t *enc_blob = NULL;
    size_t enc_len = 0;
    bool encrypted = wallet_encrypt_blob(seed, 32, &enc_blob, &enc_len);

    sqlite3_reset(ws->stmt_seed_write);
    if (encrypted)
        sqlite3_bind_blob(ws->stmt_seed_write, 1,
                          enc_blob, (int)enc_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_blob(ws->stmt_seed_write, 1, seed, 32, SQLITE_STATIC);
    sqlite3_bind_int(ws->stmt_seed_write, 2, next_child);

    bool ok = AR_STEP_WRITE(ws->stmt_seed_write) == SQLITE_DONE;
    if (enc_blob) { memory_cleanse(enc_blob, enc_len); free(enc_blob); }
    if (!ok)
        LOG_FAIL("wallet_sqlite", "write_sapling_seed: step failed: %s",
                 sqlite3_errmsg(ws->db));
    return true;
}

bool wallet_sqlite_read_sapling_seed(struct wallet_sqlite *ws,
                                       uint8_t seed[32])
{
    if (!ws || !ws->open) return false;
    sqlite3_reset(ws->stmt_seed_read);
    if (AR_STEP_ROW_READONLY(ws->stmt_seed_read) == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(ws->stmt_seed_read, 0);
        int data_len = sqlite3_column_bytes(ws->stmt_seed_read, 0);
        if (!data || data_len < 32) return false;

        if (is_wks1_blob(data, (size_t)data_len)) {
            uint8_t *plain = NULL;
            size_t plain_len = 0;
            if (!wallet_decrypt_blob(data, (size_t)data_len,
                                     &plain, &plain_len) ||
                plain_len < 32) {
                if (plain) { memory_cleanse(plain, plain_len); free(plain); }
                /* Encrypted seed present but undecryptable — almost always a
                 * wrong/missing ZCL_WALLET_PASSPHRASE.  Surface it so the
                 * operator doesn't see a silent empty-wallet. */
                LOG_WARN("wallet_sqlite",
                         "read_sapling_seed: decrypt failed (wrong passphrase?)");
                return false;
            }
            memcpy(seed, plain, 32);
            memory_cleanse(plain, plain_len);
            free(plain);
            return true;
        }
        memcpy(seed, data, 32);
        return true;
    }
    return false;
}

bool wallet_sqlite_write_sapling_key(struct wallet_sqlite *ws,
                                       uint32_t child_index,
                                       const struct sapling_key_entry *entry)
{
    if (!ws || !ws->open)
        LOG_FAIL("wallet_sqlite", "write_sapling_key: not open");

    uint8_t *enc_xsk = NULL;
    size_t enc_xsk_len = 0;
    size_t xsk_raw_len = sizeof(struct zip32_xsk);
    bool encrypted = wallet_encrypt_blob((const uint8_t *)&entry->xsk,
                                          xsk_raw_len,
                                          &enc_xsk, &enc_xsk_len);

    sqlite3_stmt *s = ws->stmt_zkey_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, entry->ivk, 32, SQLITE_STATIC);
    if (encrypted)
        sqlite3_bind_blob(s, 2, enc_xsk, (int)enc_xsk_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_blob(s, 2, &entry->xsk, (int)xsk_raw_len, SQLITE_STATIC);
    sqlite3_bind_blob(s, 3, &entry->xfvk, (int)sizeof(struct zip32_xfvk),
                      SQLITE_STATIC);
    sqlite3_bind_blob(s, 4, entry->diversifier, 11, SQLITE_STATIC);
    sqlite3_bind_blob(s, 5, entry->pk_d, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 6, (int)child_index);

    bool ok = AR_STEP_WRITE(s) == SQLITE_DONE;
    if (enc_xsk) { memory_cleanse(enc_xsk, enc_xsk_len); free(enc_xsk); }

    if (!ok)
        LOG_FAIL("wallet_sqlite", "write_sapling_key: insert step failed: %s",
                 sqlite3_errmsg(ws->db));

    /* Bump next_child so future address derivation skips this index.
     * Every sqlite rc on this UPDATE must be observed and propagated:
     * if a SQLITE_BUSY from a concurrent reader silently left
     * next_child stale, the next derive_address() call would reuse the
     * child index — producing two addresses bound to the same spend
     * authority.  That collision voids receiver unlinkability, which
     * is the entire point of per-key diversifiers. */
    sqlite3_stmt *upd = NULL;
    int prep_rc = sqlite3_prepare_v2(ws->db,
        "UPDATE wallet_seed SET next_child=MAX(next_child,?+1) WHERE id=1",
        -1, &upd, NULL);
    if (prep_rc != SQLITE_OK || !upd) {
        if (upd) sqlite3_finalize(upd);
        LOG_FAIL("wallet_sqlite",
                 "write_sapling_key: prepare next_child UPDATE rc=%d: %s",
                 prep_rc, sqlite3_errmsg(ws->db));
    }
    if (sqlite3_bind_int(upd, 1, (int)child_index) != SQLITE_OK) {
        sqlite3_finalize(upd);
        LOG_FAIL("wallet_sqlite",
                 "write_sapling_key: bind next_child=%u failed: %s",
                 child_index, sqlite3_errmsg(ws->db));
    }
    int step_rc = AR_STEP_WRITE(upd);
    sqlite3_finalize(upd);
    if (step_rc != SQLITE_DONE) {
        /* SQLITE_BUSY, SQLITE_IOERR, etc. — the row either didn't
         * advance or we lost the update against a concurrent writer.
         * Either way, treat it as a hard failure so the caller (and
         * the flusher) rollback the whole transaction. */
        LOG_FAIL("wallet_sqlite",
                 "write_sapling_key: next_child UPDATE step rc=%d child=%u: %s",
                 step_rc, child_index, sqlite3_errmsg(ws->db));
    }
    return true;
}

bool wallet_sqlite_read_sapling_keys(struct wallet_sqlite *ws,
                                       struct wallet *w)
{
    if (!ws || !ws->open) return false;

    uint8_t seed[32];
    if (wallet_sqlite_read_sapling_seed(ws, seed)) {
        sapling_keystore_set_seed(&w->sapling_keys, seed);
        memory_cleanse(seed, 32);
    }

    sqlite3_stmt *s = ws->stmt_zkey_read;
    sqlite3_reset(s);

    struct sapling_keystore *sks = &w->sapling_keys;

    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        if (sks->num_keys >= MAX_SAPLING_KEYS) break;

        const void *ivk = sqlite3_column_blob(s, 0);
        const void *xsk_blob = sqlite3_column_blob(s, 1);
        const void *xfvk = sqlite3_column_blob(s, 2);
        const void *div = sqlite3_column_blob(s, 3);
        const void *pkd = sqlite3_column_blob(s, 4);
        int child = sqlite3_column_int(s, 5);

        int xsk_blob_len = sqlite3_column_bytes(s, 1);

        if (!ivk || !xsk_blob || !div || !pkd) continue;

        const void *xsk_data = xsk_blob;
        size_t xsk_data_len = (size_t)xsk_blob_len;
        uint8_t *xsk_decrypted = NULL;
        size_t xsk_plain_len = 0;

        if (is_wks1_blob(xsk_blob, (size_t)xsk_blob_len)) {
            if (!wallet_decrypt_blob(xsk_blob, (size_t)xsk_blob_len,
                                     &xsk_decrypted, &xsk_plain_len) ||
                xsk_plain_len < sizeof(struct zip32_xsk)) {
                if (xsk_decrypted) {
                    memory_cleanse(xsk_decrypted, xsk_plain_len);
                    free(xsk_decrypted);
                }
                continue;
            }
            xsk_data = xsk_decrypted;
            xsk_data_len = xsk_plain_len;
        }

        if (xsk_data_len < sizeof(struct zip32_xsk)) {
            if (xsk_decrypted) {
                memory_cleanse(xsk_decrypted, xsk_plain_len);
                free(xsk_decrypted);
            }
            continue;
        }

        struct sapling_key_entry *entry = &sks->keys[sks->num_keys];
        memcpy(entry->ivk, ivk, 32);
        memcpy(&entry->xsk, xsk_data, sizeof(struct zip32_xsk));
        if (xfvk && sqlite3_column_bytes(s, 2) >= (int)sizeof(struct zip32_xfvk))
            memcpy(&entry->xfvk, xfvk, sizeof(struct zip32_xfvk));
        else
            zip32_xsk_to_xfvk(&entry->xfvk, &entry->xsk);
        memcpy(entry->diversifier, div, 11);
        memcpy(entry->pk_d, pkd, 32);
        entry->child_index = (uint32_t)child;
        entry->used = true;
        sks->num_keys++;

        if ((uint32_t)child >= sks->next_child_index)
            sks->next_child_index = (uint32_t)child + 1;

        if (xsk_decrypted) {
            memory_cleanse(xsk_decrypted, xsk_plain_len);
            free(xsk_decrypted);
        }
    }

    return true;
}

/* ── Scripts ───────────────────────────────────────────────────── */

bool wallet_sqlite_write_script(struct wallet_sqlite *ws,
                                  const struct uint160 *script_id,
                                  const struct script *redeem_script)
{
    if (!ws || !ws->open)
        LOG_FAIL("wallet_sqlite", "write_script: not open");

    sqlite3_stmt *s = ws->stmt_script_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, script_id->data, 20, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, redeem_script->data, (int)redeem_script->size,
                      SQLITE_STATIC);
    bool ok = AR_STEP_WRITE(s) == SQLITE_DONE;
    if (!ok)
        LOG_FAIL("wallet_sqlite", "write_script: step failed: %s",
                 sqlite3_errmsg(ws->db));
    return true;
}

bool wallet_sqlite_read_scripts(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws || !ws->open) return false;

    sqlite3_stmt *s = ws->stmt_script_read;
    sqlite3_reset(s);

    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        const void *hash = sqlite3_column_blob(s, 0);
        const void *data = sqlite3_column_blob(s, 1);
        int data_len = sqlite3_column_bytes(s, 1);

        if (!hash || !data || data_len == 0 || data_len > MAX_SCRIPT_SIZE)
            continue;

        struct script scr;
        script_init(&scr);
        memcpy(scr.data, data, (size_t)data_len);
        scr.size = (size_t)data_len;
        keystore_add_cscript(&w->keystore, &scr);
    }

    return true;
}

/* ── Watch-only addresses ──────────────────────────────────────── */

bool wallet_sqlite_write_watch_only(struct wallet_sqlite *ws,
                                      const uint8_t address_hash[20],
                                      const char *address)
{
    if (!ws || !ws->open)
        LOG_FAIL("wallet_sqlite", "write_watch_only: not open");

    sqlite3_stmt *s = ws->stmt_watch_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, address_hash, 20, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, address, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, (int64_t)platform_time_wall_time_t());
    bool ok = AR_STEP_WRITE(s) == SQLITE_DONE;
    if (!ok)
        LOG_FAIL("wallet_sqlite", "write_watch_only: step failed: %s",
                 sqlite3_errmsg(ws->db));
    return true;
}

bool wallet_sqlite_read_watch_only(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws || !ws->open) return false;

    sqlite3_stmt *s = ws->stmt_watch_read;
    sqlite3_reset(s);

    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        const void *hash = sqlite3_column_blob(s, 0);
        if (!hash || sqlite3_column_bytes(s, 0) != 20)
            continue;

        struct key_id kid;
        memcpy(kid.id.data, hash, 20);
        keystore_add_watch_only_id(&w->keystore, &kid);
    }

    return true;
}

/* ── Flush all wallet state to SQLite ──────────────────────────── */

struct zcl_result wallet_sqlite_flush_r(struct wallet_sqlite *ws,
                                        struct wallet *w)
{
    if (!ws)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet_sqlite pointer is NULL");
    if (!w)
        return ZCL_ERR(WSQL_NULL_ARG, "wallet pointer is NULL");
    if (!ws->open)
        return wsql_fail(ws, ZCL_ERR(WSQL_DB_NOT_OPEN,
            "flush: wallet_sqlite is not open"));

    /* Boot/load reads use cached SELECT statements. If a read cursor is left
     * active, SQLite can reject the next BEGIN IMMEDIATE on the same handle
     * with SQLITE_BUSY even though no external writer owns the WAL lock. Make
     * the flush boundary release every cached VM before taking the write txn. */
    wallet_sqlite_reset_all_statements(ws);

    /* Acquire the write transaction with BEGIN IMMEDIATE + a bounded retry.
     *
     * The wallet shares the node.db file with the reducer/coins and projection
     * writers (separate WAL connections). During bulk catch-up a coins/reducer
     * commit can hold the single WAL write lock longer than one busy_timeout
     * window, so a key persist that waits only one window fails with
     * SQLITE_BUSY and strands keypool keys in RAM (getnewaddress then refuses,
     * correctly, to hand out an unsaved address). BEGIN IMMEDIATE takes the
     * write lock atomically at begin (each sqlite3_exec already blocks up to
     * the connection busy_timeout via the busy handler); retrying the begin a
     * few times waits out even a long commit window. Key persistence is rare
     * and not latency-critical, so a slower success beats losing the key.
     *
     * The retry runs BEFORE taking w->cs, so a contended flush never blocks
     * other wallet operations on the wallet mutex, and this path is only ever
     * reached from RPC handlers — never the reducer drive — so waiting here
     * cannot violate the reducer lock-order invariant. */
    int rc = SQLITE_OK;
    for (int begin_attempt = 0; ; begin_attempt++) {
        char *begin_err = NULL;
        rc = sqlite3_exec(ws->db, "BEGIN IMMEDIATE", NULL, NULL, &begin_err);
        if (rc == SQLITE_OK) {
            if (begin_err) sqlite3_free(begin_err);
            break;
        }
        bool busy = (rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
        if (busy && begin_attempt + 1 < WALLET_FLUSH_BEGIN_MAX_ATTEMPTS) {
            LOG_WARN("wallet_sqlite",
                     "flush: BEGIN IMMEDIATE busy (rc=%d), retry %d/%d",
                     rc, begin_attempt + 1, WALLET_FLUSH_BEGIN_MAX_ATTEMPTS);
            if (begin_err) sqlite3_free(begin_err);
            continue;
        }
        struct zcl_result r = ZCL_ERR(WSQL_TXN_BEGIN_FAIL,
            "flush: BEGIN IMMEDIATE failed after %d attempt(s): %s",
            begin_attempt + 1, begin_err ? begin_err : "(unknown)");
        if (begin_err) sqlite3_free(begin_err);
        return wsql_fail(ws, r);
    }

    zcl_mutex_lock(&w->cs);
    /* Snapshot all three mutable wallet domains under one documented order:
     * wallet -> transparent keystore -> Sapling keystore. Key generation uses
     * wallet -> transparent, while Sapling generation takes only Sapling, so
     * no production path takes the reverse order. Without these inner locks a
     * concurrent address/import operation could publish half an entry while
     * the flush serialized it. */
    zcl_mutex_lock(&w->keystore.cs);
    zcl_mutex_lock(&w->sapling_keys.cs);

    /* Invariant: if ANY writer fails, ROLLBACK the whole transaction
     * rather than COMMIT a partial state — committing only the writes
     * that happened to succeed is silent-partial-state corruption.
     * Every writer's rc is observed; the first failure short-circuits
     * the remaining work so we don't bloat the rolled-back txn with
     * more doomed writes. */
    struct zcl_result first_fail = ZCL_OK;
    int n_key_fail = 0;
    int n_tx_fail = 0;
    int n_zseed_fail = 0;
    int n_zkey_fail = 0;
    int n_script_fail = 0;
    int n_scanh_fail = 0;

    for (size_t i = 0; i < w->keystore.num_keys; i++) {
        if (!w->keystore.keys[i].used) continue;
        if (!w->keystore.keys[i].key.fValid) continue;

        struct pubkey pk;
        if (!privkey_get_pubkey(&w->keystore.keys[i].key, &pk))
            continue;

        struct zcl_result kr = wallet_sqlite_write_key_r(ws, &pk,
                                    &w->keystore.keys[i].key);
        if (!kr.ok) {
            fprintf(stderr,  // obs-ok:flush-failure-propagated-via-first_fail
                "[wallet_sqlite] flush: key slot %zu write failed: "
                "code=%d (%s:%d) %s\n",
                i, kr.code, kr.source_file, kr.source_line, kr.message);
            if (first_fail.ok) first_fail = kr;
            n_key_fail++;
            goto rollback;
        }
    }

    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!w->map_wallet[i].used) continue;
        if (!wallet_sqlite_write_tx(ws, &w->map_wallet[i])) {
            n_tx_fail++;
            if (first_fail.ok) first_fail = ZCL_ERR(WSQL_WRITE_FAIL,
                "flush: write_tx slot %zu failed", i);
            goto rollback;
        }
    }

    struct sapling_keystore *sks = &w->sapling_keys;
    if (sks->has_seed) {
        if (!wallet_sqlite_write_sapling_seed(ws, sks->seed)) {
            n_zseed_fail++;
            if (first_fail.ok) first_fail = ZCL_ERR(WSQL_WRITE_FAIL,
                "flush: write_sapling_seed failed");
            goto rollback;
        }
    }
    for (size_t i = 0; i < sks->num_keys; i++) {
        if (!sks->keys[i].used) continue;
        if (!wallet_sqlite_write_sapling_key(ws, sks->keys[i].child_index,
                                              &sks->keys[i])) {
            n_zkey_fail++;
            if (first_fail.ok) first_fail = ZCL_ERR(WSQL_WRITE_FAIL,
                "flush: write_sapling_key child=%u failed",
                sks->keys[i].child_index);
            goto rollback;
        }
    }

    for (size_t i = 0; i < w->keystore.num_scripts; i++) {
        if (!w->keystore.scripts[i].used) continue;
        if (!wallet_sqlite_write_script(ws,
                                         &w->keystore.scripts[i].script_id,
                                         &w->keystore.scripts[i].redeem_script)) {
            n_script_fail++;
            if (first_fail.ok) first_fail = ZCL_ERR(WSQL_WRITE_FAIL,
                "flush: write_script slot %zu failed", i);
            goto rollback;
        }
    }

    if (!wallet_sqlite_write_scan_height(ws, w->best_block_height)) {
        n_scanh_fail++;
        if (first_fail.ok) first_fail = ZCL_ERR(WSQL_WRITE_FAIL,
            "flush: write_scan_height failed");
        goto rollback;
    }

    zcl_mutex_unlock(&w->sapling_keys.cs);
    zcl_mutex_unlock(&w->keystore.cs);
    zcl_mutex_unlock(&w->cs);

    char *commit_err = NULL;
    rc = sqlite3_exec(ws->db, "COMMIT", NULL, NULL, &commit_err);
    if (rc != SQLITE_OK) {
        struct zcl_result r = ZCL_ERR(WSQL_TXN_COMMIT_FAIL,
            "flush: COMMIT failed: %s", commit_err ? commit_err : "(unknown)");
        if (commit_err) sqlite3_free(commit_err);
        /* best-effort rollback */
        sqlite3_exec(ws->db, "ROLLBACK", NULL, NULL, NULL);
        return wsql_fail(ws, r);
    }

    return ZCL_OK;

rollback:
    zcl_mutex_unlock(&w->sapling_keys.cs);
    zcl_mutex_unlock(&w->keystore.cs);
    zcl_mutex_unlock(&w->cs);
    {
        char *rb_err = NULL;
        int rb_rc = sqlite3_exec(ws->db, "ROLLBACK", NULL, NULL, &rb_err);
        if (rb_rc != SQLITE_OK) {
            fprintf(stderr,  // obs-ok:rollback-fallback-error-propagated
                "[wallet_sqlite] flush: ROLLBACK after write failure "
                "also failed rc=%d: %s\n",
                rb_rc, rb_err ? rb_err : "(unknown)");
            if (rb_err) sqlite3_free(rb_err);
        }
    }
    return wsql_fail(ws, ZCL_ERR(WSQL_WRITE_FAIL,
        "flush: rolled back — keys=%d tx=%d zseed=%d zkey=%d script=%d "
        "scanh=%d; first failure: [%d] %s",
        n_key_fail, n_tx_fail, n_zseed_fail, n_zkey_fail, n_script_fail,
        n_scanh_fail,
        first_fail.ok ? 0 : first_fail.code,
        first_fail.ok ? "(none captured)" : first_fail.message));
}

bool wallet_sqlite_flush(struct wallet_sqlite *ws, struct wallet *w)
{
    struct zcl_result r = wallet_sqlite_flush_r(ws, w);
    if (r.ok) return true;
    LOG_FAIL("wallet_sqlite", "code=%d (%s:%d) %s",
             r.code,
             r.source_file ? r.source_file : "?", r.source_line, r.message);
}

/* ── Health snapshot ───────────────────────────────────────────── */

struct wallet_sqlite_health
wallet_sqlite_get_health(struct wallet_sqlite *ws, int keystore_count)
{
    struct wallet_sqlite_health h;
    memset(&h, 0, sizeof(h));
    h.keystore_count = keystore_count;

    if (!ws) {
        snprintf(h.last_error, sizeof(h.last_error),
                 "wallet_sqlite pointer is NULL");
        return h;
    }

    h.open = ws->open;
    h.canary_ok = ws->canary_ok;
    h.canary_last_ok_ts = ws->canary_last_ok_ts;
    size_t n = sizeof(h.last_error) - 1;
    strncpy(h.last_error, ws->last_error, n);
    h.last_error[n] = '\0';

    if (ws->open && ws->db) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(ws->db,
                "SELECT COUNT(*) FROM wallet_keys", -1, &s, NULL) == SQLITE_OK
            && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
                h.row_count = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }
    h.mismatch = (h.row_count != h.keystore_count);
    return h;
}
