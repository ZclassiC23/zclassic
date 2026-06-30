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
#include <math.h>

/* ── Display-only difficulty/chainwork decoders (consensus-neutral) ──
 *
 * These exist so the new Section-16 records render from the raw compact
 * "bits" target and the stored chain_work blob directly, WITHOUT calling
 * difficulty_from_bits (which is under separate review for an inverted
 * exponent). Pure arithmetic on decoded targets — no consensus surface. */

/* Decode a compact "bits" word into its full 256-bit target as a double.
 * target = mantissa * 256^(exponent-3); mantissa = bits & 0x00ffffff,
 * exponent = bits >> 24. Good enough for magnitude/ratio display. */
static double cd_target_from_bits(uint32_t bits)
{
    int exp = (int)(bits >> 24);
    double mant = (double)(bits & 0x00ffffffu);
    return mant * pow(256.0, (double)(exp - 3));
}

/* Decode a hex-encoded little-endian chain_work blob (storage order:
 * byte[0] = least-significant) into a double magnitude. SQLite hex()
 * emits the bytes in storage order, so we accumulate from the most-
 * significant byte down. Returns 0.0 on a malformed/empty string. */
static double cd_chainwork_to_double(const char *hex_le)
{
    if (!hex_le) return 0.0;
    size_t n = strlen(hex_le);
    size_t nbytes = n / 2;
    if (nbytes == 0 || nbytes > 64) return 0.0;

    uint8_t bytes[64] = {0};
    for (size_t i = 0; i < nbytes; i++) {
        char hi = hex_le[2 * i], lo = hex_le[2 * i + 1];
        int h = (hi >= '0' && hi <= '9') ? hi - '0'
              : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
              : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : 0;
        int l = (lo >= '0' && lo <= '9') ? lo - '0'
              : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
              : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : 0;
        bytes[i] = (uint8_t)((h << 4) | l);
    }

    double val = 0.0;
    for (size_t i = nbytes; i-- > 0;)
        val = val * 256.0 + (double)bytes[i];
    return val;
}

size_t factoids_emit_section_8_privacy(uint8_t *buf, size_t cap, size_t off,
                                       sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='privacy'>8. Privacy Usage Over Time</h2>");

    /* ── All-time shielded-pool summary (NEW) ─────────────────────── */
    {
        int64_t js  = fq_i64(db, "SELECT count(*) FROM joinsplits");
        int64_t ss  = fq_i64(db, "SELECT count(*) FROM sapling_spends");
        int64_t so  = fq_i64(db, "SELECT count(*) FROM sapling_outputs");
        int64_t shielded_total = js + ss + so;
        int64_t js_first = fq_i64(db,
            "SELECT COALESCE(MIN(block_height),0) FROM joinsplits");
        int64_t js_last  = fq_i64(db,
            "SELECT COALESCE(MAX(block_height),0) FROM joinsplits");
        /* SUM(blocks.sprout_value) is the CORRECT residual-pool column —
         * it cross-checks SUM(vpub_old)-SUM(vpub_new) exactly. Unlike
         * blocks.sapling_value (sign-buggy, suppressed below) it is safe
         * to publish. */
        int64_t sprout_pool = fq_i64(db,
            "SELECT COALESCE(SUM(sprout_value),0) FROM blocks");
        int64_t vpub_old = fq_i64(db,
            "SELECT COALESCE(SUM(vpub_old),0) FROM joinsplits");
        int64_t vpub_new = fq_i64(db,
            "SELECT COALESCE(SUM(vpub_new),0) FROM joinsplits");
        int64_t live_notes = so - ss;
        int64_t sprout_null = fq_i64(db, "SELECT count(*) FROM sprout_nullifiers");
        int64_t sapling_null = fq_i64(db, "SELECT count(*) FROM sapling_nullifiers");
        int64_t null_total = sprout_null + sapling_null;
        int64_t first_sapling_out = fq_i64(db,
            "SELECT COALESCE(MIN(block_height),0) FROM sapling_outputs");
        /* Distinct Sapling anchors MUST be derived from sapling_spends.anchor
         * — the dedicated sapling_anchors projection table is EMPTY on live
         * DBs (count(*) returns 0), so never read it here. */
        int64_t distinct_anchors = fq_i64(db,
            "SELECT count(DISTINCT anchor) FROM sapling_spends");

        struct sql_row_i64_2 busiest = {0};
        sql_query_row_i64_2(db,
            "SELECT block_height, count(*) AS n FROM sapling_outputs "
            "GROUP BY block_height ORDER BY n DESC LIMIT 1", &busiest);

        char tot_s[32], js_s[32], ss_s[32], so_s[32], live_s[32];
        char sn_s[32], spn_s[32], nt_s[32], anc_s[32], bcount_s[32];
        char pool_s[64], vin_s[64], vout_s[64], rcpt[32] = "";
        fmt_comma(tot_s, sizeof(tot_s), shielded_total);
        fmt_comma(js_s, sizeof(js_s), js);
        fmt_comma(ss_s, sizeof(ss_s), ss);
        fmt_comma(so_s, sizeof(so_s), so);
        fmt_comma(live_s, sizeof(live_s), live_notes);
        fmt_comma(spn_s, sizeof(spn_s), sprout_null);
        fmt_comma(sn_s, sizeof(sn_s), sapling_null);
        fmt_comma(nt_s, sizeof(nt_s), null_total);
        fmt_comma(anc_s, sizeof(anc_s), distinct_anchors);
        fmt_comma(bcount_s, sizeof(bcount_s), busiest.v1);
        fmt_zcl(pool_s, sizeof(pool_s), sprout_pool);
        fmt_zcl(vin_s, sizeof(vin_s), vpub_old);
        fmt_zcl(vout_s, sizeof(vout_s), vpub_new);
        compute_receipt_i64(rcpt, sizeof(rcpt), shielded_total, null_total,
                            "privacy_summary");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>All-time shielded operations:</b> %s "
            "(%s JoinSplits + %s Sapling spends + %s Sapling outputs)</p>"
            "<p><b>Sprout JoinSplits:</b> %s, first at block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a>, last at block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> "
            "(Sprout effectively retired)</p>"
            "<p><b>Still locked in the Sprout pool:</b> %s ZCL "
            "(SUM(sprout_value); cross-checks SUM(vpub_old)\xe2\x88\x92SUM(vpub_new))</p>"
            "<p><b>Gross Sprout shielding:</b> %s ZCL shielded in / "
            "%s ZCL unshielded out (cumulative)</p>"
            "<p><b>Sapling:</b> activated at block %d, first shielded output at "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> "
            "(%s spends, %s outputs to date)</p>"
            "<p><b>Live Sapling note set (anonymity set):</b> %s unspent notes "
            "(outputs \xe2\x88\x92 spends)</p>"
            "<p><b>Double-spend protection:</b> %s nullifiers "
            "(%s Sprout + %s Sapling)</p>"
            "<p><b>Busiest shielded block:</b> "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> "
            "with %s Sapling outputs</p>"
            "<p><b>Distinct Sapling anchors referenced:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            tot_s, js_s, ss_s, so_s,
            js_s, js_first, js_first, js_last, js_last,
            pool_s, vin_s, vout_s,
            SAPLING_ACTIVATION_HEIGHT, first_sapling_out, first_sapling_out,
            ss_s, so_s,
            live_s,
            nt_s, spn_s, sn_s,
            busiest.v0, busiest.v0, bcount_s,
            anc_s, rcpt);
    }

    APPEND(off, r, max,
        "<p style='color:#888'>Shielded operations by calendar year "
        "(from actual block timestamps): Sprout JoinSplits, Sapling spends, "
        "and Sapling outputs. The per-year net-shielded column is shown only "
        "when the underlying per-block sapling_value series is reliable "
        "(see the note below the table).</p>"
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

        /* Reliability gate for the Net Shielded column. The cumulative
         * whole-chain pool balance is SUM(blocks.sapling_value); a pool
         * BALANCE can never be negative. On this datadir it computes to a
         * negative value (~-38,928 ZCL), which means blocks.sapling_value
         * has a sign-convention / coverage bug. That is a data-layer fix
         * (out of scope here) — until it lands we must not render the
         * impossible per-year net-shielded figures as if authoritative. */
        int64_t net_shielded_total =
            fq_i64(db, "SELECT COALESCE(SUM(sapling_value),0) FROM blocks");
        bool net_shielded_unreliable = (net_shielded_total < 0);

        for (int yr = 2016; yr <= max_yr && yr < 2036; yr++) {
            int idx = yr - 2016;
            if (blk_yrs[idx] == 0) continue;
            char sv_str[64];
            if (net_shielded_unreliable)
                snprintf(sv_str, sizeof(sv_str), "n/a");
            else
                fmt_zcl(sv_str, sizeof(sv_str), sv_yrs[idx]);
            APPEND(off, r, max,
                "<tr><td>%d</td><td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td><td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td><td>%s</td></tr>",
                yr, blk_yrs[idx], js_yrs[idx], ss_yrs[idx],
                so_yrs[idx], sv_str);
        }
        APPEND(off, r, max, "</table>");
        if (net_shielded_unreliable) {
            APPEND(off, r, max,
                "<p style='color:#c80'>shielded pool: monitoring unavailable "
                "&mdash; the net-shielded column is suppressed because the "
                "cumulative pool balance computes negative (impossible). "
                "Pending a blocks.sapling_value sign/coverage fix in the data "
                "layer.</p>");
        }
    }
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

    /* ── ZSLP archaeology (NEW) ───────────────────────────────────── */
    {
        int64_t burns = fq_i64(db,
            "SELECT count(*) FROM zslp_transfers WHERE tx_type = 4");
        int64_t genesis_n = fq_i64(db,
            "SELECT count(*) FROM zslp_transfers WHERE tx_type = 1");
        int64_t mint_n = fq_i64(db,
            "SELECT count(*) FROM zslp_transfers WHERE tx_type = 2");
        int64_t send_n = fq_i64(db,
            "SELECT count(*) FROM zslp_transfers WHERE tx_type = 3");
        int64_t genesis_only = fq_i64(db,
            "SELECT count(*) FROM zslp_tokens t WHERE NOT EXISTS ("
            "SELECT 1 FROM zslp_transfers x WHERE x.token_id = t.token_id "
            "AND x.tx_type IN (2,3))");
        /* 21 tokens carry total_minted = INT64_MAX; NEVER SUM the column
         * (it overflows SQLite int64). Just count the saturated rows. */
        int64_t saturated = fq_i64(db,
            "SELECT count(*) FROM zslp_tokens "
            "WHERE total_minted = 9223372036854775807");
        int64_t last_xfer = fq_i64(db,
            "SELECT COALESCE(MAX(block_height),0) FROM zslp_transfers");
        int64_t tip = fq_i64(db, "SELECT COALESCE(MAX(height),0) FROM blocks");
        int64_t quiet_blocks = (tip > last_xfer) ? tip - last_xfer : 0;

        /* First token (WASS / "Whoopass Stew"); ticker+name are
         * attacker-controlled OP_RETURN bytes — HTML-escape before emit. */
        char first_ticker[64] = "?", first_name[256] = "?";
        int64_t first_genesis = 0;
        {
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT ticker, name, genesis_height FROM zslp_tokens "
                    "ORDER BY genesis_height ASC LIMIT 1",
                    -1, &s, NULL) == SQLITE_OK && s) {
                if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                    const char *tk = (const char *)sqlite3_column_text(s, 0);
                    const char *nm = (const char *)sqlite3_column_text(s, 1);
                    html_escape(first_ticker, sizeof(first_ticker), tk ? tk : "?");
                    html_escape(first_name, sizeof(first_name), nm ? nm : "?");
                    first_genesis = sqlite3_column_int64(s, 2);
                }
                sqlite3_finalize(s);
            }
        }

        double died_pct = total_tokens > 0
            ? (double)genesis_only * 100.0 / (double)total_tokens : 0.0;
        double quiet_weeks = (double)quiet_blocks * 75.0 / 604800.0;

        char g_s[32], m_s[32], se_s[32], b_s[32], go_s[32], tt_s[32];
        char sat_s[32], qb_s[32], rcpt[32] = "";
        fmt_comma(g_s, sizeof(g_s), genesis_n);
        fmt_comma(m_s, sizeof(m_s), mint_n);
        fmt_comma(se_s, sizeof(se_s), send_n);
        fmt_comma(b_s, sizeof(b_s), burns);
        fmt_comma(go_s, sizeof(go_s), genesis_only);
        fmt_comma(tt_s, sizeof(tt_s), total_tokens);
        fmt_comma(sat_s, sizeof(sat_s), saturated);
        fmt_comma(qb_s, sizeof(qb_s), quiet_blocks);
        compute_receipt_i64(rcpt, sizeof(rcpt), total_tokens, genesis_only,
                            "zslp_archaeology");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>ZSLP Archaeology</h3>"
            "<p><b>First token:</b> %s &mdash; \"%s\", genesis at block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>"
            "<p><b>Operations by type:</b> %s GENESIS, %s MINT, %s SEND, "
            "%s BURN</p>"
            "<p><b>Burns ever recorded:</b> %s &mdash; the credit-only ledger "
            "never writes a BURN row</p>"
            "<p><b>Died at birth:</b> %s of %s tokens (%.1f%%) had no mint or "
            "send after genesis</p>"
            "<p><b>Declared (unvalidated) supplies:</b> %s tokens declare a "
            "saturated INT64_MAX supply &mdash; ZSLP supply is attacker-"
            "controlled OP_RETURN data with no consensus cap, never a real "
            "minted total</p>"
            "<p><b>Last ZSLP activity:</b> block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> &mdash; "
            "%s blocks (~%.1f weeks) ago</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            first_ticker, first_name, first_genesis, first_genesis,
            g_s, m_s, se_s, b_s,
            b_s,
            go_s, tt_s, died_pct,
            sat_s,
            last_xfer, last_xfer, qb_s, quiet_weeks,
            rcpt);
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
    /* The chain's very first OP_RETURN (375,159) was NOT a token, so
     * "First non-ZSLP OP_RETURN" used to duplicate "First OP_RETURN".
     * Surface the (non-redundant, more informative) first ZSLP one instead. */
    int64_t first_slp = fq_i64(db,
        "SELECT MIN(block_height) FROM op_returns WHERE is_slp = 1");
    int64_t last_opret = fq_i64(db, "SELECT MAX(block_height) FROM op_returns");
    int64_t tip = fq_i64(db, "SELECT COALESCE(MAX(height),0) FROM blocks");
    int64_t since_last = (tip > last_opret) ? tip - last_opret : 0;

    /* Usage by upgrade era — one atomic scan so the buckets sum to total. */
    int64_t era_pre = 0, era_sap = 0, era_bc = 0;
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT "
            "SUM(CASE WHEN block_height < 476969 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN block_height >= 476969 AND block_height < 707000 "
            "    THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN block_height >= 707000 THEN 1 ELSE 0 END) "
            "FROM op_returns";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                era_pre = sqlite3_column_int64(s, 0);
                era_sap = sqlite3_column_int64(s, 1);
                era_bc  = sqlite3_column_int64(s, 2);
            }
            sqlite3_finalize(s);
        }
    }

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
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> "
            "(not a token \xe2\x80\x94 is_slp=0)</p>",
            tot_str, slp_str, non_str, first_opret, first_opret);

        if (first_slp > 0) {
            APPEND(off, r, max,
                "<p><b>First ZSLP OP_RETURN:</b> "
                "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>",
                first_slp, first_slp);
        }
        APPEND(off, r, max,
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>", rcpt);
    }

    /* ── OP_RETURN by upgrade era + dormancy (NEW) ────────────────── */
    {
        char pre_s[32], sap_s[32], bc_s[32], sl_s[32], rcpt[32] = "";
        fmt_comma(pre_s, sizeof(pre_s), era_pre);
        fmt_comma(sap_s, sizeof(sap_s), era_sap);
        fmt_comma(bc_s, sizeof(bc_s), era_bc);
        fmt_comma(sl_s, sizeof(sl_s), since_last);
        compute_receipt_i64(rcpt, sizeof(rcpt), last_opret, total_opret,
                            "opreturn_eras");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>OP_RETURN by upgrade era</h3>"
            "<table class='txlist'>"
            "<tr><th>Era</th><th>OP_RETURNs</th></tr>"
            "<tr><td>Pre-Sapling (&lt;476,969)</td><td>%s</td></tr>"
            "<tr><td>Sapling era (476,969\xe2\x80\x93" "706,999)</td><td>%s</td></tr>"
            "<tr><td>Buttercup+ (&ge;707,000)</td><td>%s</td></tr>"
            "</table>"
            "<p><b>Last OP_RETURN on chain:</b> block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> "
            "(%s blocks ago \xe2\x80\x94 none recent)</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            pre_s, sap_s, bc_s,
            last_opret, last_opret, sl_s, rcpt);
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

    /* Compute ALL UTXO aggregates in ONE atomic scan so every figure is a
     * single self-consistent snapshot. The live utxos projection drifts
     * between separate scans as the chain advances, so three independent
     * fq_i64() scans (the old code) could disagree with each other. */
    int64_t u_total = 0, u_value = 0, lt01 = 0, lt1 = 0, lt10 = 0;
    int64_t dust001 = 0, dust01 = 0, cb_count = 0, cb_value = 0;
    int64_t e_pre = 0, e_sap = 0, e_bub = 0, e_bc = 0, dorm_n = 0, dorm_v = 0;
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT count(*), COALESCE(SUM(value),0), "
            "SUM(CASE WHEN value < 10000000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN value < 100000000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN value < 1000000000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN value < 100000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN value < 1000000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN is_coinbase = 1 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN is_coinbase = 1 THEN value ELSE 0 END), "
            "SUM(CASE WHEN height < 476969 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN height >= 476969 AND height < 585318 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN height >= 585318 AND height < 707000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN height >= 707000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN height < 100000 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN height < 100000 THEN value ELSE 0 END) "
            "FROM utxos";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                u_total  = sqlite3_column_int64(s, 0);
                u_value  = sqlite3_column_int64(s, 1);
                lt01     = sqlite3_column_int64(s, 2);
                lt1      = sqlite3_column_int64(s, 3);
                lt10     = sqlite3_column_int64(s, 4);
                dust001  = sqlite3_column_int64(s, 5);
                dust01   = sqlite3_column_int64(s, 6);
                cb_count = sqlite3_column_int64(s, 7);
                cb_value = sqlite3_column_int64(s, 8);
                e_pre    = sqlite3_column_int64(s, 9);
                e_sap    = sqlite3_column_int64(s, 10);
                e_bub    = sqlite3_column_int64(s, 11);
                e_bc     = sqlite3_column_int64(s, 12);
                dorm_n   = sqlite3_column_int64(s, 13);
                dorm_v   = sqlite3_column_int64(s, 14);
            }
            sqlite3_finalize(s);
        }
    }

    /* Median is cheap thanks to the utxos(value) index; mean from the scan. */
    int64_t median = fq_i64(db,
        "SELECT value FROM utxos ORDER BY value LIMIT 1 "
        "OFFSET (SELECT count(*)/2 FROM utxos)");
    int64_t mean = u_total > 0 ? u_value / u_total : 0;

    /* Largest currently-unspent output (locatable cheaply on the small
     * utxos table; the all-time max over tx_outputs lives in section 14). */
    struct sql_row_i64_2 largest = {0};
    sql_query_row_i64_2(db,
        "SELECT value, height FROM utxos ORDER BY value DESC LIMIT 1", &largest);

    /* Immature coinbase outputs (within COINBASE_MATURITY=100 of the tip). */
    struct sql_row_i64_2 immature = {0};
    sql_query_row_i64_2(db,
        "SELECT count(*), COALESCE(SUM(value),0) FROM utxos "
        "WHERE is_coinbase = 1 AND height > (SELECT MAX(height) FROM utxos) - 100",
        &immature);

    {
        char u_s[32], v_s[64], med_s[64], mean_s[64];
        char l01_s[32], l1_s[32], l10_s[32], d001_s[32], d01_s[32];
        char cbc_s[32], cbv_s[64], rcpt[32] = "";
        fmt_comma(u_s, sizeof(u_s), u_total);
        fmt_zcl(v_s, sizeof(v_s), u_value);
        fmt_zcl(med_s, sizeof(med_s), median);
        fmt_zcl(mean_s, sizeof(mean_s), mean);
        fmt_comma(l01_s, sizeof(l01_s), lt01);
        fmt_comma(l1_s, sizeof(l1_s), lt1);
        fmt_comma(l10_s, sizeof(l10_s), lt10);
        fmt_comma(d001_s, sizeof(d001_s), dust001);
        fmt_comma(d01_s, sizeof(d01_s), dust01);
        fmt_comma(cbc_s, sizeof(cbc_s), cb_count);
        fmt_zcl(cbv_s, sizeof(cbv_s), cb_value);
        compute_receipt_i64(rcpt, sizeof(rcpt), u_total, u_value, "utxo_analysis");

        double pct01 = u_total > 0 ? (double)lt01 * 100.0 / (double)u_total : 0.0;
        double pct1  = u_total > 0 ? (double)lt1  * 100.0 / (double)u_total : 0.0;
        double pct10 = u_total > 0 ? (double)lt10 * 100.0 / (double)u_total : 0.0;
        double dust001_pct = u_total > 0 ? (double)dust001 * 100.0 / (double)u_total : 0.0;
        double cb_cnt_pct = u_total > 0 ? (double)cb_count * 100.0 / (double)u_total : 0.0;
        double cb_val_pct = u_value > 0 ? (double)cb_value * 100.0 / (double)u_value : 0.0;

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total UTXOs:</b> %s</p>"
            "<p><b>Total UTXO value:</b> %s ZCL</p>"
            "<p><b>Median / mean output:</b> %s ZCL / %s ZCL</p>"
            "<p><b>Value ladder:</b> &lt;0.1 ZCL %s (%.1f%%), "
            "&lt;1 ZCL %s (%.1f%%), &lt;10 ZCL %s (%.1f%%)</p>"
            "<p><b>Dust (&lt;0.001 ZCL):</b> %s (%.1f%% of set)</p>"
            "<p><b>Dust (&lt;0.01 ZCL):</b> %s</p>"
            "<p><b>Unspent coinbase:</b> %s outputs (%.1f%% of UTXOs) holding "
            "%s ZCL (%.2f%% of value)</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            u_s, v_s, med_s, mean_s,
            l01_s, pct01, l1_s, pct1, l10_s, pct10,
            d001_s, dust001_pct, d01_s,
            cbc_s, cb_cnt_pct, cbv_s, cb_val_pct,
            rcpt);

        char lv_s[64], iv_s[64], ic_s[32];
        char ep_s[32], es_s[32], eb_s[32], ec_s[32], dn_s[32], dv_s[64], rcpt2[32] = "";
        fmt_zcl(lv_s, sizeof(lv_s), largest.v0);
        fmt_comma(ic_s, sizeof(ic_s), immature.v0);
        fmt_zcl(iv_s, sizeof(iv_s), immature.v1);
        fmt_comma(ep_s, sizeof(ep_s), e_pre);
        fmt_comma(es_s, sizeof(es_s), e_sap);
        fmt_comma(eb_s, sizeof(eb_s), e_bub);
        fmt_comma(ec_s, sizeof(ec_s), e_bc);
        fmt_comma(dn_s, sizeof(dn_s), dorm_n);
        fmt_zcl(dv_s, sizeof(dv_s), dorm_v);
        compute_receipt_i64(rcpt2, sizeof(rcpt2), largest.v0, largest.v1,
                            "largest_unspent_utxo");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Largest unspent output:</b> %s ZCL at block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>"
            "<p><b>Immature coinbase:</b> %s outputs (%s ZCL) within 100 blocks "
            "of the tip</p>"
            "<p><b>UTXOs by upgrade era:</b> pre-Sapling %s, Sapling %s, "
            "Bubbles %s, Buttercup+ %s</p>"
            "<p><b>Dormant since the first 100k blocks:</b> %s outputs (%s ZCL) "
            "minted below height 100,000 and never moved</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            lv_s, largest.v1, largest.v1,
            ic_s, iv_s,
            ep_s, es_s, eb_s, ec_s,
            dn_s, dv_s,
            rcpt2);
    }

    /* Script-type split */
    APPEND(off, r, max,
        "<h3>UTXOs by script type</h3>"
        "<table class='txlist'>"
        "<tr><th>Type</th><th>UTXOs</th><th>Value (ZCL)</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT script_type, count(*), COALESCE(SUM(value),0) FROM utxos "
            "GROUP BY script_type ORDER BY count(*) DESC";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int st = sqlite3_column_int(s, 0);
                int64_t n = sqlite3_column_int64(s, 1);
                int64_t v = sqlite3_column_int64(s, 2);
                const char *label =
                    st == 0 ? "P2PKH" : st == 1 ? "P2SH" :
                    st == 2 ? "OP_RETURN" : st == 3 ? "MULTISIG" : "OTHER";
                char n_s[32], v_s2[64];
                fmt_comma(n_s, sizeof(n_s), n);
                fmt_zcl(v_s2, sizeof(v_s2), v);
                APPEND(off, r, max,
                    "<tr><td>%s</td><td>%s</td><td>%s</td></tr>",
                    label, n_s, v_s2);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
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
        /* Checkpoint data: heights + consensus checkpoint hashes. These are
         * immutable blockchain facts — no SQLite dependency. */
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

    /* ── Block-interval records (NEW): all-time vs recent cadence, plus the
     * share of near-instant and very-slow blocks. One combined self-join so
     * both counts come from a single consistent pass. */
    {
        int64_t tip = fq_i64(db, "SELECT COALESCE(MAX(height),0) FROM blocks");
        int64_t tip_time = fq_i64(db,
            "SELECT time FROM blocks WHERE height = (SELECT MAX(height) FROM blocks)");
        double avg_all = tip > 0
            ? (double)(tip_time - ZCL_EXPLORER_GENESIS_TIME) / (double)tip : 0.0;
        double avg_recent = 0.0;
        {
            char sql[160];
            snprintf(sql, sizeof(sql),
                "SELECT time FROM blocks WHERE height = %" PRId64,
                tip > 10000 ? tip - 10000 : (int64_t)0);
            int64_t t10k = fq_i64(db, sql);
            int64_t span = tip > 10000 ? 10000 : tip;
            if (span > 0 && t10k > 0)
                avg_recent = (double)(tip_time - t10k) / (double)span;
        }

        int64_t fast10 = 0, slow1h = 0, pairs = 0;
        {
            sqlite3_stmt *s = NULL;
            const char *sql =
                "SELECT "
                "SUM(CASE WHEN d > 0 AND d < 10 THEN 1 ELSE 0 END), "
                "SUM(CASE WHEN d > 3600 THEN 1 ELSE 0 END), "
                "count(*) FROM ("
                "SELECT b.time - a.time AS d FROM blocks a "
                "JOIN blocks b ON b.height = a.height + 1 "
                "WHERE a.time > 0 AND b.time > 0)";
            if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
                if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                    fast10 = sqlite3_column_int64(s, 0);
                    slow1h = sqlite3_column_int64(s, 1);
                    pairs  = sqlite3_column_int64(s, 2);
                }
                sqlite3_finalize(s);
            }
        }

        double fast_pct = pairs > 0 ? (double)fast10 * 100.0 / (double)pairs : 0.0;
        char f_s[32], sl_s[32], rcpt[32] = "";
        fmt_comma(f_s, sizeof(f_s), fast10);
        fmt_comma(sl_s, sizeof(sl_s), slow1h);
        compute_receipt_i64(rcpt, sizeof(rcpt), fast10, slow1h, "blocktime_records");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>Block Interval Records</h3>"
            "<table>"
            "<tr><td><b>All-time average interval:</b></td><td>%.1fs</td></tr>"
            "<tr><td><b>Recent (last 10k) average:</b></td><td>%.1fs</td></tr>"
            "<tr><td><b>Blocks within 10s of the previous:</b></td>"
            "<td>%s (%.1f%%)</td></tr>"
            "<tr><td><b>Blocks over 1 hour apart:</b></td><td>%s</td></tr>"
            "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
            "</table></div>",
            avg_all, avg_recent, f_s, fast_pct, sl_s, rcpt);
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
        /* CANONICAL tx counts. The transactions projection retains ~2,501
         * orphan coinbase rows from reorged-out blocks that inflate the raw
         * counts above the block count. Count coinbase by distinct canonical
         * height (== block count) and total by joining to the canonical
         * blocks table on block_hash, so neither double-counts orphans. */
        int64_t total_txs = fq_i64(db,
            "SELECT count(*) FROM transactions t "
            "JOIN blocks b ON t.block_hash = b.hash");
        int64_t coinbase_txs = fq_i64(db,
            "SELECT count(DISTINCT block_height) FROM transactions "
            "WHERE is_coinbase = 1");
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

    /* ── Transaction records (NEW) ────────────────────────────────── */
    {
        struct sql_row_i64_2 moved = {0};
        sql_query_row_i64_2(db,
            "SELECT COALESCE(SUM(value),0), count(*) FROM tx_outputs", &moved);
        int64_t max_out_val = fq_i64(db, "SELECT COALESCE(MAX(value),0) FROM tx_outputs");
        /* Largest tx by output count: MAX(vout)+1 is budget-safe; the
         * obvious GROUP BY txid over 11M+ rows times out. */
        int64_t max_vout = fq_i64(db, "SELECT COALESCE(MAX(vout),0) FROM tx_outputs");
        int64_t big_tx_height = 0;
        {
            char sql[160];
            snprintf(sql, sizeof(sql),
                "SELECT block_height FROM tx_outputs WHERE vout = %" PRId64 " LIMIT 1",
                max_vout);
            big_tx_height = fq_i64(db, sql);
        }
        int64_t big_tx_outputs = max_vout + 1;
        int64_t first_noncb = fq_i64(db,
            "SELECT MIN(block_height) FROM transactions WHERE is_coinbase = 0");
        int64_t n_inputs = fq_i64(db, "SELECT count(*) FROM tx_inputs");
        int64_t n_spending = fq_i64(db,
            "SELECT count(*) FROM transactions WHERE is_coinbase = 0");
        double avg_in = n_spending > 0 ? (double)n_inputs / (double)n_spending : 0.0;

        char mv_s[64], nout_s[32], mout_s[64], bo_s[32], rcpt[32] = "";
        fmt_zcl(mv_s, sizeof(mv_s), moved.v0);
        fmt_comma(nout_s, sizeof(nout_s), moved.v1);
        fmt_zcl(mout_s, sizeof(mout_s), max_out_val);
        fmt_comma(bo_s, sizeof(bo_s), big_tx_outputs);
        compute_receipt_i64(rcpt, sizeof(rcpt), moved.v0, moved.v1, "tx_records");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>Records</h3>"
            "<table>"
            "<tr><td><b>Total transparent value ever moved:</b></td>"
            "<td>%s ZCL across %s outputs</td></tr>"
            "<tr><td><b>Largest tx by output count:</b></td>"
            "<td>%s outputs at block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td></tr>"
            "<tr><td><b>Largest single transparent output (all-time):</b></td>"
            "<td>%s ZCL</td></tr>"
            "<tr><td><b>First non-coinbase transaction:</b></td>"
            "<td>block <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td></tr>"
            "<tr><td><b>Avg inputs per spending tx:</b></td><td>%.2f</td></tr>"
            "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
            "</table></div>",
            mv_s, nout_s,
            bo_s, big_tx_height, big_tx_height,
            mout_s,
            first_noncb, first_noncb,
            avg_in, rcpt);
    }

    /* Output script-type split (NEW) */
    APPEND(off, r, max,
        "<h3>Outputs by script type</h3>"
        "<table class='txlist'>"
        "<tr><th>Type</th><th>Outputs</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT script_type, count(*) FROM tx_outputs "
            "GROUP BY script_type ORDER BY count(*) DESC";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int st = sqlite3_column_int(s, 0);
                int64_t n = sqlite3_column_int64(s, 1);
                const char *label =
                    st == 0 ? "P2PKH" : st == 1 ? "P2SH" :
                    st == 2 ? "OP_RETURN" : st == 3 ? "MULTISIG" : "OTHER";
                char n_s[32];
                fmt_comma(n_s, sizeof(n_s), n);
                APPEND(off, r, max,
                    "<tr><td>%s</td><td>%s</td></tr>", label, n_s);
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

    /* ── Records (NEW): busiest block + longest empty-block run ────── */
    {
        int64_t max_ntx = fq_i64(db, "SELECT COALESCE(MAX(num_tx),0) FROM blocks");
        int64_t busiest_h = 0;
        {
            char sql[128];
            snprintf(sql, sizeof(sql),
                "SELECT height FROM blocks WHERE num_tx = %" PRId64 " LIMIT 1",
                max_ntx);
            busiest_h = fq_i64(db, sql);
        }

        /* Longest run of consecutive empty blocks. The gaps-and-islands
         * window over the 2.2M empty blocks times out; the budget-safe
         * inversion is the largest gap between consecutive NON-empty
         * (num_tx>1) blocks. One window pass returns the ending non-empty
         * block + that gap, from which the run bounds follow. */
        struct sql_row_i64_2 run = {0};
        sql_query_row_i64_2(db,
            "SELECT height, gap FROM ("
            "SELECT height, height - LAG(height) OVER (ORDER BY height) AS gap "
            "FROM blocks WHERE num_tx > 1) ORDER BY gap DESC LIMIT 1", &run);
        int64_t run_len = run.v1 > 0 ? run.v1 - 1 : 0;
        int64_t run_start = run.v0 - run.v1 + 1;
        int64_t run_end = run.v0 - 1;

        char bn_s[32], rl_s[32], rcpt[32] = "";
        fmt_comma(bn_s, sizeof(bn_s), max_ntx);
        fmt_comma(rl_s, sizeof(rl_s), run_len);
        compute_receipt_i64(rcpt, sizeof(rcpt), busiest_h, run_len,
                            "empty_block_records");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>Records</h3>"
            "<p><b>Busiest block ever:</b> %s transactions at block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>"
            "<p><b>Longest run of consecutive empty blocks:</b> %s "
            "(heights %" PRId64 "\xe2\x80\x93%" PRId64 ")</p>"
            "<p><b>SHA3:</b> <code>%s</code></p>"
            "</div>",
            bn_s, busiest_h, busiest_h,
            rl_s, run_start, run_end, rcpt);
    }
    return off;
}

size_t factoids_emit_section_16_difficulty(uint8_t *buf, size_t cap, size_t off,
                                           sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='difficulty'>16. Difficulty History</h2>");

    /* ── Difficulty records (NEW) — rendered from DECODED compact-bits
     * targets and the stored chain_work blob, NOT difficulty_from_bits
     * (which is under separate review for an inverted exponent). All the
     * math here is display-only and consensus-neutral. */
    {
        int64_t tip = fq_i64(db, "SELECT COALESCE(MAX(height),0) FROM blocks");

        /* One scan: global hardest target (MIN bits) + powLimit-floor stats
         * (0x1f07ffff = 520617983, the maximum target / minimum difficulty). */
        int64_t min_bits = 0, pl_count = 0, pl_last = 0;
        {
            sqlite3_stmt *s = NULL;
            const char *sql =
                "SELECT MIN(bits), "
                "SUM(CASE WHEN bits = 520617983 THEN 1 ELSE 0 END), "
                "MAX(CASE WHEN bits = 520617983 THEN height END) "
                "FROM blocks WHERE bits > 0";
            if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
                if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                    min_bits = sqlite3_column_int64(s, 0);
                    pl_count = sqlite3_column_int64(s, 1);
                    pl_last  = sqlite3_column_int64(s, 2);
                }
                sqlite3_finalize(s);
            }
        }
        int64_t hard_h = 0, hard_t = 0;
        {
            char sql[160];
            snprintf(sql, sizeof(sql),
                "SELECT height, time FROM blocks WHERE bits = %" PRId64 " "
                "ORDER BY height LIMIT 1", min_bits);
            struct sql_row_i64_2 row = {0};
            if (sql_query_row_i64_2(db, sql, &row)) { hard_h = row.v0; hard_t = row.v1; }
        }
        int64_t tip_bits = 0;
        {
            char sql[128];
            snprintf(sql, sizeof(sql),
                "SELECT bits FROM blocks WHERE height = %" PRId64, tip);
            tip_bits = fq_i64(db, sql);
        }
        /* Recent retarget uniqueness — bounded window (the all-time
         * COUNT(DISTINCT bits) over 3.16M rows exceeds the page budget). */
        struct sql_row_i64_2 uniq = {0};
        sql_query_row_i64_2(db,
            "SELECT count(DISTINCT bits), count(*) FROM blocks WHERE height >= 3000000",
            &uniq);
        /* Cumulative chain-work at the tip (stored little-endian blob). */
        char cw_hex[80] = "";
        {
            char sql[128];
            snprintf(sql, sizeof(sql),
                "SELECT hex(chain_work) FROM blocks WHERE height = %" PRId64, tip);
            fq_text(db, sql, cw_hex, sizeof(cw_hex));
        }

        double tgt_peak = cd_target_from_bits((uint32_t)min_bits);
        double tgt_now  = cd_target_from_bits((uint32_t)tip_bits);
        double fall = (tgt_peak > 0.0) ? tgt_now / tgt_peak : 0.0;
        double cw = cd_chainwork_to_double(cw_hex);
        int cw_exp10 = (cw > 0.0) ? (int)floor(log10(cw)) : 0;
        double cw_mant = (cw > 0.0) ? cw / pow(10.0, (double)cw_exp10) : 0.0;
        double cw_log2 = (cw > 0.0) ? log2(cw) : 0.0;
        double uniq_pct = uniq.v1 > 0 ? (double)uniq.v0 * 100.0 / (double)uniq.v1 : 0.0;

        char hard_ts[64], fall_s[32], pl_s[32], uq_s[32], ub_s[32], rcpt[32] = "";
        fmt_time(hard_ts, sizeof(hard_ts), hard_t);
        fmt_comma(fall_s, sizeof(fall_s), (int64_t)(fall + 0.5));
        fmt_comma(pl_s, sizeof(pl_s), pl_count);
        fmt_comma(uq_s, sizeof(uq_s), uniq.v0);
        fmt_comma(ub_s, sizeof(ub_s), uniq.v1);
        compute_receipt(rcpt, sizeof(rcpt), hard_h, "", "hardest_block");

        APPEND(off, r, max,
            "<div class='card'>"
            "<h3>Difficulty Records</h3>"
            "<p><b>All-time hardest block:</b> compact target "
            "<code>0x%08" PRIx64 "</code> at block "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a> (%s)</p>"
            "<p><b>Easiest (powLimit) target <code>0x1f07ffff</code>:</b> used "
            "by %s blocks (genesis through block %" PRId64 ")</p>"
            "<p><b>Per-block retarget:</b> %s distinct targets across the most "
            "recent %s blocks (%.1f%% unique)</p>"
            "<p><b>Cumulative chain-work at tip:</b> ~%.2f &times; "
            "10<sup>%d</sup> (~2<sup>%.1f</sup>)</p>"
            "<p><b>Difficulty vs. the Feb-2018 peak:</b> ~%s&times; lower "
            "(from decoded compact-bits targets)</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            (uint64_t)((uint32_t)min_bits), hard_h, hard_h, hard_ts,
            pl_s, pl_last,
            uq_s, ub_s, uniq_pct,
            cw_mant, cw_exp10, cw_log2,
            fall_s, rcpt);
    }

    APPEND(off, r, max,
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
            /* compute_integrity_hash covers heights > (chain_height-100),
             * i.e. chain_height-99 .. chain_height = exactly 100 blocks.
             * The printed lower bound must be chain_height-99 (was -100,
             * which advertised 101 heights). */
            chain_height > 100 ? chain_height - 99 : (int64_t)1, chain_height,
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
