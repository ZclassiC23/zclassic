/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_snapshot_shielded_v3 — copy-prove for the v3 UTXO-snapshot SHIELDED
 * frontier (the birth-defect cure).
 *
 * WHAT THIS PROVES (at the exact seam that gates the first post-seed shielded
 * tx, without a 3M-block boot):
 *
 *   1. The v3 writer folds the Sapling + Sprout frontiers AND the nullifier set
 *      into the SAME body SHA3 as the coins — the artifact stays
 *      single-hash-verifiable (uss_open verify_full_sha3 accepts it; a flipped
 *      byte in the shielded section is rejected).
 *   2. The loader round-trips every shielded region byte-for-byte via
 *      uss_shielded.
 *   3. POSITIVE (cure): installing the snapshot's frontier into anchor_kv (the
 *      exact primitive sequence boot_seed_shielded_from_snapshot runs) makes
 *      anchor_kv_latest_tree(SAPLING) return FOUND with a NON-EMPTY root and the
 *      seeded nullifiers resolve — i.e. sapling_anchors is NON-EMPTY, so the
 *      first post-seed Sapling spend can resolve its anchor and H* would climb.
 *   4. NEGATIVE control: the SAME coins WITHOUT the frontier section (a v1
 *      snapshot) restored the old way (reset the adoption cursor above genesis
 *      over an EMPTY table) reproduces the sapling-anchor-frontier-unavailable
 *      reject — anchor_kv_latest_tree(SAPLING) == HISTORY_INCOMPLETE.
 */

#include "test/test_helpers.h"

#include "chain/utxo_snapshot_loader.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
#include "storage/nullifier_kv.h"
#include "storage/snapshot_shielded.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SV_CHECK(name, expr) do {                              \
    printf("snapshot_shielded_v3: %s... ", (name));            \
    if (expr) printf("OK\n");                                  \
    else { printf("FAIL\n"); failures++; }                     \
} while (0)

#define SEED_H 30

static void sv_u256(struct uint256 *o, uint8_t seed)
{
    for (int i = 0; i < 32; i++) o->data[i] = (uint8_t)(seed + i * 7);
}

static void sv_txid(uint8_t out[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(seed + i * 3);
}

/* Seed a tiny coins set into an in-memory db (same shape as the other snapshot
 * test) so the v3 file has a real body under its SHA3. */
static bool sv_seed_coins(sqlite3 *db)
{
    if (!coins_kv_ensure_schema(db)) return false;
    uint8_t txid[32];
    uint8_t sa[] = {0x76, 0xa9, 0x14, 0x01, 0x02};
    uint8_t sb[] = {0x6a};
    sv_txid(txid, 0x10);
    if (!coins_kv_add(db, txid, 0, 11111, 10, true, sa, sizeof(sa)))
        return false;
    sv_txid(txid, 0x20);
    return coins_kv_add(db, txid, 2, 22222, 20, false, sb, sizeof(sb));
}

int test_snapshot_shielded_v3(void)
{
    printf("\n=== snapshot_shielded_v3 ===\n");
    int failures = 0;

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "snapshot_shielded_v3", "main");
    char v3_path[320], v1_path[320];
    snprintf(v3_path, sizeof(v3_path), "%s/shielded-v3.snapshot", dir);
    snprintf(v1_path, sizeof(v1_path), "%s/coins-only-v1.snapshot", dir);

    /* ── Build synthetic shielded state ─────────────────────────────────── */
    struct incremental_merkle_tree sap_tree;
    sapling_tree_init(&sap_tree);
    for (uint8_t k = 1; k <= 3; k++) {
        struct uint256 leaf; sv_u256(&leaf, (uint8_t)(0x40 + k));
        incremental_tree_append(&sap_tree, &leaf);
    }
    struct uint256 sap_root; incremental_tree_root(&sap_tree, &sap_root);
    struct uint256 empty_sap_root;
    { struct incremental_merkle_tree e; sapling_tree_init(&e);
      incremental_tree_root(&e, &empty_sap_root); }
    SV_CHECK("sapling frontier is non-empty",
             !uint256_eq(&sap_root, &empty_sap_root));

    struct byte_stream sap_bs; stream_init(&sap_bs, 4096);
    bool sap_ser = incremental_tree_serialize(&sap_tree, &sap_bs);
    SV_CHECK("sapling frontier serializes", sap_ser);

    struct incremental_merkle_tree spr_tree;
    sprout_tree_init(&spr_tree);
    for (uint8_t k = 1; k <= 2; k++) {
        struct uint256 leaf; sv_u256(&leaf, (uint8_t)(0x80 + k));
        incremental_tree_append(&spr_tree, &leaf);
    }
    struct byte_stream spr_bs; stream_init(&spr_bs, 4096);
    bool spr_ser = incremental_tree_serialize(&spr_tree, &spr_bs);
    SV_CHECK("sprout frontier serializes", spr_ser);

    /* Two nullifiers: one Sprout (pool 0), one Sapling (pool 1). */
    uint8_t nf0[32], nf1[32];
    for (int i = 0; i < 32; i++) { nf0[i] = (uint8_t)(0xc0 + i); nf1[i] = (uint8_t)(0x30 + i); }
    uint8_t nf_recs[2 * SNAPSHOT_NF_RECORD_BYTES];
    snapshot_shielded_pack_nf(nf_recs, 0, nf0, 12);
    snapshot_shielded_pack_nf(nf_recs + SNAPSHOT_NF_RECORD_BYTES, 1, nf1, 27);

    struct snapshot_shielded sh = {
        .sapling = sap_bs.data, .sapling_len = (uint32_t)sap_bs.size,
        .sprout  = spr_bs.data, .sprout_len  = (uint32_t)spr_bs.size,
        .nf_records = nf_recs,  .nf_count = 2,
    };

    /* ── Write the v3 snapshot (+ a v1 coins-only control) ──────────────── */
    uint8_t v3_sha3[32] = {0}; uint64_t v3_count = 0; int64_t v3_supply = 0;
    uint8_t anchor_hash[32];
    for (int i = 0; i < 32; i++) anchor_hash[i] = (uint8_t)(0xa0 + i);
    {
        sqlite3 *src = NULL;
        bool ok = sqlite3_open(":memory:", &src) == SQLITE_OK && sv_seed_coins(src);
        ok = ok && coins_kv_snapshot_write_v3(src, v3_path, SEED_H, anchor_hash,
                                              &sh, v3_sha3, &v3_count, &v3_supply);
        SV_CHECK("v3 snapshot written (coins + shielded)", ok && v3_count == 2);
        sqlite3_close(src);
    }
    {
        sqlite3 *src = NULL;
        uint8_t r[32]; uint64_t c = 0; int64_t s = 0;
        bool ok = sqlite3_open(":memory:", &src) == SQLITE_OK && sv_seed_coins(src);
        ok = ok && coins_kv_snapshot_write(src, v1_path, SEED_H, anchor_hash,
                                           r, &c, &s);
        SV_CHECK("v1 coins-only control written", ok && c == 2);
        sqlite3_close(src);
    }

    /* ── Loader: single-hash verify + shielded round-trip ───────────────── */
    struct uss_header hdr; char err[128] = {0};
    struct uss_handle *h = uss_open(v3_path, /*verify_full_sha3=*/true,
                                    v3_sha3, &hdr, err, sizeof(err));
    SV_CHECK("v3 body SHA3 covers coins+shielded (uss_open verifies)",
             h != NULL && hdr.version == 3);

    const uint8_t *r_sap = NULL, *r_spr = NULL, *r_nf = NULL;
    uint32_t r_sap_len = 0, r_spr_len = 0; uint64_t r_nf_count = 0;
    bool got = h && uss_shielded(h, &r_sap, &r_sap_len, &r_spr, &r_spr_len,
                                 &r_nf, &r_nf_count);
    SV_CHECK("uss_shielded round-trips all three regions",
             got && r_sap_len == sap_bs.size && r_spr_len == spr_bs.size &&
             r_nf_count == 2 &&
             memcmp(r_sap, sap_bs.data, sap_bs.size) == 0 &&
             memcmp(r_spr, spr_bs.data, spr_bs.size) == 0 &&
             memcmp(r_nf, nf_recs, sizeof(nf_recs)) == 0);
    if (h) uss_close(h);

    /* A v1 handle exposes no shielded section. */
    h = uss_open(v1_path, true, NULL, &hdr, err, sizeof(err));
    SV_CHECK("v1 control has no shielded section",
             h != NULL && hdr.version == 1 &&
             !uss_shielded(h, &r_sap, &r_sap_len, &r_spr, &r_spr_len,
                           &r_nf, &r_nf_count));
    if (h) uss_close(h);

    /* Tamper: flipping a byte inside the shielded section breaks the body SHA3
     * (the single hash really commits it). */
    {
        long sz = 0;
        FILE *f = fopen(v3_path, "rb+");
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f);
                 fseek(f, sz - 1, SEEK_SET);
                 int c = fgetc(f); fseek(f, sz - 1, SEEK_SET);
                 fputc(c ^ 0xff, f); fclose(f); }
        char terr[128] = {0};
        struct uss_handle *th = uss_open(v3_path, true, NULL, &hdr, terr,
                                         sizeof(terr));
        SV_CHECK("tampered shielded byte fails body SHA3", th == NULL);
        if (th) uss_close(th);
    }

    /* ── POSITIVE: install the frontier => sapling_anchors NON-EMPTY ────── */
    {
        sqlite3 *db = NULL;
        bool ok = sqlite3_open(":memory:", &db) == SQLITE_OK;
        /* Exact primitive sequence of boot_seed_shielded_from_snapshot: reset to
         * cursor=0 (history complete), seed the root-verified Sapling frontier,
         * add the Sprout frontier, bulk-add the nullifiers. */
        ok = ok && anchor_kv_reset_in_tx(db, 0);
        ok = ok && anchor_kv_seed_frontier_row(db, ANCHOR_POOL_SAPLING,
                                               &sap_tree, SEED_H, &sap_root);
        ok = ok && anchor_kv_add_tree(db, ANCHOR_POOL_SPROUT, &spr_tree, SEED_H);
        ok = ok && nullifier_kv_ensure_schema(db);
        ok = ok && nullifier_kv_add(db, nf0, NULLIFIER_POOL_SPROUT, 12);
        ok = ok && nullifier_kv_add(db, nf1, NULLIFIER_POOL_SAPLING, 27);
        SV_CHECK("shielded seed installs cleanly", ok);

        struct incremental_merkle_tree lt; struct uint256 lroot; int64_t lh = -1;
        enum anchor_kv_lookup_result lr =
            anchor_kv_latest_tree(db, ANCHOR_POOL_SAPLING, &lt, &lroot, &lh);
        SV_CHECK("CURE: sapling_anchors NON-EMPTY (latest_tree FOUND, matches "
                 "seed root)",
                 lr == ANCHOR_KV_FOUND && uint256_eq(&lroot, &sap_root) &&
                 lh == SEED_H);
        SV_CHECK("CURE: the seed Sapling anchor resolves by root",
                 anchor_kv_get(db, ANCHOR_POOL_SAPLING, &sap_root, NULL, NULL)
                     == ANCHOR_KV_FOUND);
        bool f0 = false, f1 = false;
        SV_CHECK("CURE: seeded nullifiers are present (double-spend guard live)",
                 nullifier_kv_get(db, nf0, NULLIFIER_POOL_SPROUT, &f0, NULL) &&
                 f0 &&
                 nullifier_kv_get(db, nf1, NULLIFIER_POOL_SAPLING, &f1, NULL) &&
                 f1);
        if (db) sqlite3_close(db);
    }

    /* ── NEGATIVE control: birth-defect restore reproduces the reject ───── */
    {
        sqlite3 *db = NULL;
        bool ok = sqlite3_open(":memory:", &db) == SQLITE_OK;
        /* Today's coins-only restore: reset the adoption cursor above genesis
         * over an EMPTY sapling_anchors table. */
        ok = ok && anchor_kv_reset_in_tx(db, SEED_H);
        SV_CHECK("control restore (cursor above genesis, empty table) applies",
                 ok);
        struct incremental_merkle_tree lt; struct uint256 lroot; int64_t lh = -1;
        enum anchor_kv_lookup_result lr =
            anchor_kv_latest_tree(db, ANCHOR_POOL_SAPLING, &lt, &lroot, &lh);
        SV_CHECK("REJECT REPRODUCED: sapling-anchor-frontier-unavailable "
                 "(latest_tree HISTORY_INCOMPLETE)",
                 lr == ANCHOR_KV_HISTORY_INCOMPLETE);
        SV_CHECK("REJECT REPRODUCED: a non-empty anchor lookup is "
                 "HISTORY_INCOMPLETE",
                 anchor_kv_get(db, ANCHOR_POOL_SAPLING, &sap_root, NULL, NULL)
                     == ANCHOR_KV_HISTORY_INCOMPLETE);
        if (db) sqlite3_close(db);
    }

    stream_free(&sap_bs);
    stream_free(&spr_bs);

    printf("=== snapshot_shielded_v3: %d failure(s) ===\n", failures);
    return failures;
}
