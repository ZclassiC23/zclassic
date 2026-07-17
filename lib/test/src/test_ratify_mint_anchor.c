/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit test for the -ratify-mint-anchor core (config/boot.h
 * boot_ratify_mint_anchor_check_and_stamp): re-derive commitment/count/applied-
 * height from a datadir's OWN durable coins_kv, compare against the compiled
 * checkpoint, and — only on full agreement — stamp the migration-complete +
 * self-folded markers the bundle exporter demands and re-arm the mint resume
 * marker. Any disagreement stamps NOTHING.
 *
 * The fixture builds a small coins_kv, derives a checkpoint FROM that durable
 * set (so a matching case is exact by construction), binds the FULL producer
 * lane before any applied frontier exists, then sets applied_height = height+1
 * (the shape a completed genesis..anchor mint leaves). */

#include "test/test_helpers.h"

#include "chain/checkpoints.h"
#include "config/boot.h"
#include "config/mint_anchor_progress.h"
#include "core/uint256.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define RMA_CHECK(name, expr) do {                                          \
    if (expr) { printf("  ratify_mint_anchor: %s... OK\n", (name)); }        \
    else { printf("  ratify_mint_anchor: %s... FAIL\n", (name)); failures++; }\
} while (0)

/* Build a fixture producer datadir: a small durable coins_kv, the FULL producer
 * lane bound (before any frontier), applied_height=`applied`, and a checkpoint
 * `cp` derived from the durable set at `height`. Returns the live db handle. */
static sqlite3 *rma_open_fixture(const char *tag, char *dir, size_t dir_cap,
                                 struct sha3_utxo_checkpoint *cp,
                                 int32_t height, int32_t applied)
{
    test_make_tmpdir(dir, dir_cap, tag, "main");
    if (!progress_store_open(dir))
        return NULL;
    sqlite3 *db = progress_store_db();
    if (!db || !coins_kv_ensure_schema(db) || !progress_meta_table_ensure(db))
        return NULL;
    /* Bind the FULL lane while the datadir has no marker/refold/frontier — the
     * shape a real producer binds before it starts folding. */
    if (!mint_anchor_producer_lane_bind(db, /*checkpoint_fold=*/false))
        return NULL;

    struct uint256 t; uint256_set_null(&t);
    t.data[0] = 0xA1; t.data[1] = 0x50; t.data[31] = 0x77;
    const unsigned char sc[4] = {0xB0, 0xB1, 0xB2, 0xB3};
    if (!coins_kv_add(db, t.data, 0, 1000, height, true, sc, sizeof(sc)) ||
        !coins_kv_add(db, t.data, 1, 2000, height, true, sc, sizeof(sc)))
        return NULL;

    memset(cp, 0, sizeof(*cp));
    cp->height = height;
    if (coins_kv_commitment(db, cp->sha3_hash) != 0)
        return NULL;
    int64_t n = coins_kv_count(db);
    if (n < 0)
        return NULL;
    cp->utxo_count = (uint64_t)n;
    cp->total_supply = 3000;
    for (int i = 0; i < 32; i++)
        cp->block_hash[i] = (uint8_t)(0x30 + i);

    char *err = NULL;
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) == SQLITE_OK
              && coins_kv_set_applied_height_in_tx(db, applied)
              && sqlite3_exec(db, "COMMIT", NULL, NULL, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    return ok ? db : NULL;
}

int test_ratify_mint_anchor(void);
int test_ratify_mint_anchor(void)
{
    printf("\n=== -ratify-mint-anchor core tests ===\n");
    int failures = 0;
    const int32_t H = 100;

    /* NULL hardening (no fixture needed). */
    {
        struct sha3_utxo_checkpoint cp; memset(&cp, 0, sizeof(cp));
        struct boot_ratify_result r;
        RMA_CHECK("null result -> false",
                  !boot_ratify_mint_anchor_check_and_stamp(NULL, &cp, NULL));
        RMA_CHECK("null pdb -> false",
                  !boot_ratify_mint_anchor_check_and_stamp(NULL, &cp, &r));
        RMA_CHECK("null pdb reason set", r.reason[0] != '\0' && !r.ratified);
    }

    /* Case (a): a matching datadir ratifies + stamps all three markers. */
    {
        char dir[256];
        struct sha3_utxo_checkpoint cp;
        sqlite3 *db = rma_open_fixture("ratify_match", dir, sizeof(dir), &cp,
                                       H, /*applied=*/H + 1);
        RMA_CHECK("match: fixture built", db != NULL);
        if (db) {
            RMA_CHECK("match: not yet proven authority",
                      !coins_kv_is_proven_authority(db, NULL));
            struct boot_ratify_result r;
            RMA_CHECK("match: RATIFIED",
                      boot_ratify_mint_anchor_check_and_stamp(db, &cp, &r));
            RMA_CHECK("match: result ratified", r.ratified);
            RMA_CHECK("match: result height", r.height == H);
            RMA_CHECK("match: result count", r.count == cp.utxo_count);
            RMA_CHECK("match: result sha3",
                      memcmp(r.sha3, cp.sha3_hash, 32) == 0);
            RMA_CHECK("match: migration+authority stamped",
                      coins_kv_is_proven_authority(db, NULL));
            RMA_CHECK("match: self-folded marker stamped",
                      coins_kv_contains_refold_marker(db));
            int32_t through = -1;
            bool legacy = false;
            RMA_CHECK("match: resume marker re-armed",
                      mint_anchor_progress_can_resume(db, &cp, &through,
                                                      &legacy));
            RMA_CHECK("match: resume applied-through == anchor", through == H);
        }
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* Case (b): a sha3 mismatch refuses and stamps NOTHING. */
    {
        char dir[256];
        struct sha3_utxo_checkpoint cp;
        sqlite3 *db = rma_open_fixture("ratify_sha3", dir, sizeof(dir), &cp,
                                       H, /*applied=*/H + 1);
        RMA_CHECK("sha3: fixture built", db != NULL);
        if (db) {
            struct sha3_utxo_checkpoint bad = cp;
            bad.sha3_hash[0] ^= 0xFF;  /* corrupt the expected root */
            struct boot_ratify_result r;
            RMA_CHECK("sha3: REFUSED",
                      !boot_ratify_mint_anchor_check_and_stamp(db, &bad, &r));
            RMA_CHECK("sha3: result not ratified", !r.ratified);
            RMA_CHECK("sha3: reason mentions sha3",
                      strstr(r.reason, "sha3") != NULL);
            RMA_CHECK("sha3: nothing stamped (no authority)",
                      !coins_kv_is_proven_authority(db, NULL));
            RMA_CHECK("sha3: nothing stamped (no self-folded)",
                      !coins_kv_contains_refold_marker(db));
        }
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* Case (c): a wrong applied height refuses and stamps NOTHING. */
    {
        char dir[256];
        struct sha3_utxo_checkpoint cp;
        sqlite3 *db = rma_open_fixture("ratify_applied", dir, sizeof(dir), &cp,
                                       H, /*applied=*/H + 5);  /* != H+1 */
        RMA_CHECK("applied: fixture built", db != NULL);
        if (db) {
            struct boot_ratify_result r;
            RMA_CHECK("applied: REFUSED",
                      !boot_ratify_mint_anchor_check_and_stamp(db, &cp, &r));
            RMA_CHECK("applied: result not ratified", !r.ratified);
            RMA_CHECK("applied: reason mentions applied",
                      strstr(r.reason, "applied") != NULL);
            RMA_CHECK("applied: nothing stamped (no authority)",
                      !coins_kv_is_proven_authority(db, NULL));
            RMA_CHECK("applied: nothing stamped (no self-folded)",
                      !coins_kv_contains_refold_marker(db));
        }
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    return failures;
}
