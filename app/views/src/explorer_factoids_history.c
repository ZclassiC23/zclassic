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

/* ── Section-local helpers (private to this TU; the shared header is
 *    off-limits) ──────────────────────────────────────────────────── */

/* Read a single REAL/double scalar (SELECT ... LIMIT 1). Returns `def` on
 * empty/error. The i64 helpers truncate, so the hodl_history.older_1y_pct
 * column (declared REAL) must be read through this one. */
static double fq_double(sqlite3 *db, const char *sql, double def)
{
    sqlite3_stmt *s = NULL;
    double v = def;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
            v = sqlite3_column_double(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

/* Current per-block coinbase subsidy in zatoshi at `height`, mirroring the
 * schedule in zcl_total_supply_zatoshi() (pre-Buttercup 12.5 ZCL; post-
 * Buttercup post_base >> (era+3), era = (h-1-707000)/1,680,000). Display-only,
 * consensus-neutral — derives the number so the supply prose cannot go stale
 * across a halving. */
static int64_t current_block_subsidy_sat(int64_t height)
{
    if (height < 1) return 0;
    if (height < BUTTERCUP_ACTIVATION_HEIGHT) return BASE_SUBSIDY_SAT; /* 12.5 ZCL */
    int64_t post_base = BASE_SUBSIDY_SAT / 2; /* 625,000,000 zat (spacing ratio 2) */
    int era = (int)((height - 1 - BUTTERCUP_ACTIVATION_HEIGHT) / POST_BC_HALVING);
    int shift = era + 3;
    if (shift >= 63) return 0;
    return post_base >> shift;
}

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
        "<tr><td><b>Equihash Params</b></td><td>N=200, K=9 (memory-hard, ASIC-resistant) "
        "for blocks 0\xe2\x80\x93" "585,317 (1344-byte solution). The Bubbles upgrade at "
        "block 585,318 switches to N=192, K=7 (400-byte solution).</td></tr>"
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
        { "Bubbly (DiffAdj)", BUBBLY_ACTIVATION_HEIGHT, 170010, "0x930b540d",
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
            /* Genesis (block 0) has zero subsidy (slow-start), so the first
             * pre-BC era earns over (count - 1) blocks. Without this the era
             * emission over-counts by one full subsidy (e.g. 707000*12.5 vs
             * the correct 706999*12.5 = 8,837,487.5 ZCL pre-BC max). The
             * "0 - 706999" range label is unchanged. */
            int64_t emission = (count - (era_start == 0 ? 1 : 0)) * subsidy;
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

    /* Proof-of-Work parameter eras. The Equihash (N,K) pair changed exactly
     * once — at the Bubbles upgrade (block 585,318) — which is observable in
     * the chain as the block solution field dropping from 1344 to 400 bytes.
     * Both byte sizes are consensus-immutable constants of the respective
     * Equihash parameters, so they are inlined (not a growing scalar). */
    {
        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt),
                            BUBBLES_ACTIVATION_HEIGHT, 400, "equihash_param_era");
        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>Proof-of-Work Parameter Eras</h3>"
            "<p style='color:#888'>ZClassic's Equihash parameters changed once, "
            "at the Bubbles upgrade (block 585,318). The switch is visible on-chain "
            "as the per-block solution field shrinking from 1344 to 400 bytes.</p>"
            "<table class='txlist'>"
            "<tr><th>Era</th><th>Block Range</th><th>Equihash (N,K)</th>"
            "<th>Solution Size</th></tr>"
            "<tr><td>Genesis \xe2\x80\x93 Bubbles</td>"
            "<td><a href='/explorer/block/0'>0</a> \xe2\x80\x93 "
            "<a href='/explorer/block/585317'>585,317</a></td>"
            "<td>N=200, K=9</td>"
            "<td>1344 bytes (2^9 indices \xc3\x97 21 bits)</td></tr>"
            "<tr><td>Bubbles \xe2\x80\x93 tip</td>"
            "<td><a href='/explorer/block/585318'>585,318</a> \xe2\x80\x93 present</td>"
            "<td>N=192, K=7</td>"
            "<td>400 bytes (2^7 indices \xc3\x97 25 bits)</td></tr>"
            "</table>"
            "<p style='color:#888'><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            rcpt);
    }
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

    /* known_height values are immutable blockchain facts; the sapling/
     * op_return firsts re-verified against the live node index on 2026-06-29
     * (MIN(block_height) of sapling_spends/sapling_outputs/op_returns).
     * They serve as fallbacks when
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
          "SELECT MIN(block_height) FROM sapling_spends", -1, 476978 },
        { "First Sapling shielded output",
          "SELECT MIN(block_height) FROM sapling_outputs", -1, 476977 },
        { "First OP_RETURN",
          "SELECT MIN(block_height) FROM op_returns", -1, 375159 },
        { "Bubbles upgrade activation", NULL,
          BUBBLES_ACTIVATION_HEIGHT, -1 },
        { "Bubbly activation (DiffAdj)", NULL,
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
        /* Last on-chain Sprout JoinSplit (h=2,124,937, 2023-10-07): the
         * legacy Sprout shielded pool falls out of use. Data-derived
         * (MAX block_height over joinsplits), so no consensus assumption. */
        { "Last Sprout JoinSplit (old shielded pool retires)",
          "SELECT MAX(block_height) FROM joinsplits", -1, 2124937 },
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

    /* Largest UNSPENT transparent output — scans the live utxos set, so it is
     * the largest coin still spendable today (a larger output that was later
     * spent is, by construction, not here — see the all-time row below). */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT u.value, u.height, b.time FROM utxos u "
                          "JOIN blocks b ON u.height = b.height "
                          "ORDER BY u.value DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), row.v0);
            RECORD_ROW("Largest unspent transparent output",
                "%s ZCL", vstr, row.v1, row.v2);
        }
    }

    /* Largest transparent output EVER created (tx_outputs scan, includes coins
     * since spent). Pinning the exact tx by value times out, so no block link.
     * MAX(value) over tx_outputs runs once per cached build (~0.5s, no 2s cap). */
    {
        int64_t max_ever = fq_i64(db, "SELECT MAX(value) FROM tx_outputs");
        if (max_ever > 0) {
            char vstr[64], rcpt[32] = "";
            fmt_zcl(vstr, sizeof(vstr), max_ever);
            compute_receipt_i64(rcpt, sizeof(rcpt), max_ever, 0,
                                "Largest transparent output ever");
            APPEND(off, r, max,
                "<tr><td>Largest transparent output ever</td><td>%s ZCL</td>"
                "<td style='color:#666'>spent</td>"
                "<td style='color:#666'>\xe2\x80\x94</td>"
                "<td><code>%s</code></td></tr>",
                vstr, rcpt);
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

    /* Most blocks mined in a single UTC calendar day. Buckets by integer day
     * index (time/86400) rather than the date() string so the GROUP BY over all
     * ~3.16M blocks sorts integers (budget-safe) instead of doing 3M strftime
     * calls + a string sort. The winning day index is rendered back to a date. */
    {
        struct sql_row_i64_2 row;
        const char *sql = "SELECT time/86400 AS d, COUNT(*) AS n FROM blocks "
                          "WHERE time>0 GROUP BY d ORDER BY n DESC LIMIT 1";
        if (sql_query_row_i64_2(db, sql, &row) && row.v1 > 0) {
            time_t day_ts = (time_t)(row.v0 * 86400);
            struct tm tmv;
            char day[16] = "?";
            if (gmtime_r(&day_ts, &tmv))
                strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
            char nstr[32], rcpt[32] = "";
            fmt_comma(nstr, sizeof(nstr), row.v1);
            compute_receipt_i64(rcpt, sizeof(rcpt), row.v1, row.v0, "blocks_per_day");
            APPEND(off, r, max,
                "<tr><td>Most blocks mined in one UTC day</td><td>%s blocks</td>"
                "<td style='color:#666'>\xe2\x80\x94</td><td>%s UTC</td>"
                "<td><code>%s</code></td></tr>",
                nstr, day, rcpt);
        }
    }

    /* Oldest coin still unspent: the earliest-height UTXO (a genesis-era
     * coinbase that has never moved). Links to its mining block. */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT u.height, u.value, b.time FROM utxos u "
                          "JOIN blocks b ON u.height = b.height "
                          "ORDER BY u.height ASC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), row.v1);
            RECORD_ROW("Oldest coin still unspent",
                "%s ZCL", vstr, row.v0, row.v2);
        }
    }

    /* HODL wave: value-weighted share of supply dormant > 1 year, from the
     * hodl_history projection (older_1y_pct is REAL — read via fq_double). */
    {
        double latest = fq_double(db,
            "SELECT older_1y_pct FROM hodl_history ORDER BY height DESC LIMIT 1", -1.0);
        double peak = fq_double(db,
            "SELECT MAX(older_1y_pct) FROM hodl_history", -1.0);
        int64_t sample_h = fq_i64(db,
            "SELECT height FROM hodl_history ORDER BY height DESC LIMIT 1");
        if (latest >= 0.0 && sample_h > 0) {
            char rcpt[32] = "";
            compute_receipt_i64(rcpt, sizeof(rcpt), sample_h,
                                (int64_t)(latest * 1000.0), "hodl_dormant_1y");
            APPEND(off, r, max,
                "<tr><td>Supply dormant &gt; 1 year (HODL wave)</td>"
                "<td>%.2f%% now \xc2\xb7 peak %.2f%%</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td style='color:#666'>latest sample</td>"
                "<td><code>%s</code></td></tr>",
                latest, peak, sample_h, sample_h, rcpt);
        }
    }

    /* Coins untouched since before the Buttercup upgrade (creation height <
     * 707,000), as a share of the live UTXO set — the coin-COUNT companion to
     * the value-weighted HODL figure above. */
    {
        int64_t pre_bc = fq_i64(db, "SELECT count(*) FROM utxos WHERE height < 707000");
        int64_t total_utxo = fq_i64(db, "SELECT count(*) FROM utxos");
        if (total_utxo > 0 && pre_bc > 0) {
            char pstr[32], tstr[32], valbuf[96];
            double pct = 100.0 * (double)pre_bc / (double)total_utxo;
            fmt_comma(pstr, sizeof(pstr), pre_bc);
            fmt_comma(tstr, sizeof(tstr), total_utxo);
            snprintf(valbuf, sizeof(valbuf), "%s of %s UTXOs (%.1f%%)",
                     pstr, tstr, pct);
            int64_t bc_time = get_block_time(db, BUTTERCUP_ACTIVATION_HEIGHT);
            RECORD_ROW("Coins predating Buttercup (still unspent)",
                "%s", valbuf, (int64_t)BUTTERCUP_ACTIVATION_HEIGHT, bc_time);
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

    char cur_sub_str[64];
    fmt_zcl(cur_sub_str, sizeof(cur_sub_str),
            current_block_subsidy_sat(chain_height));

    APPEND(off, r, max,
        "<h2 id='supply'>6. Supply Milestones</h2>"
        "<p style='color:#888'>Buttercup-aware emission: pre-707,000 at 12.5 ZCL/block; "
        "post-707,000 the subsidy halves every 1,680,000 blocks (6.25 &gt;&gt; (era+3)). "
        "Current block subsidy: <b>%s ZCL</b> (derived from the live tip, so it "
        "tracks each halving). ZClassic has no founders/dev tax \xe2\x80\x94 the "
        "entire coinbase goes to the miner.</p>"
        "<table class='txlist'>"
        "<tr><th>Milestone</th><th>Block</th><th>Date</th><th>SHA3</th></tr>",
        cur_sub_str);

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

    /* Live supply dashboard. Every figure derives from the in-binary emission
     * schedule (compute_supply_at_height / zcl_max_supply_zatoshi) + the live
     * UTXO projection, so it stays correct as the chain grows; only past-event
     * heights/dates are constants. */
    {
        int64_t mined_sat   = compute_supply_at_height(chain_height);
        int64_t cap_sat     = zcl_max_supply_zatoshi();
        int64_t sub_sat     = current_block_subsidy_sat(chain_height);
        int     spacing     = chain_height >= BUTTERCUP_ACTIVATION_HEIGHT ? 75 : 150;
        int64_t blocks_per_day = 86400 / spacing;
        int64_t daily_sat   = blocks_per_day * sub_sat;
        int64_t transparent_sat = fq_i64(db, "SELECT COALESCE(SUM(value),0) FROM utxos");
        int64_t utxo_count  = fq_i64(db, "SELECT count(*) FROM utxos");
        int64_t gap_sat     = mined_sat - transparent_sat;
        if (gap_sat < 0) gap_sat = 0;

        double pct_cap = cap_sat > 0
            ? 100.0 * (double)mined_sat / (double)cap_sat : 0.0;
        double annual_infl = mined_sat > 0
            ? 100.0 * ((double)daily_sat * 365.25) / (double)mined_sat : 0.0;
        double gap_pct = mined_sat > 0
            ? 100.0 * (double)gap_sat / (double)mined_sat : 0.0;

        /* First/only post-Buttercup halving (block 2,387,001, 2024-06-16). */
        int64_t halving_time   = get_block_time(db, 2387001);
        int64_t halving_supply = compute_supply_at_height(2387001);

        /* Next halving + ETA projected from the live tip at the target spacing. */
        int64_t next_h       = explorer_next_halving_height((int)chain_height);
        int64_t blocks_to_go = next_h - chain_height;
        if (blocks_to_go < 0) blocks_to_go = 0;
        int64_t days_to_go   = blocks_to_go * spacing / 86400;
        int64_t tip_time     = get_block_time(db, chain_height);
        int64_t eta_time     = tip_time + blocks_to_go * spacing;

        char mined_str[64], cap_str[64], sub_str2[64], daily_str[64];
        char transp_str[64], gap_str[64], hsup_str[64];
        char uc_str[32], btg_str[32], hts[64], etats[64], rcpt[32] = "";
        fmt_zcl(mined_str, sizeof(mined_str), mined_sat);
        fmt_zcl(cap_str, sizeof(cap_str), cap_sat);
        fmt_zcl(sub_str2, sizeof(sub_str2), sub_sat);
        fmt_zcl(daily_str, sizeof(daily_str), daily_sat);
        fmt_zcl(transp_str, sizeof(transp_str), transparent_sat);
        fmt_zcl(gap_str, sizeof(gap_str), gap_sat);
        fmt_zcl(hsup_str, sizeof(hsup_str), halving_supply);
        fmt_comma(uc_str, sizeof(uc_str), utxo_count);
        fmt_comma(btg_str, sizeof(btg_str), blocks_to_go);
        fmt_time(hts, sizeof(hts), halving_time);
        fmt_time(etats, sizeof(etats), eta_time);
        compute_receipt_i64(rcpt, sizeof(rcpt), mined_sat, cap_sat, "supply_dashboard");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>Supply Dashboard (live)</h3>"
            "<table>"
            "<tr><td><b>Mined supply (at tip)</b></td><td>%s ZCL</td></tr>"
            "<tr><td><b>Asymptotic emission cap</b></td><td>%s ZCL "
            "<span style='color:#888'>(tail-summed schedule; NOT 21M)</span></td></tr>"
            "<tr><td><b>Percent of cap mined</b></td><td>%.3f%%</td></tr>"
            "<tr><td><b>Current block subsidy</b></td><td>%s ZCL</td></tr>"
            "<tr><td><b>Daily issuance (target)</b></td><td>~%s ZCL/day "
            "<span style='color:#888'>(%" PRId64 " blocks \xc3\x97 subsidy)</span></td></tr>"
            "<tr><td><b>Annualized inflation</b></td><td>~%.2f%% "
            "<span style='color:#888'>(disinflationary)</span></td></tr>"
            "<tr><td><b>First/only post-Buttercup halving</b></td>"
            "<td>block <a href='/explorer/block/2387001'>2,387,001</a> \xc2\xb7 %s "
            "\xc2\xb7 supply %s ZCL</td></tr>"
            "<tr><td><b>Next halving</b></td>"
            "<td>block <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> \xc2\xb7 "
            "~%s blocks (~%" PRId64 " days, est. %s) to go</td></tr>"
            "<tr><td><b>Transparent UTXO pool</b></td>"
            "<td>%s ZCL across %s UTXOs</td></tr>"
            "<tr><td><b>Shielded + burned (supply gap)</b></td>"
            "<td>%s ZCL (%.3f%% of mined)</td></tr>"
            "<tr><td><b>SHA3 Receipt</b></td><td><code>%s</code></td></tr>"
            "</table></div>",
            mined_str, cap_str, pct_cap, sub_str2,
            daily_str, blocks_per_day, annual_infl,
            hts, hsup_str,
            next_h, next_h, btg_str, days_to_go, etats,
            transp_str, uc_str,
            gap_str, gap_pct, rcpt);
    }
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
    int64_t addr_over_1000 = fq_i64(db, "SELECT count(*) FROM addresses WHERE balance >= 100000000000");
    int64_t addr_over_1m = fq_i64(db, "SELECT count(*) FROM addresses WHERE balance >= 100000000000000");

    {
        int64_t addr_zero = addr_total - addr_nonzero;
        if (addr_zero < 0) addr_zero = 0;

        char t_str[32], nz_str[32], z_str[32];
        char o1_str[32], o100_str[32], o1k_str[32], o1m_str[64];
        fmt_comma(t_str, sizeof(t_str), addr_total);
        fmt_comma(nz_str, sizeof(nz_str), addr_nonzero);
        fmt_comma(z_str, sizeof(z_str), addr_zero);
        fmt_comma(o1_str, sizeof(o1_str), addr_over_1);
        fmt_comma(o100_str, sizeof(o100_str), addr_over_100);
        fmt_comma(o1k_str, sizeof(o1k_str), addr_over_1000);
        if (addr_over_1m > 0)
            fmt_comma(o1m_str, sizeof(o1m_str), addr_over_1m);
        else
            snprintf(o1m_str, sizeof(o1m_str),
                     "None \xe2\x80\x94 no address holds a million ZCL");

        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), addr_total, addr_nonzero, "addr_stats");

        /* The addresses table is the live UTXO-holder index — one row per
         * address that currently owns transparent coins, NOT a full historical
         * address census. "Currently holding coins" = balance > 0; the small
         * remainder are indexed rows whose balance has since fallen to zero. */
        APPEND(off, r, max,
            "<div class='card'>"
            "<p style='color:#888'>The <code>addresses</code> table is the live "
            "transparent UTXO-holder index (one row per address that currently "
            "owns coins), not a full historical address census. Percentages in "
            "this section are shares of <b>transparent</b> coins held, not of "
            "total emission \xe2\x80\x94 much of the supply is shielded or already "
            "spent.</p>"
            "<p><b>Addresses currently holding coins:</b> %s "
            "<span style='color:#888'>(of %s in the index; %s carry a zero "
            "balance)</span></p>"
            "<p><b>Holding \xe2\x89\xa5 1 ZCL:</b> %s</p>"
            "<p><b>Holding \xe2\x89\xa5 100 ZCL:</b> %s</p>"
            "<p><b>Holding \xe2\x89\xa5 1,000 ZCL:</b> %s</p>"
            "<p><b>Holding \xe2\x89\xa5 1,000,000 ZCL:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            nz_str, t_str, z_str, o1_str, o100_str, o1k_str, o1m_str, rcpt);
    }

    /* Distribution & concentration — all shares are of held transparent coins
     * (SUM of address balances), the denominator stated above, NOT of total
     * emission. Runs once per cached factoids build. */
    {
        int64_t total_held = fq_i64(db, "SELECT COALESCE(SUM(balance),0) FROM addresses");
        int64_t top10 = fq_i64(db,
            "SELECT COALESCE(SUM(balance),0) FROM "
            "(SELECT balance FROM addresses ORDER BY balance DESC LIMIT 10)");
        int64_t top100 = fq_i64(db,
            "SELECT COALESCE(SUM(balance),0) FROM "
            "(SELECT balance FROM addresses ORDER BY balance DESC LIMIT 100)");
        int64_t richest = fq_i64(db, "SELECT COALESCE(MAX(balance),0) FROM addresses");
        int64_t median = fq_i64(db,
            "SELECT balance FROM addresses WHERE balance>0 ORDER BY balance "
            "LIMIT 1 OFFSET (SELECT COUNT(*) FROM addresses WHERE balance>0)/2");
        int64_t single_use = fq_i64(db,
            "SELECT COALESCE(SUM(CASE WHEN first_seen_height=last_seen_height "
            "THEN 1 ELSE 0 END),0) FROM addresses");
        int64_t oldest_funded = fq_i64(db,
            "SELECT MIN(first_seen_height) FROM addresses WHERE balance>0");
        int64_t newest = fq_i64(db, "SELECT MAX(first_seen_height) FROM addresses");
        int64_t tip = fq_i64(db, "SELECT MAX(height) FROM blocks");

        int64_t mean = addr_nonzero > 0 ? total_held / addr_nonzero : 0;
        double top10_pct   = total_held > 0 ? 100.0 * (double)top10   / (double)total_held : 0.0;
        double top100_pct  = total_held > 0 ? 100.0 * (double)top100  / (double)total_held : 0.0;
        double richest_pct = total_held > 0 ? 100.0 * (double)richest / (double)total_held : 0.0;
        double single_pct  = addr_total  > 0 ? 100.0 * (double)single_use / (double)addr_total : 0.0;
        int64_t newest_below = (tip > 0 && newest > 0 && tip >= newest) ? tip - newest : 0;
        int64_t oldest_time = oldest_funded > 0 ? get_block_time(db, oldest_funded) : 0;

        char total_str[64], richest_str[64], mean_str[64], median_str[64];
        char held_cnt[32], su_str[32], ots[64], rcpt[32] = "";
        fmt_zcl(total_str, sizeof(total_str), total_held);
        fmt_zcl(richest_str, sizeof(richest_str), richest);
        fmt_zcl(mean_str, sizeof(mean_str), mean);
        fmt_zcl(median_str, sizeof(median_str), median);
        fmt_comma(held_cnt, sizeof(held_cnt), addr_nonzero);
        fmt_comma(su_str, sizeof(su_str), single_use);
        fmt_time(ots, sizeof(ots), oldest_time);
        compute_receipt_i64(rcpt, sizeof(rcpt), top10, top100, "addr_concentration");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>Distribution &amp; Concentration</h3>"
            "<p style='color:#888'>Shares are of the %s ZCL held across %s funded "
            "transparent addresses (the held-coin denominator, not total "
            "emission).</p>"
            "<table>"
            "<tr><td><b>Top 10 addresses hold</b></td><td>%.2f%% of held coins</td></tr>"
            "<tr><td><b>Top 100 addresses hold</b></td><td>%.2f%% of held coins</td></tr>"
            "<tr><td><b>Richest address</b></td>"
            "<td>%s ZCL (%.2f%% of held coins)</td></tr>"
            "<tr><td><b>Mean holder</b></td><td>%s ZCL</td></tr>"
            "<tr><td><b>Median holder</b></td><td>%s ZCL</td></tr>"
            "<tr><td><b>Single-use addresses</b></td>"
            "<td>%s (%.1f%%, first seen = last seen)</td></tr>"
            "<tr><td><b>Oldest still-funded address</b></td>"
            "<td>block <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> "
            "\xc2\xb7 %s</td></tr>"
            "<tr><td><b>Newest address</b></td>"
            "<td>block <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> "
            "(%" PRId64 " below tip)</td></tr>"
            "<tr><td><b>SHA3 Receipt</b></td><td><code>%s</code></td></tr>"
            "</table></div>",
            total_str, held_cnt,
            top10_pct, top100_pct,
            richest_str, richest_pct,
            mean_str, median_str,
            su_str, single_pct,
            oldest_funded, oldest_funded, ots,
            newest, newest, newest_below,
            rcpt);
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
