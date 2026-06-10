/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer stats VIEW: phase-1 data gathering.
 *
 * Part of the deep-stats page split: this TU owns the heavier phase-1
 * aggregate queries that fill a struct stats_ctx / struct stats_chart_data
 * (deep chain queries + 40-bucket chart aggregates over 5 time ranges).
 * The public entry point (explorer_stats_build) and the lighter inline
 * phase-1 queries live in explorer_stats_view.c; the section emitters live
 * in explorer_stats_sections.c. Shared data structs come from
 * views/explorer_stats_internal.h. */

#include "views/explorer_stats_internal.h"

/* Phase 1h: deep chain queries (tx_outputs, joinsplits, sapling_*,
 * sprout_nullifiers, view_integrity, firsts & records). Writes
 * directly into the supplied stats_ctx. */
void gather_deep_chain_data(sqlite3 *db, struct stats_ctx *c)
{
    printf("Stats: querying deep chain data...\n"); fflush(stdout);

    /* Transaction I/O */
    c->total_outputs = stats_q_i64(db, "SELECT count(*) FROM tx_outputs");
    c->total_inputs  = stats_q_i64(db, "SELECT count(*) FROM tx_inputs");
    c->p2pkh_outputs = stats_q_i64(db, "SELECT count(*) FROM tx_outputs WHERE script_type=0");
    c->p2sh_outputs  = stats_q_i64(db, "SELECT count(*) FROM tx_outputs WHERE script_type=1");
    c->max_output_value = stats_q_i64(db, "SELECT MAX(value) FROM tx_outputs");
    c->total_value_moved = stats_q_i64(db, "SELECT COALESCE(SUM(value),0) FROM tx_outputs");

    /* Shielded deep dive (per-tx tables) */
    c->total_joinsplits   = stats_q_i64(db, "SELECT count(*) FROM joinsplits");
    c->total_vpub_old     = stats_q_i64(db, "SELECT COALESCE(SUM(vpub_old),0) FROM joinsplits");
    c->total_vpub_new     = stats_q_i64(db, "SELECT COALESCE(SUM(vpub_new),0) FROM joinsplits");
    c->total_sap_spends   = stats_q_i64(db, "SELECT count(*) FROM sapling_spends");
    c->total_sap_outputs  = stats_q_i64(db, "SELECT count(*) FROM sapling_outputs");
    c->total_sprout_nulls = stats_q_i64(db, "SELECT count(*) FROM sprout_nullifiers");
    c->unique_sap_anchors = stats_q_i64(db, "SELECT count(DISTINCT anchor) FROM sapling_spends");

    /* SHA3 integrity chain */
    c->integrity_count = stats_q_i64(db, "SELECT count(*) FROM view_integrity");
    c->integrity_min_h = stats_q_i64(db, "SELECT MIN(height) FROM view_integrity");
    c->integrity_max_h = stats_q_i64(db, "SELECT MAX(height) FROM view_integrity");
    snprintf(c->integrity_latest_hash, sizeof(c->integrity_latest_hash), "N/A");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT hex(sha3_hash) FROM view_integrity ORDER BY height DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const char *h = (const char *)sqlite3_column_text(s, 0);
                if (h) snprintf(c->integrity_latest_hash, sizeof(c->integrity_latest_hash), "%s", h);
            }
            sqlite3_finalize(s);
        }
    }

    /* Chain firsts & records */
    c->first_noncoinbase = stats_q_i64(db,
        "SELECT MIN(block_height) FROM transactions WHERE is_coinbase=0");
    struct explorer_first_privacy_heights first_privacy = {0};
    explorer_query_first_privacy_heights(db, &first_privacy);
    c->first_joinsplit_h = first_privacy.joinsplit_height;
    c->first_sapling_h = first_privacy.sapling_height;
    c->first_opreturn_h = stats_q_i64(db,
        "SELECT MIN(block_height) FROM op_returns");
    c->first_zslp_h = stats_q_i64(db,
        "SELECT MIN(genesis_height) FROM zslp_tokens");

    c->most_js_block = 0; c->most_js_count = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT block_height, count(*) as cnt FROM joinsplits "
                "GROUP BY block_height ORDER BY cnt DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->most_js_block = sqlite3_column_int64(s, 0);
                c->most_js_count = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
    c->most_sap_out_block = 0; c->most_sap_out_count = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT block_height, count(*) as cnt FROM sapling_outputs "
                "GROUP BY block_height ORDER BY cnt DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->most_sap_out_block = sqlite3_column_int64(s, 0);
                c->most_sap_out_count = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
    c->fastest_block_h = 0; c->fastest_block_gap = 0;
    c->slowest_block_h = 0; c->slowest_block_gap = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT b2.height, b2.time - b1.time as gap "
                "FROM blocks b1 JOIN blocks b2 ON b2.height = b1.height + 1 "
                "WHERE b1.time > 0 AND b2.time > b1.time "
                "ORDER BY gap ASC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->fastest_block_h   = sqlite3_column_int64(s, 0);
                c->fastest_block_gap = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
        s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT b2.height, b2.time - b1.time as gap "
                "FROM blocks b1 JOIN blocks b2 ON b2.height = b1.height + 1 "
                "WHERE b1.time > 0 AND b2.time > b1.time "
                "ORDER BY gap DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->slowest_block_h   = sqlite3_column_int64(s, 0);
                c->slowest_block_gap = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
}

/* Phase 1i: 40-bucket aggregates over 5 time ranges (24h/7d/30d/1y/all).
 * Single batch query per range. */
void gather_chart_data(sqlite3 *db, int tip, double diff,
                              struct stats_chart_data *out)
{
    printf("Stats: computing chart data...\n"); fflush(stdout);
    struct { const char *label; const char *id; int blocks; } ranges[] = {
        {"24h", "24h", 576}, {"7d", "7d", 4032}, {"30d", "30d", 17280},
        {"1yr", "1y", 210240}, {"All", "all", tip},
    };

    for (int ri = 0; ri < 5; ri++) {
        int total = ranges[ri].blocks;
        if (total > tip) total = tip;
        int step = total / 40;
        if (step < 1) step = 1;
        int base = tip - total;
        if (base < 0) base = 0;

        /* Batch: get all 40 data points in one query using
         * (height - base) / step as bucket index */
        for (int i = 0; i < 40; i++) {
            out->diff_data[ri][i] = diff;
            out->hr_data[ri][i] = diff * 8192.0 / 150.0;
            out->sprout_c[ri][i] = 0;
            out->sapling_c[ri][i] = 0;
            out->txcount[ri][i] = 0;
        }

        /* Single query: aggregate by bucket */
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT (height - %d) / %d AS bucket, "
            "MAX(bits), "
            "COALESCE(SUM(sprout_value),0), "
            "COALESCE(SUM(sapling_value),0), "
            "COALESCE(SUM(num_tx),0) "
            "FROM blocks WHERE height > %d AND height <= %d "
            "GROUP BY bucket ORDER BY bucket",
            base, step, base, tip);
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int bucket = sqlite3_column_int(s, 0);
                if (bucket < 0 || bucket >= 40) continue;
                uint32_t bits = (uint32_t)sqlite3_column_int(s, 1);
                if (bits > 0) {
                    out->diff_data[ri][bucket] = explorer_difficulty_from_bits(bits);
                    out->hr_data[ri][bucket] = out->diff_data[ri][bucket] * 8192.0 / 150.0;
                }
                /* Sprout: positive=shielding, show as positive on chart (pool inflow)
                 * Sapling: negative=shielding, negate to show inflow as positive */
                out->sprout_c[ri][bucket]  = (double)sqlite3_column_int64(s, 2) / 1e8;
                out->sapling_c[ri][bucket] = -(double)sqlite3_column_int64(s, 3) / 1e8;
                out->txcount[ri][bucket]   = (double)sqlite3_column_int64(s, 4);
            }
            sqlite3_finalize(s);
        }
        for (int i = 0; i < 40; i++) {
            int h = base + (i + 1) * step;
            if (h > tip) h = tip;
            snprintf(out->labels[ri][i], sizeof(out->labels[ri][i]), "%d", h);
        }
    }
}
