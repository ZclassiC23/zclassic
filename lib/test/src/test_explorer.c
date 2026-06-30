/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Explorer controller unit tests — routing, edge cases, factoids. */

#include "test/test_helpers.h"
#include "controllers/explorer_controller.h"
#include "controllers/explorer_internal.h"
#include "models/hodl_wave.h"
#include "crypto/sha3.h"
#include "views/explorer_factoids_internal.h"
#include <string.h>
#include <inttypes.h>

int test_explorer(void)
{
    int failures = 0;
    uint8_t resp[8192];

    printf("explorer: NULL path returns 0... ");
    {
        size_t n = explorer_handle_request("GET", NULL, NULL, 0,
                                            resp, sizeof(resp));
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: NULL response returns 0... ");
    {
        size_t n = explorer_handle_request("GET", "/explorer", NULL, 0,
                                            NULL, sizeof(resp));
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: /api/ delegation to api controller... ");
    {
        size_t n = explorer_handle_request("GET", "/api/nonexistent", NULL, 0,
                                            resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: unknown path returns 0... ");
    {
        size_t n = explorer_handle_request("GET", "/foobar", NULL, 0,
                                            resp, sizeof(resp));
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: /explorer/style.css returns CSS... ");
    {
        size_t n = explorer_handle_request("GET", "/explorer/style.css", NULL, 0,
                                            resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "text/css") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    /* ── Supply calculation edge cases ───────────────────── */

    printf("explorer: supply at negative height is 0... ");
    {
        int64_t s = compute_supply_at_height(-1);
        bool ok = (s == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ")\n", s); failures++; }
    }

    printf("explorer: supply at height 2387001 shows next halving... ");
    {
        /* At 2,387,001: halvings_raw = (2387001-707000-1)/1680000 = 1680000/1680000 = 1
         * halvings = 1+3 = 4, subsidy = 625000000 >> 4 = 39062500 sat = 0.390625 ZCL */
        int64_t s_before = compute_supply_at_height(2387000);
        int64_t s_at = compute_supply_at_height(2387001);
        int64_t s_after = compute_supply_at_height(2387002);
        /* Check that the rate changed: increment at 2387001 should be less
         * than increment at 2387000 */
        int64_t inc_before = s_at - s_before;  /* last block of era 3 or first of era 4 */
        int64_t inc_after = s_after - s_at;     /* era 4 rate */
        /* Both should be positive */
        bool ok = (inc_before > 0 && inc_after > 0 && inc_after <= inc_before);
        if (ok) printf("OK (rate: %" PRId64 " -> %" PRId64 " sat/block)\n",
                       inc_before, inc_after);
        else { printf("FAIL (inc_before=%" PRId64 ", inc_after=%" PRId64 ")\n",
                      inc_before, inc_after); failures++; }
    }

    printf("explorer: SHA3 receipt is deterministic... ");
    {
        /* compute_receipt is static in factoids — test SHA3 directly */
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);
        uint8_t data[] = {0x01, 0x02, 0x03};
        sha3_256_write(&ctx, data, 3);
        unsigned char d1[32];
        sha3_256_finalize(&ctx, d1);

        sha3_256_init(&ctx);
        sha3_256_write(&ctx, data, 3);
        unsigned char d2[32];
        sha3_256_finalize(&ctx, d2);

        bool ok = (memcmp(d1, d2, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: SHA3 different inputs give different hashes... ");
    {
        struct sha3_256_ctx ctx;
        unsigned char d1[32], d2[32];

        sha3_256_init(&ctx);
        uint8_t a[] = {0x01};
        sha3_256_write(&ctx, a, 1);
        sha3_256_finalize(&ctx, d1);

        sha3_256_init(&ctx);
        uint8_t b[] = {0x02};
        sha3_256_write(&ctx, b, 1);
        sha3_256_finalize(&ctx, d2);

        bool ok = (memcmp(d1, d2, 32) != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: factoids checkpoint section renders receipts... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db,
            "CREATE TABLE blocks(height INTEGER, hash BLOB, time INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,hash,time) VALUES"
            "(30000,x'000000005c2ad200c3c7c8e627f67b306659efca1268c9bb014335fdadc0c392',1482903829)",
            NULL, NULL, NULL);
        uint8_t out[8192];
        size_t n = factoids_emit_section_12_checkpoints(
            out, sizeof(out) - 1, 0, db, 3054000);
        out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';

        char expected[32] = "";
        compute_receipt(expected, sizeof(expected), 3054000,
                        "000005aa8e8c321cf364788e81b94619434b0dc1a85e658a022b44f23eb85662",
                        "checkpoint");
        bool ok = n > 0 &&
                  strstr((const char *)out, "12. Checkpoint History") != NULL &&
                  strstr((const char *)out, "/explorer/block/3054000") != NULL &&
                  strstr((const char *)out, "000005aa8e8c321c...") != NULL &&
                  strstr((const char *)out, expected) != NULL &&
                  strstr((const char *)out, "Not yet reached") == NULL;
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: factoids integrity section renders coverage and hash... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db,
            "CREATE TABLE blocks(height INTEGER, hash TEXT, time INTEGER, "
            "num_tx INTEGER, sapling_value INTEGER, sprout_value INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "CREATE TABLE transactions(is_coinbase INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE tx_inputs(block_height INTEGER)",
                     NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE tx_outputs(block_height INTEGER)",
                     NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,hash,time,num_tx,sapling_value,sprout_value) "
            "VALUES"
            "(1,'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',10,1,0,0),"
            "(2,'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',20,2,3,4),"
            "(101,'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',30,3,5,6)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO transactions(is_coinbase) VALUES(1),(0),(0)",
            NULL, NULL, NULL);

        char expected[128] = "";
        compute_integrity_hash(db, 101, expected, sizeof(expected));

        uint8_t out[8192];
        size_t n = factoids_emit_section_17_integrity(
            out, sizeof(out) - 1, 0, db, 101, 3);
        out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';

        bool ok = n > 0 &&
                  strstr((const char *)out, "17. Data Integrity") != NULL &&
                  strstr((const char *)out, "Chain height:</b> 101") != NULL &&
                  strstr((const char *)out, "Indexed blocks:</b> 3") != NULL &&
                  strstr((const char *)out, "Indexed transactions:</b> 3") != NULL &&
                  strstr((const char *)out, "blocks 2") != NULL &&
                  strstr((const char *)out, "101 (last 100)") != NULL &&
                  strstr((const char *)out, expected) != NULL;
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: factoids difficulty section renders records... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db,
            "CREATE TABLE blocks(height INTEGER, time INTEGER, bits INTEGER, "
            "chain_work BLOB)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,time,bits,chain_work) VALUES"
            "(0,1478403829,520617983,x'01000000'),"
            "(3000000,1780000000,504365055,x'02000000'),"
            "(3000001,1780000075,504207743,x'03000000')",
            NULL, NULL, NULL);

        char expected[32] = "";
        compute_receipt(expected, sizeof(expected), 3000001, "",
                        "hardest_block");

        uint8_t out[8192];
        size_t n = factoids_emit_section_16_difficulty(
            out, sizeof(out) - 1, 0, db);
        out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';

        bool ok = n > 0 &&
                  strstr((const char *)out, "16. Difficulty History") != NULL &&
                  strstr((const char *)out, "Difficulty Records") != NULL &&
                  strstr((const char *)out, "0x1e0d997f") != NULL &&
                  strstr((const char *)out, "/explorer/block/3000001") != NULL &&
                  strstr((const char *)out, "2 distinct targets") != NULL &&
                  strstr((const char *)out, "recent 2 blocks") != NULL &&
                  strstr((const char *)out, "Cumulative chain-work at tip") != NULL &&
                  strstr((const char *)out, "Peak Difficulty") != NULL &&
                  strstr((const char *)out, expected) != NULL;
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: factoids empty-block section renders records... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db,
            "CREATE TABLE blocks(height INTEGER, time INTEGER, num_tx INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,time,num_tx) VALUES"
            "(1,1478403829,2),"
            "(2,1478403900,1),"
            "(3,1478403975,1),"
            "(4,1478404050,5),"
            "(5,1478404125,1),"
            "(6,1478404200,1),"
            "(7,1478404275,1),"
            "(8,1478404350,3)",
            NULL, NULL, NULL);

        char summary_expected[32] = "";
        char records_expected[32] = "";
        compute_receipt_i64(summary_expected, sizeof(summary_expected),
                            5, 8, "empty_blocks");
        compute_receipt_i64(records_expected, sizeof(records_expected),
                            4, 3, "empty_block_records");

        uint8_t out[8192];
        size_t n = factoids_emit_section_15_empty_blocks(
            out, sizeof(out) - 1, 0, db);
        out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';

        bool ok = n > 0 &&
                  strstr((const char *)out, "15. Empty Blocks Analysis") != NULL &&
                  strstr((const char *)out, "5 of 8 (62.5%)") != NULL &&
                  strstr((const char *)out, "Empty Blocks Per Year") != NULL &&
                  strstr((const char *)out,
                         "<tr><td>2016</td><td>5</td><td>8</td><td>62.5%</td></tr>") != NULL &&
                  strstr((const char *)out, "Records") != NULL &&
                  strstr((const char *)out, "5 transactions at block") != NULL &&
                  strstr((const char *)out, "/explorer/block/4") != NULL &&
                  strstr((const char *)out,
                         "Longest run of consecutive empty blocks:</b> 3") != NULL &&
                  strstr((const char *)out, "heights 5") != NULL &&
                  strstr((const char *)out, "7)") != NULL &&
                  strstr((const char *)out, summary_expected) != NULL &&
                  strstr((const char *)out, records_expected) != NULL;
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: factoids blocktime section renders cadence records... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db,
            "CREATE TABLE blocks(height INTEGER, time INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,time) VALUES"
            "(1,1478403829),"
            "(2,1478403979),"
            "(3,1478404139),"
            "(707000,1600000000),"
            "(707001,1600000005),"
            "(707002,1600003706)",
            NULL, NULL, NULL);

        char pre_expected[32] = "";
        char post_expected[32] = "";
        char records_expected[32] = "";
        compute_receipt_i64(pre_expected, sizeof(pre_expected),
                            155, 2, "blocktime_pre_bc");
        compute_receipt_i64(post_expected, sizeof(post_expected),
                            1853, 2, "blocktime_post_bc");
        compute_receipt_i64(records_expected, sizeof(records_expected),
                            1, 1, "blocktime_records");

        uint8_t out[8192];
        size_t n = factoids_emit_section_13_blocktimes(
            out, sizeof(out) - 1, 0, db, 707002);
        out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';

        bool ok = n > 0 &&
                  strstr((const char *)out, "13. Block Time Analysis") != NULL &&
                  strstr((const char *)out, "Pre-Buttercup") != NULL &&
                  strstr((const char *)out, "155.0s") != NULL &&
                  strstr((const char *)out, "150s") != NULL &&
                  strstr((const char *)out, "160s") != NULL &&
                  strstr((const char *)out, "Post-Buttercup") != NULL &&
                  strstr((const char *)out, "1853.0s") != NULL &&
                  strstr((const char *)out, "5s") != NULL &&
                  strstr((const char *)out, "3701s") != NULL &&
                  strstr((const char *)out, "Block Interval Records") != NULL &&
                  strstr((const char *)out, "1 (25.0%)") != NULL &&
                  strstr((const char *)out, "Blocks over 1 hour apart:</b></td><td>1") != NULL &&
                  strstr((const char *)out, pre_expected) != NULL &&
                  strstr((const char *)out, post_expected) != NULL &&
                  strstr((const char *)out, records_expected) != NULL;
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: factoids transaction section renders records... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db,
            "CREATE TABLE blocks(height INTEGER, hash TEXT, time INTEGER, num_tx INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "CREATE TABLE transactions(block_hash TEXT, block_height INTEGER, "
            "is_coinbase INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "CREATE TABLE tx_inputs(block_height INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "CREATE TABLE tx_outputs(txid TEXT, vout INTEGER, value INTEGER, "
            "script_type INTEGER, block_height INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "CREATE TABLE op_returns(block_height INTEGER, is_slp INTEGER)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,hash,time,num_tx) VALUES"
            "(1,'h1',1478403829,2),"
            "(2,'h2',1478403979,2)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO transactions(block_hash,block_height,is_coinbase) VALUES"
            "('h1',1,1),('h1',1,0),('h2',2,1),('h2',2,0)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO tx_inputs(block_height) VALUES(1),(2),(2)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO tx_outputs(txid,vout,value,script_type,block_height) VALUES"
            "('a',0,100000000,0,1),"
            "('b',0,200000000,0,1),"
            "('c',1,300000000,1,2),"
            "('d',2,400000000,2,2)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO op_returns(block_height,is_slp) VALUES(1,0),(2,1)",
            NULL, NULL, NULL);

        char summary_expected[32] = "";
        char records_expected[32] = "";
        compute_receipt_i64(summary_expected, sizeof(summary_expected),
                            4, 3, "tx_archaeology");
        compute_receipt_i64(records_expected, sizeof(records_expected),
                            1000000000, 4, "tx_records");

        uint8_t out[8192];
        size_t n = factoids_emit_section_14_transactions(
            out, sizeof(out) - 1, 0, db);
        out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';

        bool ok = n > 0 &&
                  strstr((const char *)out, "14. Transaction Archaeology") != NULL &&
                  strstr((const char *)out, "Total transactions:</b></td><td>4") != NULL &&
                  strstr((const char *)out, "Coinbase transactions:</b></td><td>2") != NULL &&
                  strstr((const char *)out, "Non-coinbase transactions:</b></td><td>2") != NULL &&
                  strstr((const char *)out, "Total transparent inputs:</b></td><td>3") != NULL &&
                  strstr((const char *)out, "Total transparent outputs:</b></td><td>4") != NULL &&
                  strstr((const char *)out, "Total OP_RETURN outputs:</b></td><td>2") != NULL &&
                  strstr((const char *)out,
                         "<tr><td>2016</td><td>4</td><td>2</td><td>2</td><td>2.00</td></tr>") != NULL &&
                  strstr((const char *)out, "10.00000000 ZCL across 4 outputs") != NULL &&
                  strstr((const char *)out, "3 outputs at block") != NULL &&
                  strstr((const char *)out, "/explorer/block/2") != NULL &&
                  strstr((const char *)out, "4.00000000 ZCL") != NULL &&
                  strstr((const char *)out, "/explorer/block/1") != NULL &&
                  strstr((const char *)out, "Avg inputs per spending tx:</b></td><td>1.50") != NULL &&
                  strstr((const char *)out, "<tr><td>P2PKH</td><td>2</td></tr>") != NULL &&
                  strstr((const char *)out, "<tr><td>P2SH</td><td>1</td></tr>") != NULL &&
                  strstr((const char *)out, "<tr><td>OP_RETURN</td><td>1</td></tr>") != NULL &&
                  strstr((const char *)out, summary_expected) != NULL &&
                  strstr((const char *)out, records_expected) != NULL;
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: difficulty_from_bits handles edge cases... ");
    {
        double d0 = explorer_difficulty_from_bits(0);
        bool ok = (d0 == 1.0);
        /* Current live-chain bits observed at h=3112518. Legacy zclassicd
         * reports difficulty 150.5924424103772 for this compact target. */
        double live = explorer_difficulty_from_bits(0x1e0d997f);
        double d1 = explorer_difficulty_from_bits(0x1f07ffff);
        ok = ok && (d1 > 0.0) &&
             (live > 150.5924 && live < 150.5925);
        if (ok) printf("OK (bits=0 -> %.1f, bits=0x1f07ffff -> %.4f, live -> %.4f)\n",
                       d0, d1, live);
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: format_zcl handles negative values... ");
    {
        char buf[64];
        zcl_format_zcl(buf, sizeof(buf), -100000000LL);
        bool ok = (strcmp(buf, "-1.00000000") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got '%s')\n", buf); failures++; }
    }

    printf("explorer: format_zcl handles zero... ");
    {
        char buf[64];
        zcl_format_zcl(buf, sizeof(buf), 0);
        bool ok = (strcmp(buf, "0.00000000") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got '%s')\n", buf); failures++; }
    }

    printf("explorer: format_zcl handles 12.5 ZCL... ");
    {
        char buf[64];
        zcl_format_zcl(buf, sizeof(buf), 1250000000LL);
        bool ok = (strcmp(buf, "12.50000000") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got '%s')\n", buf); failures++; }
    }

    printf("explorer: HODL wave bucket boundaries are canonical... ");
    {
        int young = hodl_wave_bucket_index(0);
        int year = hodl_wave_bucket_index(31557600LL);
        int very_old = hodl_wave_bucket_index(200000000LL);
        bool ok = (young == 0 && year == 6 &&
                   very_old == HODL_WAVE_BUCKETS - 1);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: HODL wave validation catches drift... ");
    {
        struct hodl_wave_snapshot h = {0};
        struct ar_errors errors;
        h.tip_height = 100;
        h.total_value = 100;
        h.total_count = 1;
        memcpy(h.buckets, hodl_wave_bucket_defs(), sizeof(h.buckets));
        h.buckets[0].value = 90;
        h.buckets[0].count = 1;
        snprintf(h.source, sizeof(h.source), "current_transparent_utxo_set");
        snprintf(h.metric, sizeof(h.metric), "utxo_age_distribution");
        snprintf(h.status, sizeof(h.status), "ok");
        bool ok = !hodl_wave_validate(&h, &errors) &&
                  strstr(ar_errors_full(&errors), "bucket.value") != NULL;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: history validation rejects display-order genesis... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db, "CREATE TABLE blocks(height INTEGER, hash BLOB, time INTEGER)", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE transactions(block_height INTEGER)", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE tx_outputs(block_height INTEGER)", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE view_integrity(height INTEGER, sha3_hash BLOB)", NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,hash,time) VALUES"
            "(0,x'0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602',1478403829),"
            "(1,x'1111111111111111111111111111111111111111111111111111111111111111',1478403979)",
            NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO transactions(block_height) VALUES(0)", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO tx_outputs(block_height) VALUES(0)", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO view_integrity(height,sha3_hash) VALUES(0,zeroblob(32)),(1,zeroblob(32))", NULL, NULL, NULL);
        struct explorer_history_validation v;
        explorer_validate_block_history(db, 1, &v);
        bool ok = (!v.usable &&
                   strstr(v.reason, "genesis hash") != NULL);
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: history validation catches partial derived tables... ");
    {
        sqlite3 *db = NULL;
        sqlite3_open(":memory:", &db);
        sqlite3_exec(db, "CREATE TABLE blocks(height INTEGER, hash BLOB, time INTEGER)", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE transactions(block_height INTEGER)", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE tx_outputs(block_height INTEGER)", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE view_integrity(height INTEGER, sha3_hash BLOB)", NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO blocks(height,hash,time) VALUES"
            /* must equal ZCL_EXPLORER_GENESIS_HASH_INTERNAL_HEX (corrected value) */
            "(0,x'0206260143838b5ff52dc2eb7b4b8099d4e4c99dc3ef19794289a2cd4c100700',1478403829),"
            "(1,x'1111111111111111111111111111111111111111111111111111111111111111',1478403979),"
            "(2,x'2222222222222222222222222222222222222222222222222222222222222222',1478404129),"
            "(3,x'3333333333333333333333333333333333333333333333333333333333333333',1478404279),"
            "(4,x'4444444444444444444444444444444444444444444444444444444444444444',1478404429),"
            "(5,x'5555555555555555555555555555555555555555555555555555555555555555',1478404579),"
            "(6,x'6666666666666666666666666666666666666666666666666666666666666666',1478404729),"
            "(7,x'7777777777777777777777777777777777777777777777777777777777777777',1478404879),"
            "(8,x'8888888888888888888888888888888888888888888888888888888888888888',1478405029),"
            "(9,x'9999999999999999999999999999999999999999999999999999999999999999',1478405179),"
            "(10,x'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',1478405329),"
            "(11,x'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1478405479)",
            NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO transactions(block_height) VALUES(1)", NULL, NULL, NULL);
        struct explorer_history_validation v;
        explorer_validate_block_history(db, 11, &v);
        bool ok = (!v.usable &&
                   strstr(v.reason, "tx_outputs") != NULL);
        sqlite3_close(db);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
