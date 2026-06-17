/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MVP criterion #5 CI gate: store end-to-end.
 *
 * This gate proves the shipped store flow survives the persistence boundary:
 *   1. browse the seeded store catalog and fetch a CSRF token
 *   2. create an order through the HTTP controller path
 *   3. persist a confirmed Sapling note for that order's payment address
 *   4. run store payment reconciliation
 *   5. reopen through fresh controller/model calls and assert:
 *      - order status advanced to TOKENS SENT
 *      - token balance was credited exactly once
 *      - token-gated access succeeds after reconciliation
 */

#include "test/test_helpers.h"
#include "controllers/store_controller.h"
#include "controllers/zslp_controller.h"
#include "models/block.h"
#include "models/store.h"
#include "models/store_blob.h"
#include "models/wallet_tx.h"
#include "crypto/sha3.h"

/* For the SHIELDED gate (test_store_e2e_shielded): a real Sapling output
 * built by the production payer path, ivk-decrypted by a merchant wallet. */
#include "core/uint256.h"
#include "primitives/transaction.h"
#include "sapling/constants.h"
#include "sapling/sapling.h"
#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include "util/safe_alloc.h"

#include <unistd.h>

/* The exact payload the buyer must receive on gated download. It embeds a
 * NUL byte (and non-ASCII bytes) so a passing assertion proves the
 * response path is binary-safe — not %.*s-truncated at the first NUL. */
static const uint8_t P11_5_BLOB[] = {
    'Z','C','L','2','3',' ','S','E','C','R','E','T',' ',
    'P','A','Y','L','O','A','D', 0x00, 0xDE, 0xAD, 0xBE, 0xEF
};

/* Store P11_5_BLOB and stamp it onto an existing product so a token-gated
 * download serves the real bytes. Returns false on any DB error. */
static bool p11_5_attach_blob_to_product(struct node_db *ndb,
                                         int64_t product_id)
{
    uint8_t hash[32];
    if (!db_store_blob_put(ndb, P11_5_BLOB, sizeof(P11_5_BLOB),
                           "application/octet-stream", "secret.bin", hash))
        return false;
    return db_store_product_save_content(ndb, product_id, hash);
}

static void p11_5_setup_datadir(char *datadir, size_t datadir_size)
{
    snprintf(datadir, datadir_size, ".zcl_test_store_e2e_%d", (int)getpid());
    mkdir(datadir, 0755);
}

static void p11_5_cleanup_datadir(const char *datadir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", datadir ? datadir : "");
    (void)system(cmd);
}

static bool p11_5_fetch_csrf_token(const char *datadir,
                                   int64_t product_id,
                                   char *out,
                                   size_t out_size)
{
    uint8_t page[16384];
    char path[64];
    const char *needle = "name='csrf_token' value='";
    const char *p;
    const char *end;
    size_t n;
    size_t token_len;

    if (!datadir || !out || out_size == 0)
        return false;

    snprintf(path, sizeof(path), "/store/product/%lld", (long long)product_id);
    n = store_handle_request("GET", path, NULL, 0,
                             page, sizeof(page), datadir);
    if (n == 0)
        return false;

    page[(n < sizeof(page)) ? n : (sizeof(page) - 1)] = '\0';
    p = strstr((const char *)page, needle);
    if (!p)
        return false;
    p += strlen(needle);
    end = strchr(p, '\'');
    if (!end)
        return false;

    token_len = (size_t)(end - p);
    if (token_len >= out_size)
        return false;

    memcpy(out, p, token_len);
    out[token_len] = '\0';
    return true;
}

static bool p11_5_seed_tip_block(struct node_db *ndb, int height)
{
    struct db_block blk;
    static uint8_t solution[] = {0x51, 0x52};

    memset(&blk, 0, sizeof(blk));
    memset(blk.hash, 0x11, sizeof(blk.hash));
    memset(blk.prev_hash, 0x22, sizeof(blk.prev_hash));
    memset(blk.merkle_root, 0x33, sizeof(blk.merkle_root));
    memset(blk.chain_work, 0x44, sizeof(blk.chain_work));
    blk.height = height;
    blk.time = 123456789u;
    blk.bits = 0x1d00ffffu;
    blk.status = 3;
    blk.solution = solution;
    blk.solution_len = sizeof(solution);
    return db_block_save(ndb, &blk);
}

static bool p11_5_seed_confirmed_note(struct node_db *ndb,
                                      const char *address,
                                      int64_t order_id,
                                      int64_t value,
                                      int block_height)
{
    struct db_sapling_note note;

    memset(&note, 0, sizeof(note));
    memset(note.txid, 0x61, sizeof(note.txid));
    memset(note.rcm, 0x62, sizeof(note.rcm));
    memset(note.ivk, 0x63, sizeof(note.ivk));
    memset(note.diversifier, 0x64, sizeof(note.diversifier));
    memset(note.pk_d, 0x65, sizeof(note.pk_d));
    memset(note.cm, 0x66, sizeof(note.cm));
    memset(note.nullifier, 0x67, sizeof(note.nullifier));
    note.output_index = 0;
    note.value = value;
    note.block_height = block_height;
    snprintf(note.address, sizeof(note.address), "%s", address);
    /* Carry the order-binding memo a real payer is now instructed to place
     * (store payment page) — so this gate exercises the LIVE memo-bound
     * reconcile (db_store_received_payment_for_memo), not the legacy
     * address-only finder. A 512-byte zero-padded memo with the token at
     * the head, mirroring a recovered Sapling note. */
    snprintf((char *)note.memo, sizeof(note.memo),
             "ZCL23ORDER:%lld", (long long)order_id);
    note.memo_len = ZC_MEMO_SIZE;
    return db_sapling_note_save(ndb, &note);
}

int test_store_e2e_gate(void)
{
    int failures = 0;

    printf("\n=== store e2e (MVP #5) ===\n");
    printf("store_e2e order -> confirmed payment -> token access... ");

    if (!getenv("ZCL_STRESS_TESTS")) {
        printf("SKIP (set ZCL_STRESS_TESTS=1 to run deterministic gate)\n");
        return 0;
    }

    {
        char datadir[256];
        char dbpath[320];
        char csrf[64] = "";
        char body[256];
        uint8_t resp[16384];
        struct node_db ndb;
        struct db_store_order_summary summaries[8];
        struct db_store_order_view order;
        size_t n = 0;
        bool ok = true;
        int order_count;
        uint64_t bal;
        const char *fail_step = "setup";

        p11_5_setup_datadir(datadir, sizeof(datadir));
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", datadir);
        memset(&ndb, 0, sizeof(ndb));
        memset(&order, 0, sizeof(order));

        if (ok) fail_step = "fetch csrf";
        ok = p11_5_fetch_csrf_token(datadir, 1, csrf, sizeof(csrf));
        if (ok) {
            fail_step = "create order";
            snprintf(body, sizeof(body),
                     "product_id=1&customer_addr=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn"
                     "&csrf_token=%s",
                     csrf);
            n = store_handle_request("POST", "/store/orders",
                                     (const uint8_t *)body, strlen(body),
                                     resp, sizeof(resp), datadir);
            ok = n > 0;
            ok = ok && strstr((char *)resp, "HTTP/1.1 200 OK") != NULL;
            ok = ok && strstr((char *)resp, "Order #") != NULL;
        }

        if (ok) fail_step = "open after create";
        ok = ok && node_db_open(&ndb, dbpath);
        if (ok) {
            order_count = db_store_order_list_recent(&ndb, summaries,
                                                     sizeof(summaries) / sizeof(summaries[0]));
            if (ok) fail_step = "list recent";
            ok = order_count == 1;
            if (ok) fail_step = "load order";
            ok = ok && db_store_order_find_view(&ndb, summaries[0].id, &order);
            if (ok) fail_step = "pending status";
            ok = ok && order.status == STORE_ORDER_PENDING;
            if (ok) fail_step = "payment addr present";
            ok = ok && order.payment_addr[0] != '\0';
            if (ok) fail_step = "seed tip block";
            ok = ok && p11_5_seed_tip_block(&ndb, 100);
            if (ok) fail_step = "seed confirmed note";
            ok = ok && p11_5_seed_confirmed_note(&ndb, order.payment_addr,
                                                 summaries[0].id,
                                                 order.amount_zatoshi, 97);
            /* Attach the real file payload to product #1 (the seeded
             * ZCL23ACCESS product the order purchased) so the token-gated
             * download serves bytes instead of the HTML access page. */
            if (ok) fail_step = "attach product blob";
            ok = ok && p11_5_attach_blob_to_product(&ndb, 1);
            node_db_close(&ndb);
        }

        if (ok) fail_step = "process payments";
        if (ok)
            store_process_payments(datadir);

        memset(&ndb, 0, sizeof(ndb));
        if (ok) fail_step = "reopen after reconcile";
        ok = ok && node_db_open(&ndb, dbpath);
        if (ok) {
            memset(&order, 0, sizeof(order));
            if (ok) fail_step = "reload order";
            ok = db_store_order_find_view(&ndb, summaries[0].id, &order);
            if (ok) fail_step = "sent status";
            ok = ok && order.status == STORE_ORDER_SENT;
            if (ok) fail_step = "credited balance";
            bal = zslp_balance(datadir, "ZCL23ACCESS",
                               "t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn");
            ok = ok && (bal == 10);
            node_db_close(&ndb);
        }

        if (ok) {
            fail_step = "status page";
            n = store_handle_request("GET", "/store/orders/1", NULL, 0,
                                     resp, sizeof(resp), datadir);
            ok = n > 0;
            ok = ok && strstr((char *)resp, "HTTP/1.1 200 OK") != NULL;
            ok = ok && strstr((char *)resp, "Tokens Sent") != NULL;
            ok = ok && strstr((char *)resp, order.payment_addr) != NULL;
        }

        if (ok) fail_step = "dedupe reconcile";
        if (ok)
            store_process_payments(datadir);

        if (ok) fail_step = "dedupe balance";
        bal = zslp_balance(datadir, "ZCL23ACCESS",
                           "t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn");
        ok = ok && (bal == 10);

        if (ok) {
            fail_step = "token access serves real bytes";
            n = store_handle_request("GET",
                                     "/store/access?addr=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&token=ZCL23ACCESS",
                                     NULL, 0, resp, sizeof(resp), datadir);
            ok = n > 0;
            ok = ok && strstr((char *)resp, "HTTP/1.1 200 OK") != NULL;
            ok = ok && strstr((char *)resp,
                              "Content-Type: application/octet-stream") != NULL;

            /* Locate the body after the header terminator. */
            const uint8_t *hdr_end = NULL;
            if (ok) {
                for (size_t i = 0; i + 4 <= n; i++) {
                    if (memcmp(resp + i, "\r\n\r\n", 4) == 0) {
                        hdr_end = resp + i + 4;
                        break;
                    }
                }
                ok = (hdr_end != NULL);
            }
            /* Buyer must receive the exact blob bytes, NUL included. */
            if (ok) {
                size_t body_len = n - (size_t)(hdr_end - resp);
                ok = (body_len == sizeof(P11_5_BLOB)) &&
                     (memcmp(hdr_end, P11_5_BLOB, sizeof(P11_5_BLOB)) == 0);
            }
            /* Defence-in-depth: served bytes re-hash to the stored hash. */
            if (ok) {
                uint8_t got_hash[32], want_hash[32];
                zcl_sha3_256(hdr_end, sizeof(P11_5_BLOB), got_hash);
                zcl_sha3_256(P11_5_BLOB, sizeof(P11_5_BLOB), want_hash);
                ok = (memcmp(got_hash, want_hash, 32) == 0);
            }
        }

        if (ok) {
            printf("OK (order=%lld payment=%s balance=10)\n",
                   (long long)summaries[0].id, order.payment_addr);
            p11_5_cleanup_datadir(datadir);
        } else {
            printf("FAIL (%s; debug datadir: %s)\n", fail_step, datadir);
            failures++;
        }
    }

    return failures;
}

/* ── MVP #5 SHIELDED gate: real ivk-decrypt + memo-bound credit ──────────────
 *
 * The gate above proves the store PLUMBING but fabricates the payment (a note
 * with a placeholder ivk = memset(0x63), matched by address string). This gate
 * proves the cryptographic heart C5 actually claims ("buyer pays SHIELDED"):
 * a REAL Sapling output is encrypted to a merchant wallet's address carrying an
 * order-binding memo, the merchant wallet's ivk-DECRYPTS it (the production
 * receive path wallet_try_sapling_decrypt — params-free, see
 * test_shielded_receive_slice), and the order is credited ONLY via the recovered
 * memo (db_store_received_payment_for_memo). It is non-gameable: the persisted
 * note's value+memo come from the AEAD decrypt (not fabricated), and a real
 * payment whose memo names a DIFFERENT order does NOT credit this order — which
 * the legacy address+amount finder (db_store_received_payment) wrongly would. */

/* Build a single-output shielded tx paying (to_d,to_pk_d) `value`, with memo
 * "ZCL23ORDER:<order_id>". Uses the production payer path
 * (sapling_build_output_description) — proof degrades to zero with no params,
 * so the cv/cm/epk/enc_ciphertext are real AEAD over the recipient's PUBLIC
 * address material. Mirrors test_shielded_receive_slice's builder. */
static bool p11_6_build_paid_output_tx(struct transaction *tx,
                                       const uint8_t to_d[11],
                                       const uint8_t to_pk_d[32],
                                       uint64_t value,
                                       int64_t order_id)
{
    uint8_t ovk[32];
    memset(ovk, 0x5a, 32);

    uint8_t memo[ZC_MEMO_SIZE];
    memset(memo, 0, sizeof(memo));
    snprintf((char *)memo, sizeof(memo), "ZCL23ORDER:%lld", (long long)order_id);

    uint8_t od_cv[32], od_cm[32], od_epk[32];
    uint8_t od_enc[ZC_SAPLING_ENCCIPHERTEXT_SIZE];
    uint8_t od_out[ZC_SAPLING_OUTCIPHERTEXT_SIZE];
    uint8_t od_proof[GROTH_PROOF_SIZE];
    memset(od_cv, 0, sizeof(od_cv));
    memset(od_cm, 0, sizeof(od_cm));
    memset(od_epk, 0, sizeof(od_epk));
    memset(od_enc, 0, sizeof(od_enc));
    memset(od_out, 0, sizeof(od_out));
    memset(od_proof, 0, sizeof(od_proof));

    if (!sapling_build_output_description(ovk, to_d, to_pk_d, value, memo,
                                          od_cv, od_cm, od_epk,
                                          od_enc, od_out, od_proof, NULL))
        return false;

    transaction_init(tx);
    tx->version = 4;
    tx->overwintered = true;
    tx->value_balance = -(int64_t)value;
    tx->v_shielded_output = zcl_calloc(1, sizeof(struct output_description),
                                       "p11_6_output_desc");
    if (!tx->v_shielded_output) {
        transaction_free(tx);
        return false;
    }
    tx->num_shielded_output = 1;

    struct output_description *od = &tx->v_shielded_output[0];
    memcpy(od->cv.data, od_cv, 32);
    memcpy(od->cm.data, od_cm, 32);
    memcpy(od->ephemeral_key.data, od_epk, 32);
    memcpy(od->enc_ciphertext, od_enc, ZC_SAPLING_ENCCIPHERTEXT_SIZE);
    memcpy(od->out_ciphertext, od_out, ZC_SAPLING_OUTCIPHERTEXT_SIZE);
    memcpy(od->zkproof, od_proof, GROTH_PROOF_SIZE);
    return true;
}

/* Persist `rn` (a note RECOVERED by ivk-decrypt) into wallet_sapling_notes at
 * `pay_addr`, confirmed at height 97. Every consensus-relevant field comes from
 * the decrypt; only address (string column) and block_height are set by us. */
static bool p11_6_persist_recovered_note(struct node_db *ndb,
                                         const struct sapling_received_note *rn,
                                         const struct uint256 *txid,
                                         const char *pay_addr)
{
    struct db_sapling_note note;
    memset(&note, 0, sizeof(note));
    memcpy(note.txid, txid->data, 32);
    note.output_index = rn->output_index;
    note.value = (int64_t)rn->value;
    memcpy(note.rcm, rn->rcm, 32);
    memcpy(note.memo, rn->memo, ZC_MEMO_SIZE);
    note.memo_len = ZC_MEMO_SIZE;
    memcpy(note.ivk, rn->ivk, 32);
    memcpy(note.diversifier, rn->diversifier, 11);
    memcpy(note.pk_d, rn->pk_d, 32);
    memcpy(note.cm, rn->cm, 32);
    memcpy(note.nullifier, rn->nf, 32);
    note.block_height = 97;
    snprintf(note.address, sizeof(note.address), "%s", pay_addr);
    return db_sapling_note_save(ndb, &note);
}

int test_store_e2e_shielded(void);
int test_store_e2e_shielded(void)
{
    int failures = 0;

    printf("\n=== store e2e SHIELDED (MVP #5: real ivk-decrypt + memo-bound) ===\n");
    printf("store_e2e_shielded order -> REAL sapling note ivk-decrypt -> "
           "memo-bound credit... ");

    if (!getenv("ZCL_STRESS_TESTS")) {
        printf("SKIP (set ZCL_STRESS_TESTS=1 to run deterministic gate)\n");
        return 0;
    }

    char datadir[256];
    char dbpath[320];
    char csrf[64] = "";
    char body[256];
    uint8_t resp[16384];
    struct node_db ndb;
    struct db_store_order_summary summaries[8];
    struct db_store_order_view order;
    struct wallet *w = NULL;
    struct transaction tx, tx2;
    bool tx_built = false, tx2_built = false;
    bool ok = true;
    size_t n = 0;
    int order_count = 0;
    int64_t order_id = 0, amount = 0;
    int64_t other_order = 0;
    const char *fail_step = "setup";
    uint8_t to_d[ZC_DIVERSIFIER_SIZE];
    uint8_t to_pk_d[32];

    snprintf(datadir, sizeof(datadir), ".zcl_test_store_e2e_sh_%d", (int)getpid());
    mkdir(datadir, 0755);
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", datadir);
    memset(&ndb, 0, sizeof(ndb));
    memset(&order, 0, sizeof(order));

    /* (1) Create an order through the HTTP controller (same as the gate). */
    fail_step = "fetch csrf";
    ok = p11_5_fetch_csrf_token(datadir, 1, csrf, sizeof(csrf));
    if (ok) {
        fail_step = "create order";
        snprintf(body, sizeof(body),
                 "product_id=1&customer_addr=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn"
                 "&csrf_token=%s",
                 csrf);
        n = store_handle_request("POST", "/store/orders",
                                 (const uint8_t *)body, strlen(body),
                                 resp, sizeof(resp), datadir);
        ok = n > 0 && strstr((char *)resp, "HTTP/1.1 200 OK") != NULL &&
             strstr((char *)resp, "Order #") != NULL;
    }
    ok = ok && node_db_open(&ndb, dbpath);
    if (ok) {
        order_count = db_store_order_list_recent(&ndb, summaries,
                                                 sizeof(summaries) / sizeof(summaries[0]));
        fail_step = "list recent";
        ok = order_count == 1;
        fail_step = "load order";
        ok = ok && db_store_order_find_view(&ndb, summaries[0].id, &order);
        fail_step = "payment addr present";
        ok = ok && order.payment_addr[0] != '\0';
        if (ok) {
            order_id = summaries[0].id;
            amount = order.amount_zatoshi;
            other_order = order_id + 777;
        }
        fail_step = "order has positive amount";
        ok = ok && amount > 0;
        fail_step = "seed tip block";
        ok = ok && p11_5_seed_tip_block(&ndb, 100);
    }

    /* (2) Merchant wallet derives the address whose ivk will decrypt payments. */
    if (ok) {
        w = zcl_calloc(1, sizeof(struct wallet), "p11_6_wallet");
        fail_step = "merchant wallet alloc";
        ok = w != NULL;
    }
    if (ok) {
        wallet_init(w);
        uint8_t seed[32];
        memset(seed, 0x11, 32);
        fail_step = "merchant sapling seed";
        ok = sapling_keystore_set_seed(&w->sapling_keys, seed);
        fail_step = "merchant sapling address";
        ok = ok && sapling_keystore_new_address(&w->sapling_keys, to_d, to_pk_d);
    }

    /* (3) REAL payment: build a Sapling output to the merchant carrying the
     * order memo, ivk-DECRYPT it, and persist the RECOVERED note. */
    if (ok) {
        tx_built = p11_6_build_paid_output_tx(&tx, to_d, to_pk_d,
                                              (uint64_t)amount, order_id);
        fail_step = "build paid output (params-free)";
        ok = tx_built;
    }
    struct uint256 txid;
    memset(&txid, 0, sizeof(txid));
    txid.data[0] = 0x5C;
    txid.data[1] = (uint8_t)(order_id & 0xff);
    if (ok) {
        int found = wallet_try_sapling_decrypt(w, &tx, &txid);
        fail_step = "merchant ivk-decrypts the paying note";
        ok = found == 1 && w->num_sapling_notes == 1;
    }
    if (ok) {
        const struct sapling_received_note *rn = &w->sapling_notes[0];
        char expect[64];
        int el = snprintf(expect, sizeof(expect), "ZCL23ORDER:%lld",
                          (long long)order_id);
        fail_step = "recovered value == order amount";
        ok = rn->value == (uint64_t)amount;
        fail_step = "recovered memo binds the order";
        ok = ok && el > 0 && (size_t)el < sizeof(expect) &&
             memcmp(rn->memo, expect, (size_t)el) == 0 && rn->memo[el] == '\0';
        fail_step = "persist recovered note";
        ok = ok && p11_6_persist_recovered_note(&ndb, rn, &txid,
                                                order.payment_addr);
    }

    /* (4) The memo-bound finder credits this order; the legacy finder agrees
     * (one note, one order — they cannot diverge yet). */
    if (ok) {
        fail_step = "memo-bound finder credits the order";
        ok = db_store_received_payment_for_memo(&ndb, order.payment_addr,
                                                order_id, 100) == amount;
        fail_step = "legacy address+amount finder also sees the note";
        ok = ok && db_store_received_payment(&ndb, order.payment_addr, 100) == amount;
    }

    /* (5) NEGATIVE: a REAL payment to the SAME address whose memo names a
     * DIFFERENT order must NOT credit this order — yet the legacy address+amount
     * finder wrongly counts both. This is the hole the memo bind closes. */
    if (ok) {
        tx2_built = p11_6_build_paid_output_tx(&tx2, to_d, to_pk_d,
                                               (uint64_t)amount, other_order);
        fail_step = "build other-order payment";
        ok = tx2_built;
    }
    struct uint256 txid2;
    memset(&txid2, 0, sizeof(txid2));
    txid2.data[0] = 0x5D;
    txid2.data[1] = (uint8_t)(other_order & 0xff);
    if (ok) {
        int found2 = wallet_try_sapling_decrypt(w, &tx2, &txid2);
        fail_step = "second payment ivk-decrypts";
        ok = found2 == 1 && w->num_sapling_notes == 2;
    }
    if (ok) {
        fail_step = "persist second recovered note";
        ok = p11_6_persist_recovered_note(&ndb, &w->sapling_notes[1], &txid2,
                                          order.payment_addr);
    }
    if (ok) {
        fail_step = "memo finder still credits ONLY this order's amount";
        ok = db_store_received_payment_for_memo(&ndb, order.payment_addr,
                                                order_id, 100) == amount;
        fail_step = "memo finder credits the OTHER order separately";
        ok = ok && db_store_received_payment_for_memo(&ndb, order.payment_addr,
                                                      other_order, 100) == amount;
        fail_step = "legacy finder over-counts both (the closed hole)";
        ok = ok && db_store_received_payment(&ndb, order.payment_addr, 100)
                   == amount * 2;
    }

    if (ndb.open)
        node_db_close(&ndb);
    if (tx_built)
        transaction_free(&tx);
    if (tx2_built)
        transaction_free(&tx2);
    if (w) {
        wallet_free(w);
        free(w);
    }

    if (ok) {
        printf("OK (order=%lld payment=%s amount=%lld, ivk-decrypted, memo-bound)\n",
               (long long)order_id, order.payment_addr, (long long)amount);
        p11_5_cleanup_datadir(datadir);
    } else {
        printf("FAIL (%s; debug datadir: %s)\n", fail_step, datadir);
        failures++;
    }

    return failures;
}
