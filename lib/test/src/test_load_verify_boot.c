/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_load_verify_boot — the additive load-verify normal-boot routing gate.
 *
 * The load-bearing property: a baked anchor snapshot is LOADED into coins_kv on a
 * normal boot ONLY when its recomputed body SHA3 EQUALS the compiled SHA3 UTXO
 * checkpoint; a snapshot whose SHA3 MISMATCHES is REFUSED (never seeds a bad set),
 * and the boot falls back to the proven path. This test pins the public gate
 * (boot_load_verify_snapshot_eligible) AND the exact loader binding the seed uses
 * (uss_open with expected_sha3 = the compiled checkpoint), against a scaled-down
 * synthetic snapshot whose body SHA3 IS an installed checkpoint override.
 *
 *   (a) MATCH   : snapshot SHA3 == checkpoint  → eligible=true; the verified set
 *                 iterates into coins_kv at the anchor count.
 *   (b) MISMATCH: snapshot SHA3 != checkpoint  → uss_open(.,expected=cp) returns
 *                 NULL AND eligible=false → REFUSED, coins_kv stays untouched.
 *   (c) ABSENT  : no snapshot file            → eligible=false (safe fallback).
 *   (d) HEALTHY : coins_kv already the proven authority → eligible=false even with
 *                 a matching snapshot present (a synced node is never reset).
 */

#include "test/test_helpers.h"

#include "config/boot.h"
#include "models/database.h"
#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "chain/checkpoints.h"
#include "chain/utxo_snapshot_loader.h"
#include "crypto/sha3.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LV_CHECK(name, expr) do {                                       \
    if (expr) { printf("  load_verify_boot: %s... OK\n", (name)); }      \
    else { printf("  load_verify_boot: %s... FAIL\n", (name)); failures++; } \
} while (0)

static void lv_wle32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void lv_wle64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

/* Build a tiny but valid USS snapshot body (N records) and return the body SHA3
 * + the assembled file bytes. `tamper` flips one body byte so the on-disk SHA3
 * differs from the header's claimed root (but we DELIBERATELY do NOT update the
 * header root, so the header still claims `body_sha3` — the loader's full-body
 * recompute catches the divergence, modeling a corrupted/forged artifact). */
struct lv_built {
    uint8_t  file[8192];
    size_t   file_len;
    uint8_t  body_sha3[32];
    uint64_t count;
    int64_t  total;
};
static void lv_build_snapshot(struct lv_built *b, bool tamper)
{
    uint8_t body[4096] = {0};
    size_t off = 0;
    uint8_t txid[32];
    for (int i = 0; i < 32; i++) txid[i] = (uint8_t)(0x40 + i);

    struct rec { uint32_t vout; int64_t value; uint8_t s[5]; uint32_t sl;
                 uint32_t height; uint8_t cb; };
    struct rec recs[3] = {
        { 0, 11111, {0x76,0xa9,0x14,0,0}, 3, 10, 1 },
        { 1, 22222, {0x6a,0,0,0,0},       1, 20, 0 },
        { 2, 33333, {0x21,0x02,0x03,0x04,0x05}, 5, 30, 0 },
    };
    int64_t total = 0;
    for (int i = 0; i < 3; i++) {
        memcpy(body + off, txid, 32); off += 32;
        lv_wle32(body + off, recs[i].vout); off += 4;
        lv_wle64(body + off, (uint64_t)recs[i].value); off += 8;
        lv_wle32(body + off, recs[i].sl); off += 4;
        memcpy(body + off, recs[i].s, recs[i].sl); off += recs[i].sl;
        lv_wle32(body + off, recs[i].height); off += 4;
        body[off++] = recs[i].cb;
        total += recs[i].value;
    }
    size_t body_len = off;
    b->count = 3;
    b->total = total;

    /* Hash the CLEAN body — that is the root the header claims AND the override
     * checkpoint will carry. */
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, body, body_len);
    sha3_256_finalize(&ctx, b->body_sha3);

    /* Optionally tamper a body byte AFTER hashing → on-disk body no longer
     * matches the header root. */
    if (tamper) body[20] ^= 0xff;

    uint8_t header[104] = {0};
    memcpy(header, "ZCLUTXO\x00", 8);
    lv_wle32(header + 8, 1);                 /* version */
    lv_wle32(header + 16, 1234);             /* height */
    lv_wle64(header + 24, b->count);
    lv_wle64(header + 32, (uint64_t)b->total);
    /* anchor block hash header+40 left zero */
    memcpy(header + 72, b->body_sha3, 32);   /* claimed root = CLEAN body sha3 */

    memcpy(b->file, header, 104);
    memcpy(b->file + 104, body, body_len);
    b->file_len = 104 + body_len;
}

static bool lv_write(const char *path, const struct lv_built *b)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(b->file, 1, b->file_len, f);
    fclose(f);
    return w == b->file_len;
}

/* File-scope seed callback (mirrors config/src/boot_refold_staged.c
 * mint_load_record_cb): insert one snapshot record into coins_kv. */
struct lv_seed_lc { sqlite3 *db; uint64_t n; bool ok; };
static bool lv_seed_cb(const struct uss_record *r, void *vctx)
{
    struct lv_seed_lc *lc = vctx;
    if (!coins_kv_add(lc->db, r->txid, r->vout, r->value, (int32_t)r->height,
                      r->is_coinbase != 0, r->script, (size_t)r->script_len)) {
        lc->ok = false;
        return false;
    }
    lc->n++;
    return true;
}

int test_load_verify_boot(void);
int test_load_verify_boot(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "load_verify_boot", "main");

    /* progress.kv with a coins_kv schema (empty → NOT proven authority). */
    LV_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *pdb = progress_store_db();
    LV_CHECK("pdb handle", pdb != NULL);
    LV_CHECK("coins_kv schema", coins_kv_ensure_schema(pdb));
    LV_CHECK("coins_kv empty", coins_kv_count(pdb) == 0);

    /* node.db (mint_snapshot_path needs a non-NULL ndb; with ZCL_MINT_ANCHOR_OUT
     * set it never derefs the path, but the eligibility guard requires non-NULL). */
    char dbpath[320];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);
    struct node_db ndb;
    LV_CHECK("node_db opens", node_db_open(&ndb, dbpath));

    /* Point the mint-snapshot path helper directly at our sidecar via the env
     * override the production path-derivation honors. */
    char snap_path[400];
    snprintf(snap_path, sizeof(snap_path), "%s/utxo-anchor.snapshot", dir);
    setenv("ZCL_MINT_ANCHOR_OUT", snap_path, 1);

    /* Build a MATCHING snapshot and install its body SHA3 as the checkpoint. */
    struct lv_built ok_snap;
    lv_build_snapshot(&ok_snap, /*tamper=*/false);
    struct sha3_utxo_checkpoint cp_ovr;
    memset(&cp_ovr, 0, sizeof(cp_ovr));
    cp_ovr.height = 1234;
    memcpy(cp_ovr.sha3_hash, ok_snap.body_sha3, 32);
    cp_ovr.utxo_count = ok_snap.count;
    cp_ovr.total_supply = ok_snap.total;
    checkpoints_set_sha3_override_for_test(&cp_ovr);

    /* (c) ABSENT snapshot → eligible=false (safe fallback, no file yet). */
    unlink(snap_path);
    LV_CHECK("(c) absent snapshot → NOT eligible",
             !boot_load_verify_snapshot_eligible(&ndb, pdb));

    /* (a) MATCH: write the matching snapshot. */
    LV_CHECK("write matching snapshot", lv_write(snap_path, &ok_snap));
    LV_CHECK("(a) matching snapshot → eligible",
             boot_load_verify_snapshot_eligible(&ndb, pdb));

    /* (a) the verified set actually LOADS via the same loader binding the seed
     * uses (uss_open with expected_sha3 = the checkpoint), and iterates into
     * coins_kv at the anchor count. */
    {
        char err[128] = {0};
        struct uss_header hdr;
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        struct uss_handle *h = uss_open(snap_path, /*verify_full_sha3=*/true,
                                        cp->sha3_hash, &hdr, err, sizeof(err));
        LV_CHECK("(a) uss_open binds checkpoint root + verifies", h != NULL);
        if (h) {
            LV_CHECK("(a) header count == checkpoint", hdr.count == cp->utxo_count);
            /* Seed coins_kv exactly like config/src/boot_refold_staged.c's
             * mint_load_record_cb does (file-scope lv_seed_cb below). */
            struct lv_seed_lc lc = { .db = pdb, .n = 0, .ok = true };
            int64_t emitted = uss_iter(h, lv_seed_cb, &lc);
            LV_CHECK("(a) seeded all records", emitted == (int64_t)hdr.count && lc.ok);
            LV_CHECK("(a) coins_kv holds the anchor count",
                     coins_kv_count(pdb) == (int64_t)cp->utxo_count);
            uss_close(h);
        }
    }

    /* (d) HEALTHY: coins_kv now holds the set; mark the migration-complete stamp
     * + applied height so it is the PROVEN authority → eligible must be FALSE even
     * though the matching snapshot is still present (never reset a synced node). */
    {
        char *terr = NULL;
        sqlite3_exec(pdb, "BEGIN IMMEDIATE", NULL, NULL, &terr);
        uint8_t one = 1;
        (void)progress_meta_set_in_tx(pdb, COINS_KV_MIGRATION_COMPLETE_KEY, &one, 1);
        (void)coins_kv_set_applied_height_in_tx(pdb, 1234);
        sqlite3_exec(pdb, "COMMIT", NULL, NULL, &terr);
        if (terr) sqlite3_free(terr);
        LV_CHECK("(d) proven authority → NOT eligible",
                 coins_kv_is_proven_authority(pdb, NULL) &&
                 !boot_load_verify_snapshot_eligible(&ndb, pdb));
    }

    /* Reset coins_kv back to empty for the mismatch case (not proven). */
    LV_CHECK("reset coins_kv for mismatch case", coins_kv_reset_for_reseed(pdb));
    LV_CHECK("coins_kv empty again", coins_kv_count(pdb) == 0);

    /* (b) MISMATCH: write a snapshot whose on-disk body SHA3 != the header's
     * claimed root == the checkpoint. uss_open(.,expected=cp) must REFUSE it
     * (NULL) and the gate must be FALSE → a bad set NEVER seeds. */
    {
        struct lv_built bad_snap;
        lv_build_snapshot(&bad_snap, /*tamper=*/true);
        /* Header still claims ok_snap.body_sha3 (== checkpoint) but the body was
         * tampered → full-body recompute diverges. */
        LV_CHECK("write tampered snapshot", lv_write(snap_path, &bad_snap));

        char err[128] = {0};
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        struct uss_handle *h = uss_open(snap_path, /*verify_full_sha3=*/true,
                                        cp->sha3_hash, NULL, err, sizeof(err));
        LV_CHECK("(b) tampered snapshot REFUSED by loader", h == NULL);
        if (h) uss_close(h);
        LV_CHECK("(b) tampered snapshot → NOT eligible",
                 !boot_load_verify_snapshot_eligible(&ndb, pdb));
        LV_CHECK("(b) coins_kv NEVER seeded by a bad set",
                 coins_kv_count(pdb) == 0);
    }

    /* (b2) MISMATCH via wrong-checkpoint: a clean snapshot whose root != the
     * compiled checkpoint (different override). uss_open binds expected_sha3 and
     * rejects at the header memcmp BEFORE the body recompute. */
    {
        struct lv_built ok2;
        lv_build_snapshot(&ok2, /*tamper=*/false);
        LV_CHECK("rewrite clean snapshot", lv_write(snap_path, &ok2));
        /* Install a DIFFERENT checkpoint root (flip a byte) so the snapshot's
         * (valid) root no longer equals the checkpoint. */
        struct sha3_utxo_checkpoint cp_wrong = cp_ovr;
        cp_wrong.sha3_hash[0] ^= 0xff;
        checkpoints_set_sha3_override_for_test(&cp_wrong);

        char err[128] = {0};
        struct uss_handle *h = uss_open(snap_path, /*verify_full_sha3=*/true,
                                        cp_wrong.sha3_hash, NULL, err, sizeof(err));
        LV_CHECK("(b2) wrong-checkpoint snapshot REFUSED", h == NULL);
        if (h) uss_close(h);
        LV_CHECK("(b2) wrong-checkpoint → NOT eligible",
                 !boot_load_verify_snapshot_eligible(&ndb, pdb));
        LV_CHECK("(b2) coins_kv still untouched", coins_kv_count(pdb) == 0);
    }

    /* Teardown. */
    checkpoints_set_sha3_override_for_test(NULL);
    unsetenv("ZCL_MINT_ANCHOR_OUT");
    node_db_close(&ndb);
    unlink(snap_path);
    return failures;
}
