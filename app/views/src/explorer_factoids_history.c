/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer factoids VIEW — chain-structure sections (1-7).
 *
 * Owns the "origin, emission, and ownership" half of the historian page:
 * Genesis Story,
 * Network Upgrade History, Mining Era Analysis, Network Milestones,
 * All-Time Records, Supply Milestones, and Address Statistics. The
 * activity + archaeology + integrity sections (8-17) live in
 * explorer_factoids_chaindata.c; the public entry points and JSON API
 * live in explorer_factoids_view.c. Shared SHA3/format/block helpers
 * come from views/explorer_factoids_internal.h. */

#include "views/explorer_factoids_internal.h"

size_t factoids_emit_section_1_genesis(uint8_t *buf, size_t cap, size_t off,
                                       sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='genesis'>1. Genesis Story</h2>");

    int64_t genesis_time = ZCL_EXPLORER_GENESIS_TIME;

    /* Genesis (height 0) is not in the SQLite index — use known constants */
    char genesis_hash[128] =
        ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX;
    char genesis_coinbase[128] = "";
    /* Try DB first, fall back to constant */
    fq_text(db, "SELECT hex(hash) FROM blocks WHERE height = 0",
            genesis_hash, sizeof(genesis_hash));
    if (!genesis_hash[0])
        snprintf(genesis_hash, sizeof(genesis_hash),
            ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX);
    fq_text(db, "SELECT hex(txid) FROM transactions WHERE block_height = 0 AND is_coinbase = 1 LIMIT 1",
            genesis_coinbase, sizeof(genesis_coinbase));
    if (!genesis_coinbase[0])
        snprintf(genesis_coinbase, sizeof(genesis_coinbase),
            "427DBF0AE8E079C6527EA1CB308C6E3C98FA5435F4D715D31176EA00CF2B6119");

    char tstr[64];
    fmt_time(tstr, sizeof(tstr), genesis_time);

    char gen_receipt[32] = "";
    compute_receipt(gen_receipt, sizeof(gen_receipt), 0, genesis_hash, "Genesis");

    APPEND(off, r, max,
        "<div class='card'>"
        "<h3>Block 0: The Beginning</h3>"
        "<p style='color:#888'>ZClassic launched on November 6, 2016 as a fork of Zcash "
        "with no founder's reward tax and the same Equihash (200,9) proof-of-work. "
        "Community-driven from day one.</p>"
        "<table>"
        "<tr><td><b>Genesis Hash</b></td><td><code style='word-break:break-all'>"
        "<a href='/explorer/block/0'>%.64s</a></code></td></tr>"
        "<tr><td><b>Timestamp</b></td><td>%s (Unix: 1478403829)</td></tr>"
        "<tr><td><b>Genesis Coinbase</b></td><td><code style='word-break:break-all'>"
        "<a href='/explorer/tx/%.64s'>%.16s...</a></code></td></tr>"
        "<tr><td><b>Message Start</b></td><td><code>0x24 0xe9 0x27 0x64</code></td></tr>"
        "<tr><td><b>Default Port</b></td><td>8033 (mainnet), 18033 (testnet)</td></tr>"
        "<tr><td><b>BIP44 Coin Type</b></td><td>147</td></tr>"
        "<tr><td><b>Address Prefix (t1)</b></td><td><code>0x1C 0xB8</code></td></tr>"
        "<tr><td><b>Equihash Params</b></td><td>N=200, K=9 (memory-hard, ASIC-resistant)</td></tr>"
        "<tr><td><b>PoW Limit</b></td><td><code>0x0007ffff...fff</code></td></tr>"
        "<tr><td><b>SHA3 Receipt</b></td><td><code>%s</code></td></tr>"
        "</table></div>",
        genesis_hash, tstr, genesis_coinbase, genesis_coinbase, gen_receipt);

    /* First 10 blocks table */
    APPEND(off, r, max,
        "<h3>First 10 Blocks</h3>"
        "<table class='txlist'>"
        "<tr><th>Height</th><th>Time</th><th>Block Hash</th><th>SHA3 Receipt</th></tr>");

    /* Genesis (height 0) is not in SQLite — add manually */
    {
        char rcpt0[32] = "";
        compute_receipt(rcpt0, sizeof(rcpt0), 0, genesis_hash, "first10");
        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/0'>0</a></td>"
            "<td>2016-11-06 03:43:49 UTC</td>"
            "<td><code style='word-break:break-all'>%.16s...</code></td>"
            "<td><code>%s</code></td></tr>",
            genesis_hash, rcpt0);
    }
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT height, time, hex(hash) FROM blocks WHERE height >= 1 AND height < 10 ORDER BY height";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t t = sqlite3_column_int64(s, 1);
                const char *hash = (const char *)sqlite3_column_text(s, 2);
                char ts[64], rcpt[32] = "";
                fmt_time(ts, sizeof(ts), t);
                compute_receipt(rcpt, sizeof(rcpt), h, hash ? hash : "", "first10");
                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td>"
                    "<td><code style='word-break:break-all'>%.16s...</code></td>"
                    "<td><code>%s</code></td></tr>",
                    h, h, ts, hash ? hash : "?", rcpt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_2_upgrades(uint8_t *buf, size_t cap, size_t off,
                                        sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='upgrades'>2. Network Upgrade History</h2>"
        "<p style='color:#888'>Every consensus upgrade with activation height, "
        "protocol version, branch ID, and purpose. Each secured with SHA3 receipt.</p>"
        "<table class='txlist'>"
        "<tr><th>Upgrade</th><th>Height</th><th>Date</th>"
        "<th>Proto</th><th>Branch ID</th><th>Purpose</th><th>SHA3</th></tr>");

    struct {
        const char *name;
        int64_t height;
        int proto;
        const char *branch_id;
        const char *purpose;
    } upgrades[] = {
        { "Sprout (Base)", 0, 170002, "0x00000000",
          "Initial ZClassic network launch" },
        { "Overwinter", OVERWINTER_ACTIVATION_HEIGHT, 170005, "0x5ba81b19",
          "Transaction format v3, replay protection, expiry" },
        { "Sapling", SAPLING_ACTIVATION_HEIGHT, 170007, "0x76b809bb",
          "Shielded transactions (Groth16 proofs, 100x faster)" },
        { "Bubbles", BUBBLES_ACTIVATION_HEIGHT, 170009, "0x821a451c",
          "ZClassic-specific protocol enhancements" },
        /* Bubbly intentionally shares branch ID 0x930b540d with Buttercup
         * in consensus (lib/consensus/src/upgrades.c:16-17) — Buttercup
         * did not change transaction-binding format, so it reuses Bubbly's
         * branch ID. The duplicate is correct. */
        { "DiffAdj (Bubbly)", BUBBLY_ACTIVATION_HEIGHT, 170010, "0x930b540d",
          "Difficulty adjustment algorithm refinement" },
        { "Buttercup", BUTTERCUP_ACTIVATION_HEIGHT, 170011, "0x930b540d",
          "Block time 150s\xe2\x86\x92" "75s, halving doubled, subsidy adjusted" },
    };
    int n_upgrades = (int)(sizeof(upgrades) / sizeof(upgrades[0]));

    for (int i = 0; i < n_upgrades; i++) {
        char bhash[128] = "";
        int64_t btime = 0;
        get_block_at(db, upgrades[i].height, bhash, sizeof(bhash), &btime);

        char ts[64], rcpt[32] = "";
        fmt_time(ts, sizeof(ts), btime);
        compute_receipt(rcpt, sizeof(rcpt), upgrades[i].height, bhash, upgrades[i].name);

        if (btime > 0 || upgrades[i].height == 0) {
            APPEND(off, r, max,
                "<tr><td><b>%s</b></td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s</td><td>%d</td><td><code>%s</code></td>"
                "<td>%s</td><td><code>%s</code></td></tr>",
                upgrades[i].name, upgrades[i].height, upgrades[i].height,
                ts, upgrades[i].proto, upgrades[i].branch_id,
                upgrades[i].purpose, rcpt);
        } else {
            APPEND(off, r, max,
                "<tr><td><b>%s</b></td>"
                "<td>%" PRId64 "</td>"
                "<td style='color:#666'>Not yet reached</td>"
                "<td>%d</td><td><code>%s</code></td>"
                "<td>%s</td><td><code>--</code></td></tr>",
                upgrades[i].name, upgrades[i].height,
                upgrades[i].proto, upgrades[i].branch_id,
                upgrades[i].purpose);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_3_mining_eras(uint8_t *buf, size_t cap, size_t off,
                                           int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='mining-eras'>3. Mining Era Analysis</h2>"
        "<p style='color:#888'>Block reward schedule showing the Buttercup transition "
        "at block 707,000 which halved block time and shifted the halving schedule.</p>"
        "<table class='txlist'>"
        "<tr><th>Era</th><th>Block Range</th><th>Subsidy/Block</th>"
        "<th>Target Spacing</th><th>Blocks</th><th>Total Emission</th><th>SHA3</th></tr>");

    /* Pre-Buttercup eras */
    {
        int64_t era_start = 0;
        int64_t pre_bc_end = chain_height < BUTTERCUP_ACTIVATION_HEIGHT
                           ? chain_height : BUTTERCUP_ACTIVATION_HEIGHT;
        int era_num = 0;
        int64_t subsidy = BASE_SUBSIDY_SAT;

        while (era_start < pre_bc_end && subsidy > 0 && era_num < 10) {
            int64_t next = ((era_start / PRE_BC_HALVING) + 1) * PRE_BC_HALVING;
            int64_t end = next < pre_bc_end ? next : pre_bc_end;
            int64_t count = end - era_start;

            char sub_str[64], emit_str[64], rcpt[32] = "";
            fmt_zcl(sub_str, sizeof(sub_str), subsidy);
            int64_t emission = count * subsidy;
            fmt_zcl(emit_str, sizeof(emit_str), emission);
            char blk_str[32];
            fmt_comma(blk_str, sizeof(blk_str), count);
            compute_receipt_i64(rcpt, sizeof(rcpt), era_start, end, "mining_era");

            APPEND(off, r, max,
                "<tr><td>Pre-BC #%d</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a>"
                " \xe2\x80\x93 <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s ZCL</td><td>150s</td><td>%s</td><td>%s ZCL</td>"
                "<td><code>%s</code></td></tr>",
                era_num, era_start, era_start, end - 1, end - 1,
                sub_str, blk_str, emit_str, rcpt);

            era_start = end;
            era_num++;
            int halvings = (int)(era_start / PRE_BC_HALVING);
            subsidy = (halvings >= 64) ? 0 : (BASE_SUBSIDY_SAT >> halvings);
        }
    }

    /* Post-Buttercup eras */
    if (chain_height > BUTTERCUP_ACTIVATION_HEIGHT) {
        int64_t era_offset = 0;
        int64_t remaining = chain_height - BUTTERCUP_ACTIVATION_HEIGHT;
        int64_t bc_base = BASE_SUBSIDY_SAT / 2;  /* 6.25 ZCL */
        int era_num = 0;

        while (remaining > 0 && era_num < 10) {
            int halvings_raw = (era_offset > 0)
                ? (int)((era_offset - 1) / POST_BC_HALVING) : 0;
            int halvings = halvings_raw + 3;
            if (halvings >= 64) break;
            int64_t era_subsidy = bc_base >> halvings;
            if (era_subsidy <= 0) break;

            int64_t next_boundary = ((int64_t)(halvings_raw + 1)) * POST_BC_HALVING + 1;
            int64_t blocks_in_era = next_boundary - era_offset;
            if (blocks_in_era > remaining) blocks_in_era = remaining;

            int64_t abs_start = BUTTERCUP_ACTIVATION_HEIGHT + era_offset;
            int64_t abs_end = abs_start + blocks_in_era;

            char sub_str[64], emit_str[64], rcpt[32] = "";
            fmt_zcl(sub_str, sizeof(sub_str), era_subsidy);
            int64_t emission = blocks_in_era * era_subsidy;
            fmt_zcl(emit_str, sizeof(emit_str), emission);
            char blk_str[32];
            fmt_comma(blk_str, sizeof(blk_str), blocks_in_era);
            compute_receipt_i64(rcpt, sizeof(rcpt), abs_start, abs_end, "mining_era_bc");

            APPEND(off, r, max,
                "<tr><td>Post-BC #%d (halv=%d)</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a>"
                " \xe2\x80\x93 <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s ZCL</td><td>75s</td><td>%s</td><td>%s ZCL</td>"
                "<td><code>%s</code></td></tr>",
                era_num, halvings, abs_start, abs_start, abs_end - 1, abs_end - 1,
                sub_str, blk_str, emit_str, rcpt);

            remaining -= blocks_in_era;
            era_offset += blocks_in_era;
            era_num++;
        }
    }

    /* Total supply row */
    {
        int64_t total_supply = compute_supply_at_height(chain_height);
        char total_str[64], rcpt[32] = "";
        fmt_zcl(total_str, sizeof(total_str), total_supply);
        compute_receipt_i64(rcpt, sizeof(rcpt), chain_height, total_supply, "total_supply");
        APPEND(off, r, max,
            "<tr style='border-top:2px solid #33ff99'>"
            "<td colspan='4'><b>Total Mined Supply</b></td>"
            "<td></td><td><b>%s ZCL</b></td>"
            "<td><code>%s</code></td></tr>",
            total_str, rcpt);
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_4_milestones(uint8_t *buf, size_t cap, size_t off,
                                          sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='milestones'>4. Network Milestones</h2>"
        "<p style='color:#888'>Key firsts in the chain's history.</p>"
        "<table class='txlist'>"
        "<tr><th>Milestone</th><th>Block</th><th>Time</th><th>SHA3 Receipt</th></tr>");

    struct milestone {
        const char *name;
        const char *sql;
        int64_t fixed_height;
        int64_t known_height;  /* authoritative fallback when SQL table is empty */
    };

    /* known_height values are immutable blockchain facts verified against
     * zclassicd getblock RPC on 2026-03-25.  They serve as fallbacks when
     * Phase B indexing tables (joinsplits, sapling_spends, sapling_outputs,
     * op_returns) haven't been populated yet. */
    struct milestone milestones[] = {
        { "Genesis (Nov 6, 2016)", NULL, 0, -1 },
        { "First non-coinbase tx",
          "SELECT MIN(block_height) FROM transactions WHERE is_coinbase = 0", -1, -1 },
        { "First Sprout JoinSplit",
          "SELECT MIN(block_height) FROM joinsplits", -1, 241 },
        { "Overwinter + Sapling activation", NULL,
          OVERWINTER_ACTIVATION_HEIGHT, -1 },
        { "First Sapling shielded spend",
          "SELECT MIN(block_height) FROM sapling_spends", -1, 477214 },
        { "First Sapling shielded output",
          "SELECT MIN(block_height) FROM sapling_outputs", -1, 477214 },
        { "First OP_RETURN",
          "SELECT MIN(block_height) FROM op_returns", -1, 649950 },
        { "Bubbles upgrade activation", NULL,
          BUBBLES_ACTIVATION_HEIGHT, -1 },
        { "DiffAdj (Bubbly) activation", NULL,
          BUBBLY_ACTIVATION_HEIGHT, -1 },
        { "First ZSLP token genesis",
          "SELECT MIN(genesis_height) FROM zslp_tokens", -1, -1 },
        { "Buttercup activation (75s blocks)", NULL,
          BUTTERCUP_ACTIVATION_HEIGHT, -1 },
        /* The pre-Buttercup halving at height 840000 never fired —
         * Buttercup activated at 707000 first and rolled the schedule
         * into the post-BC era. The previous "First halving (pre-
         * Buttercup)" milestone row was removed because it would
         * permanently render "Not yet reached." */
        { "Block 1,000,000", NULL, 1000000, -1 },
        { "Block 2,000,000", NULL, 2000000, -1 },
        { "Block 3,000,000", NULL, 3000000, -1 },
    };

    int n_milestones = (int)(sizeof(milestones) / sizeof(milestones[0]));
    for (int i = 0; i < n_milestones; i++) {
        int64_t height = milestones[i].fixed_height;
        if (milestones[i].sql)
            height = fq_i64(db, milestones[i].sql);

        /* If SQL returned nothing (table empty), fall back to known height */
        if (height <= 0 && milestones[i].known_height > 0)
            height = milestones[i].known_height;

        if (height <= 0 && i > 0) {
            /* Distinguish "not yet reached" from "index not yet built" —
             * if chain is past Sapling activation and a table is empty,
             * the data exists on-chain but hasn't been indexed yet. */
            bool index_pending = (milestones[i].sql != NULL &&
                                  chain_height > 500000);
            APPEND(off, r, max,
                "<tr><td>%s</td><td colspan='3' style='color:#666'>%s</td></tr>",
                milestones[i].name,
                index_pending ? "Pending index rebuild" : "Not yet reached");
            continue;
        }
        if (height > chain_height && milestones[i].fixed_height >= 0) {
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td>%" PRId64 "</td>"
                "<td style='color:#666'>Not yet reached</td>"
                "<td><code>--</code></td></tr>",
                milestones[i].name, height);
            continue;
        }

        char bhash[128] = "";
        int64_t btime = 0;
        get_block_at(db, height, bhash, sizeof(bhash), &btime);

        char receipt[32] = "", ts[64];
        compute_receipt(receipt, sizeof(receipt), height, bhash, milestones[i].name);
        fmt_time(ts, sizeof(ts), btime);

        APPEND(off, r, max,
            "<tr><td>%s</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>%s</td>"
            "<td><code>%s</code></td></tr>",
            milestones[i].name, height, height, ts, receipt);
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_5_records(uint8_t *buf, size_t cap, size_t off,
                                       sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max, "<h2 id='records'>5. All-Time Records</h2>"
        "<table class='txlist'>"
        "<tr><th>Record</th><th>Value</th><th>Block</th><th>Time</th><th>SHA3</th></tr>");

    /* Record helper macro */
    #define RECORD_ROW(label, val_fmt, val_args, height_val, time_val) do { \
        char _ts[64], _rcpt[32] = ""; \
        fmt_time(_ts, sizeof(_ts), time_val); \
        compute_receipt(_rcpt, sizeof(_rcpt), height_val, "", label); \
        APPEND(off, r, max, \
            "<tr><td>" label "</td><td>" val_fmt "</td>" \
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>" \
            "<td>%s</td><td><code>%s</code></td></tr>", \
            val_args, (int64_t)(height_val), (int64_t)(height_val), _ts, _rcpt); \
    } while(0)

    /* Largest transparent output */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT u.value, u.height, b.time FROM utxos u "
                          "JOIN blocks b ON u.height = b.height "
                          "ORDER BY u.value DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), row.v0);
            RECORD_ROW("Largest transparent output",
                "%s ZCL", vstr, row.v1, row.v2);
        }
    }

    /* Most transactions in a single block */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT b.height, b.num_tx, b.time FROM blocks b "
                          "ORDER BY b.num_tx DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            RECORD_ROW("Most transactions in a block",
                "%" PRId64 " tx", row.v1, row.v0, row.v2);
        }
    }

    /* Most JoinSplits in a single block */
    {
        struct sql_row_i64_2 row;
        const char *sql = "SELECT block_height, count(*) as cnt FROM joinsplits "
                          "GROUP BY block_height ORDER BY cnt DESC LIMIT 1";
        if (sql_query_row_i64_2(db, sql, &row)) {
            int64_t t = get_block_time(db, row.v0);
            RECORD_ROW("Most JoinSplits in a block",
                "%" PRId64, row.v1, row.v0, t);
        }
    }

    /* Most Sapling outputs in a single block */
    {
        struct sql_row_i64_2 row;
        const char *sql = "SELECT block_height, count(*) as cnt FROM sapling_outputs "
                          "GROUP BY block_height ORDER BY cnt DESC LIMIT 1";
        if (sql_query_row_i64_2(db, sql, &row)) {
            int64_t t = get_block_time(db, row.v0);
            RECORD_ROW("Most Sapling outputs in a block",
                "%" PRId64, row.v1, row.v0, t);
        }
    }

    /* Highest difficulty ever */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT height, bits, time FROM blocks "
                          "WHERE bits > 0 ORDER BY bits ASC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            double diff = explorer_difficulty_from_bits((uint32_t)row.v1);
            char dstr[64];
            snprintf(dstr, sizeof(dstr), "%.2f", diff);
            RECORD_ROW("Highest difficulty",
                "%s", dstr, row.v0, row.v2);
        }
    }

    /* Longest gap between blocks */
    {
        struct sql_row_i64_3 row;
        const char *sql =
            "SELECT a.height, a.time, (b.time - a.time) as gap "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 "
            "ORDER BY gap DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char gstr[64];
            snprintf(gstr, sizeof(gstr), "%" PRId64 "m %" PRId64 "s",
                     row.v2 / 60, row.v2 % 60);
            RECORD_ROW("Longest block gap",
                "%s", gstr, row.v0, row.v1);
        }
    }

    /* Shortest gap between blocks */
    {
        struct sql_row_i64_3 row;
        const char *sql =
            "SELECT a.height, a.time, (b.time - a.time) as gap "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 AND (b.time - a.time) > 0 "
            "ORDER BY gap ASC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char gstr[64];
            snprintf(gstr, sizeof(gstr), "%" PRId64 "s", row.v2);
            RECORD_ROW("Shortest block gap",
                "%s", gstr, row.v0, row.v1);
        }
    }

    /* blocks.sapling_value uses zclassicd's valueDelta convention:
     * positive = t→z shielding (pool grew), negative = z→t unshielding
     * (pool shrank). Verified against zclassicd getblock for blocks
     * 1275039 (delta=-29,963 ZCL, unshielding) and 2670693
     * (delta=+40,380 ZCL, shielding). */

    /* Largest single-block shielding (t→z): most positive sapling_value */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT height, sapling_value, time FROM blocks "
                          "WHERE sapling_value > 0 ORDER BY sapling_value DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), row.v1);
            RECORD_ROW("Largest single-block shielding (t\xe2\x86\x92z)",
                "%s ZCL", vstr, row.v0, row.v2);
        }
    }

    /* Largest single-block unshielding (z→t): most negative sapling_value */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT height, sapling_value, time FROM blocks "
                          "WHERE sapling_value < 0 ORDER BY sapling_value ASC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), -row.v1);
            RECORD_ROW("Largest single-block unshielding (z\xe2\x86\x92t)",
                "%s ZCL", vstr, row.v0, row.v2);
        }
    }

    #undef RECORD_ROW
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_6_supply(uint8_t *buf, size_t cap, size_t off,
                                      sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='supply'>6. Supply Milestones</h2>"
        "<p style='color:#888'>Buttercup-aware calculation: pre-707000 at 12.5 ZCL/block, "
        "post-707000 at 0.78125 ZCL/block (6.25 &gt;&gt; 3), with 1.68M-block halvings.</p>"
        "<table class='txlist'>"
        "<tr><th>Milestone</th><th>Block</th><th>Date</th><th>SHA3</th></tr>");

    /* Find block heights where supply reaches milestones via binary search */
    struct { const char *label; int64_t target_sat; } supply_milestones[] = {
        { "1,000,000 ZCL mined",  ZATOSHI_PER_ZCL * 1000000LL },
        { "5,000,000 ZCL mined",  ZATOSHI_PER_ZCL * 5000000LL },
        /* Pre-Buttercup max: last pre-BC block is BUTTERCUP_ACTIVATION_HEIGHT-1
         * (= 706999), genesis (h=0) has zero subsidy in zcl_total_supply, so
         * the pre-BC supply is 706999 * 12.5 = 8,837,487.5 ZCL. */
        { "8,837,487.5 ZCL (pre-Buttercup max)",
          (BUTTERCUP_ACTIVATION_HEIGHT - 1) * BASE_SUBSIDY_SAT },
        { "10,000,000 ZCL mined", ZATOSHI_PER_ZCL * 10000000LL },
        { "10,500,000 ZCL mined", ZATOSHI_PER_ZCL * 10500000LL },
    };

    for (int i = 0; i < (int)(sizeof(supply_milestones)/sizeof(supply_milestones[0])); i++) {
        int64_t target = supply_milestones[i].target_sat;

        /* Binary search for the height where supply >= target */
        int64_t lo = 0, hi = 100000000LL; /* 100M blocks max */
        if (hi > chain_height + 50000000LL) hi = chain_height + 50000000LL;
        int64_t milestone_height = -1;

        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            int64_t supply = compute_supply_at_height(mid);
            if (supply >= target) {
                milestone_height = mid;
                hi = mid - 1;
            } else {
                lo = mid + 1;
            }
        }

        if (milestone_height < 0 || milestone_height > 100000000LL) {
            APPEND(off, r, max,
                "<tr><td>%s</td><td colspan='3' style='color:#666'>Beyond max supply</td></tr>",
                supply_milestones[i].label);
            continue;
        }

        char bhash[128] = "";
        int64_t btime = 0;
        if (milestone_height <= chain_height) {
            get_block_at(db, milestone_height, bhash, sizeof(bhash), &btime);
        }

        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), milestone_height, target, "supply_milestone");

        if (btime > 0) {
            char ts[64];
            fmt_time(ts, sizeof(ts), btime);
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s</td><td><code>%s</code></td></tr>",
                supply_milestones[i].label, milestone_height, milestone_height, ts, rcpt);
        } else {
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td>%" PRId64 "</td>"
                "<td style='color:#666'>Not yet reached</td>"
                "<td><code>%s</code></td></tr>",
                supply_milestones[i].label, milestone_height, rcpt);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_7_addresses(uint8_t *buf, size_t cap, size_t off,
                                         sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='addresses'>7. Address Statistics</h2>");

    struct explorer_address_stats address_stats = {0};
    explorer_query_address_stats(db, &address_stats);
    int64_t addr_total = address_stats.total;
    int64_t addr_nonzero = address_stats.nonzero;
    int64_t addr_over_1 = fq_i64(db, "SELECT count(*) FROM addresses WHERE balance >= 100000000");
    int64_t addr_over_100 = fq_i64(db, "SELECT count(*) FROM addresses WHERE balance >= 10000000000");

    {
        char t_str[32], nz_str[32], o1_str[32], o100_str[32];
        fmt_comma(t_str, sizeof(t_str), addr_total);
        fmt_comma(nz_str, sizeof(nz_str), addr_nonzero);
        fmt_comma(o1_str, sizeof(o1_str), addr_over_1);
        fmt_comma(o100_str, sizeof(o100_str), addr_over_100);

        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), addr_total, addr_nonzero, "addr_stats");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total unique addresses seen:</b> %s</p>"
            "<p><b>Addresses with balance &gt; 0:</b> %s</p>"
            "<p><b>Addresses with \xe2\x89\xa5 1 ZCL:</b> %s</p>"
            "<p><b>Addresses with \xe2\x89\xa5 100 ZCL:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            t_str, nz_str, o1_str, o100_str, rcpt);
    }

    /* Top 10 richest addresses */
    APPEND(off, r, max,
        "<h3>Top 10 Richest Addresses</h3>"
        "<table class='txlist'>"
        "<tr><th>#</th><th>Address Hash</th><th>Balance</th>"
        "<th>UTXOs</th><th>First Seen</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT hex(address_hash), balance, utxo_count, first_seen_height "
            "FROM addresses ORDER BY balance DESC LIMIT 10";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            int rank = 1;
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const char *ah = (const char *)sqlite3_column_text(s, 0);
                int64_t bal = sqlite3_column_int64(s, 1);
                int64_t uc = sqlite3_column_int64(s, 2);
                int64_t fsh = sqlite3_column_int64(s, 3);
                char bstr[64];
                fmt_zcl(bstr, sizeof(bstr), bal);
                APPEND(off, r, max,
                    "<tr><td>%d</td><td><code>%.16s...</code></td>"
                    "<td>%s ZCL</td><td>%" PRId64 "</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td></tr>",
                    rank, ah ? ah : "?", bstr, uc, fsh, fsh);
                rank++;
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}
