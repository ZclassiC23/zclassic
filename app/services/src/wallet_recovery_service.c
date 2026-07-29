/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wallet_recovery_service — implementation.
 * See services/wallet_recovery_service.h. */

#include "services/wallet_recovery_service.h"

#include "chain/chainparams.h"
#include "command/native_command.h"            /* the ONE read-only db open */
#include "keys/key.h"                          /* ecc_start_once */
#include "models/database.h"
#include "models/utxo.h"                       /* gap-scan history oracle */
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

/* ── "has this address been used?" — the gap scan's oracle ───────────
 *
 * Answered from the chain THIS datadir already holds. A restore into a
 * datadir that has synced can therefore walk past the standard lookahead
 * and rebuild every address the user actually handed out. A restore into
 * an empty datadir has nothing to consult, gets a NULL oracle, and derives
 * the floor — which is stated in the report rather than glossed as full
 * coverage. See wallet_derive_gap_limited in wallet/wallet.h. */
static bool wrc_address_used(const struct key_id *id, void *ctx)
{
    struct node_db *ndb = (struct node_db *)ctx;
    if (!id || !ndb || !ndb->open)
        return false;
    return db_address_has_chain_history(ndb, id->id.data);
}

/* Rebuild the key material the phrase determines, into an already-rooted
 * wallet. `chain` may be NULL (no history oracle available). */
static struct zcl_result wrc_rebuild_keys(struct wallet *w,
                                          struct node_db *chain,
                                          struct wallet_recovery_report *out)
{
    struct wallet_gap_scan scan;
    if (!wallet_derive_gap_limited(w, chain ? wrc_address_used : NULL,
                                   chain, &scan))
        return ZCL_ERR(-64, "could not derive the address keys from the "
                            "phrase");

    out->receiving_keys = scan.external_derived;
    out->change_keys = scan.internal_derived;
    out->receiving_keys_used = scan.external_used;
    out->change_keys_used = scan.internal_used;
    out->chain_history_consulted = scan.oracle_consulted;
    out->transparent_scan_truncated = scan.ceiling_hit;

    /* The shielded side: a fixed lookahead, not a scan — there is no cheap
     * offline oracle for shielded use. See WALLET_RECOVERY_SHIELDED_LOOKAHEAD. */
    for (uint32_t i = 0; i < (uint32_t)WALLET_RECOVERY_SHIELDED_LOOKAHEAD; i++) {
        uint8_t div[ZC_DIVERSIFIER_SIZE];
        uint8_t pk_d[32];
        if (!sapling_keystore_new_address(&w->sapling_keys, div, pk_d))
            return ZCL_ERR(-64, "could not derive shielded address %u of %d",
                           i, WALLET_RECOVERY_SHIELDED_LOOKAHEAD);
        out->shielded_children = i + 1;
    }

    out->keys_minted = (int)w->keystore.num_keys;
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
    memset(seed, 0, sizeof(seed));
    /* Three different facts, three different answers. "No seed row" and "a
     * seed row I cannot decrypt" used to be the same false here, and that
     * told the owner of an encrypted, phrase-backed wallet that their
     * wallet predates recovery phrases and their money could only come back
     * from a file. It can come back from their words; they have to unlock
     * it first. The caller renders LOCKED as its own answer. */
    out->seed_state_before = wallet_sqlite_sapling_seed_state(&ws, seed);
    out->seed_present_before =
        wallet_seed_state_row_present(out->seed_state_before);
    bool seed_readable = (out->seed_state_before == WALLET_SEED_PLAINTEXT ||
                          out->seed_state_before == WALLET_SEED_UNLOCKED);

    /* "Would a recovery phrase bring these keys back?" is answered by
     * derivation, exactly as boot answers it: load the wallet and ask
     * wallet_hd_adopt_seed whether this seed governs key 0. A LOCKED seed
     * cannot be asked that question at all — which is precisely why it must
     * not be answered "no". */
    if (seed_readable) {
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
    memory_cleanse(seed, sizeof(seed));

    /* Single-writer proof BEFORE anything opens the datadir, on both paths.
     * A datadir that does not exist yet cannot be held, so this is safe to
     * run before the directory is created. */
    struct zcl_result lock_r = wallet_restore_datadir_free(req->datadir);
    if (!lock_r.ok) {
        LOG_WARN(WRC_TAG, "refusing recovery: %s", lock_r.message);
        return ZCL_ERR(-61, "%s", lock_r.message);
    }

    struct stat st;
    out->target_created = stat(out->target_db, &st) != 0;

    /* ── open the target ─────────────────────────────────────────────
     *
     * A PLAN opens nothing for writing and creates nothing at all: no
     * datadir, no node.db, no scratch database anywhere. Its whole promise
     * to the caller is "nothing has been written yet", and the previous
     * shape broke that promise while printing it — mkdir plus an 864 KB
     * fresh node.db, on a preview. So the plan reads an EXISTING target
     * read-only (which is also the only handle that can answer "is a
     * wallet already here?"), and when there is no target yet it simply
     * has nothing to read. */
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    sqlite3 *ro_db = NULL;
    bool have_db = false;

    if (req->dry_run) {
        if (!out->target_created) {
            char ro_path[1200];
            enum zcl_node_db_ro_status ro = zcl_native_node_db_open_readonly(
                req->datadir, &ro_db, &ndb, ro_path, sizeof(ro_path));
            if (ro != ZCL_NODE_DB_RO_OK)
                return ZCL_ERR(-63,
                    "there is a node.db at %s but it cannot be read, so this "
                    "plan cannot tell you whether recovering here would "
                    "overwrite a wallet. Nothing was written",
                    out->target_db);
            have_db = true;
        }
    } else {
        struct zcl_result dr = wrc_ensure_datadir(req->datadir, out);
        if (!dr.ok)
            return dr;
        if (!node_db_open(&ndb, out->target_db))
            return ZCL_ERR(-63, "cannot open target database %s",
                           out->target_db);
        have_db = true;
    }

    struct wallet_sqlite ws;
    bool have_ws = false;
    if (have_db) {
        struct zcl_result wr = wallet_sqlite_open_r(&ws, ndb.db);
        if (!wr.ok) {
            if (req->dry_run) zcl_native_node_db_close_readonly(&ro_db, &ndb);
            else node_db_close(&ndb);
            return ZCL_ERR(-63, "cannot open the wallet tables in %s: %s",
                           out->target_db, wr.message);
        }
        have_ws = true;

        out->keys_before = db_wallet_key_count(&ndb);
        out->keys_after = out->keys_before;
        /* The ROW question, not the readability one. A LOCKED seed is a
         * seed: recovering over it would put a second seed on a wallet
         * whose first one is merely shut, and the owner would have no way
         * to tell which keys came from which. */
        out->seed_state_before = wallet_sqlite_sapling_seed_state(&ws, NULL);
        out->seed_present_before =
            wallet_seed_state_row_present(out->seed_state_before);
    }

    /* Never write a second seed over a wallet that already has keys. */
    if (out->keys_before > 0 || out->seed_present_before) {
        if (have_ws) wallet_sqlite_close(&ws);
        if (req->dry_run) zcl_native_node_db_close_readonly(&ro_db, &ndb);
        else if (have_db) node_db_close(&ndb);
        LOG_WARN(WRC_TAG, "refusing recovery: %s already holds a wallet "
                 "(%lld keys, seed_state=%d)", out->target_db,
                 (long long)out->keys_before, (int)out->seed_state_before);
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
        if (have_ws) wallet_sqlite_close(&ws);
        if (req->dry_run) zcl_native_node_db_close_readonly(&ro_db, &ndb);
        else if (have_db) node_db_close(&ndb);
        return ZCL_ERR(-64, "out of memory building the recovered wallet");
    }
    wallet_init(w);

    struct zcl_result rc = ZCL_OK;
    if (!wallet_init_from_recovery_phrase(w, req->phrase)) {
        rc = ZCL_ERR(-64, "could not root the wallet on the recovery phrase");
        goto done;
    }
    /* Same scan on both paths, against the same chain, so the plan's
     * numbers are the commit's numbers. An open node.db is not by itself a
     * chain: a freshly created one has the tables and no rows, and every
     * address probe against it answers false for a reason that has nothing
     * to do with the address. Pass the oracle only when there is something
     * to ask, so "no history found" is never printed for "nothing to look
     * in". */
    struct node_db *chain =
        (have_db && db_address_index_populated(&ndb)) ? &ndb : NULL;
    rc = wrc_rebuild_keys(w, chain, out);
    if (!rc.ok)
        goto done;

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
    if (have_ws) wallet_sqlite_close(&ws);
    if (req->dry_run) zcl_native_node_db_close_readonly(&ro_db, &ndb);
    else if (have_db) node_db_close(&ndb);
    return rc;
}
