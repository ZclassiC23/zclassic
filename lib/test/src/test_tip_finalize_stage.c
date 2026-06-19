/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Unit tests for Wave S S-9 tip_finalize stage. */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "json/json.h"
#include "jobs/tip_finalize_stage.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TF_CHECK(name, expr) do { \
    printf("tip_finalize: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

enum tf_fail_kind {
    TF_FAIL_NONE = 0,
    TF_FAIL_REORG,
    TF_FAIL_PRECONDITION,
    TF_FAIL_UTXO_COUNT,
};

struct synth_chain_tf {
    struct block_index *blocks;
    struct uint256     *hashes;
    int n;
    int upstream_fail_height;
    enum tf_fail_kind fail_kind;
};

static int mkdir_p_tf(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void synthetic_hash(struct uint256 *out, int h)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0xa0 + h);
}

static bool synth_chain_tf_build(struct synth_chain_tf *sc, int n)
{
    sc->blocks = calloc((size_t)n, sizeof(struct block_index));
    sc->hashes = calloc((size_t)n, sizeof(struct uint256));
    if (!sc->blocks || !sc->hashes) return false;
    for (int i = 0; i < n; i++) {
        synthetic_hash(&sc->hashes[i], i);
        block_index_init(&sc->blocks[i]);
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = 4;
        sc->blocks[i].nTime = (uint32_t)(1700005000u + (uint32_t)i);
        sc->blocks[i].nBits = 0x1f07ffff;
        sc->blocks[i].nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        arith_uint256_set_u64(&sc->blocks[i].nChainWork,
                              (uint64_t)i + 1);
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    if (sc->fail_kind == TF_FAIL_PRECONDITION && n > 2)
        sc->blocks[2].nStatus = BLOCK_VALID_SCRIPTS;
    if (sc->fail_kind == TF_FAIL_REORG && n > 2)
        sc->blocks[2].pprev = NULL;
    sc->n = n;
    return true;
}

static void synth_chain_tf_free(struct synth_chain_tf *sc)
{
    free(sc->blocks);
    free(sc->hashes);
    memset(sc, 0, sizeof(*sc));
}

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

static bool seed_utxo_apply(sqlite3 *db, int n, int upstream_fail_height)
{
    if (!exec_sql(db,
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "  height               INTEGER PRIMARY KEY,"
        "  status               TEXT    NOT NULL,"
        "  ok                   INTEGER NOT NULL,"
        "  spent_count          INTEGER NOT NULL,"
        "  added_count          INTEGER NOT NULL,"
        "  total_value_delta    INTEGER NOT NULL,"
        "  first_failure_kind   TEXT,"
        "  first_failure_detail BLOB,"
        "  applied_at           INTEGER NOT NULL"
        ")"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxo_apply_log "
        "(height, status, ok, spent_count, added_count, "
        " total_value_delta, applied_at) "
        "VALUES (?, ?, ?, 1, 2, 1, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h < n; h++) {
        int ok = (h == upstream_fail_height) ? 0 : 1;
        sqlite3_bind_int(st, 1, h);
        sqlite3_bind_text(st, 2, ok ? "verified" : "value_overflow",
                          -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, ok);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return false;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
        "VALUES('utxo_apply', ?, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, n);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool seed_coins_applied_height(sqlite3 *db, int32_t height)
{
    if (!progress_meta_table_ensure(db))
        return false;
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    bool ok = coins_kv_set_applied_height_in_tx(db, height);
    const char *finish = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, finish, NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    return ok;
}

/* Seed a script_validate_log row (the reducer's authoritative script verdict)
 * with an explicit ok and block_hash, so the tip_finalize hash-guarded fallback
 * can be exercised: a matching hash + ok=1 heals a drifted block_index bit; a
 * mismatched hash (stale orphan row) or ok=0 must be rejected. */
static bool seed_script_log(sqlite3 *db, int height, int ok,
                            const struct uint256 *block_hash)
{
    if (!exec_sql(db,
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL,"
        "  tx_count INTEGER NOT NULL, input_count INTEGER NOT NULL,"
        "  first_failure_txid BLOB, first_failure_vin INTEGER,"
        "  first_failure_serror INTEGER, validated_at INTEGER NOT NULL,"
        "  block_hash BLOB)"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO script_validate_log "
        "(height, status, ok, tx_count, input_count, validated_at, block_hash) "
        "VALUES (?, 'verified', ?, 1, 0, 1, ?)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok);
    if (block_hash)
        sqlite3_bind_blob(st, 3, block_hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 3);
    bool done = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return done;
}

static bool log_row_at(sqlite3 *db, int height, int *out_ok,
                       char *out_status, size_t status_size,
                       int *out_reorg_depth, int64_t *out_utxo_size)
{
    *out_ok = -1;
    if (out_status && status_size) out_status[0] = 0;
    if (out_reorg_depth) *out_reorg_depth = -1;
    if (out_utxo_size) *out_utxo_size = -2;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, status, reorg_depth, utxo_size_after "
        "FROM tip_finalize_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt && out_status && status_size)
            snprintf(out_status, status_size, "%s", (const char *)txt);
        if (out_reorg_depth)
            *out_reorg_depth = sqlite3_column_int(st, 2);
        if (out_utxo_size)
            *out_utxo_size = sqlite3_column_int64(st, 3);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

static bool log_tip_hash_at(sqlite3 *db, int height, struct uint256 *out)
{
    uint256_set_null(out);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT tip_hash FROM tip_finalize_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (blob && n == 32) {
            memcpy(out->data, blob, 32);
            found = true;
        }
    }
    sqlite3_finalize(st);
    return found;
}

static uint64_t cursor_at(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    uint64_t out = 0;
    if (sqlite3_prepare_v2(db,
        "SELECT cursor FROM stage_cursor WHERE name = ?",
        -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        out = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return out;
}

static bool fake_utxo_count(int height_after, int64_t *out_count, void *user)
{
    struct synth_chain_tf *sc = user;
    if (sc && sc->fail_kind == TF_FAIL_UTXO_COUNT && height_after == 2) {
        *out_count = 99;
        return true;
    }
    *out_count = height_after;
    return true;
}

static int tf_setup(const char *tag, int log_rows,
                    enum tf_fail_kind fail_kind,
                    int upstream_fail_height,
                    char *dir_out, size_t dir_out_size,
                    struct main_state *ms, struct synth_chain_tf *sc)
{
    test_fmt_tmpdir(dir_out, dir_out_size, "tip_finalize", tag);
    mkdir_p_tf("./test-tmp");
    mkdir_p_tf(dir_out);
    if (!progress_store_open(dir_out)) return 1;

    memset(sc, 0, sizeof(*sc));
    sc->fail_kind = fail_kind;
    sc->upstream_fail_height = upstream_fail_height;
    memset(ms, 0, sizeof(*ms));
    main_state_init(ms);
    if (!synth_chain_tf_build(sc, log_rows + 1)) return 2;
    for (int i = 0; i <= log_rows; i++) {
        if (!block_map_insert(&ms->map_block_index, sc->blocks[i].phashBlock,
                              &sc->blocks[i]))
            return 2;
    }
    active_chain_move_window_tip(&ms->chain_active, &sc->blocks[log_rows]);
    if (fail_kind == TF_FAIL_REORG && ms->chain_active.chain) {
        for (int i = 0; i <= log_rows; i++)
            ms->chain_active.chain[i] = &sc->blocks[i];
        ms->chain_active.height = log_rows;
    }

    if (!seed_utxo_apply(progress_store_db(), log_rows,
                         upstream_fail_height))
        return 3;
    if (!tip_finalize_stage_init(ms)) return 4;
    tip_finalize_stage_set_utxo_counter(fake_utxo_count, sc);
    return 0;
}

static void tf_teardown(const char *dir, struct main_state *ms,
                        struct synth_chain_tf *sc)
{
    tip_finalize_stage_shutdown();
    main_state_free(ms);
    synth_chain_tf_free(sc);
    progress_store_close();
    test_cleanup_tmpdir(dir);
}

int test_tip_finalize_stage(void);
int test_tip_finalize_stage(void)
{
    printf("\n=== tip_finalize_stage tests ===\n");
    int failures = 0;

    blocker_module_init();

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("authority_guard: setup",
                 tf_setup("authority_guard", 3, TF_FAIL_NONE, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        TF_CHECK("authority_guard: seeded from restored tip",
                 active_chain_height(&ms.chain_active) == 3);
        TF_CHECK("authority_guard: raw low-level tip write succeeds",
                 active_chain_move_window_tip(&ms.chain_active, &sc.blocks[1]));
        TF_CHECK("authority_guard: public height unchanged",
                 active_chain_height(&ms.chain_active) == 3);
        TF_CHECK("authority_guard: reducer height unchanged",
                 tip_finalize_stage_last_height() == 3);
        TF_CHECK("authority_guard: public tip unchanged",
                 active_chain_tip(&ms.chain_active) == &sc.blocks[3]);
        TF_CHECK("authority_guard: cache records raw local write",
                 active_chain_cached_tip(&ms.chain_active) == &sc.blocks[1]);
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        bool ok_setup = true;
        test_fmt_tmpdir(dir, sizeof(dir), "tip_finalize", "stale_cursor");
        mkdir_p_tf("./test-tmp");
        mkdir_p_tf(dir);
        ok_setup = ok_setup && progress_store_open(dir);
        memset(&sc, 0, sizeof(sc));
        memset(&ms, 0, sizeof(ms));
        main_state_init(&ms);
        ok_setup = ok_setup && synth_chain_tf_build(&sc, 6);
        for (int i = 0; ok_setup && i <= 5; i++)
            ok_setup = block_map_insert(&ms.map_block_index,
                                        sc.blocks[i].phashBlock,
                                        &sc.blocks[i]);
        ok_setup = ok_setup &&
            active_chain_move_window_tip(&ms.chain_active, &sc.blocks[5]);
        ok_setup = ok_setup && seed_utxo_apply(progress_store_db(), 3, -1);
        ok_setup = ok_setup &&
            seed_coins_applied_height(progress_store_db(), 3);
        ok_setup = ok_setup && exec_sql(progress_store_db(),
            "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
            "VALUES('tip_finalize', 2, 1)");
        ok_setup = ok_setup && tip_finalize_stage_init(&ms);
        tip_finalize_stage_set_utxo_counter(fake_utxo_count, &sc);
        TF_CHECK("stale_cursor: setup", ok_setup);
        /* The anchor floor is the restored tip's OWN height (5), never
         * tip+1: stamping 6 would claim the unfinalized 5→6 transition and
         * skip it forever (the served-tip-trails-by-one defect). */
        TF_CHECK("stale_cursor: cursor anchored at restored tip",
                 tip_finalize_stage_cursor() == 5);
        TF_CHECK("stale_cursor: header_admit not anchored from active tip",
                 cursor_at(progress_store_db(), "header_admit") == 0);
        TF_CHECK("stale_cursor: validate_headers not anchored from active tip",
                 cursor_at(progress_store_db(), "validate_headers") == 0);
        TF_CHECK("stale_cursor: body_fetch not anchored from active tip",
                 cursor_at(progress_store_db(), "body_fetch") == 0);
        TF_CHECK("stale_cursor: body_persist not anchored from active tip",
                 cursor_at(progress_store_db(), "body_persist") == 0);
        TF_CHECK("stale_cursor: script_validate not anchored from active tip",
                 cursor_at(progress_store_db(), "script_validate") == 0);
        TF_CHECK("stale_cursor: proof_validate not anchored from active tip",
                 cursor_at(progress_store_db(), "proof_validate") == 0);
        TF_CHECK("stale_cursor: utxo_apply remains at coins frontier",
                 cursor_at(progress_store_db(), "utxo_apply") == 3);
        TF_CHECK("stale_cursor: public height remains restored tip",
                 active_chain_height(&ms.chain_active) == 5);
        int row_ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        TF_CHECK("stale_cursor: anchor row written",
                 log_row_at(progress_store_db(), 5, &row_ok, status,
                            sizeof(status), &depth, &utxos));
        TF_CHECK("stale_cursor: anchor row ok",
                 row_ok == 1 && strcmp(status, "anchor") == 0);
        TF_CHECK("stale_cursor: stale lower rows do not replay",
                 tip_finalize_stage_step_once() == JOB_IDLE);
        /* CS-F3: the repeated cursor_in>utxo_apply gap step still idles —
         * the per-tick WARN is throttled but the verdict is unchanged. */
        TF_CHECK("stale_cursor: repeat gap step stays IDLE",
                 tip_finalize_stage_step_once() == JOB_IDLE);
        TF_CHECK("stale_cursor: public height still restored tip",
                 active_chain_height(&ms.chain_active) == 5);
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("repair_replay: setup",
                 tf_setup("repair_replay", 3, TF_FAIL_NONE, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        TF_CHECK("repair_replay: public starts at restored tip",
                 active_chain_height(&ms.chain_active) == 3);
        TF_CHECK("repair_replay: force cursor below authority",
                 exec_sql(progress_store_db(),
                    "INSERT OR REPLACE INTO stage_cursor"
                    "(name, cursor, updated_at) "
                    "VALUES('tip_finalize', 1, 1)"));
        TF_CHECK("repair_replay: lower row replays",
                 tip_finalize_stage_step_once() == JOB_ADVANCED);
        TF_CHECK("repair_replay: cursor advanced",
                 tip_finalize_stage_cursor() == 2);
        TF_CHECK("repair_replay: public height did not regress",
                 active_chain_height(&ms.chain_active) == 3);
        TF_CHECK("repair_replay: authority height did not regress",
                 tip_finalize_stage_last_height() == 3);
        TF_CHECK("repair_replay: local window may rewind for replay",
                 active_chain_cached_tip(&ms.chain_active) == &sc.blocks[2]);
        int row_ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        TF_CHECK("repair_replay: replay row written",
                 log_row_at(progress_store_db(), 1, &row_ok, status,
                            sizeof(status), &depth, &utxos));
        TF_CHECK("repair_replay: replay row finalized",
                 row_ok == 1 && strcmp(status, "finalized") == 0);
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("happy: setup",
                 tf_setup("happy", 3, TF_FAIL_NONE, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        TF_CHECK("happy: authority seeded from restored tip",
                 active_chain_height(&ms.chain_active) == 3);
        TF_CHECK("happy: drains 3", tip_finalize_stage_drain(100) == 3);
        TF_CHECK("happy: cursor at 3", tip_finalize_stage_cursor() == 3);
        TF_CHECK("happy: finalized_total == 3",
                 tip_finalize_stage_finalized_total() == 3);
        TF_CHECK("happy: work_added_low == 3",
                 tip_finalize_stage_total_work_added_low() == 3);
        for (int h = 0; h < 3; h++) {
            int ok = -1, depth = -1; int64_t utxos = -1; char status[32];
            log_row_at(progress_store_db(), h, &ok, status, sizeof(status),
                       &depth, &utxos);
            TF_CHECK("happy: row ok=1", ok == 1);
            TF_CHECK("happy: row status finalized",
                     strcmp(status, "finalized") == 0);
            TF_CHECK("happy: row reorg_depth=0", depth == 0);
            TF_CHECK("happy: utxo size matches delta",
                     utxos == (int64_t)h + 1);
        }
        TF_CHECK("happy: next step IDLE",
                 tip_finalize_stage_step_once() == JOB_IDLE);
        tf_teardown(dir, &ms, &sc);
    }

    /* Live-frontier no-skip (task #30 root cause, forensic 2026-06-12 at
     * h=3144857): every chain_set_active_tip re-anchors the JUST-PUBLISHED
     * tip via set_authoritative_tip. The old height+1 anchor target bumped
     * the cursor past the pending tip→tip+1 transition, so each new block
     * could only be published when ITS successor arrived — the served tip
     * trailed the network by one full inter-block interval (the alternating
     * finalized/anchor lattice in tip_finalize_log). This block pins:
     * (a) the self re-anchor is a cursor no-op, (b) the successor publishes
     * on FIRST arrival, (c) the resolver returns a self-consistent pair in
     * the advance→anchor crash window, (d) a stale re-commit never
     * downgrades a finalized row to an anchor row. */
    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        bool ok_setup = true;
        test_fmt_tmpdir(dir, sizeof(dir), "tip_finalize", "noskip");
        mkdir_p_tf("./test-tmp");
        mkdir_p_tf(dir);
        ok_setup = ok_setup && progress_store_open(dir);
        memset(&sc, 0, sizeof(sc));
        memset(&ms, 0, sizeof(ms));
        main_state_init(&ms);
        ok_setup = ok_setup && synth_chain_tf_build(&sc, 5);
        /* Block 4 exists in the synth chain but has NOT "arrived" yet —
         * keep it out of the map so the most-work candidate is the tip. */
        for (int i = 0; ok_setup && i <= 3; i++)
            ok_setup = block_map_insert(&ms.map_block_index,
                                        sc.blocks[i].phashBlock,
                                        &sc.blocks[i]);
        ok_setup = ok_setup &&
            active_chain_move_window_tip(&ms.chain_active, &sc.blocks[3]);
        ok_setup = ok_setup && seed_utxo_apply(progress_store_db(), 3, -1);
        ok_setup = ok_setup && tip_finalize_stage_init(&ms);
        tip_finalize_stage_set_utxo_counter(fake_utxo_count, &sc);
        TF_CHECK("noskip: setup", ok_setup);
        TF_CHECK("noskip: drains to the tip", tip_finalize_stage_drain(100) == 3);
        TF_CHECK("noskip: cursor at tip", tip_finalize_stage_cursor() == 3);

        /* (a) The live-path re-anchor of the self-published tip. */
        tip_finalize_stage_set_authoritative_tip(3, sc.hashes[3].data);
        TF_CHECK("noskip: self re-anchor does not bump the cursor",
                 tip_finalize_stage_cursor() == 3);
        int row_ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        TF_CHECK("noskip: anchor row written at tip",
                 log_row_at(progress_store_db(), 3, &row_ok, status,
                            sizeof(status), &depth, &utxos));
        TF_CHECK("noskip: anchor row ok",
                 row_ok == 1 && strcmp(status, "anchor") == 0);
        TF_CHECK("noskip: idle while successor absent",
                 tip_finalize_stage_step_once() == JOB_IDLE);

        /* (b) Successor arrives: map + utxo witness, ONE step publishes it. */
        ok_setup = block_map_insert(&ms.map_block_index,
                                    sc.blocks[4].phashBlock, &sc.blocks[4]);
        ok_setup = ok_setup && seed_utxo_apply(progress_store_db(), 4, -1);
        TF_CHECK("noskip: successor arrival setup", ok_setup);
        TF_CHECK("noskip: successor publishes on FIRST arrival",
                 tip_finalize_stage_step_once() == JOB_ADVANCED);
        TF_CHECK("noskip: cursor advanced", tip_finalize_stage_cursor() == 4);
        TF_CHECK("noskip: successor is the published tip",
                 tip_finalize_stage_last_height() == 4);
        TF_CHECK("noskip: anchor row upgraded to finalized",
                 log_row_at(progress_store_db(), 3, &row_ok, status,
                            sizeof(status), &depth, &utxos) &&
                 row_ok == 1 && strcmp(status, "finalized") == 0);

        /* (c) Crash-window resolver: cursor=4, no row at 4 yet (the next
         * trusted-tip anchor has not landed). The naive cursor-1 raw read
         * would pair (3, hash(4)) — the splice-class poisoned pair; the
         * resolver must return (4, hash(4)). */
        int rh = -1; uint8_t rhash[32];
        TF_CHECK("noskip: resolver finds a durable tip",
                 tip_finalize_stage_resolve_durable_tip(progress_store_db(),
                                                        &rh, rhash));
        TF_CHECK("noskip: resolver pair is self-consistent",
                 rh == 4 && memcmp(rhash, sc.hashes[4].data, 32) == 0);

        /* (d) A stale authority re-commit of an already-finalized height
         * must not downgrade the finalized row (it carries the successor
         * hash block_hash_at reads) and must not move the cursor. */
        tip_finalize_stage_set_authoritative_tip(3, sc.hashes[3].data);
        TF_CHECK("noskip: stale re-commit keeps the finalized row",
                 log_row_at(progress_store_db(), 3, &row_ok, status,
                            sizeof(status), &depth, &utxos) &&
                 row_ok == 1 && strcmp(status, "finalized") == 0);
        TF_CHECK("noskip: stale re-commit keeps the cursor",
                 tip_finalize_stage_cursor() == 4);
        tf_teardown(dir, &ms, &sc);
    }

    {
        /* AUTHORITY PAIR SELF-CONSISTENCY (2026-06-11 height-splice fix):
         * after a finalize PUBLISH the served pair must be the tip block's
         * OWN (height, hash) — active_chain_tip()->nHeight ==
         * active_chain_height(). The pre-fix step path published
         * (next_h, hash(next_h+1)), leaving the authority height one BELOW
         * the block its own hash resolves to; accept_block_header's
         * label-trust install then turned that pair into a -1 header-graph
         * splice on a tip re-delivery. */
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("authority_pair: setup",
                 tf_setup("authority_pair", 3, TF_FAIL_NONE, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        TF_CHECK("authority_pair: drains 3",
                 tip_finalize_stage_drain(100) == 3);
        struct block_index *tip = active_chain_tip(&ms.chain_active);
        TF_CHECK("authority_pair: tip resolves to the served block",
                 tip == &sc.blocks[3]);
        TF_CHECK("authority_pair: height label == tip block's own height",
                 tip != NULL &&
                 active_chain_height(&ms.chain_active) == tip->nHeight);
        TF_CHECK("authority_pair: published authority did not regress",
                 tip_finalize_stage_last_height() == 3);
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("reorg: setup",
                 tf_setup("reorg", 3, TF_FAIL_REORG, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        TF_CHECK("reorg: first finalizes",
                 tip_finalize_stage_step_once() == JOB_ADVANCED);
        TF_CHECK("reorg: second logs and advances",
                 tip_finalize_stage_step_once() == JOB_ADVANCED);
        TF_CHECK("reorg: counter == 1",
                 tip_finalize_stage_reorg_detected_total() == 1);
        int ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        log_row_at(progress_store_db(), 1, &ok, status, sizeof(status),
                   &depth, &utxos);
        TF_CHECK("reorg: h=1 ok=0", ok == 0);
        TF_CHECK("reorg: h=1 status",
                 strcmp(status, "reorg_detected") == 0);
        TF_CHECK("reorg: depth > 0", depth > 0);
        TF_CHECK("reorg: cursor advances to 2",
                 tip_finalize_stage_cursor() == 2);
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("reorg_replay: setup",
                 tf_setup("reorg_replay", 3, TF_FAIL_NONE, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        TF_CHECK("reorg_replay: initial drain",
                 tip_finalize_stage_drain(100) == 3);
        TF_CHECK("reorg_replay: cursor at old tip",
                 tip_finalize_stage_cursor() == 3);

        sc.hashes[2].data[0] = 0xf2;
        sc.hashes[3].data[0] = 0xf3;
        sc.blocks[2].pprev = &sc.blocks[1];
        sc.blocks[3].pprev = &sc.blocks[2];
        arith_uint256_set_u64(&sc.blocks[2].nChainWork, 20);
        arith_uint256_set_u64(&sc.blocks[3].nChainWork, 30);
        TF_CHECK("reorg_replay: installs coherent fork",
                 active_chain_move_window_tip(&ms.chain_active, &sc.blocks[3]));

        TF_CHECK("reorg_replay: rewinds and replays fork block",
                 tip_finalize_stage_step_once() == JOB_ADVANCED);
        TF_CHECK("reorg_replay: cursor is fork+1",
                 tip_finalize_stage_cursor() == 2);
        TF_CHECK("reorg_replay: counter increments",
                 tip_finalize_stage_reorg_detected_total() == 1);
        int ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        struct uint256 logged;
        log_row_at(progress_store_db(), 1, &ok, status, sizeof(status),
                   &depth, &utxos);
        TF_CHECK("reorg_replay: row rewritten ok", ok == 1);
        TF_CHECK("reorg_replay: row rewritten finalized",
                 strcmp(status, "finalized") == 0);
        TF_CHECK("reorg_replay: row hash updated",
                 log_tip_hash_at(progress_store_db(), 1, &logged) &&
                 uint256_eq(&logged, sc.blocks[2].phashBlock));
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("precondition: setup",
                 tf_setup("precondition", 3, TF_FAIL_PRECONDITION, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        /* block[2] lacks BLOCK_HAVE_DATA -> finalizing height 1 sees a
         * TRANSIENT have_data_missing lookahead. Height 0 finalizes; the
         * stage then HOLDS at height 1 (JOB_IDLE) instead of stranding it by
         * advancing the cursor past an un-finalized height (the live
         * 3134304<->3134302 oscillation). */
        TF_CHECK("precondition: drains only height 0 (holds at 1)",
                 tip_finalize_stage_drain(100) == 1);
        TF_CHECK("precondition: cursor held at 1 (not advanced past)",
                 tip_finalize_stage_cursor() == 1);
        TF_CHECK("precondition: successor_pending counter fired",
                 tip_finalize_stage_successor_pending_total() == 1);
        TF_CHECK("precondition: genuine-fork counter NOT fired",
                 tip_finalize_stage_precondition_failed_total() == 0);
        int ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        TF_CHECK("precondition: NO junk row written at h=1",
                 log_row_at(progress_store_db(), 1, &ok, status,
                            sizeof(status), &depth, &utxos) == false);
        /* CS-F1 WARN-storm throttle: repeated holds on the UNCHANGED
         * (height,reason) pair are counted (precondition_repeat_count in the
         * zcl_state dump) instead of re-logging the WARN every idle tick. */
        TF_CHECK("precondition: repeat hold stays IDLE",
                 tip_finalize_stage_step_once() == JOB_IDLE);
        TF_CHECK("precondition: repeat hold stays IDLE x2",
                 tip_finalize_stage_step_once() == JOB_IDLE);
        {
            struct json_value v;
            json_init(&v);
            char buf[1536];
            bool dumped = tip_finalize_dump_state_json(&v, NULL);
            size_t n = dumped ? json_write(&v, buf, sizeof(buf)) : 0;
            TF_CHECK("precondition: repeat count == 2 in dump",
                     n > 0 && n < sizeof(buf) &&
                     strstr(buf, "\"precondition_repeat_count\":2") != NULL);
            json_free(&v);
        }
        /* Land the successor body: block[2] now has HAVE_DATA, so BOTH height 1
         * (lookahead = block[2]) and height 2 (lookahead = the always-ready
         * block[3]) become finalizable. The held frontier drains forward two
         * blocks; cursor lands at 3 (utxo_apply_log has rows only through 2). */
        sc.blocks[2].nStatus |= BLOCK_HAVE_DATA;
        TF_CHECK("precondition: drains 1 and 2 once successor lands",
                 tip_finalize_stage_drain(100) == 2 &&
                 tip_finalize_stage_cursor() == 3);
        tf_teardown(dir, &ms, &sc);
    }

    {
        /* The block_index BLOCK_VALID_SCRIPTS mirror drifted CLEAR for the
         * lookahead (block[2]) — the live 3134954 wedge. The reducer's
         * hash-bound script_validate_log proves the scripts WERE validated, so
         * the gate trusts the log, finalizes past it, and heals the bit. */
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("script_log_heal: setup",
                 tf_setup("script_log_heal", 3, TF_FAIL_NONE, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        sc.blocks[2].nStatus = BLOCK_HAVE_DATA;  /* VALID_SCRIPTS cleared */
        TF_CHECK("script_log_heal: seed matching ok=1 row",
                 seed_script_log(progress_store_db(), 2, 1,
                                 sc.blocks[2].phashBlock));
        TF_CHECK("script_log_heal: drains all 3 via authoritative log",
                 tip_finalize_stage_drain(100) == 3);
        TF_CHECK("script_log_heal: cursor at 3",
                 tip_finalize_stage_cursor() == 3);
        TF_CHECK("script_log_heal: drifted bit healed on block[2]",
                 (sc.blocks[2].nStatus & BLOCK_VALID_MASK) >=
                     BLOCK_VALID_SCRIPTS);
        tf_teardown(dir, &ms, &sc);
    }

    {
        /* REORG SAFETY: a stale ok=1 row left by an orphaned block that once
         * held height 2 carries a DIFFERENT block_hash. The height-keyed log
         * must NOT be trusted — finalizing the active (unvalidated) block here
         * would be a consensus break. The gate rejects the mismatch and holds. */
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("script_log_stale_reorg: setup",
                 tf_setup("script_log_stale", 3, TF_FAIL_NONE, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        sc.blocks[2].nStatus = BLOCK_HAVE_DATA;  /* VALID_SCRIPTS cleared */
        struct uint256 wrong; uint256_set_null(&wrong); wrong.data[0] = 0x11;
        TF_CHECK("script_log_stale_reorg: seed mismatched-hash ok=1 row",
                 seed_script_log(progress_store_db(), 2, 1, &wrong));
        TF_CHECK("script_log_stale_reorg: finalizes only h0, holds at 1",
                 tip_finalize_stage_drain(100) == 1);
        TF_CHECK("script_log_stale_reorg: cursor held at 1 (no false finalize)",
                 tip_finalize_stage_cursor() == 1);
        TF_CHECK("script_log_stale_reorg: block[2] bit NOT healed",
                 (sc.blocks[2].nStatus & BLOCK_VALID_MASK) <
                     BLOCK_VALID_SCRIPTS);
        tf_teardown(dir, &ms, &sc);
    }

    {
        /* ok=0 must never heal even with a matching hash: a recorded script
         * FAILURE is authoritative too — finalizing over it is forbidden. */
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("script_log_ok0: setup",
                 tf_setup("script_log_ok0", 3, TF_FAIL_NONE, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        sc.blocks[2].nStatus = BLOCK_HAVE_DATA;  /* VALID_SCRIPTS cleared */
        TF_CHECK("script_log_ok0: seed matching-hash ok=0 row",
                 seed_script_log(progress_store_db(), 2, 0,
                                 sc.blocks[2].phashBlock));
        TF_CHECK("script_log_ok0: finalizes only h0, holds at 1",
                 tip_finalize_stage_drain(100) == 1);
        TF_CHECK("script_log_ok0: cursor held at 1",
                 tip_finalize_stage_cursor() == 1);
        /* NEVER-LIE keystone: the SERVED tip is never published past the
         * cursor's advance over a non-finalizable height. update_last_advance
         * is called ONLY on the ok=1 finalized-publish path (tip_finalize_stage.c
         * ~471-494); the script-fail HOLD here takes JOB_IDLE and never publishes.
         * The last legitimately-served tip is the restored seed (blocks[3], h=3),
         * which init published; h=0 finalized but the publish guard
         * (published_before=3, 1>=3 false) kept it off the served tip, and h=1
         * holds. So the served tip MUST stay at 3 (the last ok=1 state) and MUST
         * NOT track the cursor (which holds at 1). Exact-equality, mutation-
         * sensitive: any publish from the ok=0 / hold path would drop it to <=1. */
        TF_CHECK("script_log_ok0: served tip stays at last ok=1 finalize (h=3)",
                 tip_finalize_stage_last_height() == 3);
        TF_CHECK("script_log_ok0: served tip not bumped to the held cursor",
                 tip_finalize_stage_last_height() !=
                     (int64_t)tip_finalize_stage_cursor());
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("utxo_count: setup",
                 tf_setup("utxo_count", 3, TF_FAIL_UTXO_COUNT, -1,
                          dir, sizeof(dir), &ms, &sc) == 0);
        TF_CHECK("utxo_count: drains 3", tip_finalize_stage_drain(100) == 3);
        TF_CHECK("utxo_count: counter == 1",
                 tip_finalize_stage_utxo_count_diverged_total() == 1);
        int ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        log_row_at(progress_store_db(), 1, &ok, status, sizeof(status),
                   &depth, &utxos);
        TF_CHECK("utxo_count: h=1 ok=0", ok == 0);
        TF_CHECK("utxo_count: h=1 status",
                 strcmp(status, "utxo_count_diverged") == 0);
        TF_CHECK("utxo_count: live count recorded", utxos == 99);
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("upstream_failed: setup",
                 tf_setup("upstream", 3, TF_FAIL_NONE, 2, dir, sizeof(dir),
                          &ms, &sc) == 0);
        TF_CHECK("upstream_failed: drains 3",
                 tip_finalize_stage_drain(100) == 3);
        TF_CHECK("upstream_failed: counter == 1",
                 tip_finalize_stage_upstream_failed_total() == 1);
        int ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        log_row_at(progress_store_db(), 2, &ok, status, sizeof(status),
                   &depth, &utxos);
        TF_CHECK("upstream_failed: h=2 ok=0", ok == 0);
        TF_CHECK("upstream_failed: h=2 status",
                 strcmp(status, "upstream_failed") == 0);
        tf_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("idle: setup",
                 tf_setup("idle", 3, TF_FAIL_NONE, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        sqlite3_exec(progress_store_db(),
            "UPDATE stage_cursor SET cursor=1 WHERE name='utxo_apply'",
            NULL, NULL, NULL);
        TF_CHECK("idle: advances one", tip_finalize_stage_drain(100) == 1);
        TF_CHECK("idle: next step IDLE",
                 tip_finalize_stage_step_once() == JOB_IDLE);
        TF_CHECK("idle: cursor stays 1", tip_finalize_stage_cursor() == 1);
        tf_teardown(dir, &ms, &sc);
    }

    {
        TF_CHECK("guard: step_once with no init returns IDLE",
                 tip_finalize_stage_step_once() == JOB_IDLE);
        TF_CHECK("guard: init(NULL) rejected",
                 !tip_finalize_stage_init(NULL));
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        TF_CHECK("dump: setup",
                 tf_setup("dump", 2, TF_FAIL_NONE, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        tip_finalize_stage_drain(100);
        struct json_value v;
        json_init(&v);
        TF_CHECK("dump: returns true",
                 tip_finalize_dump_state_json(&v, NULL));
        char buf[1024];
        size_t n = json_write(&v, buf, sizeof(buf));
        TF_CHECK("dump: serializes", n > 0 && n < sizeof(buf));
        TF_CHECK("dump: stage_name",
                 strstr(buf, "\"stage_name\":\"tip_finalize\"") != NULL);
        TF_CHECK("dump: cursor=2", strstr(buf, "\"cursor\":2") != NULL);
        TF_CHECK("dump: finalized_total=2",
                 strstr(buf, "\"finalized_total\":2") != NULL);
        TF_CHECK("dump: precondition_repeat_count=0",
                 strstr(buf, "\"precondition_repeat_count\":0") != NULL);
        json_free(&v);
        tf_teardown(dir, &ms, &sc);
    }

    /* TASK #31 — seed-anchor cursor unification: tip_finalize_stage_seed_anchor
     * stamps the tip_finalize cursor to the seeded tip's OWN height H (the
     * served-tip convention), NEVER H+1. A cursor of H+1 would claim the
     * unfinalized H→H+1 transition and skip it forever (the cursor is
     * monotonic) — one late block per cold-import/snapshot seed: block H+1
     * could only publish when H+2 arrived. This pins that the seed cursor is H
     * and the very next arriving block (H+1) publishes on its FIRST step. */
    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        bool ok_setup = true;
        test_fmt_tmpdir(dir, sizeof(dir), "tip_finalize", "seed_no_late_block");
        mkdir_p_tf("./test-tmp");
        mkdir_p_tf(dir);
        ok_setup = ok_setup && progress_store_open(dir);
        memset(&sc, 0, sizeof(sc));
        memset(&ms, 0, sizeof(ms));
        main_state_init(&ms);
        /* Synth chain 0..4. The seeded tip is H=3; block 4 (H+1) has NOT
         * arrived yet (kept out of the map until the second phase). */
        ok_setup = ok_setup && synth_chain_tf_build(&sc, 5);
        for (int i = 0; ok_setup && i <= 3; i++)
            ok_setup = block_map_insert(&ms.map_block_index,
                                        sc.blocks[i].phashBlock,
                                        &sc.blocks[i]);
        ok_setup = ok_setup &&
            active_chain_move_window_tip(&ms.chain_active, &sc.blocks[3]);
        /* Upstream applied through H=3 (next-height cursor == 4). */
        ok_setup = ok_setup && seed_utxo_apply(progress_store_db(), 4, -1);
        ok_setup = ok_setup && tip_finalize_stage_init(&ms);
        tip_finalize_stage_set_utxo_counter(fake_utxo_count, &sc);
        TF_CHECK("seed_no_late_block: setup", ok_setup);

        /* The cold-import/snapshot seed of the durable served tip at H=3. */
        TF_CHECK("seed_no_late_block: seed_anchor at H succeeds",
                 tip_finalize_stage_seed_anchor(3, sc.hashes[3].data, true));
        /* THE INVARIANT: the tip_finalize cursor floor is H (3), never H+1. */
        TF_CHECK("seed_no_late_block: tip_finalize cursor == served-tip H",
                 tip_finalize_stage_cursor() == 3);
        int row_ok = -1, depth = -1; int64_t utxos = -1; char status[32];
        TF_CHECK("seed_no_late_block: anchor row written at H",
                 log_row_at(progress_store_db(), 3, &row_ok, status,
                            sizeof(status), &depth, &utxos) &&
                 row_ok == 1 && strcmp(status, "anchor") == 0);
        /* The seed's utxo_apply trust stamp is INSERT OR IGNORE: the real
         * 'verified' verdict row at H (pre-written by seed_utxo_apply above)
         * must survive the seed un-clobbered. */
        {
            sqlite3_stmt *st = NULL;
            char ua_status[32] = ""; int ua_ok = -1;
            TF_CHECK("seed_no_late_block: real utxo_apply row at H survives",
                     sqlite3_prepare_v2(progress_store_db(),
                         "SELECT status, ok FROM utxo_apply_log "
                         "WHERE height = 3", -1, &st, NULL) == SQLITE_OK &&
                     sqlite3_step(st) == SQLITE_ROW &&
                     (snprintf(ua_status, sizeof(ua_status), "%s",
                               sqlite3_column_text(st, 0)) > 0) &&
                     (ua_ok = sqlite3_column_int(st, 1)) == 1 &&
                     strcmp(ua_status, "verified") == 0);
            if (st) sqlite3_finalize(st);
        }
        /* The boot resolver returns the seeded tip self-consistently from the
         * served-tip cursor (no +1, no -1 splice). */
        int rh = -1; uint8_t rhash[32];
        TF_CHECK("seed_no_late_block: resolver returns the seeded tip",
                 tip_finalize_stage_resolve_durable_tip(progress_store_db(),
                                                        &rh, rhash) &&
                 rh == 3 && memcmp(rhash, sc.hashes[3].data, 32) == 0);
        /* No successor yet → the stage idles (it does NOT regress or spin). */
        TF_CHECK("seed_no_late_block: idle while successor absent",
                 tip_finalize_stage_step_once() == JOB_IDLE);

        /* Block H+1 (=4) ARRIVES: map + utxo witness. Under the served-tip
         * cursor it publishes on the FIRST step — no late block. Under the old
         * H+1 stamp the cursor would already be 4 and this transition skipped,
         * so block 4 would wait for block 5. */
        ok_setup = block_map_insert(&ms.map_block_index,
                                    sc.blocks[4].phashBlock, &sc.blocks[4]);
        ok_setup = ok_setup && seed_utxo_apply(progress_store_db(), 5, -1);
        TF_CHECK("seed_no_late_block: successor arrival setup", ok_setup);
        TF_CHECK("seed_no_late_block: H+1 publishes on FIRST arrival",
                 tip_finalize_stage_step_once() == JOB_ADVANCED);
        TF_CHECK("seed_no_late_block: cursor advanced to H+1",
                 tip_finalize_stage_cursor() == 4);
        TF_CHECK("seed_no_late_block: H+1 is the published tip",
                 tip_finalize_stage_last_height() == 4);
        tf_teardown(dir, &ms, &sc);
    }

    /* TASK #31, second member — the COLD-IMPORT row gap: on a real import
     * the utxo_apply cursor is seeded to H+1 with NO log rows at all (the
     * coins arrived inside the verified chainstate; utxo_apply never
     * re-applies ≤H). step_finalize at cursor H consumes the utxo_apply row
     * AT H, so without the seed's own trust stamp the stage idles forever on
     * uv_row_missing — copy-proven 2026-06-12 run 2: upstream applied through
     * the live tip while tip_finalize held at the seed. This pins that the
     * seed itself supplies the row and the first live successor publishes. */
    {
        char dir[256]; struct main_state ms; struct synth_chain_tf sc;
        bool ok_setup = true;
        test_fmt_tmpdir(dir, sizeof(dir), "tip_finalize", "seed_cold_import_row_gap");
        mkdir_p_tf("./test-tmp");
        mkdir_p_tf(dir);
        ok_setup = ok_setup && progress_store_open(dir);
        memset(&sc, 0, sizeof(sc));
        memset(&ms, 0, sizeof(ms));
        main_state_init(&ms);
        ok_setup = ok_setup && synth_chain_tf_build(&sc, 5);
        for (int i = 0; ok_setup && i <= 3; i++)
            ok_setup = block_map_insert(&ms.map_block_index,
                                        sc.blocks[i].phashBlock,
                                        &sc.blocks[i]);
        ok_setup = ok_setup &&
            active_chain_move_window_tip(&ms.chain_active, &sc.blocks[3]);
        /* The cold-import shape: utxo_apply cursor at H+1 with ZERO rows
         * (seed_utxo_apply with n=0 creates the table + cursor only). */
        ok_setup = ok_setup && seed_utxo_apply(progress_store_db(), 0, -1);
        ok_setup = ok_setup && exec_sql(progress_store_db(),
            "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
            "VALUES('utxo_apply', 4, 1)");
        ok_setup = ok_setup && tip_finalize_stage_init(&ms);
        /* Production never wires the utxo counter (test-only seam) — and on
         * a cold import sums-through(H) sees only the zero-delta anchor row,
         * so a wired counter would false-diverge. NULL also clears the
         * previous case's seam (its user pointer is out of scope). */
        tip_finalize_stage_set_utxo_counter(NULL, NULL);
        TF_CHECK("seed_cold_import_row_gap: setup", ok_setup);

        TF_CHECK("seed_cold_import_row_gap: seed_anchor at H succeeds",
                 tip_finalize_stage_seed_anchor(3, sc.hashes[3].data, true));
        /* THE PIN: the seed supplied its own utxo_apply trust row at H. */
        {
            sqlite3_stmt *st = NULL;
            char ua_status[32] = ""; int ua_ok = -1;
            TF_CHECK("seed_cold_import_row_gap: seed stamps utxo_apply row at H",
                     sqlite3_prepare_v2(progress_store_db(),
                         "SELECT status, ok FROM utxo_apply_log "
                         "WHERE height = 3", -1, &st, NULL) == SQLITE_OK &&
                     sqlite3_step(st) == SQLITE_ROW &&
                     (snprintf(ua_status, sizeof(ua_status), "%s",
                               sqlite3_column_text(st, 0)) > 0) &&
                     (ua_ok = sqlite3_column_int(st, 1)) == 1 &&
                     strcmp(ua_status, "anchor") == 0);
            if (st) sqlite3_finalize(st);
        }
        /* No successor yet → idle (and NOT uv_row_missing-pinned). */
        TF_CHECK("seed_cold_import_row_gap: idle while successor absent",
                 tip_finalize_stage_step_once() == JOB_IDLE);

        /* The first live block H+1 (=4) arrives: index entry + its OWN
         * apply verdict row + cursor → 5 (exactly what the live reducer
         * writes). The stage must publish it on the FIRST step. */
        ok_setup = block_map_insert(&ms.map_block_index,
                                    sc.blocks[4].phashBlock, &sc.blocks[4]);
        /* added-spent must equal the counter's count-after(5)==5: the seed
         * anchor row at 3 contributes (0,0), so this live row carries the
         * whole delta. (In production the counter seam is never wired.) */
        ok_setup = ok_setup && exec_sql(progress_store_db(),
            "INSERT OR REPLACE INTO utxo_apply_log "
            "(height, status, ok, spent_count, added_count, "
            " total_value_delta, applied_at) "
            "VALUES (4, 'verified', 1, 0, 5, 5, 1)");
        ok_setup = ok_setup && exec_sql(progress_store_db(),
            "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
            "VALUES('utxo_apply', 5, 1)");
        TF_CHECK("seed_cold_import_row_gap: successor arrival setup", ok_setup);
        TF_CHECK("seed_cold_import_row_gap: H+1 publishes on FIRST arrival",
                 tip_finalize_stage_step_once() == JOB_ADVANCED);
        TF_CHECK("seed_cold_import_row_gap: cursor advanced to H+1",
                 tip_finalize_stage_cursor() == 4);
        TF_CHECK("seed_cold_import_row_gap: H+1 is the published tip",
                 tip_finalize_stage_last_height() == 4);
        tf_teardown(dir, &ms, &sc);
    }

    printf("tip_finalize_stage tests: %s\n",
           failures ? "FAILED" : "PASSED");
    return failures;
}
