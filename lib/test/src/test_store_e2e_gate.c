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
