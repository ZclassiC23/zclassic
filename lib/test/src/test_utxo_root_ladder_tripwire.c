/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the OBSERVE-ONLY golden UTXO-root ladder tripwire
 * (app/jobs/src/utxo_root_ladder_tripwire.c). Mirrors
 * test_sha3_windows.c's structure for its sibling tripwire: SKIP/silent on
 * a healthy verdict, FIRES a typed PERMANENT blocker naming the divergent
 * height on a mismatch, respects the operator kill-switch, and — the hard
 * parity guard — never moves chain_linkage_hold's accept/reject gate either
 * way (see test_sha3_windows.c:173-207 for the sibling proof this mirrors).
 *
 * The tripwire's header is internal to app/jobs/src (not on the test
 * include path by design — same convention as test_sha3_windows.c), so
 * mirror the enum + declare the entry points directly. Kept in lockstep
 * with utxo_root_ladder_tripwire.h. */

#include "test/test_helpers.h"

#include "chain/utxo_root_ladder.h"
#include "models/utxo_root_ladder_verify.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "validation/chain_linkage_check.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum utxo_root_ladder_tripwire_result {
    UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY  = 0,
    UTXO_ROOT_LADDER_TRIPWIRE_MISMATCH = 1,
};
extern enum utxo_root_ladder_tripwire_result
utxo_root_ladder_tripwire_report(const struct utxo_root_ladder_verify_result *results,
                                 size_t n);
extern void utxo_root_ladder_tripwire_at_boundary(int height);

#define TRIPWIRE_BLOCKER_ID "utxo_ladder_mismatch"

int test_utxo_root_ladder_tripwire(void)
{
    int failures = 0;

    printf("\n=== test_utxo_root_ladder_tripwire ===\n");

    /* report() on an empty/NULL result set: HEALTHY, no blocker. */
    printf("utxo_root_ladder_tripwire: report() is silent on an empty result "
          "set... ");
    {
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        enum utxo_root_ladder_tripwire_result rc =
            utxo_root_ladder_tripwire_report(NULL, 0);
        if (rc == UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY &&
            !blocker_exists(TRIPWIRE_BLOCKER_ID)) printf("OK\n");
        else { printf("FAIL (rc=%d)\n", (int)rc); failures++; }
    }

    /* Every rung MATCH or NOT_YET_REACHED: SILENT — normal daily operation
     * for most nodes most of the time. */
    printf("utxo_root_ladder_tripwire: report() is SILENT when healthy "
          "(MATCH/NOT_YET_REACHED only)... ");
    {
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        struct utxo_root_ladder_verify_result results[2] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_MATCH },
            { 200000, UTXO_ROOT_LADDER_VERIFY_NOT_YET_REACHED },
        };
        enum utxo_root_ladder_tripwire_result rc =
            utxo_root_ladder_tripwire_report(results, 2);
        if (rc == UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY &&
            !blocker_exists(TRIPWIRE_BLOCKER_ID)) printf("OK\n");
        else { printf("FAIL (rc=%d blocker=%d)\n", (int)rc,
                      (int)blocker_exists(TRIPWIRE_BLOCKER_ID)); failures++; }
    }

    /* A synthetic DIVERGENT rung FIRES: typed PERMANENT blocker, and the
     * reason names the exact divergent height. */
    printf("utxo_root_ladder_tripwire: report() FIRES on a synthetic "
          "divergent boundary-root, names the height... ");
    {
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        struct utxo_root_ladder_verify_result results[2] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_MATCH },
            { 3056758, UTXO_ROOT_LADDER_VERIFY_DIVERGENT },
        };
        enum utxo_root_ladder_tripwire_result rc =
            utxo_root_ladder_tripwire_report(results, 2);
        int cls = blocker_class_for(TRIPWIRE_BLOCKER_ID);
        if (rc == UTXO_ROOT_LADDER_TRIPWIRE_MISMATCH &&
            blocker_exists(TRIPWIRE_BLOCKER_ID) &&
            cls == BLOCKER_PERMANENT) printf("OK\n");
        else { printf("FAIL (rc=%d blocker=%d class=%d)\n", (int)rc,
                      (int)blocker_exists(TRIPWIRE_BLOCKER_ID), cls);
               failures++; }
    }
    blocker_clear(TRIPWIRE_BLOCKER_ID);

    /* at_boundary(): the kill switch fully suppresses the check, even at a
     * real %100 boundary height against a genuinely corrupted store. */
    printf("utxo_root_ladder_tripwire: at_boundary() respects "
          "ZCL_DISABLE_UTXO_LADDER_TRIPWIRE... ");
    if (g_utxo_root_ladder_count > 0) {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_tripwire_kill", "main");
        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(coins_kv_ensure_schema(db));

        const struct utxo_root_ladder_entry *rung = &g_utxo_root_ladder[0];
        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ASSERT(coins_kv_boundary_root_set(db, rung->height, corrupted));

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        setenv("ZCL_DISABLE_UTXO_LADDER_TRIPWIRE", "1", 1);
        utxo_root_ladder_tripwire_at_boundary(0);   /* 0 % 100 == 0 */
        unsetenv("ZCL_DISABLE_UTXO_LADDER_TRIPWIRE");
        bool ok = !blocker_exists(TRIPWIRE_BLOCKER_ID);

        progress_store_close();
        test_rm_rf_recursive(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL (blocker fired despite kill switch)\n"); failures++; }
    } else {
        printf("OK (no compiled ladder rungs to corrupt)\n");
    }

    /* at_boundary(): with the kill switch OFF, a genuinely corrupted store
     * FIRES for real (end-to-end path, not just report()'s unit API). */
    printf("utxo_root_ladder_tripwire: at_boundary() FIRES against a "
          "corrupted store (kill switch off)... ");
    if (g_utxo_root_ladder_count > 0) {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_tripwire_fire", "main");
        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(coins_kv_ensure_schema(db));

        const struct utxo_root_ladder_entry *rung = &g_utxo_root_ladder[0];
        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ASSERT(coins_kv_boundary_root_set(db, rung->height, corrupted));

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        utxo_root_ladder_tripwire_at_boundary(0);
        bool fired = blocker_exists(TRIPWIRE_BLOCKER_ID);

        progress_store_close();
        test_rm_rf_recursive(dir);
        if (fired) printf("OK\n");
        else { printf("FAIL (tripwire stayed silent against a corrupted "
                      "store)\n"); failures++; }
    } else {
        printf("OK (no compiled ladder rungs to corrupt)\n");
    }

    /* at_boundary(): off-boundary heights are a pure no-op — no db touch,
     * no evidence — even against the same corrupted store. */
    printf("utxo_root_ladder_tripwire: at_boundary() SKIPs an off-boundary "
          "height... ");
    if (g_utxo_root_ladder_count > 0) {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_tripwire_offb", "main");
        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(coins_kv_ensure_schema(db));

        const struct utxo_root_ladder_entry *rung = &g_utxo_root_ladder[0];
        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ASSERT(coins_kv_boundary_root_set(db, rung->height, corrupted));

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        utxo_root_ladder_tripwire_at_boundary(101);   /* 101 % 100 != 0 */
        bool ok = !blocker_exists(TRIPWIRE_BLOCKER_ID);

        progress_store_close();
        test_rm_rf_recursive(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL (fired on an off-boundary height)\n"); failures++; }
    } else {
        printf("OK (no compiled ladder rungs)\n");
    }

    /* HARD PARITY GUARD: firing this tripwire must NOT change the
     * accept/reject gate. chain_linkage_hold is THE latch that would refuse
     * tip moves; assert it is byte-identical (no HOLD, refuse_from == -1)
     * whether report() saw a healthy or a divergent verdict — mirrors
     * test_sha3_windows.c:173-207's proof for the sibling tripwire. */
    printf("utxo_root_ladder_tripwire: is OBSERVE-ONLY (accept/reject "
          "unchanged)... ");
    {
        bool ok = true;
        bool hold_before = chain_linkage_hold_active();
        int  refuse_before = chain_linkage_hold_refuse_from();

        struct utxo_root_ladder_verify_result healthy[1] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_MATCH },
        };
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        (void)utxo_root_ladder_tripwire_report(healthy, 1);
        ok = ok && (chain_linkage_hold_active() == hold_before);
        ok = ok && (chain_linkage_hold_refuse_from() == refuse_before);

        struct utxo_root_ladder_verify_result divergent[1] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_DIVERGENT },
        };
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        (void)utxo_root_ladder_tripwire_report(divergent, 1);
        ok = ok && (chain_linkage_hold_active() == hold_before);
        ok = ok && (chain_linkage_hold_refuse_from() == refuse_before);
        /* Evidence exists, but it is a plain blocker — NOT a pipeline HOLD. */
        ok = ok && blocker_exists(TRIPWIRE_BLOCKER_ID);

        if (ok) printf("OK\n");
        else { printf("FAIL (hold_before=%d refuse_before=%d hold_now=%d "
                      "refuse_now=%d)\n",
                      (int)hold_before, refuse_before,
                      (int)chain_linkage_hold_active(),
                      chain_linkage_hold_refuse_from()); failures++; }
    }

    /* Leave no evidence blocker behind for other groups. */
    blocker_clear(TRIPWIRE_BLOCKER_ID);

    goto _done;
_test_next:
    /* ASSERT() jumps here on a hard failure inside one of the sections
     * above (mirrors test_utxo_root_ladder.c's convention) — failures was
     * already incremented at the ASSERT site. */
    printf("(section aborted)\n");
_done:
    if (failures == 0)
        printf("=== test_utxo_root_ladder_tripwire: all cases passed ===\n");
    else
        printf("=== test_utxo_root_ladder_tripwire: %d failure(s) ===\n",
              failures);
    return failures;
}
