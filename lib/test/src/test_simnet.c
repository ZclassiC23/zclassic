/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pins the simnet foundation slice (lib/sim/src/simnet.c): a RAM-only,
 * single-node chain harness that mints blocks through the REAL consensus
 * code (connect_block) with no disk and no real PoW.
 *
 * The keystone loop:
 *   mint coinbase  →  coin becomes spendable  →  spend the (matured)
 *   coinbase  →  input consumed + new output present, all in the in-memory
 *   coins view.
 *
 * If any of these asserts fail it means the harness assembled a block the
 * real validator rejects — the fix is ALWAYS in the harness's block
 * construction, never in a consensus predicate.
 */

#include "test/test_helpers.h"

#include "models/explorer_index.h"
#include "models/znam.h"
#include "sim/simnet.h"
#include "core/uint256.h"
#include "znam/znam.h"
#include "zslp/slp.h"

#include <stdio.h>
#include <string.h>

#define SN_CHECK(name, expr) do {          \
    printf("%s... ", (name));              \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static void sim_test_p2pkh_script(struct script *sp, unsigned char seed)
{
    sp->data[0] = 0x76;  /* OP_DUP */
    sp->data[1] = 0xa9;  /* OP_HASH160 */
    sp->data[2] = 0x14;  /* push 20 */
    for (int i = 0; i < 20; i++)
        sp->data[3 + i] = (unsigned char)(seed + i);
    sp->data[23] = 0x88; /* OP_EQUALVERIFY */
    sp->data[24] = 0xac; /* OP_CHECKSIG */
    sp->size = 25;
}

static bool sim_test_make_opreturn_spend_outputs(struct transaction *tx,
                                                 const struct uint256 *prev_txid,
                                                 uint32_t prev_n,
                                                 const uint8_t *opret,
                                                 size_t opret_len,
                                                 const int64_t *out_values,
                                                 const unsigned char *addr_seeds,
                                                 size_t noutputs)
{
    transaction_init(tx);
    if (!prev_txid || !opret || opret_len == 0 || !out_values ||
        !addr_seeds || noutputs == 0)
        return false;
    if (!transaction_alloc(tx, 1, noutputs + 1))
        return false;

    tx->version = 1;
    tx->vin[0].prevout.hash = *prev_txid;
    tx->vin[0].prevout.n = prev_n;
    {
        uint8_t sig[] = {0x00, 0x00};
        script_set(&tx->vin[0].script_sig, sig, sizeof(sig));
    }
    tx->vin[0].sequence = 0xFFFFFFFFu;

    tx->vout[0].value = 0;
    script_set(&tx->vout[0].script_pub_key, opret, opret_len);
    for (size_t i = 0; i < noutputs; i++) {
        tx->vout[i + 1].value = out_values[i];
        sim_test_p2pkh_script(&tx->vout[i + 1].script_pub_key, addr_seeds[i]);
    }

    transaction_compute_hash(tx);
    return true;
}

static bool sim_test_make_opreturn_spend(struct transaction *tx,
                                         const struct uint256 *prev_txid,
                                         uint32_t prev_n,
                                         const uint8_t *opret,
                                         size_t opret_len,
                                         int64_t out_value,
                                         unsigned char addr_seed)
{
    int64_t values[1] = { out_value };
    unsigned char seeds[1] = { addr_seed };
    return sim_test_make_opreturn_spend_outputs(tx, prev_txid, prev_n,
                                                opret, opret_len, values,
                                                seeds, 1);
}

static bool sim_test_make_p2pkh_spend(struct transaction *tx,
                                      const struct uint256 *prev_txid,
                                      uint32_t prev_n, int64_t out_value,
                                      unsigned char addr_seed)
{
    transaction_init(tx);
    if (!prev_txid)
        return false;
    if (!transaction_alloc(tx, 1, 1))
        return false;

    tx->version = 1;
    tx->vin[0].prevout.hash = *prev_txid;
    tx->vin[0].prevout.n = prev_n;
    {
        uint8_t sig[] = {0x00, 0x00};
        script_set(&tx->vin[0].script_sig, sig, sizeof(sig));
    }
    tx->vin[0].sequence = 0xFFFFFFFFu;
    tx->vout[0].value = out_value;
    sim_test_p2pkh_script(&tx->vout[0].script_pub_key, addr_seed);

    transaction_compute_hash(tx);
    return true;
}

static void sim_test_hex_bytes(const uint8_t *data, size_t len,
                               char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!data || out_len < len * 2 + 1)
        return;
    for (size_t i = 0; i < len; i++)
        snprintf(out + (i * 2), out_len - (i * 2), "%02X", data[i]);
}

static void sim_test_slp_wire_token_id(const struct uint256 *internal,
                                       struct uint256 *wire)
{
    for (int i = 0; i < 32; i++)
        wire->data[i] = internal->data[31 - i];
}

static bool sim_test_apply_slp(struct node_db *ndb,
                               const struct transaction *tx, int height)
{
    if (!tx || tx->num_vout == 0)
        return false;
    struct slp_message msg;
    if (!slp_parse(tx->vout[0].script_pub_key.data,
                   tx->vout[0].script_pub_key.size, &msg))
        return false;
    explorer_index_apply_slp(ndb, tx, &msg, height);
    return true;
}

static int64_t sim_test_transfer_amount(
    const struct db_zslp_transfer_info *xfers, int n, int tx_type, int vout)
{
    for (int i = 0; i < n; i++) {
        if (xfers[i].tx_type == tx_type && xfers[i].vout == vout)
            return xfers[i].amount;
    }
    return -1;
}

static bool sim_test_index_block(struct node_db *ndb,
                                 const struct transaction *txs, size_t ntx,
                                 int height, unsigned char hash_seed)
{
    if (!ndb || !ndb->open || !txs || ntx == 0)
        return false;

    struct block blk;
    block_init(&blk);
    blk.vtx = (struct transaction *)txs;
    blk.num_vtx = ntx;

    struct uint256 bhash;
    memset(bhash.data, hash_seed, sizeof(bhash.data));
    struct block_index pindex;
    block_index_init(&pindex);
    pindex.nHeight = height;
    pindex.phashBlock = &bhash;

    uint8_t prev_receipt[32] = {0};
    uint8_t out_receipt[32];
    bool ok = explorer_index_block(ndb, &blk, &pindex, prev_receipt,
                                   out_receipt, NULL, NULL);

    blk.vtx = NULL;
    blk.num_vtx = 0;
    return ok;
}

int test_simnet(void)
{
    printf("\n=== simnet in-memory chain harness ===\n");
    int failures = 0;

    struct simnet sim;
    SN_CHECK("simnet_init succeeds", simnet_init(&sim));
    SN_CHECK("fresh tip is the synthetic base (height 99)",
             simnet_tip_height(&sim) == 99);

    /* 1. Mint a coinbase block through real connect_block. */
    struct uint256 cb_txid;
    uint256_set_null(&cb_txid);
    bool minted = simnet_mint_coinbase(&sim, &cb_txid);
    SN_CHECK("mint coinbase drives block through connect_block", minted);
    SN_CHECK("tip advanced to height 100", simnet_tip_height(&sim) == 100);
    SN_CHECK("coinbase coin is spendable in the RAM coins view",
             simnet_coin_exists(&sim, &cb_txid));

    int64_t cb_value = 0;
    SN_CHECK("coinbase output value readable from coins view",
             simnet_coin_value(&sim, &cb_txid, 0, &cb_value));
    SN_CHECK("coinbase output value is the minted subsidy stub",
             cb_value == 1000000);

    /* 2. Spend the (now matured) coinbase to a chosen output. The harness
     *    mints the spending block at a height that satisfies the REAL
     *    coinbase-maturity predicate (>= 100 blocks). */
    struct uint256 spend_txid;
    uint256_set_null(&spend_txid);
    bool spent = simnet_spend(&sim, &cb_txid, 0, 900000, &spend_txid);
    SN_CHECK("spend the matured coinbase through connect_block", spent);

    /* Maturity forces the spend block to height 100 + COINBASE_MATURITY. */
    SN_CHECK("spend block minted at a mature height (>= 200)",
             simnet_tip_height(&sim) >= 200);

    /* 3. The coins view reflects the spend: input consumed, output present. */
    SN_CHECK("spent coinbase is consumed (no live output remains)",
             !simnet_coin_exists(&sim, &cb_txid));
    SN_CHECK("new spend output is present in the coins view",
             simnet_coin_exists(&sim, &spend_txid));

    int64_t spend_value = 0;
    SN_CHECK("spend output value readable from coins view",
             simnet_coin_value(&sim, &spend_txid, 0, &spend_value));
    SN_CHECK("spend output carries the chosen value", spend_value == 900000);

    /* 4. A second independent coinbase still mints cleanly on top. */
    struct uint256 cb2_txid;
    uint256_set_null(&cb2_txid);
    bool minted2 = simnet_mint_coinbase(&sim, &cb2_txid);
    SN_CHECK("a further coinbase mints on top of the spend block", minted2);
    SN_CHECK("second coinbase coin exists", simnet_coin_exists(&sim, &cb2_txid));

    /* 5. Public arbitrary-tx mint: OP_RETURN is accepted by consensus and
     *    pruned as unspendable while the transparent output remains live. */
    struct transaction custom;
    uint8_t opret[] = {0x6a, 0x04, 'S', 'I', 'M', '1'};
    bool built_custom =
        sim_test_make_opreturn_spend(&custom, &spend_txid, 0, opret,
                                     sizeof(opret), 800000, 0x40);
    struct uint256 custom_txid = built_custom ? custom.hash : (struct uint256){0};
    SN_CHECK("build arbitrary OP_RETURN spend tx", built_custom);
    bool minted_custom = built_custom && simnet_mint_txs(&sim, &custom, 1);
    SN_CHECK("simnet_mint_txs accepts transparent+OP_RETURN block",
             minted_custom);
    SN_CHECK("arbitrary tx consumed its input",
             minted_custom && !simnet_coin_exists(&sim, &spend_txid));
    int64_t custom_value = 0;
    SN_CHECK("arbitrary tx transparent output remains in coins view",
             minted_custom &&
             simnet_coin_value(&sim, &custom_txid, 1, &custom_value) &&
             custom_value == 800000);

    /* 6. ZSLP overlay slice: real SLP builders, real simnet acceptance,
     *    real chain-derived token/transfer projection. */
    struct node_db zslp_db;
    memset(&zslp_db, 0, sizeof(zslp_db));
    bool zslp_open = node_db_open(&zslp_db, ":memory:");
    SN_CHECK("ZSLP projection db opens", zslp_open);

    uint8_t genesis_script[512];
    size_t genesis_script_len =
        slp_build_genesis(genesis_script, sizeof(genesis_script),
                          "SIM", "Simnet Token", "", NULL, 0, 0, 1000);
    SN_CHECK("build real SLP GENESIS script", genesis_script_len > 0);

    struct transaction slp_genesis;
    struct transaction slp_genesis_projection;
    transaction_init(&slp_genesis_projection);
    bool built_slp_genesis =
        genesis_script_len > 0 &&
        sim_test_make_opreturn_spend(&slp_genesis, &custom_txid, 1,
                                     genesis_script, genesis_script_len,
                                     700000, 0x60);
    bool copied_slp_genesis =
        built_slp_genesis &&
        transaction_copy(&slp_genesis_projection, &slp_genesis);
    struct uint256 slp_token_id =
        built_slp_genesis ? slp_genesis.hash : (struct uint256){0};
    SN_CHECK("build SLP GENESIS tx", built_slp_genesis && copied_slp_genesis);
    bool minted_slp_genesis =
        built_slp_genesis && copied_slp_genesis &&
        simnet_mint_txs(&sim, &slp_genesis, 1);
    int slp_genesis_height = simnet_tip_height(&sim);
    SN_CHECK("mint SLP GENESIS through simnet", minted_slp_genesis);
    bool applied_slp_genesis =
        zslp_open && minted_slp_genesis &&
        sim_test_apply_slp(&zslp_db, &slp_genesis_projection,
                           slp_genesis_height);
    SN_CHECK("fold SLP GENESIS into token projection", applied_slp_genesis);

    char slp_token_hex[65];
    sim_test_hex_bytes(slp_token_id.data, 32, slp_token_hex,
                       sizeof(slp_token_hex));
    struct db_zslp_token_info slp_info;
    memset(&slp_info, 0, sizeof(slp_info));
    bool found_slp_token =
        zslp_open && db_zslp_token_find(&zslp_db, slp_token_hex, &slp_info);
    SN_CHECK("ZSLP projection sees token",
             found_slp_token && strcmp(slp_info.ticker, "SIM") == 0 &&
             strcmp(slp_info.name, "Simnet Token") == 0 &&
             slp_info.total_minted == 1000);

    struct uint256 slp_wire_token_id;
    sim_test_slp_wire_token_id(&slp_token_id, &slp_wire_token_id);
    uint64_t send_qty[2] = { 250, 750 };
    uint8_t send_script[512];
    size_t send_script_len =
        slp_build_send(send_script, sizeof(send_script), &slp_wire_token_id,
                       send_qty, 2);
    SN_CHECK("build real SLP SEND script", send_script_len > 0);

    int64_t send_values[2] = { 300000, 300000 };
    unsigned char send_seeds[2] = { 0x80, 0xA0 };
    struct transaction slp_send;
    struct transaction slp_send_projection;
    transaction_init(&slp_send_projection);
    bool built_slp_send =
        send_script_len > 0 &&
        sim_test_make_opreturn_spend_outputs(&slp_send, &slp_token_id, 1,
                                             send_script, send_script_len,
                                             send_values, send_seeds, 2);
    bool copied_slp_send =
        built_slp_send && transaction_copy(&slp_send_projection, &slp_send);
    struct uint256 slp_send_txid =
        built_slp_send ? slp_send.hash : (struct uint256){0};
    SN_CHECK("build SLP SEND tx", built_slp_send && copied_slp_send);
    bool minted_slp_send =
        built_slp_send && copied_slp_send &&
        simnet_mint_txs(&sim, &slp_send, 1);
    int slp_send_height = simnet_tip_height(&sim);
    SN_CHECK("mint SLP SEND through simnet", minted_slp_send);
    bool applied_slp_send =
        zslp_open && minted_slp_send &&
        sim_test_apply_slp(&zslp_db, &slp_send_projection, slp_send_height);
    SN_CHECK("fold SLP SEND into transfer projection", applied_slp_send);

    struct db_zslp_transfer_info slp_xfers[4];
    memset(slp_xfers, 0, sizeof(slp_xfers));
    int slp_xfer_count =
        zslp_open ? db_zslp_transfer_list_by_token(&zslp_db, slp_token_hex,
                                                   slp_xfers, 4) : 0;
    SN_CHECK("ZSLP projection sees genesis/send transfer balances",
             slp_xfer_count == 3 &&
             sim_test_transfer_amount(slp_xfers, slp_xfer_count,
                                      SLP_TX_GENESIS, 1) == 1000 &&
             sim_test_transfer_amount(slp_xfers, slp_xfer_count,
                                      SLP_TX_SEND, 1) == 250 &&
             sim_test_transfer_amount(slp_xfers, slp_xfer_count,
                                      SLP_TX_SEND, 2) == 750);

    transaction_free(&slp_genesis_projection);
    transaction_free(&slp_send_projection);
    if (zslp_open)
        node_db_close(&zslp_db);

    /* 7. ZNAM overlay slice: real ZNAM REGISTER builder, simnet acceptance,
     *    explorer owner derivation from the indexed funding output, and
     *    model-backed name resolution. */
    struct node_db znam_db;
    memset(&znam_db, 0, sizeof(znam_db));
    bool znam_open = node_db_open(&znam_db, ":memory:");
    SN_CHECK("ZNAM projection db opens", znam_open);

    struct transaction znam_owner_fund;
    struct transaction znam_owner_fund_projection;
    transaction_init(&znam_owner_fund_projection);
    bool built_znam_owner_fund =
        sim_test_make_p2pkh_spend(&znam_owner_fund, &slp_send_txid, 1,
                                  250000, 0xC0);
    bool copied_znam_owner_fund =
        built_znam_owner_fund &&
        transaction_copy(&znam_owner_fund_projection, &znam_owner_fund);
    struct uint256 znam_owner_fund_txid =
        built_znam_owner_fund ? znam_owner_fund.hash : (struct uint256){0};
    SN_CHECK("build ZNAM owner funding tx",
             built_znam_owner_fund && copied_znam_owner_fund);
    bool minted_znam_owner_fund =
        built_znam_owner_fund && copied_znam_owner_fund &&
        simnet_mint_txs(&sim, &znam_owner_fund, 1);
    int znam_owner_fund_height = simnet_tip_height(&sim);
    SN_CHECK("mint ZNAM owner funding tx through simnet",
             minted_znam_owner_fund);
    bool indexed_znam_owner_fund =
        znam_open && minted_znam_owner_fund &&
        sim_test_index_block(&znam_db, &znam_owner_fund_projection, 1,
                             znam_owner_fund_height, 0xD1);
    SN_CHECK("index ZNAM owner funding output", indexed_znam_owner_fund);

    uint8_t znam_script[512];
    size_t znam_script_len =
        znam_build_register(znam_script, sizeof(znam_script),
                            "alice", ZNAM_TYPE_TADDR, "t1simtarget");
    SN_CHECK("build real ZNAM REGISTER script", znam_script_len > 0);

    struct transaction znam_register;
    struct transaction znam_register_projection;
    transaction_init(&znam_register_projection);
    bool built_znam_register =
        znam_script_len > 0 &&
        sim_test_make_opreturn_spend(&znam_register, &znam_owner_fund_txid, 0,
                                     znam_script, znam_script_len,
                                     200000, 0xE0);
    bool copied_znam_register =
        built_znam_register &&
        transaction_copy(&znam_register_projection, &znam_register);
    SN_CHECK("build ZNAM REGISTER tx",
             built_znam_register && copied_znam_register);
    bool minted_znam_register =
        built_znam_register && copied_znam_register &&
        simnet_mint_txs(&sim, &znam_register, 1);
    int znam_register_height = simnet_tip_height(&sim);
    SN_CHECK("mint ZNAM REGISTER through simnet", minted_znam_register);
    bool indexed_znam_register =
        znam_open && minted_znam_register &&
        sim_test_index_block(&znam_db, &znam_register_projection, 1,
                             znam_register_height, 0xD2);
    SN_CHECK("fold ZNAM REGISTER into name projection", indexed_znam_register);

    struct znam_entry resolved;
    memset(&resolved, 0, sizeof(resolved));
    bool resolved_znam =
        znam_open && db_znam_find(&znam_db, "alice", &resolved);
    SN_CHECK("ZNAM projection resolves registered name",
             resolved_znam && strcmp(resolved.name, "alice") == 0 &&
             resolved.target_type == ZNAM_TYPE_TADDR &&
             strcmp(resolved.target_value, "t1simtarget") == 0 &&
             resolved.owner_address[0] != '\0' &&
             resolved.reg_height == znam_register_height);

    transaction_free(&znam_owner_fund_projection);
    transaction_free(&znam_register_projection);
    if (znam_open)
        node_db_close(&znam_db);

    /* 8. Negative path: spending an absent coin fails cleanly (no crash). */
    struct uint256 bogus;
    memset(bogus.data, 0xAB, 32);
    struct uint256 unused;
    SN_CHECK("spending an absent coin is rejected",
             !simnet_spend(&sim, &bogus, 0, 1, &unused));

    simnet_free(&sim);
    return failures;
}
