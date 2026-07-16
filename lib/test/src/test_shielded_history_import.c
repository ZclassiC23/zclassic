/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_shielded_history_import — the COMPLETE, ATOMIC historical anchor +
 * nullifier importer (app/services/src/shielded_history_import_service.c).
 *
 * Hermetic (temp LevelDB fixture built via dbwrapper with the exact zclassicd
 * key/value shapes; temp progress.kv via progress_store — no live datadir, no
 * ~/.zcash-params, no chain). Proves the two merge-bar properties:
 *
 *   COMPLETENESS — a fixture chainstate with a known small set imports so that
 *   anchor_kv/nullifier_kv contain EXACTLY that set (both pools), both
 *   activation cursors flip to 0, and both gap blockers clear.
 *
 *   ATOMICITY — a fixture whose tip Sapling anchor row is MISSING refuses the
 *   whole import: nothing is committed, both cursors stay POSITIVE (safe
 *   wedge), and both gap blockers stay. A partial set can never flip a cursor. */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "jobs/utxo_apply_anchors.h"
#include "jobs/utxo_apply_nullifiers.h"
#include "models/block.h"
#include "models/database.h"
#include "sapling/incremental_merkle_tree.h"
#include "services/shielded_history_import_service.h"
#include "storage/anchor_kv.h"
#include "storage/dbwrapper.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define SHI_BOUNDARY 3176326   /* the wedge height + 1 (positive cursor) */
#define SHI_TIP_H    3176325   /* the chainstate tip height */

#define SHI_CHECK(name, expr) do {                          \
    printf("  shielded_history_import: %s... ", (name));    \
    if ((expr)) printf("OK\n");                             \
    else { printf("FAIL\n"); failures++; }                  \
} while (0)

/* ── fixture builders ── */

static void shi_fill(struct uint256 *h, uint8_t seed, size_t idx)
{
    for (size_t i = 0; i < 32; i++)
        h->data[i] = (uint8_t)(seed ^ (idx * 7 + i));
}

/* The deterministic best-block hash every fixture chainstate stamps as its 'B'
 * pointer — the integration scenarios (D/E) key the node.db header row off it,
 * exactly as import_complete_shielded_mode binds via db_block_find_by_hash. */
static void shi_best_block(struct uint256 *bb)
{
    shi_fill(bb, 0xBB, 999);
}

/* A non-empty Sapling frontier of `n` synthetic commitments -> its root. */
static void shi_sapling_tree(size_t n, struct incremental_merkle_tree *out,
                             struct uint256 *root_out)
{
    sapling_tree_init(out);
    for (size_t i = 0; i < n; i++) {
        struct uint256 cm;
        shi_fill(&cm, 0x5A, i + 1);
        incremental_tree_append(out, &cm);
    }
    incremental_tree_root(out, root_out);
}

static void shi_sprout_tree(size_t n, struct incremental_merkle_tree *out,
                            struct uint256 *root_out)
{
    sprout_tree_init(out);
    for (size_t i = 0; i < n; i++) {
        struct uint256 cm;
        shi_fill(&cm, 0xA5, i + 1);
        incremental_tree_append(out, &cm);
    }
    incremental_tree_root(out, root_out);
}

/* Write one (key -> value) row into the LevelDB at `dir`. Opens/closes per
 * call: LevelDB is single-writer, so no write may overlap the reader's LOCK. */
static bool shi_put(const char *dir, const char *key, size_t klen,
                    const void *val, size_t vlen)
{
    struct db_wrapper db;
    memset(&db, 0, sizeof(db));
    if (!db_wrapper_open(&db, dir, 1u << 20, false, false))
        return false;
    bool ok = db_write(&db, key, klen, (const char *)val, vlen, true);
    db_wrapper_close(&db);
    return ok;
}

static bool shi_put_anchor(const char *dir, char prefix,
                           const struct incremental_merkle_tree *tree,
                           const struct uint256 *root)
{
    struct byte_stream s;
    stream_init(&s, 256);
    bool ok = incremental_tree_serialize(tree, &s) && !s.error;
    char key[33];
    key[0] = prefix;
    memcpy(key + 1, root->data, 32);
    ok = ok && shi_put(dir, key, sizeof(key), s.data, s.size);
    stream_free(&s);
    return ok;
}

static bool shi_put_nullifier(const char *dir, char prefix,
                              const struct uint256 *nf)
{
    char key[33];
    key[0] = prefix;
    memcpy(key + 1, nf->data, 32);
    const uint8_t present = 0x01;   /* serialized C++ `bool true` */
    return shi_put(dir, key, sizeof(key), &present, 1);
}

static bool shi_put_pointer(const char *dir, char key_byte,
                            const struct uint256 *root)
{
    return shi_put(dir, &key_byte, 1, root->data, 32);
}

static int64_t shi_count(sqlite3 *db, const char *table)
{
    char sql[96];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return -1;
    int64_t n = -1;
    if (sqlite3_step(s) == SQLITE_ROW)
        n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

static bool shi_cursor_is(sqlite3 *db, int pool, int64_t want)
{
    int64_t c = -1;
    bool found = false;
    return anchor_kv_activation_cursor(db, pool, &c, &found) && found &&
           c == want;
}

static bool shi_nf_cursor_is(sqlite3 *db, int64_t want)
{
    int64_t c = -1;
    bool found = false;
    return nullifier_kv_activation_cursor(db, &c, &found) && found && c == want;
}

/* Seed a wedged progress.kv: both anchor pools + the nullifier marker at a
 * POSITIVE boundary, with both permanent gap blockers raised. */
static bool shi_seed_wedge(sqlite3 *db)
{
    blocker_clear(UTXO_APPLY_ANCHOR_GAP_BLOCKER_ID);
    blocker_clear(UTXO_APPLY_NF_GAP_BLOCKER_ID);
    if (!anchor_kv_initialize_history(db, SHI_BOUNDARY) ||
        !nullifier_kv_initialize_history(db, SHI_BOUNDARY))
        return false;
    utxo_apply_anchor_gap_blocker_refresh(db);
    utxo_apply_nullifier_gap_blocker_refresh(db);
    return true;
}

/* Build the common shielded set into `cs_dir`. When `omit_tip_sapling` is true
 * the best Sapling anchor ROW is skipped while its 'z' pointer still names it
 * (the "missing anchor" fixture). Fills the tip Sapling root for the caller. */
static bool shi_build_chainstate(const char *cs_dir, bool omit_tip_sapling,
                                 struct uint256 *tip_sapling_root_out)
{
    struct incremental_merkle_tree sap_a, sap_b, sap_tip;
    struct uint256 sr_a, sr_b, sr_tip;
    shi_sapling_tree(3, &sap_a, &sr_a);
    shi_sapling_tree(7, &sap_b, &sr_b);
    shi_sapling_tree(11, &sap_tip, &sr_tip);   /* designated tip frontier */
    *tip_sapling_root_out = sr_tip;

    struct incremental_merkle_tree spr_a, spr_best;
    struct uint256 pr_a, pr_best;
    shi_sprout_tree(2, &spr_a, &pr_a);
    shi_sprout_tree(5, &spr_best, &pr_best);

    if (!shi_put_anchor(cs_dir, 'Z', &sap_a, &sr_a) ||
        !shi_put_anchor(cs_dir, 'Z', &sap_b, &sr_b))
        return false;
    if (!omit_tip_sapling &&
        !shi_put_anchor(cs_dir, 'Z', &sap_tip, &sr_tip))
        return false;
    if (!shi_put_anchor(cs_dir, 'A', &spr_a, &pr_a) ||
        !shi_put_anchor(cs_dir, 'A', &spr_best, &pr_best))
        return false;

    /* nullifiers: 3 Sapling, 2 Sprout */
    struct uint256 nf;
    for (int i = 0; i < 3; i++) {
        shi_fill(&nf, 0x51, (size_t)i + 100);
        if (!shi_put_nullifier(cs_dir, 'S', &nf))
            return false;
    }
    for (int i = 0; i < 2; i++) {
        shi_fill(&nf, 0x53, (size_t)i + 200);
        if (!shi_put_nullifier(cs_dir, 's', &nf))
            return false;
    }

    /* best pointers + best block */
    struct uint256 best_block;
    shi_best_block(&best_block);
    if (!shi_put_pointer(cs_dir, 'z', &sr_tip) ||   /* always names the tip */
        !shi_put_pointer(cs_dir, 'a', &pr_best) ||
        !shi_put_pointer(cs_dir, 'B', &best_block))
        return false;
    return true;
}

/* Write one connected header row into node.db exactly as --importblockindex
 * does: hash=best_block, a connected status (>=BLOCK_VALID_TRANSACTIONS), and
 * blocks.sapling_root set to `sapling_root` — the FIXED header import copies the
 * block header's hashFinalSaplingRoot here; an OLD import left it all-zero. This
 * is the precise column import_complete_shielded_mode binds the tip against via
 * db_block_find_by_hash. */
static bool shi_write_header_row(struct node_db *ndb,
                                 const struct uint256 *best_block,
                                 const struct uint256 *sapling_root)
{
    static uint8_t sol[1] = {0};
    struct db_block b;
    memset(&b, 0, sizeof(b));
    memcpy(b.hash, best_block->data, 32);
    b.height = SHI_TIP_H;
    memset(b.prev_hash, 0x10, 32);
    b.version = 4;
    memset(b.merkle_root, 0x11, 32);
    b.time = 1700000000u;
    b.bits = 0x2000ffffu;
    memset(b.nonce, 0x22, 32);
    b.solution = sol;
    b.solution_len = sizeof(sol);
    b.status = BLOCK_VALID_SCRIPTS;   /* connected floor for the bind query */
    b.num_tx = 1;
    memcpy(b.sapling_root, sapling_root->data, 32);
    return db_block_save(ndb, &b);
}

int test_shielded_history_import(void);
int test_shielded_history_import(void)
{
    int failures = 0;

    /* ── Scenario A: COMPLETE set imports, cursors flip, blockers clear ── */
    {
        char cs_dir[256], pg_dir[256];
        test_make_tmpdir(cs_dir, sizeof(cs_dir), "shi_complete", "cs");
        test_make_tmpdir(pg_dir, sizeof(pg_dir), "shi_complete", "pg");

        struct uint256 tip_root;
        SHI_CHECK("build complete fixture chainstate",
                  shi_build_chainstate(cs_dir, false, &tip_root));

        SHI_CHECK("progress.kv opens", progress_store_open(pg_dir));
        sqlite3 *db = progress_store_db();
        SHI_CHECK("wedge seeded (cursors positive + blockers raised)",
                  db && shi_seed_wedge(db));
        SHI_CHECK("anchor gap blocker present before import",
                  blocker_exists(UTXO_APPLY_ANCHOR_GAP_BLOCKER_ID));
        SHI_CHECK("nullifier gap blocker present before import",
                  blocker_exists(UTXO_APPLY_NF_GAP_BLOCKER_ID));

        struct shielded_import_report rep;
        bool ok = shielded_history_import_from_chainstate(
            db, cs_dir, SHI_TIP_H, &tip_root, &rep);
        SHI_CHECK("import returns success", ok);
        SHI_CHECK("import committed", rep.committed);
        SHI_CHECK("tip Sapling anchor bound to header root",
                  rep.tip_anchor_bound);
        SHI_CHECK("imported exactly 3 Sapling anchors",
                  rep.sapling_anchors == 3);
        SHI_CHECK("imported exactly 2 Sprout anchors",
                  rep.sprout_anchors == 2);
        SHI_CHECK("imported exactly 3 Sapling nullifiers",
                  rep.sapling_nullifiers == 3);
        SHI_CHECK("imported exactly 2 Sprout nullifiers",
                  rep.sprout_nullifiers == 2);

        /* tables hold EXACTLY the set (no extras) */
        SHI_CHECK("sapling_anchors table has exactly 3 rows",
                  shi_count(db, "sapling_anchors") == 3);
        SHI_CHECK("sprout_anchors table has exactly 2 rows",
                  shi_count(db, "sprout_anchors") == 2);
        SHI_CHECK("nullifiers table has exactly 5 rows",
                  shi_count(db, "nullifiers") == 5);

        /* the tip frontier is selectable for the forward fold */
        {
            struct incremental_merkle_tree latest;
            struct uint256 latest_root;
            SHI_CHECK("latest Sapling frontier == tip anchor",
                      anchor_kv_latest_tree(db, ANCHOR_POOL_SAPLING, &latest,
                                            &latest_root, NULL) ==
                          ANCHOR_KV_FOUND &&
                      uint256_eq(&latest_root, &tip_root));
        }

        /* both cursors flipped to zero */
        SHI_CHECK("Sprout anchor cursor == 0",
                  shi_cursor_is(db, ANCHOR_POOL_SPROUT, 0));
        SHI_CHECK("Sapling anchor cursor == 0",
                  shi_cursor_is(db, ANCHOR_POOL_SAPLING, 0));
        SHI_CHECK("nullifier cursor == 0", shi_nf_cursor_is(db, 0));

        /* both gap blockers cleared */
        SHI_CHECK("anchor gap blocker cleared after import",
                  !blocker_exists(UTXO_APPLY_ANCHOR_GAP_BLOCKER_ID));
        SHI_CHECK("nullifier gap blocker cleared after import",
                  !blocker_exists(UTXO_APPLY_NF_GAP_BLOCKER_ID));

        progress_store_close();
        test_rm_rf_recursive(cs_dir);
        test_rm_rf_recursive(pg_dir);
    }

    /* ── Scenario B: MISSING tip anchor -> atomic refusal, wedge intact ── */
    {
        char cs_dir[256], pg_dir[256];
        test_make_tmpdir(cs_dir, sizeof(cs_dir), "shi_missing", "cs");
        test_make_tmpdir(pg_dir, sizeof(pg_dir), "shi_missing", "pg");

        struct uint256 tip_root;
        SHI_CHECK("build fixture with MISSING tip Sapling anchor row",
                  shi_build_chainstate(cs_dir, true, &tip_root));

        SHI_CHECK("progress.kv opens (missing)", progress_store_open(pg_dir));
        sqlite3 *db = progress_store_db();
        SHI_CHECK("wedge seeded (missing)", db && shi_seed_wedge(db));

        struct shielded_import_report rep;
        bool ok = shielded_history_import_from_chainstate(
            db, cs_dir, SHI_TIP_H, &tip_root, &rep);
        SHI_CHECK("import REFUSES (returns false)", !ok);
        SHI_CHECK("import did NOT commit", !rep.committed);

        /* nothing committed: both anchor tables empty (rolled back) */
        SHI_CHECK("sapling_anchors table empty after refusal",
                  shi_count(db, "sapling_anchors") == 0);
        SHI_CHECK("sprout_anchors table empty after refusal",
                  shi_count(db, "sprout_anchors") == 0);
        SHI_CHECK("nullifiers table empty after refusal",
                  shi_count(db, "nullifiers") == 0);

        /* cursors STILL positive (safe wedge held) */
        SHI_CHECK("Sprout anchor cursor still positive",
                  shi_cursor_is(db, ANCHOR_POOL_SPROUT, SHI_BOUNDARY));
        SHI_CHECK("Sapling anchor cursor still positive",
                  shi_cursor_is(db, ANCHOR_POOL_SAPLING, SHI_BOUNDARY));
        SHI_CHECK("nullifier cursor still positive",
                  shi_nf_cursor_is(db, SHI_BOUNDARY));

        /* blockers still raised */
        SHI_CHECK("anchor gap blocker still present after refusal",
                  blocker_exists(UTXO_APPLY_ANCHOR_GAP_BLOCKER_ID));
        SHI_CHECK("nullifier gap blocker still present after refusal",
                  blocker_exists(UTXO_APPLY_NF_GAP_BLOCKER_ID));

        progress_store_close();
        test_rm_rf_recursive(cs_dir);
        test_rm_rf_recursive(pg_dir);
    }

    /* ── Scenario C: tip-root mismatch -> pre-tx refusal, wedge intact ── */
    {
        char cs_dir[256], pg_dir[256];
        test_make_tmpdir(cs_dir, sizeof(cs_dir), "shi_mismatch", "cs");
        test_make_tmpdir(pg_dir, sizeof(pg_dir), "shi_mismatch", "pg");

        struct uint256 tip_root;
        SHI_CHECK("build complete fixture (mismatch scenario)",
                  shi_build_chainstate(cs_dir, false, &tip_root));

        SHI_CHECK("progress.kv opens (mismatch)", progress_store_open(pg_dir));
        sqlite3 *db = progress_store_db();
        SHI_CHECK("wedge seeded (mismatch)", db && shi_seed_wedge(db));

        struct uint256 wrong_root;
        shi_fill(&wrong_root, 0xEE, 424242);   /* != real tip root */
        struct shielded_import_report rep;
        bool ok = shielded_history_import_from_chainstate(
            db, cs_dir, SHI_TIP_H, &wrong_root, &rep);
        SHI_CHECK("import REFUSES on tip-root mismatch", !ok && !rep.committed);
        SHI_CHECK("mismatch left both anchor cursors positive",
                  shi_cursor_is(db, ANCHOR_POOL_SPROUT, SHI_BOUNDARY) &&
                  shi_cursor_is(db, ANCHOR_POOL_SAPLING, SHI_BOUNDARY));
        SHI_CHECK("mismatch left nullifier cursor positive",
                  shi_nf_cursor_is(db, SHI_BOUNDARY));
        SHI_CHECK("mismatch committed nothing",
                  shi_count(db, "sapling_anchors") == 0 &&
                  shi_count(db, "nullifiers") == 0);

        progress_store_close();
        test_rm_rf_recursive(cs_dir);
        test_rm_rf_recursive(pg_dir);
    }

    /* ── Scenario D: INTEGRATION — header row carries the real
     * hashFinalSaplingRoot (the FIXED --importblockindex) → the bind source
     * (blocks.sapling_root read via db_block_find_by_hash) is POPULATED, equals
     * the chainstate tip anchor, and the import binds + clears the wedge. This
     * exercises the exact tip-bind seam import_complete_shielded_mode uses. */
    {
        char cs_dir[256], pg_dir[256], nd_dir[256];
        test_make_tmpdir(cs_dir, sizeof(cs_dir), "shi_bind", "cs");
        test_make_tmpdir(pg_dir, sizeof(pg_dir), "shi_bind", "pg");
        test_make_tmpdir(nd_dir, sizeof(nd_dir), "shi_bind", "nd");

        struct uint256 tip_root, best_block;
        SHI_CHECK("build complete fixture (bind scenario)",
                  shi_build_chainstate(cs_dir, false, &tip_root));
        shi_best_block(&best_block);

        char ndb_path[320];
        snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", nd_dir);
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        SHI_CHECK("node.db opens (bind)", node_db_open(&ndb, ndb_path));
        SHI_CHECK("header row written with REAL header sapling_root",
                  shi_write_header_row(&ndb, &best_block, &tip_root));

        /* the exact tip-bind read import_complete_shielded_mode performs */
        struct db_block blk;
        bool found = db_block_find_by_hash(&ndb, best_block.data, &blk);
        SHI_CHECK("bind: best block found in header chain", found);
        SHI_CHECK("bind: status is connected (>= BLOCK_VALID_TRANSACTIONS)",
                  found && blk.status >= BLOCK_VALID_TRANSACTIONS);
        struct uint256 bind_root;
        uint256_set_null(&bind_root);
        if (found) memcpy(bind_root.data, blk.sapling_root, 32);
        SHI_CHECK("bind: sapling_root is POPULATED (non-zero)",
                  !uint256_is_null(&bind_root));
        SHI_CHECK("bind: sapling_root == header-committed tip root",
                  uint256_eq(&bind_root, &tip_root));
        SHI_CHECK("bind: tip height read from header row",
                  found && blk.height == SHI_TIP_H);
        node_db_close(&ndb);

        SHI_CHECK("progress.kv opens (bind)", progress_store_open(pg_dir));
        sqlite3 *db = progress_store_db();
        SHI_CHECK("wedge seeded (bind)", db && shi_seed_wedge(db));

        struct shielded_import_report rep;
        bool ok = shielded_history_import_from_chainstate(
            db, cs_dir, SHI_TIP_H, &bind_root, &rep);
        SHI_CHECK("import binds + commits using the header-derived root",
                  ok && rep.committed && rep.tip_anchor_bound);
        SHI_CHECK("bind cleared wedge: Sapling anchor cursor == 0",
                  shi_cursor_is(db, ANCHOR_POOL_SAPLING, 0));
        SHI_CHECK("bind cleared wedge: nullifier cursor == 0",
                  shi_nf_cursor_is(db, 0));
        SHI_CHECK("bind cleared anchor gap blocker",
                  !blocker_exists(UTXO_APPLY_ANCHOR_GAP_BLOCKER_ID));

        progress_store_close();
        test_rm_rf_recursive(cs_dir);
        test_rm_rf_recursive(pg_dir);
        test_rm_rf_recursive(nd_dir);
    }

    /* ── Scenario E: INTEGRATION — header row left blocks.sapling_root all-zero
     * (an OLD --importblockindex that dropped the header field). The bind source
     * is detectably zero, and feeding that zero root to the importer must be
     * REFUSED (fork-safety guard) with a full rollback: no anchor/nullifier row
     * committed, both cursors stay positive, gap blockers stay raised. */
    {
        char cs_dir[256], pg_dir[256], nd_dir[256];
        test_make_tmpdir(cs_dir, sizeof(cs_dir), "shi_zero", "cs");
        test_make_tmpdir(pg_dir, sizeof(pg_dir), "shi_zero", "pg");
        test_make_tmpdir(nd_dir, sizeof(nd_dir), "shi_zero", "nd");

        struct uint256 tip_root, best_block;
        SHI_CHECK("build complete fixture (zero-column scenario)",
                  shi_build_chainstate(cs_dir, false, &tip_root));
        shi_best_block(&best_block);

        char ndb_path[320];
        snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", nd_dir);
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        SHI_CHECK("node.db opens (zero)", node_db_open(&ndb, ndb_path));
        struct uint256 zero_root;
        uint256_set_null(&zero_root);
        SHI_CHECK("header row written with ZERO sapling_root (old import)",
                  shi_write_header_row(&ndb, &best_block, &zero_root));

        struct db_block blk;
        bool found = db_block_find_by_hash(&ndb, best_block.data, &blk);
        struct uint256 bind_root;
        uint256_set_null(&bind_root);
        if (found) memcpy(bind_root.data, blk.sapling_root, 32);
        SHI_CHECK("bind: best block found but sapling_root DETECTABLY zero",
                  found && uint256_is_null(&bind_root));
        node_db_close(&ndb);

        SHI_CHECK("progress.kv opens (zero)", progress_store_open(pg_dir));
        sqlite3 *db = progress_store_db();
        SHI_CHECK("wedge seeded (zero)", db && shi_seed_wedge(db));

        struct shielded_import_report rep;
        bool ok = shielded_history_import_from_chainstate(
            db, cs_dir, SHI_TIP_H, &bind_root, &rep);
        SHI_CHECK("service REFUSES a zero tip root (fork-safety guard)",
                  !ok && !rep.committed);
        SHI_CHECK("zero-root refusal left both anchor cursors positive",
                  shi_cursor_is(db, ANCHOR_POOL_SPROUT, SHI_BOUNDARY) &&
                  shi_cursor_is(db, ANCHOR_POOL_SAPLING, SHI_BOUNDARY));
        SHI_CHECK("zero-root refusal left nullifier cursor positive",
                  shi_nf_cursor_is(db, SHI_BOUNDARY));
        SHI_CHECK("zero-root refusal committed nothing",
                  shi_count(db, "sapling_anchors") == 0 &&
                  shi_count(db, "nullifiers") == 0);
        SHI_CHECK("zero-root refusal left anchor gap blocker raised",
                  blocker_exists(UTXO_APPLY_ANCHOR_GAP_BLOCKER_ID));

        progress_store_close();
        test_rm_rf_recursive(cs_dir);
        test_rm_rf_recursive(pg_dir);
        test_rm_rf_recursive(nd_dir);
    }

    return failures;
}
