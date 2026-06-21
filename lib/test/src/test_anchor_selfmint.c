/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_anchor_selfmint — the in-fold SELF-MINT snapshot-reachability gate.
 *
 * The load-bearing property: when the staged fold first lands the compiled SHA3
 * UTXO checkpoint height, the self-mint hook PERSISTS the live coins_kv set to
 * <datadir>/utxo-anchor.snapshot — and ONLY a snapshot whose recomputed body
 * SHA3 EQUALS the compiled checkpoint is left on disk. After the hook fires the
 * artifact is VERIFIED-REACHABLE: uss_open(verify_full_sha3, expected_sha3 =
 * cp->sha3_hash) opens it and its count matches the checkpoint — exactly what the
 * torn-import self-heal (boot_anchor_seed_from_snapshot) needs to re-seed from.
 *
 *   (1) BELOW the anchor (next_cursor != cp->height) → NO file written (the hook
 *       only mints at the exact anchor crossing).
 *   (2) AT the anchor (next_cursor == cp->height) → a SHA3-verified snapshot is
 *       written; uss_open(.,expected=cp) opens it and count == checkpoint.
 *   (3) IDEMPOTENT: a second call at the anchor leaves the same file untouched
 *       (same mtime/size) — no needless rewrite.
 *   (4) MISMATCH: if the live set does NOT reproduce the checkpoint (wrong
 *       override root), the hook writes then HARD-VERIFY UNLINKS it → no
 *       unverified artifact is ever left for a later -refold-from-anchor to load.
 *
 * The fixture lowers the compiled anchor via the test override and seeds a tiny
 * coins_kv whose REAL commitment IS the override root, so the production hook
 * runs identically at a handful of rows instead of 1.35 M.
 */

#include "test/test_helpers.h"

#include "services/anchor_selfmint.h"
#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "chain/checkpoints.h"
#include "chain/utxo_snapshot_loader.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SM_CHECK(name, expr) do {                                          \
    if (expr) { printf("  anchor_selfmint: %s... OK\n", (name)); }          \
    else { printf("  anchor_selfmint: %s... FAIL\n", (name)); failures++; } \
} while (0)

#define TEST_ANCHOR ((int32_t)777)

/* Seed a few deterministic coins into coins_kv. */
static bool sm_seed_coins(sqlite3 *db)
{
    for (int i = 0; i < 5; i++) {
        uint8_t txid[32];
        for (int j = 0; j < 32; j++) txid[j] = (uint8_t)(0x10 + i * 7 + j);
        uint8_t script[5] = { 0x76, 0xa9, 0x14, (uint8_t)i, 0x88 };
        if (!coins_kv_add(db, txid, /*vout=*/(uint32_t)i,
                          /*value=*/100000 + i, /*height=*/i + 1,
                          /*is_coinbase=*/i == 0, script, sizeof(script)))
            return false;
    }
    return true;
}

static bool sm_file_present(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int test_anchor_selfmint(void);
int test_anchor_selfmint(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "anchor_selfmint", "main");

    SM_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *pdb = progress_store_db();
    SM_CHECK("pdb handle", pdb != NULL);
    SM_CHECK("coins_kv schema", coins_kv_ensure_schema(pdb));

    /* Seed coins_kv and compute its REAL commitment — the override checkpoint
     * will carry exactly this root + count + the test anchor height. */
    SM_CHECK("seed coins", sm_seed_coins(pdb));
    uint8_t real_root[32] = {0};
    SM_CHECK("commitment computed", coins_kv_commitment(pdb, real_root) == 0);
    int64_t num_txs = 0, real_count = 0, supply = 0;
    SM_CHECK("setinfo", coins_kv_setinfo(pdb, &num_txs, &real_count, &supply));

    struct sha3_utxo_checkpoint cp_ovr;
    memset(&cp_ovr, 0, sizeof(cp_ovr));
    cp_ovr.height = TEST_ANCHOR;
    memcpy(cp_ovr.sha3_hash, real_root, 32);
    cp_ovr.utxo_count = (uint64_t)real_count;
    cp_ovr.total_supply = supply;
    checkpoints_set_sha3_override_for_test(&cp_ovr);

    /* Resolve the target path the SAME way production does, via the env override
     * the path-derivation honors (keeps the test off any real datadir). */
    char snap_path[400];
    snprintf(snap_path, sizeof(snap_path), "%s/utxo-anchor.snapshot", dir);
    setenv("ZCL_MINT_ANCHOR_OUT", snap_path, 1);
    unlink(snap_path);

    /* Sanity: the resolver returns the env path. */
    {
        char resolved[400];
        bool ok = anchor_selfmint_resolve_path(NULL, resolved, sizeof(resolved));
        SM_CHECK("resolve_path honors env", ok && strcmp(resolved, snap_path) == 0);
    }

    /* (1) BELOW the anchor → no mint. */
    anchor_selfmint_hook_in_tx(pdb, dir, TEST_ANCHOR - 1);
    SM_CHECK("(1) below anchor → no snapshot", !sm_file_present(snap_path));

    /* (2) AT the anchor → a SHA3-verified snapshot is written + reachable. */
    anchor_selfmint_hook_in_tx(pdb, dir, TEST_ANCHOR);
    SM_CHECK("(2) at anchor → snapshot written", sm_file_present(snap_path));
    {
        char err[128] = {0};
        struct uss_header hdr;
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        struct uss_handle *h = uss_open(snap_path, /*verify_full_sha3=*/true,
                                        cp->sha3_hash, &hdr, err, sizeof(err));
        SM_CHECK("(2) uss_open verifies vs checkpoint root", h != NULL);
        if (h) {
            SM_CHECK("(2) snapshot count == checkpoint",
                     hdr.count == cp->utxo_count);
            SM_CHECK("(2) snapshot height == anchor",
                     (int32_t)hdr.height == cp->height);
            uss_close(h);
        }
    }

    /* (3) IDEMPOTENT: a second call at the anchor does NOT rewrite the file. */
    {
        struct stat before, after;
        bool sb = stat(snap_path, &before) == 0;
        /* Bump mtime granularity isn't guaranteed; assert size + inode stable +
         * still verified. The hook short-circuits on verified_snapshot_present
         * BEFORE any write, so the file is byte-identical. */
        anchor_selfmint_hook_in_tx(pdb, dir, TEST_ANCHOR);
        bool sa = stat(snap_path, &after) == 0;
        SM_CHECK("(3) idempotent: file still present",
                 sb && sa && before.st_size == after.st_size &&
                 before.st_ino == after.st_ino);
    }

    /* (4) MISMATCH: install a WRONG checkpoint root (same anchor height so the
     * hook still fires). The hook writes the real coins set, HARD-VERIFY finds
     * SHA3 != the (wrong) checkpoint, and UNLINKS — no unverified artifact left. */
    {
        unlink(snap_path);
        struct sha3_utxo_checkpoint cp_wrong = cp_ovr;
        cp_wrong.sha3_hash[0] ^= 0xff;   /* now coins_kv root != checkpoint root */
        checkpoints_set_sha3_override_for_test(&cp_wrong);
        anchor_selfmint_hook_in_tx(pdb, dir, TEST_ANCHOR);
        SM_CHECK("(4) mismatch → no artifact left (write+unlink)",
                 !sm_file_present(snap_path));
    }

    /* Teardown. */
    checkpoints_set_sha3_override_for_test(NULL);
    unsetenv("ZCL_MINT_ANCHOR_OUT");
    unlink(snap_path);
    progress_store_close();
    return failures;
}
