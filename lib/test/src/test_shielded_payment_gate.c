/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MVP criterion #4 CI gate: transparent -> shielded payment.
 *
 * The gate exercises the shipped RPC/controller path end-to-end inside
 * `test_zcl`:
 *   1. create a wallet-owned transparent funding address
 *   2. persist one spendable wallet UTXO into node.db
 *   3. create a wallet-owned Sapling address via `z_getnewaddress`
 *   4. send from t-address to z-address via `z_sendmany`
 *   5. assert the tx reached mempool, has a shielded output, and can be
 *      decrypted back into the wallet's note set
 *
 * This is an opt-in stress gate because it depends on real Sapling proving
 * params and builds a real proof. Run with:
 *   ZCL_STRESS_TESTS=1 build/bin/test_zcl
 *   ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=shielded_payment build/bin/test_zcl
 */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "controllers/wallet_controller.h"
#include "controllers/wallet_shielded_controller.h"
#include "wallet/keystore.h"
#include "wallet/wallet_sqlite.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "consensus/validation.h"
#include "sapling/sapling_prover.h"
#include "validation/accept_to_mempool.h"
#include "validation/contextual_check_tx.h"

#include <errno.h>
#include <stdatomic.h>

static bool p11_4_params_available(char *params_dir, size_t params_dir_size)
{
    const char *home = getenv("HOME");
    if (!home || !params_dir || params_dir_size == 0)
        return false;

    snprintf(params_dir, params_dir_size, "%s/.zcash-params", home);

    const char *files[] = {
        "sapling-spend.params",
        "sapling-output.params",
        "sprout-groth16.params",
        "sprout-verifying.key",
    };

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[768];
        snprintf(path, sizeof(path), "%s/%s", params_dir, files[i]);
        if (access(path, R_OK) != 0)
            return false;
    }

    return true;
}

static bool p11_4_make_tmpdir(char *tmpdir, size_t tmpdir_size)
{
    if (!tmpdir || tmpdir_size < 32)
        return false;
    snprintf(tmpdir, tmpdir_size, "./test-tmp/p11_4_%d", (int)getpid());
    mkdir("./test-tmp", 0755);
    if (mkdir(tmpdir, 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

static bool p11_4_build_funding_utxo(struct node_db *ndb,
                                     struct coins_view_cache *coins_tip,
                                     const struct key_id *kid,
                                     const struct script *script,
                                     int tip_height)
{
    struct db_wallet_utxo utxo;
    memset(&utxo, 0, sizeof(utxo));
    memset(utxo.txid, 0x41, 32);
    utxo.vout = 0;
    utxo.value = 3 * COIN_VALUE;
    memcpy(utxo.address_hash, kid->id.data, 20);
    utxo.script = (uint8_t *)script->data;
    utxo.script_len = script->size;
    utxo.height = tip_height - 1;
    utxo.is_coinbase = false;
    if (!db_wallet_utxo_save(ndb, &utxo))
        return false;

    /* The wallet selector reads its owned UTXOs from node.db, while the
     * shared mempool-admission gate reads the consensus UTXO view. A real
     * node keeps those projections in sync; this fixture must populate both
     * so the RPC exercises the complete production acceptance context. */
    struct uint256 txid;
    memcpy(txid.data, utxo.txid, sizeof(txid.data));
    struct coins_cache_entry *entry =
        coins_view_cache_modify_new(coins_tip, &txid);
    if (!entry)
        return false;
    coins_alloc(&entry->coins, 1);
    if (!entry->coins.vout)
        return false;
    entry->coins.vout[0].value = utxo.value;
    entry->coins.vout[0].script_pub_key = *script;
    entry->coins.height = utxo.height;
    entry->coins.version = 1;
    entry->coins.is_coinbase = false;
    return true;
}

static bool p11_4_rpc_z_getnewaddress(struct rpc_table *tbl,
                                      char *out, size_t out_size)
{
    struct json_value params;
    struct json_value result;
    json_init(&params);
    json_init(&result);
    json_set_array(&params);

    bool ok = rpc_table_execute(tbl, "z_getnewaddress", &params, &result);
    ok = ok && result.type == JSON_STR;
    ok = ok && json_get_str(&result) != NULL;
    if (ok) {
        snprintf(out, out_size, "%s", json_get_str(&result));
    }

    json_free(&params);
    json_free(&result);
    return ok;
}

static bool p11_4_rpc_z_sendmany(struct rpc_table *tbl,
                                 const char *from_addr,
                                 const char *zaddr,
                                 char *txid_hex, size_t txid_hex_size)
{
    struct json_value params;
    struct json_value result;
    struct json_value from;
    struct json_value recipients;
    struct json_value recipient;
    struct json_value addr;
    struct json_value amount;

    json_init(&params);
    json_init(&result);
    json_init(&from);
    json_init(&recipients);
    json_init(&recipient);
    json_init(&addr);
    json_init(&amount);

    json_set_array(&params);
    json_set_str(&from, from_addr);
    json_push_back(&params, &from);

    json_set_array(&recipients);
    json_set_object(&recipient);
    json_set_str(&addr, zaddr);
    json_push_kv(&recipient, "address", &addr);
    json_set_str(&amount, "1.25000000");
    json_push_kv(&recipient, "amount", &amount);
    json_push_back(&recipients, &recipient);
    json_push_back(&params, &recipients);

    bool ok = rpc_table_execute(tbl, "z_sendmany", &params, &result);
    ok = ok && result.type == JSON_STR;
    ok = ok && json_get_str(&result) != NULL;
    if (ok)
        snprintf(txid_hex, txid_hex_size, "%s", json_get_str(&result));

    json_free(&params);
    json_free(&result);
    return ok;
}

int test_shielded_payment_gate(void)
{
    int failures = 0;

    printf("\n=== shielded payment (MVP #4) ===\n");
    printf("shielded_payment wallet shield + private send e2e... ");

    if (!getenv("ZCL_STRESS_TESTS")) {
        printf("SKIP (set ZCL_STRESS_TESTS=1 to run deterministic gate)\n");
        return 0;
    }

    char params_dir[512];
    if (!p11_4_params_available(params_dir, sizeof(params_dir))) {
        /* ZCL_STRESS_TESTS is set (checked above) → the caller (make
         * ci-stress / the mvp-stress job) INTENDS to run #4 for real.
         * A missing fixture is then a provisioning failure, NOT a pass:
         * skip-to-green here is the false-green this gate must never emit.
         * Fail LOUD so the operator wires ~/.zcash-params onto the runner.
         * (The bare-dev skip lives above, guarded by !getenv.) */
        printf("FAIL (ZCL_STRESS_TESTS=1 but Sapling params absent at "
               "%s/.zcash-params — provision the ~770MB fixture on this "
               "runner or unset ZCL_STRESS_TESTS for a bare dev run)\n",
               getenv("HOME") ? getenv("HOME") : "$HOME");
        return 1;
    }

    if (!sapling_init_params(params_dir)) {
        printf("FAIL (sapling_init_params(%s) failed)\n", params_dir);
        return 1;
    }
    if (!zclassic_sapling_prover_is_ready()) {
        printf("FAIL (Sapling prover backend=%s status=%s)\n",
               zclassic_sapling_prover_backend(),
               zclassic_sapling_prover_status());
        return 1;
    }

    char tmpdir[256];
    char dbpath[320];
    struct node_db ndb;
    struct wallet_sqlite wsql;
    bool wsql_open = false;
    struct wallet *wallet = NULL;
    struct main_state ms;
    struct tx_mempool mempool;
    struct coins_view null_coins_view;
    struct coins_view_cache coins_tip;
    struct block_index tip;
    struct rpc_table tbl;
    struct transaction sent_tx;
    struct privkey funding_key;
    struct pubkey funding_pubkey;
    struct key_id funding_kid;
    struct tx_destination funding_dest;
    struct script funding_script;
    char funding_addr[128];
    char zaddr[128];
    char txid_hex[65];
    const char *fail_step = "setup";
    bool ok = true;

    memset(&ndb, 0, sizeof(ndb));
    memset(&wsql, 0, sizeof(wsql));
    memset(&ms, 0, sizeof(ms));
    memset(&mempool, 0, sizeof(mempool));
    memset(&null_coins_view, 0, sizeof(null_coins_view));
    memset(&coins_tip, 0, sizeof(coins_tip));
    memset(&tip, 0, sizeof(tip));
    memset(&tbl, 0, sizeof(tbl));
    memset(&sent_tx, 0, sizeof(sent_tx));
    memset(&funding_key, 0, sizeof(funding_key));
    memset(&funding_pubkey, 0, sizeof(funding_pubkey));
    memset(&funding_kid, 0, sizeof(funding_kid));
    memset(&funding_dest, 0, sizeof(funding_dest));
    memset(&funding_script, 0, sizeof(funding_script));
    memset(funding_addr, 0, sizeof(funding_addr));
    memset(zaddr, 0, sizeof(zaddr));
    memset(txid_hex, 0, sizeof(txid_hex));

    if (!p11_4_make_tmpdir(tmpdir, sizeof(tmpdir))) {
        printf("FAIL (tmpdir)\n");
        return 1;
    }
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", tmpdir);

    wallet = zcl_calloc(1, sizeof(*wallet), "p11_4_wallet");
    if (!wallet) {
        printf("FAIL (wallet alloc)\n");
        return 1;
    }
    wallet_init(wallet);
    if (!sapling_keystore_generate_seed(&wallet->sapling_keys)) {
        wallet_free(wallet);
        free(wallet);
        printf("FAIL (sapling seed)\n");
        return 1;
    }
    main_state_init(&ms);
    tx_mempool_init(&mempool, 0);
    coins_view_cache_init(&coins_tip, &null_coins_view);
    rpc_table_init(&tbl);
    block_index_init(&tip);
    transaction_init(&sent_tx);

    tip.nHeight = 500000;
    tip.nTime = (uint32_t)platform_time_wall_time_t();
    active_chain_move_window_tip(&ms.chain_active, &tip);
    wallet->best_block = &tip;
    wallet->best_block_height = tip.nHeight;

    ok = node_db_open(&ndb, dbpath);
    if (!ok)
        printf("(node_db_open failed for %s)\n", dbpath);

    if (ok) {
        /* z_getnewaddress fail-closes (wallet_shielded_controller.c) unless
         * ctx->wallet_db is a real, open wallet_sqlite — same as production
         * boot.c, which opens wallet_sqlite over the same node.db handle
         * right after node_db_open succeeds. A NULL wallet_db here would
         * trip that guard on the very first RPC call. */
        struct zcl_result wsql_r = wallet_sqlite_open_r(&wsql, ndb.db);
        ok = wsql_r.ok;
        if (ok) {
            wsql_open = true;
        } else {
            printf("(wallet_sqlite_open_r failed: code=%d message=%s)\n",
                   wsql_r.code, wsql_r.message);
        }
    }

    if (ok) {
        privkey_make_new(&funding_key, true);
        privkey_get_pubkey(&funding_key, &funding_pubkey);
        funding_kid = pubkey_get_id(&funding_pubkey);
        ok = keystore_add_key(&wallet->keystore, &funding_key);
    }

    if (ok) {
        funding_dest.type = DEST_KEY_ID;
        funding_dest.id.key = funding_kid;
        script_for_destination(&funding_script, &funding_dest);
        const struct chain_params *cp = chain_params_get();
        size_t pk_pfx_len = 0;
        size_t sc_pfx_len = 0;
        const unsigned char *pk_pfx =
            chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
        const unsigned char *sc_pfx =
            chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);
        ok = encode_destination(&funding_dest, pk_pfx, pk_pfx_len,
                                sc_pfx, sc_pfx_len,
                                funding_addr, sizeof(funding_addr));
    }

    if (ok)
        ok = p11_4_build_funding_utxo(&ndb, &coins_tip, &funding_kid,
                                      &funding_script, tip.nHeight);

    if (ok) {
        /* register_wallet_rpc_commands() already registers the shielded
         * sub-controller (wallet_controller.c calls
         * register_wallet_shielded_rpc_commands internally), so a second
         * explicit call here would re-register z_getnewaddress and trip the
         * rpc_table_must_append duplicate-name guard. One call registers the
         * full transparent + shielded surface the gate exercises. */
        register_wallet_rpc_commands(&tbl);
        {
            char warmup_status[64];
            if (rpc_is_in_warmup(warmup_status, sizeof(warmup_status)))
                set_rpc_warmup_finished();
        }
        rpc_wallet_set_state(wallet, &ms, tmpdir, &wsql, &mempool, NULL);
        rpc_wallet_set_node_db(&ndb);
        rpc_wallet_set_coins_tip(&coins_tip);
    }

    if (ok) {
        fail_step = "z_getnewaddress";
        ok = p11_4_rpc_z_getnewaddress(&tbl, zaddr, sizeof(zaddr));
    }

    if (ok) {
        fail_step = "zaddr_prefix";
        ok = strncmp(zaddr, "zs1", 3) == 0;
    }

    /* Make the gate prove that Groth16 checks really ran even if another test
     * changed the process-wide historical-proof deferral policy. */
    int previous_defer_height = atomic_exchange_explicit(
        &g_deferred_proof_validation_below_height, -1,
        memory_order_relaxed);

    if (ok) {
        fail_step = "z_sendmany";
        ok = p11_4_rpc_z_sendmany(&tbl, funding_addr, zaddr,
                                  txid_hex, sizeof(txid_hex));
    }

    if (ok) {
        fail_step = "post_send_checks";
        struct uint256 txid;
        memset(&txid, 0, sizeof(txid));
        uint256_set_hex(&txid, txid_hex);
        ok = tx_mempool_lookup(&mempool, &txid, &sent_tx);
        ok = ok && sent_tx.num_shielded_output == 1;
        ok = ok && sent_tx.value_balance == -125000000LL;

        /* A second, independent admission into an empty pool exercises the
         * complete consensus/mempool boundary: structure, Sapling output
         * proof + binding signature, transparent input/script, and fee. */
        struct tx_mempool proof_pool;
        tx_mempool_init(&proof_pool, 0);
        enum mempool_accept_result accepted = accept_to_mempool(
            &proof_pool, &coins_tip, &ms, chain_params_get(), &sent_tx);
        ok = ok && accepted == MEMPOOL_ACCEPT_OK;
        ok = ok && tx_mempool_exists(&proof_pool, &sent_tx.hash);
        tx_mempool_free(&proof_pool);

        /* Negative control: changing one proof byte must be rejected by the
         * same full admission boundary and must leave its pool empty. */
        struct transaction tampered_tx;
        transaction_init(&tampered_tx);
        bool copied = transaction_copy(&tampered_tx, &sent_tx);
        if (copied && tampered_tx.num_shielded_output == 1) {
            tampered_tx.v_shielded_output[0].zkproof[0] ^= 0x01;
            transaction_compute_hash(&tampered_tx);
        }
        struct tx_mempool reject_pool;
        tx_mempool_init(&reject_pool, 0);
        enum mempool_accept_result rejected = copied
            ? accept_to_mempool(&reject_pool, &coins_tip, &ms,
                                chain_params_get(), &tampered_tx)
            : MEMPOOL_ACCEPT_INTERNAL_ERROR;
        ok = ok && copied;
        ok = ok && rejected == MEMPOOL_ACCEPT_INVALID;
        ok = ok && tx_mempool_size(&reject_pool) == 0;
        tx_mempool_free(&reject_pool);
        transaction_free(&tampered_tx);

        ok = ok && wallet_try_sapling_decrypt(wallet, &sent_tx, &txid) == 1;
        ok = ok && wallet->num_sapling_notes == 1;
        ok = ok && wallet_get_sapling_balance(wallet) == 125000000LL;
    }

    atomic_store_explicit(&g_deferred_proof_validation_below_height,
                          previous_defer_height, memory_order_relaxed);

    rpc_wallet_set_node_db(NULL);
    rpc_wallet_set_coins_tip(NULL);
    rpc_wallet_set_state(NULL, NULL, NULL, NULL, NULL, NULL);

    transaction_free(&sent_tx);
    coins_view_cache_free(&coins_tip);
    tx_mempool_free(&mempool);
    if (wsql_open)
        wallet_sqlite_close(&wsql);
    node_db_close(&ndb);
    main_state_free(&ms);
    wallet_free(wallet);
    free(wallet);
    test_cleanup_tmpdir(tmpdir);

    if (ok) {
        printf("OK (t->z proof verified, fully admitted, tamper rejected, "
               "decrypted to 1.25000000 ZCL)\n");
    } else {
        printf("FAIL (%s)\n", fail_step);
        failures++;
    }

    return failures;
}
