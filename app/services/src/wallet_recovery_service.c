/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wallet_recovery_service — implementation.
 * See services/wallet_recovery_service.h. */

#include "services/wallet_recovery_service.h"

#include "chain/chainparams.h"
#include "keys/key.h"                          /* ecc_start_once */
#include "models/database.h"
#include "models/wallet_key.h"
#include "services/wallet_restore_service.h"   /* datadir single-writer proof */
#include "support/cleanse.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "wallet/bip44.h"
#include "wallet/mnemonic.h"
#include "wallet/sapling_keys.h"
#include "wallet/wallet.h"
#include "wallet/wallet_sqlite.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define WRC_TAG "wallet_recovery"

/* ── helpers ─────────────────────────────────────────────────────── */

static void wrc_reset(struct wallet_recovery_report *out, const char *datadir)
{
    memset(out, 0, sizeof(*out));
    if (datadir && datadir[0]) {
        snprintf(out->datadir, sizeof(out->datadir), "%s", datadir);
        snprintf(out->target_db, sizeof(out->target_db), "%s/node.db",
                 datadir);
    }
}

/* Count the words in a phrase without copying it anywhere. */
static int wrc_word_count(const char *phrase)
{
    int n = 0;
    bool in_word = false;
    for (const char *p = phrase; *p; p++) {
        bool sp = (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r');
        if (!sp && !in_word) { n++; in_word = true; }
        else if (sp) in_word = false;
    }
    return n;
}

/* Make the datadir if it is not there. A directory that does not exist
 * cannot be held by a node, so this cannot race the lock proof. */
static struct zcl_result wrc_ensure_datadir(const char *datadir,
                                            struct wallet_recovery_report *out)
{
    struct stat st;
    if (stat(datadir, &st) != 0) {
        if (mkdir(datadir, 0700) != 0)
            return ZCL_ERR(-63, "cannot create datadir %s: %s", datadir,
                           strerror(errno));
        out->datadir_created = true;
        return ZCL_OK;
    }
    if (!S_ISDIR(st.st_mode))
        return ZCL_ERR(-63, "%s is not a directory", datadir);
    return ZCL_OK;
}

/* Derive and encode the two public addresses the phrase's seed reproduces.
 * Pure derivation; writes nothing. */
static bool wrc_fill_addresses(const uint8_t seed[32],
                               struct wallet_recovery_report *out)
{
    /* Offline entry point: nothing has booted, so the process-wide signing
     * context every key derivation needs may not exist yet. */
    if (!ecc_start_once())
        LOG_FAIL(WRC_TAG, "no secp256k1 signing context; cannot derive keys");

    if (!wallet_seed_address_at(seed, 0, BIP44_EXTERNAL, 0,
                                out->first_address,
                                sizeof(out->first_address)))
        LOG_FAIL(WRC_TAG, "could not derive the first receiving address");

    /* The shielded side: same seed, ZIP32 tree, first account. A scratch
     * keystore keeps this out of any wallet the caller owns. */
    struct sapling_keystore *sks =
        zcl_malloc(sizeof(*sks), "wallet_recovery.sapling_probe");
    if (!sks)
        LOG_FAIL(WRC_TAG, "out of memory deriving the shielded address");
    sapling_keystore_init(sks);
    bool ok = sapling_keystore_set_seed(sks, seed);
    uint8_t div[ZC_DIVERSIFIER_SIZE];
    uint8_t pk_d[32];
    ok = ok && sapling_keystore_new_address(sks, div, pk_d);
    if (ok) {
        const struct chain_params *cp = chain_params_get();
        ok = sapling_encode_payment_address(
                div, pk_d, cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
                out->first_shielded_address,
                sizeof(out->first_shielded_address));
    }
    sapling_keystore_free(sks);
    free(sks);
    if (!ok)
        LOG_FAIL(WRC_TAG, "could not derive the first shielded address");
    return true;
}

/* ── status ──────────────────────────────────────────────────────── */

struct zcl_result wallet_recovery_status_preflight(
    const char *datadir, struct wallet_recovery_report *out)
{
    if (!out)
        return ZCL_ERR(-63, "report is NULL");
    wrc_reset(out, datadir);
    if (!datadir || !datadir[0])
        return ZCL_ERR(-63, "datadir path is empty");

    /* Single-writer proof BEFORE anything opens the datadir. The read
     * itself cannot write any more — the caller opens node.db READ-ONLY —
     * but a node that is mid-migration is rewriting the very wallet tables
     * this answer is derived from, and the leaf aims at the operator's
     * running node when the caller names no datadir, which is exactly when
     * that would happen. */
    struct zcl_result lock_r = wallet_restore_datadir_free(datadir);
    if (!lock_r.ok) {
        LOG_WARN(WRC_TAG, "refusing to inspect a held datadir: %s",
                 lock_r.message);
        return ZCL_ERR(-61, "%s", lock_r.message);
    }
    return ZCL_OK;
}

struct zcl_result wallet_recovery_status(const char *datadir,
                                         struct node_db *ndb,
                                         struct wallet_recovery_report *out)
{
    if (!out)
        return ZCL_ERR(-63, "report is NULL");
    if (!out->target_db[0])
        wrc_reset(out, datadir);
    if (!datadir || !datadir[0])
        return ZCL_ERR(-63, "datadir path is empty");
    /* The handle is the caller's, opened READ-ONLY. Nothing here opens,
     * creates, migrates or closes a database — that is the whole point of
     * taking it as a parameter rather than a path. */
    if (!ndb || !ndb->open || !ndb->db)
        return ZCL_ERR(-63, "no open node.db to read at %s", out->target_db);

    /* Offline entry point — the derivation below needs a signing context. */
    if (!ecc_start_once())
        return ZCL_ERR(-63, "no secp256k1 signing context; cannot check "
                            "whether this wallet descends from its seed");

    struct wallet_sqlite ws;
    {
        struct zcl_result wr = wallet_sqlite_open_r(&ws, ndb->db);
        if (!wr.ok)
            return ZCL_ERR(-63, "cannot open the wallet tables in %s: %s",
                           out->target_db, wr.message);
    }

    out->keys_before = db_wallet_key_count(ndb);
    out->keys_after = out->keys_before;

    uint8_t seed[32];
    out->seed_present_before = wallet_sqlite_read_sapling_seed(&ws, seed);

    /* "Would a recovery phrase bring these keys back?" is answered by
     * derivation, exactly as boot answers it: load the wallet and ask
     * wallet_hd_adopt_seed whether this seed governs key 0. */
    if (out->seed_present_before) {
        struct wallet *w = zcl_malloc(sizeof(*w), "wallet_recovery.status");
        if (!w) {
            memory_cleanse(seed, sizeof(seed));
            wallet_sqlite_close(&ws);
            return ZCL_ERR(-63, "out of memory loading the wallet");
        }
        wallet_init(w);
        /* A read failure here would leave an empty keystore, and an empty
         * keystore makes wallet_hd_adopt_seed say yes unconditionally —
         * i.e. it would report a legacy wallet as phrase-recoverable. Fail
         * the question instead of answering it wrong. */
        struct zcl_result kr = wallet_sqlite_read_keys_r(&ws, w);
        if (!kr.ok) {
            wallet_free(w);
            free(w);
            memory_cleanse(seed, sizeof(seed));
            wallet_sqlite_close(&ws);
            return ZCL_ERR(-63, "cannot read this wallet's keys, so whether "
                           "a phrase would bring them back is unknown: %s",
                           kr.message);
        }
        out->seed_installed = wallet_hd_adopt_seed(w, seed);
        if (out->seed_installed)
            (void)wallet_seed_address_at(seed, 0, BIP44_EXTERNAL, 0,
                                         out->first_address,
                                         sizeof(out->first_address));
        wallet_free(w);
        free(w);
    }
    memory_cleanse(seed, sizeof(seed));

    /* The wallet_sqlite statement set is ours; the node_db handle under it
     * is the caller's and stays open. */
    wallet_sqlite_close(&ws);
    return ZCL_OK;
}

/* ── recover ─────────────────────────────────────────────────────── */

struct zcl_result wallet_recovery_run(const struct wallet_recovery_request *req,
                                      struct wallet_recovery_report *out)
{
    if (!out)
        return ZCL_ERR(-60, "report is NULL");
    if (!req)
        return ZCL_ERR(-60, "request is NULL");
    wrc_reset(out, req->datadir);
    out->dry_run = req->dry_run;

    if (!req->datadir || !req->datadir[0])
        return ZCL_ERR(-63, "datadir path is empty");
    if (!req->phrase || !req->phrase[0])
        return ZCL_ERR(-60, "a recovery phrase is required");

    out->word_count = wrc_word_count(req->phrase);
    /* Refuse on shape, and describe the shape — never the words. */
    if (!mnemonic_validate(req->phrase)) {
        LOG_WARN(WRC_TAG, "refusing recovery: phrase failed BIP39 validation "
                 "(%d words)", out->word_count);
        return ZCL_ERR(-60,
            "that is not a valid recovery phrase (%d words read). A phrase is "
            "12, 15, 18, 21 or 24 words from the BIP39 word list, in the "
            "order they were written down, and its last word is a checksum — "
            "so a single mistyped or reordered word fails here rather than "
            "silently opening an empty wallet",
            out->word_count);
    }
    out->phrase_valid = true;

    uint8_t seed[32];
    if (!mnemonic_to_wallet_seed(req->phrase, NULL, seed)) {
        memory_cleanse(seed, sizeof(seed));
        return ZCL_ERR(-64, "could not derive the wallet seed from the phrase");
    }

    /* The addresses come from the phrase alone: a dry run can show the user
     * exactly which wallet these words open before anything is written. */
    if (!wrc_fill_addresses(seed, out)) {
        memory_cleanse(seed, sizeof(seed));
        return ZCL_ERR(-64, "could not derive addresses from the phrase");
    }

    struct zcl_result dr = wrc_ensure_datadir(req->datadir, out);
    if (!dr.ok) {
        memory_cleanse(seed, sizeof(seed));
        return dr;
    }

    /* Single-writer proof BEFORE anything opens the datadir. */
    struct zcl_result lock_r = wallet_restore_datadir_free(req->datadir);
    if (!lock_r.ok) {
        memory_cleanse(seed, sizeof(seed));
        LOG_WARN(WRC_TAG, "refusing recovery: %s", lock_r.message);
        return ZCL_ERR(-61, "%s", lock_r.message);
    }

    struct stat st;
    out->target_created = stat(out->target_db, &st) != 0;

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, out->target_db)) {
        memory_cleanse(seed, sizeof(seed));
        return ZCL_ERR(-63, "cannot open target database %s", out->target_db);
    }
    struct wallet_sqlite ws;
    {
        struct zcl_result wr = wallet_sqlite_open_r(&ws, ndb.db);
        if (!wr.ok) {
            node_db_close(&ndb);
            memory_cleanse(seed, sizeof(seed));
            return ZCL_ERR(-63, "cannot open the wallet tables in %s: %s",
                           out->target_db, wr.message);
        }
    }

    out->keys_before = db_wallet_key_count(&ndb);
    out->keys_after = out->keys_before;
    uint8_t existing_seed[32];
    out->seed_present_before =
        wallet_sqlite_read_sapling_seed(&ws, existing_seed);
    memory_cleanse(existing_seed, sizeof(existing_seed));

    /* Never write a second seed over a wallet that already has keys. */
    if (out->keys_before > 0 || out->seed_present_before) {
        wallet_sqlite_close(&ws);
        node_db_close(&ndb);
        memory_cleanse(seed, sizeof(seed));
        LOG_WARN(WRC_TAG, "refusing recovery: %s already holds a wallet "
                 "(%lld keys, seed=%d)", out->target_db,
                 (long long)out->keys_before, (int)out->seed_present_before);
        return ZCL_ERR(-62,
            "%s already holds a wallet (%lld keys). Recovering here would "
            "leave a wallet whose keys came from two different seeds and no "
            "way to tell them apart. Recover into an empty datadir instead, "
            "then move what you need",
            out->datadir, (long long)out->keys_before);
    }

    /* Build the wallet the phrase describes. struct wallet is far too big
     * for the stack. */
    struct wallet *w = zcl_malloc(sizeof(*w), "wallet_recovery.wallet");
    if (!w) {
        wallet_sqlite_close(&ws);
        node_db_close(&ndb);
        memory_cleanse(seed, sizeof(seed));
        return ZCL_ERR(-64, "out of memory building the recovered wallet");
    }
    wallet_init(w);
    memory_cleanse(seed, sizeof(seed));

    struct zcl_result rc = ZCL_OK;
    if (!wallet_init_from_recovery_phrase(w, req->phrase)) {
        rc = ZCL_ERR(-64, "could not root the wallet on the recovery phrase");
        goto done;
    }
    if (!wallet_top_up_key_pool(w, DEFAULT_KEYPOOL_SIZE)) {
        rc = ZCL_ERR(-64, "could not derive the address keys from the phrase");
        goto done;
    }
    /* One shielded address too, so the recovered wallet can receive on both
     * sides without a second command. */
    {
        uint8_t div[ZC_DIVERSIFIER_SIZE];
        uint8_t pk_d[32];
        if (!sapling_keystore_new_address(&w->sapling_keys, div, pk_d)) {
            rc = ZCL_ERR(-64, "could not derive the shielded address");
            goto done;
        }
    }
    out->keys_minted = (int)w->keystore.num_keys;

    if (req->dry_run) {
        out->keys_after = out->keys_before;
        goto done;
    }

    /* One writer: the encryption-aware wallet_sqlite layer, in one
     * transaction, exactly as boot flushes a freshly minted keypool. */
    struct zcl_result fr = wallet_sqlite_flush_r(&ws, w);
    if (!fr.ok) {
        rc = ZCL_ERR(-65, "could not save the recovered wallet: %s",
                     fr.message);
        goto done;
    }
    out->keys_after = db_wallet_key_count(&ndb);
    out->seed_installed = true;
    node_db_wal_checkpoint(&ndb);

done:
    wallet_free(w);
    free(w);
    wallet_sqlite_close(&ws);
    node_db_close(&ndb);
    return rc;
}
