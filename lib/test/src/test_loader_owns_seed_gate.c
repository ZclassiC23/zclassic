/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_loader_owns_seed_gate — regression gate for the two DEPLOYED
 * daily-driver fixes that previously had NO test (a refactor could revert
 * either and re-wedge the live node with green CI).
 *
 * FIX 1 — loader_owns_seed (config/src/boot_services.c app_init_services).
 *   When -load-snapshot-at-own-height is set, the loader at boot.c has already
 *   re-seeded coins_kv from the body-digest-verified assisted snapshot at its
 *   OWN height and
 *   raised the tip_finalize trusted base there; it is the authoritative seed
 *   for this boot. Both fallback seeders
 *   (boot_refold_from_anchor_arm_if_torn AND
 *   block_index_loader_seed_stages_from_cold_import) MUST be skipped — they
 *   re-seed from the COMPILED checkpoint, dropping the trusted base and
 *   re-wedging forward sync. The skip decision is the PURE predicate
 *   boot_loader_owns_seed(ctx): true iff ctx && ctx->load_snapshot_at_own_height
 *   != NULL. This test pins:
 *     (1) flag SET    -> true  (skip fallbacks; loader owns the seed)
 *     (2) flag UNSET  -> false (a normal boot still runs the cold-import seed)
 *     (3) ctx == NULL -> false (no crash, no skip)
 *   REGRESSION: if the gate is reverted to ignore load_snapshot_at_own_height
 *   (e.g. always-false / drop the field from the condition), case (1) flips to
 *   false and this test FAILs — exactly the seed-clobber that re-wedges the
 *   live node.
 *
 * FIX 3 — forged-snapshot anchor-hash FATAL
 *   (config/src/boot_refold_staged.c boot_load_snapshot_at_own_height_reset).
 *   The loaded snapshot's anchor_block_hash MUST byte-equal the in-binary
 *   PoW-proven header hash at the snapshot height; on mismatch the loader
 *   FATALs (refuses a forged / wrong-chain snapshot) rather than seeding
 *   contaminated coins. The match decision is the PURE predicate
 *   boot_snapshot_anchor_hash_matches(index_hash, snapshot_hash): true iff the
 *   two 32-byte hashes are byte-identical. This test pins:
 *     (1) identical 32 bytes        -> true  (chain location matches)
 *     (2) one differing byte        -> false (forged/wrong chain; FATAL fires)
 *     (3) all-zero vs real          -> false (the empty-anchor forgery)
 *     (4) NULL on either side       -> false (refuse, no deref)
 *   REGRESSION: if the memcmp is weakened to always-pass (return true), case
 *   (2)/(3) flip to true and this test FAILs — exactly the forged-snapshot
 *   acceptance that seeds contaminated coins.
 *
 * Both predicates are PURE (no side effects), so this test runs in-process with
 * no fork, no datadir, no progress store — fully deterministic and fast.
 */

#define _GNU_SOURCE
#include "test/test_helpers.h"

#include "config/boot.h"          /* struct app_context, boot_snapshot_anchor_hash_matches */
#include "config/boot_internal.h" /* boot_loader_owns_seed */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOSG_CHECK(name, expr) do {                       \
    printf("loader_owns_seed_gate: %s... ", (name));      \
    if (expr) printf("OK\n");                             \
    else { printf("FAIL\n"); failures++; }                \
} while (0)

int test_loader_owns_seed_gate(void)
{
    int failures = 0;

    /* ── FIX 1: boot_loader_owns_seed ───────────────────────────────── */
    {
        struct app_context ctx;
        memset(&ctx, 0, sizeof ctx);

        /* (2) flag UNSET — a normal boot: cold-import seed must still run. */
        ctx.load_snapshot_at_own_height = NULL;
        LOSG_CHECK("fix1: flag unset -> false (normal boot keeps cold-import seed)",
                   boot_loader_owns_seed(&ctx) == false);

        /* (1) flag SET — loader owns the seed; BOTH fallbacks must be skipped.
         * This is the load-bearing case: reverting the gate re-wedges sync. */
        ctx.load_snapshot_at_own_height = "/some/snapshot/path";
        LOSG_CHECK("fix1: flag set -> true (skip both fallback seeders)",
                   boot_loader_owns_seed(&ctx) == true);

        /* (3) ctx == NULL — must not crash, must not skip. */
        LOSG_CHECK("fix1: ctx NULL -> false",
                   boot_loader_owns_seed(NULL) == false);
    }

    /* ── FIX 3: boot_snapshot_anchor_hash_matches ───────────────────── */
    {
        unsigned char a[32], b[32];
        for (int i = 0; i < 32; i++) { a[i] = (unsigned char)(0x10 + i); }
        memcpy(b, a, 32);

        /* (1) identical -> chain location matches; payload remains assisted. */
        LOSG_CHECK("fix3: identical hashes -> true (chain location matches)",
                   boot_snapshot_anchor_hash_matches(a, b) == true);

        /* (2) one byte differs -> forged/wrong chain, FATAL must fire. The
         * differing byte is the LAST one (a naive prefix-only compare would
         * miss it). */
        b[31] ^= 0x01;
        LOSG_CHECK("fix3: last byte differs -> false (forged -> FATAL)",
                   boot_snapshot_anchor_hash_matches(a, b) == false);
        b[31] ^= 0x01; /* restore */

        /* a first-byte difference too (full 32-byte compare). */
        b[0] ^= 0xff;
        LOSG_CHECK("fix3: first byte differs -> false",
                   boot_snapshot_anchor_hash_matches(a, b) == false);
        b[0] ^= 0xff; /* restore */

        /* (3) all-zero snapshot anchor (the empty/forged anchor) vs a real
         * index hash -> false. */
        unsigned char zero[32];
        memset(zero, 0, 32);
        LOSG_CHECK("fix3: all-zero anchor vs real -> false",
                   boot_snapshot_anchor_hash_matches(a, zero) == false);

        /* sanity: identical all-zero would match — but the live path never
         * has a zero index hash; this only documents the predicate is a pure
         * byte-compare, not a special-case. */
        LOSG_CHECK("fix3: identical all-zero -> true (pure byte-compare)",
                   boot_snapshot_anchor_hash_matches(zero, zero) == true);

        /* (4) NULL on either side -> refuse, no deref. */
        LOSG_CHECK("fix3: NULL index hash -> false",
                   boot_snapshot_anchor_hash_matches(NULL, b) == false);
        LOSG_CHECK("fix3: NULL snapshot hash -> false",
                   boot_snapshot_anchor_hash_matches(a, NULL) == false);
    }

    return failures;
}
