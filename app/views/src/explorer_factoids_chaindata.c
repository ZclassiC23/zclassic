/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer factoids VIEW — activity, archaeology & integrity sections (8-17).
 *
 * Owns the "what the chain carries and how it behaves" half of the historian
 * page: Privacy
 * Usage Over Time, ZSLP Token History, OP_RETURN Archaeology, Dust & UTXO
 * Analysis, Checkpoint History, Block Time Analysis, Transaction
 * Archaeology, Empty Blocks, Difficulty History, and Data Integrity. The
 * structure sections (1-7) live in explorer_factoids_history.c; the
 * public entry points and JSON API live in explorer_factoids_view.c.
 * Shared SHA3/format/block helpers come from
 * views/explorer_factoids_internal.h. */

#include "views/explorer_factoids_internal.h"

size_t factoids_emit_section_8_privacy(uint8_t *buf, size_t cap, size_t off,
                                       sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='privacy'>8. Privacy Usage Over Time</h2>"
        "<p style='color:#888'>Shielded operations by calendar year "
        "(from actual block timestamps). Includes Sprout JoinSplits, "
        "Sapling spends, Sapling outputs, and net shielding volume.</p>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Blocks</th><th>JoinSplits</th>"
        "<th>Sapling Spends</th><th>Sapling Outputs</th>"
        "<th>Net Shielded (ZCL)</th></tr>");

    {
        int64_t blk_yrs[20] = {0}, js_yrs[20] = {0};
        int64_t ss_yrs[20] = {0}, so_yrs[20] = {0};
        int64_t sv_yrs[20] = {0};  /* net sapling value */
        int max_yr = 2016;
        sqlite3_stmt *s = NULL;

        /* Blocks per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', time, 'unixepoch') AS INTEGER), "
                "count(*) FROM blocks WHERE time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                int idx = yr - 2016;
                if (idx >= 0 && idx < 20) {
                    blk_yrs[idx] = sqlite3_column_int64(s, 1);
                    if (yr > max_yr) max_yr = yr;
                }
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* JoinSplits per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER), "
                "count(*) FROM joinsplits j "
                "JOIN blocks b ON j.block_height = b.height "
                "WHERE b.time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    js_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* Sapling spends per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER), "
                "count(*) FROM sapling_spends sp "
                "JOIN blocks b ON sp.block_height = b.height "
                "WHERE b.time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    ss_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* Sapling outputs per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER), "
                "count(*) FROM sapling_outputs so "
                "JOIN blocks b ON so.block_height = b.height "
                "WHERE b.time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    so_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* Net shielding volume per year. Convention (verified against
         * running zclassicd valuePools): blocks.sapling_value stores
         * pool-balance deltas in "positive = into pool" sign, so
         * "Net Shielded" = SUM(sapling_value) directly — positive when
         * the year was a net shield-in, negative when net unshield. */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', time, 'unixepoch') AS INTEGER), "
                "SUM(sapling_value) FROM blocks "
                "WHERE sapling_value != 0 AND time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    sv_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        for (int yr = 2016; yr <= max_yr && yr < 2036; yr++) {
            int idx = yr - 2016;
            if (blk_yrs[idx] == 0) continue;
            char sv_str[64];
            fmt_zcl(sv_str, sizeof(sv_str), sv_yrs[idx]);
            APPEND(off, r, max,
                "<tr><td>%d</td><td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td><td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td><td>%s</td></tr>",
                yr, blk_yrs[idx], js_yrs[idx], ss_yrs[idx],
                so_yrs[idx], sv_str);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_9_zslp(uint8_t *buf, size_t cap, size_t off,
                                    sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='zslp'>9. ZSLP Token History</h2>");

    struct explorer_token_stats token_stats = {0};
    explorer_query_token_stats(db, &token_stats);
    int64_t total_tokens = token_stats.token_count;
    int64_t total_transfers = token_stats.transfer_count;

    {
        char tk_str[32], xf_str[32], rcpt[32] = "";
        fmt_comma(tk_str, sizeof(tk_str), total_tokens);
        fmt_comma(xf_str, sizeof(xf_str), total_transfers);
        compute_receipt_i64(rcpt, sizeof(rcpt), total_tokens, total_transfers, "zslp_summary");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total tokens created:</b> %s</p>"
            "<p><b>Total transfers:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            tk_str, xf_str, rcpt);
    }

    /* First 10 tokens */
    APPEND(off, r, max,
        "<h3>First 10 Tokens</h3>"
        "<table class='txlist'>"
        "<tr><th>Ticker</th><th>Name</th><th>Decimals</th>"
        "<th>Genesis Block</th><th>Token ID</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT ticker, name, decimals, genesis_height, hex(token_id) "
                          "FROM zslp_tokens ORDER BY genesis_height ASC LIMIT 10";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const char *ticker = (const char *)sqlite3_column_text(s, 0);
                const char *name = (const char *)sqlite3_column_text(s, 1);
                int dec = sqlite3_column_int(s, 2);
                int64_t gh = sqlite3_column_int64(s, 3);
                const char *tid = (const char *)sqlite3_column_text(s, 4);
                /* ticker + name come from attacker-controlled OP_RETURN
                 * bytes; HTML-escape before emitting. */
                char esc_ticker[64], esc_name[256];
                html_escape(esc_ticker, sizeof(esc_ticker),
                            ticker ? ticker : "?");
                html_escape(esc_name, sizeof(esc_name), name ? name : "?");
                APPEND(off, r, max,
                    "<tr><td>%s</td><td>%s</td><td>%d</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td><code>%.16s...</code></td></tr>",
                    esc_ticker, esc_name, dec,
                    gh, gh, tid ? tid : "?");
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* Most active tokens */
    APPEND(off, r, max,
        "<h3>Most Active Tokens</h3>"
        "<table class='txlist'>"
        "<tr><th>Ticker</th><th>Name</th><th>Transfers</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT t.ticker, t.name, count(x.rowid) as cnt "
            "FROM zslp_tokens t "
            "LEFT JOIN zslp_transfers x ON x.token_id = t.token_id "
            "GROUP BY t.token_id ORDER BY cnt DESC LIMIT 10";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const char *ticker = (const char *)sqlite3_column_text(s, 0);
                const char *name = (const char *)sqlite3_column_text(s, 1);
                int64_t cnt = sqlite3_column_int64(s, 2);
                char esc_ticker[64], esc_name[256];
                html_escape(esc_ticker, sizeof(esc_ticker),
                            ticker ? ticker : "?");
                html_escape(esc_name, sizeof(esc_name), name ? name : "?");
                APPEND(off, r, max,
                    "<tr><td>%s</td><td>%s</td><td>%" PRId64 "</td></tr>",
                    esc_ticker, esc_name, cnt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_10_opreturn(uint8_t *buf, size_t cap, size_t off,
                                         sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='opreturn'>10. OP_RETURN Archaeology</h2>");

    struct explorer_op_return_stats op_return_stats = {0};
    explorer_query_op_return_stats(db, &op_return_stats);
    int64_t total_opret = op_return_stats.total;
    int64_t slp_opret = op_return_stats.zslp;
    int64_t nonslp_opret = total_opret - slp_opret;
    int64_t first_opret = fq_i64(db, "SELECT MIN(block_height) FROM op_returns");
    int64_t first_nonslp = fq_i64(db,
        "SELECT MIN(block_height) FROM op_returns WHERE is_slp = 0");

    {
        char tot_str[32], slp_str[32], non_str[32], rcpt[32] = "";
        fmt_comma(tot_str, sizeof(tot_str), total_opret);
        fmt_comma(slp_str, sizeof(slp_str), slp_opret);
        fmt_comma(non_str, sizeof(non_str), nonslp_opret);
        compute_receipt_i64(rcpt, sizeof(rcpt), total_opret, first_opret, "opreturn_stats");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total OP_RETURN outputs:</b> %s</p>"
            "<p><b>ZSLP OP_RETURNs:</b> %s</p>"
            "<p><b>Non-ZSLP OP_RETURNs:</b> %s</p>"
            "<p><b>First OP_RETURN at block:</b> "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>",
            tot_str, slp_str, non_str, first_opret, first_opret);

        if (first_nonslp > 0) {
            APPEND(off, r, max,
                "<p><b>First non-ZSLP OP_RETURN:</b> "
                "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>",
                first_nonslp, first_nonslp);
        }
        APPEND(off, r, max,
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>", rcpt);
    }
    return off;
}

size_t factoids_emit_section_11_dust(uint8_t *buf, size_t cap, size_t off,
                                     sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='dust'>11. Dust &amp; UTXO Analysis</h2>");

    struct explorer_utxo_stats utxo_stats = {0};
    explorer_query_utxo_stats(db, &utxo_stats);
    int64_t utxo_total = utxo_stats.count;
    int64_t dust_1000 = fq_i64(db, "SELECT count(*) FROM utxos WHERE value < 100000");
    int64_t dust_10000 = fq_i64(db, "SELECT count(*) FROM utxos WHERE value < 1000000");
    int64_t utxo_value_total = utxo_stats.total_value_sat;
    int64_t coinbase_utxos = fq_i64(db,
        "SELECT count(*) FROM utxos WHERE is_coinbase = 1");

    {
        char u_str[32], d1_str[32], d2_str[32], cb_str[32], val_str[64], rcpt[32] = "";
        fmt_comma(u_str, sizeof(u_str), utxo_total);
        fmt_comma(d1_str, sizeof(d1_str), dust_1000);
        fmt_comma(d2_str, sizeof(d2_str), dust_10000);
        fmt_comma(cb_str, sizeof(cb_str), coinbase_utxos);
        fmt_zcl(val_str, sizeof(val_str), utxo_value_total);
        compute_receipt_i64(rcpt, sizeof(rcpt), utxo_total, utxo_value_total, "utxo_analysis");

        double dust_pct = utxo_total > 0
            ? (double)dust_1000 * 100.0 / (double)utxo_total : 0.0;

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total UTXOs:</b> %s</p>"
            "<p><b>Total UTXO value:</b> %s ZCL</p>"
            "<p><b>Dust (&lt;0.001 ZCL):</b> %s (%.1f%% of UTXO set)</p>"
            "<p><b>Dust (&lt;0.01 ZCL):</b> %s</p>"
            "<p><b>Unspent coinbase outputs:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            u_str, val_str, d1_str, dust_pct, d2_str, cb_str, rcpt);
    }
    return off;
}

size_t factoids_emit_section_12_checkpoints(uint8_t *buf, size_t cap, size_t off,
                                            sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='checkpoints'>12. Checkpoint History</h2>"
        "<p style='color:#888'>Hardcoded consensus checkpoints — "
        "blocks that all nodes must agree on.</p>"
        "<table class='txlist'>"
        "<tr><th>Height</th><th>Date</th><th>Block Hash</th><th>SHA3</th></tr>");

    {
        /* Checkpoint data: heights + authoritative hashes from zclassicd.
         * These are immutable blockchain facts — no SQLite dependency. */
        struct { int64_t height; const char *hash; } checkpoints[] = {
            { 0,       "0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602" },
            { 30000,   "000000005c2ad200c3c7c8e627f67b306659efca1268c9bb014335fdadc0c392" },
            { 160000,  "000000065093005a1a46ee95d6d66c2b07008220ca64dd3b3a93bbd1945480c0" },
            { 468200,  "000000009bd5548c851c2b237894d6807a53bf1e2808402545e27a995ae4f3c3" },
            { 2013514, "000019679aa2ea97a3f18bd9265bc91a09929ea0b1acc0fc5ef77cdf3cf906e7" },
            { 2879438, "000007e8fccb9e4831c7d7376a283b016ead6166491f951f4f083dbe366992b2" },
            { 3054000, "000005aa8e8c321cf364788e81b94619434b0dc1a85e658a022b44f23eb85662" },
        };
        int n_cp = (int)(sizeof(checkpoints) / sizeof(checkpoints[0]));
        for (int i = 0; i < n_cp; i++) {
            int64_t btime = 0;
            char bhash_unused[128] = "";
            get_block_at(db, checkpoints[i].height, bhash_unused, sizeof(bhash_unused), &btime);

            char height_s[32], ts[64], rcpt[32] = "";
            char hash_short[20];
            snprintf(height_s, sizeof(height_s), "%" PRId64, checkpoints[i].height);
            fmt_time(ts, sizeof(ts), btime);
            compute_receipt(rcpt, sizeof(rcpt), checkpoints[i].height,
                            checkpoints[i].hash, "checkpoint");
            snprintf(hash_short, sizeof(hash_short), "%.16s", checkpoints[i].hash);

            struct template_var vars[] = {
                { "height",     height_s },
                { "time",       checkpoints[i].height <= chain_height ? ts : "<span style='color:#666'>Not yet reached</span>" },
                { "hash_short", hash_short },
                { "receipt",    checkpoints[i].height <= chain_height ? rcpt : "--" },
            };
            off += template_render(TMPL_CHECKPOINT_ROW,
                                   vars, sizeof(vars)/sizeof(vars[0]),
                                   (char *)r + off, max - off);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_13_blocktimes(uint8_t *buf, size_t cap, size_t off,
                                           sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='blocktimes'>13. Block Time Analysis</h2>"
        "<p style='color:#888'>Pre-Buttercup target: 150s. "
        "Post-Buttercup target: 75s. Actual times from chain data.</p>");

    /* Pre-Buttercup stats. Bound the join on a.height + 1 < activation
     * so we don't pair the last pre-BC block with the first post-BC
     * block (different target spacing across that boundary would
     * poison the pre-BC max gap). */
    char pre_bc_sql[512];
    snprintf(pre_bc_sql, sizeof(pre_bc_sql),
        "SELECT AVG(b.time - a.time), "
        "MIN(CASE WHEN b.time - a.time > 0 THEN b.time - a.time END), "
        "MAX(b.time - a.time), "
        "count(*) "
        "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
        "WHERE a.time > 0 AND b.time > 0 AND b.height < %d",
        BUTTERCUP_ACTIVATION_HEIGHT);
    {
        sqlite3_stmt *s = NULL;
        const char *sql = pre_bc_sql;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                double avg = sqlite3_column_double(s, 0);
                int64_t mn = sqlite3_column_int64(s, 1);
                int64_t mx = sqlite3_column_int64(s, 2);
                int64_t cnt = sqlite3_column_int64(s, 3);
                char rcpt[32] = "";
                compute_receipt_i64(rcpt, sizeof(rcpt), (int64_t)avg, cnt,
                                    "blocktime_pre_bc");
                APPEND(off, r, max,
                    "<div class='card'>"
                    "<h3>Pre-Buttercup (blocks 0\xe2\x80\x93" "706,999, target 150s)</h3>"
                    "<table>"
                    "<tr><td><b>Mean block time:</b></td><td>%.1fs</td></tr>"
                    "<tr><td><b>Shortest gap:</b></td><td>%" PRId64 "s</td></tr>"
                    "<tr><td><b>Longest gap:</b></td><td>%" PRId64 "s (%.1f min)</td></tr>"
                    "<tr><td><b>Block pairs analyzed:</b></td><td>%" PRId64 "</td></tr>"
                    "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
                    "</table></div>",
                    avg, mn, mx, (double)mx / 60.0, cnt, rcpt);
            }
            sqlite3_finalize(s);
        }
    }

    /* Post-Buttercup stats. Use a.height (not b.height) so the very
     * first pair starts at the activation block, but the join's
     * b = a+1 means b.height >= activation+1; that's intentional
     * symmetric with the pre-BC bound above. */
    char post_bc_sql[512];
    snprintf(post_bc_sql, sizeof(post_bc_sql),
        "SELECT AVG(b.time - a.time), "
        "MIN(CASE WHEN b.time - a.time > 0 THEN b.time - a.time END), "
        "MAX(b.time - a.time), "
        "count(*) "
        "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
        "WHERE a.time > 0 AND b.time > 0 AND a.height >= %d",
        BUTTERCUP_ACTIVATION_HEIGHT);
    if (chain_height > BUTTERCUP_ACTIVATION_HEIGHT) {
        sqlite3_stmt *s = NULL;
        const char *sql = post_bc_sql;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                double avg = sqlite3_column_double(s, 0);
                int64_t mn = sqlite3_column_int64(s, 1);
                int64_t mx = sqlite3_column_int64(s, 2);
                int64_t cnt = sqlite3_column_int64(s, 3);
                char rcpt[32] = "";
                compute_receipt_i64(rcpt, sizeof(rcpt), (int64_t)avg, cnt,
                                    "blocktime_post_bc");
                APPEND(off, r, max,
                    "<div class='card'>"
                    "<h3>Post-Buttercup (blocks 707,000+, target 75s)</h3>"
                    "<table>"
                    "<tr><td><b>Mean block time:</b></td><td>%.1fs</td></tr>"
                    "<tr><td><b>Shortest gap:</b></td><td>%" PRId64 "s</td></tr>"
                    "<tr><td><b>Longest gap:</b></td><td>%" PRId64 "s (%.1f min)</td></tr>"
                    "<tr><td><b>Block pairs analyzed:</b></td><td>%" PRId64 "</td></tr>"
                    "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
                    "</table></div>",
                    avg, mn, mx, (double)mx / 60.0, cnt, rcpt);
            }
            sqlite3_finalize(s);
        }
    }
    return off;
}

size_t factoids_emit_section_14_transactions(uint8_t *buf, size_t cap, size_t off,
                                             sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='transactions'>14. Transaction Archaeology</h2>");

    {
        struct explorer_transaction_stats transaction_stats = {0};
        explorer_query_transaction_stats(db, &transaction_stats);
        struct explorer_op_return_stats op_return_stats = {0};
        explorer_query_op_return_stats(db, &op_return_stats);
        int64_t total_txs = transaction_stats.total;
        int64_t coinbase_txs = transaction_stats.coinbase;
        int64_t non_coinbase = total_txs - coinbase_txs;
        int64_t total_inputs = transaction_stats.inputs;
        int64_t total_outputs = transaction_stats.outputs;
        int64_t total_opret = op_return_stats.total;

        char tx_str[32], cb_str[32], nc_str[32], in_str[32], out_str[32], op_str[32];
        fmt_comma(tx_str, sizeof(tx_str), total_txs);
        fmt_comma(cb_str, sizeof(cb_str), coinbase_txs);
        fmt_comma(nc_str, sizeof(nc_str), non_coinbase);
        fmt_comma(in_str, sizeof(in_str), total_inputs);
        fmt_comma(out_str, sizeof(out_str), total_outputs);
        fmt_comma(op_str, sizeof(op_str), total_opret);

        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), total_txs, total_inputs, "tx_archaeology");

        APPEND(off, r, max,
            "<div class='card'>"
            "<table>"
            "<tr><td><b>Total transactions:</b></td><td>%s</td></tr>"
            "<tr><td><b>Coinbase transactions:</b></td><td>%s</td></tr>"
            "<tr><td><b>Non-coinbase transactions:</b></td><td>%s</td></tr>"
            "<tr><td><b>Total transparent inputs:</b></td><td>%s</td></tr>"
            "<tr><td><b>Total transparent outputs:</b></td><td>%s</td></tr>"
            "<tr><td><b>Total OP_RETURN outputs:</b></td><td>%s</td></tr>"
            "<tr><td><b>Avg outputs/tx:</b></td><td>%.2f</td></tr>"
            "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
            "</table></div>",
            tx_str, cb_str, nc_str, in_str, out_str, op_str,
            total_txs > 0 ? (double)total_outputs / (double)total_txs : 0.0,
            rcpt);
    }

    /* Transactions per year */
    APPEND(off, r, max,
        "<h3>Transactions Per Year</h3>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Total Tx</th><th>Coinbase</th>"
        "<th>Non-Coinbase</th><th>Avg Tx/Block</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER) AS yr, "
            "count(*) AS total, "
            "SUM(CASE WHEN t.is_coinbase = 1 THEN 1 ELSE 0 END) AS cb "
            "FROM transactions t "
            "JOIN blocks b ON t.block_height = b.height "
            "WHERE b.time > 0 GROUP BY yr ORDER BY yr";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                int64_t total = sqlite3_column_int64(s, 1);
                int64_t cb = sqlite3_column_int64(s, 2);
                int64_t nc = total - cb;
                double avg = cb > 0 ? (double)total / (double)cb : 0.0;
                APPEND(off, r, max,
                    "<tr><td>%d</td><td>%" PRId64 "</td>"
                    "<td>%" PRId64 "</td><td>%" PRId64 "</td>"
                    "<td>%.2f</td></tr>",
                    yr, total, cb, nc, avg);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_15_empty_blocks(uint8_t *buf, size_t cap, size_t off,
                                             sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='empty'>15. Empty Blocks Analysis</h2>"
        "<p style='color:#888'>Blocks with only a coinbase transaction "
        "(num_tx = 1, no user transactions).</p>");

    {
        int64_t empty_total = fq_i64(db,
            "SELECT count(*) FROM blocks WHERE num_tx = 1");
        int64_t total_blocks_2 = fq_i64(db, "SELECT count(*) FROM blocks");
        double empty_pct = total_blocks_2 > 0
            ? (double)empty_total * 100.0 / (double)total_blocks_2 : 0.0;

        char em_str[32], tb_str[32], rcpt[32] = "";
        fmt_comma(em_str, sizeof(em_str), empty_total);
        fmt_comma(tb_str, sizeof(tb_str), total_blocks_2);
        compute_receipt_i64(rcpt, sizeof(rcpt), empty_total, total_blocks_2,
                            "empty_blocks");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Empty blocks (coinbase only):</b> %s of %s (%.1f%%)</p>"
            "<p><b>SHA3:</b> <code>%s</code></p>"
            "</div>",
            em_str, tb_str, empty_pct, rcpt);
    }

    /* Empty blocks per year */
    APPEND(off, r, max,
        "<h3>Empty Blocks Per Year</h3>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Empty Blocks</th><th>Total Blocks</th><th>%%</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT CAST(strftime('%Y', time, 'unixepoch') AS INTEGER) AS yr, "
            "SUM(CASE WHEN num_tx = 1 THEN 1 ELSE 0 END) AS empty, "
            "count(*) AS total "
            "FROM blocks WHERE time > 0 GROUP BY yr ORDER BY yr";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                int64_t empty = sqlite3_column_int64(s, 1);
                int64_t total = sqlite3_column_int64(s, 2);
                double pct = total > 0 ? (double)empty * 100.0 / (double)total : 0.0;
                APPEND(off, r, max,
                    "<tr><td>%d</td><td>%" PRId64 "</td>"
                    "<td>%" PRId64 "</td><td>%.1f%%</td></tr>",
                    yr, empty, total, pct);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_16_difficulty(uint8_t *buf, size_t cap, size_t off,
                                           sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='difficulty'>16. Difficulty History</h2>"
        "<p style='color:#888'>Peak difficulty per calendar year.</p>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Peak Difficulty</th><th>Block</th><th>SHA3</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT CAST(strftime('%Y', b1.time, 'unixepoch') AS INTEGER) AS yr, "
            "MIN(b1.bits) AS min_bits, "
            "(SELECT b2.height FROM blocks b2 "
            " WHERE b2.bits = MIN(b1.bits) "
            " AND CAST(strftime('%Y', b2.time, 'unixepoch') AS INTEGER) = "
            "     CAST(strftime('%Y', b1.time, 'unixepoch') AS INTEGER) "
            " LIMIT 1) AS peak_height "
            "FROM blocks b1 "
            "WHERE b1.time > 0 AND b1.bits > 0 "
            "GROUP BY yr ORDER BY yr";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                uint32_t bits = (uint32_t)sqlite3_column_int64(s, 1);
                int64_t ph = sqlite3_column_int64(s, 2);
                double diff = explorer_difficulty_from_bits(bits);
                char dstr[64], rcpt[32] = "";
                snprintf(dstr, sizeof(dstr), "%.4f", diff);
                compute_receipt_i64(rcpt, sizeof(rcpt), ph, (int64_t)bits,
                                    "difficulty_peak");
                APPEND(off, r, max,
                    "<tr><td>%d</td><td>%s</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td><code>%s</code></td></tr>",
                    yr, dstr, ph, ph, rcpt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

size_t factoids_emit_section_17_integrity(uint8_t *buf, size_t cap, size_t off,
                                          sqlite3 *db, int64_t chain_height,
                                          int64_t block_count)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='integrity'>17. Data Integrity</h2>");

    struct explorer_transaction_stats integrity_tx_stats = {0};
    explorer_query_transaction_stats(db, &integrity_tx_stats);
    int64_t tx_count = integrity_tx_stats.total;

    char integrity_hash[128] = "";
    compute_integrity_hash(db, chain_height, integrity_hash,
                           sizeof(integrity_hash));

    {
        char blk_str[32], tx_str[32];
        fmt_comma(blk_str, sizeof(blk_str), block_count);
        fmt_comma(tx_str, sizeof(tx_str), tx_count);

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Chain height:</b> %" PRId64 "</p>"
            "<p><b>Indexed blocks:</b> %s</p>"
            "<p><b>Indexed transactions:</b> %s</p>"
            "<p><b>SHA3-256 coverage:</b> blocks %" PRId64 " \xe2\x80\x93 %" PRId64
            " (last 100)</p>"
            "<p><b>Integrity hash:</b><br>"
            "<code style='word-break:break-all;color:#33ff99'>%s</code></p>"
            "</div>",
            chain_height, blk_str, tx_str,
            chain_height > 100 ? chain_height - 100 : (int64_t)0, chain_height,
            integrity_hash);
    }

    APPEND(off, r, max,
        "<div class='card' style='margin-top:16px'>"
        "<h3>How to Verify</h3>"
        "<p style='color:#888'>Recompute by replaying blocks from genesis. "
        "Each block's hash chains:</p>"
        "<code style='display:block;padding:12px;background:#0c0c0c;border-radius:4px;"
        "word-break:break-all;color:#ccc'>"
        "rolling_SHA3 += (height_le64 || time_le64 || num_tx_le32 || "
        "sapling_value_le64 || sprout_value_le64 || block_hash_hex_string)"
        "</code>"
        "<p style='color:#888;margin-top:8px'>36 bytes of packed integers + "
        "variable-length hex hash string per block, fed sequentially into SHA3-256.</p>"
        "<p style='color:#888;margin-top:12px'>Milestone receipts: "
        "<code>SHA3(height_le64 || block_hash_hex || fact_name_ascii)</code> "
        "\xe2\x80\x94 first 16 hex chars.</p>"
        "<p style='color:#888'>Record receipts: "
        "<code>SHA3(val1_le64 || val2_le64 || label_ascii)</code> "
        "\xe2\x80\x94 first 16 hex chars.</p>"
        "</div>");
    return off;
}
