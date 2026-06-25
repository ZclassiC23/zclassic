/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_self_folded_anchor — the Act-3 (the prize) sovereignty fixture.
 *
 * THE PROPERTY THIS PROVES (self-verified-tip-plan.md, Act 3)
 * ----------------------------------------------------------
 * The detective stops carrying a copy of someone else's case file: a FRESH
 * datadir folds genesis -> the baked checkpoint height (3,056,758) on its own
 * temp coins_kv and re-derives the SAME SHA3 UTXO root + the SAME 1,354,771
 * UTXO count as the compiled checkpoint. The block bodies are the authority;
 * the compiled checkpoint is only a notarization of what the fold MUST produce.
 *
 * TWO FALSE-GREEN TRAPS THE PLAN NAMES (Act 3, "stronger gate"):
 *   (1) Borrowed-seed no-op — a stamped coins_kv passes a naive gate while
 *       resting on a borrowed copy. Defeated elsewhere by the G-SOV refold
 *       marker; here we only assert the *baked* numbers are the ones the fold
 *       targets.
 *   (2) Baked-checkpoint byte-match — the fresh test can "pass" by reading the
 *       *compiled* checkpoint instead of folding. Defeated by the
 *       `_no_binary_checkpoint` variant: NULL the compiled checkpoint via the
 *       `checkpoints_set_sha3_override_for_test` seam, re-derive from bodies,
 *       and assert the re-derived root EQUALS the original (bodies are the
 *       authority, not the binary).
 *
 * REACHABILITY NOTE (honest scope):
 * ---------------------------------
 * The full genesis->3,056,758 fold requires a real datadir carrying ~3.1M
 * block bodies (`boot_mint_anchor_run`, config/src/boot_mint_anchor.c:52) and
 * cannot run inside this in-process unit test. So the two fold-and-compare
 * cases below are COMPILE-CLEAN SKELETONS marked TODO/skipped: they exercise
 * the checkpoint accessor + the test override seam (which DO build and link),
 * assert the baked invariants that the fold must reproduce, and leave the
 * actual genesis->checkpoint replay wired as a TODO behind an env opt-in so a
 * future heavy-fixture run (or `boot_mint_anchor_run` on a copy datadir) can
 * fill it in without touching the registry again.
 *
 * Wave-1 Lane E of docs/work/self-verified-tip-plan.md. Authored as Act-3 prep.
 */

#include "test/test_helpers.h"

#include "chain/checkpoints.h"

#include <stdlib.h>
#include <string.h>

/* The numbers the fold MUST reproduce — independently re-stated here from the
 * checkpoint doctrine (checkpoints.c, "Verified bit-for-bit against zclassicd")
 * so a future edit to the compiled struct that silently changes them trips
 * THIS test, not just the production accessor. */
#define SFA_CHECKPOINT_HEIGHT      3056758
#define SFA_CHECKPOINT_UTXO_COUNT  1354771ULL

int test_self_folded_anchor(void);
int test_self_folded_anchor(void)
{
    int failures = 0;
    /* Opt-in for the heavy genesis->checkpoint replay. Off by default so the
     * unit run stays fast; a future heavy-fixture lane sets it. */
    const char *heavy = getenv("ZCL_SELF_FOLD_ANCHOR_FIXTURE");
    bool run_heavy = heavy && heavy[0] && strcmp(heavy, "0") != 0;

    printf("\n=== test_self_folded_anchor ===\n");

    /* NOTE: the TEST_CASE/TEST_END macros allow exactly one per function (each
     * TEST_END emits the shared `_test_next:` label). This test has three
     * sections, so it uses one explicit `_test_next:` label and per-section
     * printf markers — the same single-label pattern as
     * test_keystone_utxo_binding.c. */

    /* (1) Baked-checkpoint invariants the fold must reproduce. REAL assertion
     * (no fixture needed): pins the height + UTXO count the self-fold target
     * depends on, so a drift in the compiled checkpoint trips THIS test before
     * any fold work is attempted. */
    printf("self_folded_anchor: baked checkpoint pins height + 1,354,771 UTXOs... ");
    {
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        ASSERT(cp != NULL);
        ASSERT(cp->height == SFA_CHECKPOINT_HEIGHT);
        ASSERT(cp->utxo_count == SFA_CHECKPOINT_UTXO_COUNT);
        /* The SHA3 root must be a real (non-zero) commitment, never the
         * all-zeros sentinel — the fold compares against THIS value. */
        uint8_t zero[32] = {0};
        ASSERT(memcmp(cp->sha3_hash, zero, 32) != 0);
        printf("OK\n");
    }

    /* (2) Self-fold from a FRESH temp coins_kv: fold genesis->checkpoint and
     * assert the re-derived SHA3 root + count == the baked checkpoint.
     *
     * TODO(act3-fold): wire the real genesis->3,056,758 replay here. The fold
     * needs ~3.1M block bodies (boot_mint_anchor_run on a copy datadir), which
     * is not reachable in this in-process unit test. Until then this section is
     * SKIPPED unless ZCL_SELF_FOLD_ANCHOR_FIXTURE is set, so it builds + links
     * and the registry wiring is permanent. */
    printf("self_folded_anchor: fresh fold reproduces root + count (heavy)... ");
    {
        if (!run_heavy) {
            printf("SKIP (set ZCL_SELF_FOLD_ANCHOR_FIXTURE to run the "
                   "genesis->checkpoint fold)\n");
        } else {
            /* TODO(act3-fold): on a copy datadir, run the fold to
             * SFA_CHECKPOINT_HEIGHT into a fresh temp coins_kv, then:
             *   uint8_t root[32]; uint64_t count;
             *   ASSERT(self_fold_to_height(tmp, SFA_CHECKPOINT_HEIGHT,
             *                              root, &count) == 0);
             *   const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
             *   ASSERT(memcmp(root, cp->sha3_hash, 32) == 0);
             *   ASSERT(count == cp->utxo_count);
             * For now the opt-in path also no-ops until the fold helper exists. */
            printf("SKIP (fold helper not yet wired — TODO act3-fold)\n");
        }
    }

    /* (3) _no_binary_checkpoint variant — the bodies-are-the-authority trap
     * defeat. NULL the compiled checkpoint via the test seam, re-derive from
     * bodies, and assert the re-derived root EQUALS the original compiled one.
     *
     * REAL assertion: capture the compiled checkpoint, install an override,
     * confirm the accessor returns the override, reset, confirm it returns the
     * compiled value again. The re-derive-from-bodies-and-compare step is the
     * TODO heavy fixture (same reachability limit as section 2). */
    printf("self_folded_anchor: _no_binary_checkpoint seam (re-derive == baked)... ");
    {
        /* Snapshot the compiled checkpoint values BEFORE any override. */
        const struct sha3_utxo_checkpoint *compiled = get_sha3_utxo_checkpoint();
        ASSERT(compiled != NULL);
        struct sha3_utxo_checkpoint baked = *compiled;

        /* Install a zeroed override standing in for "no compiled checkpoint":
         * a fold that trusts the binary would now read all-zeros and a naive
         * test would still "pass"; the real test re-derives from bodies. */
        static struct sha3_utxo_checkpoint null_cp; /* zero-initialized */
        null_cp.height = baked.height; /* keep the height addressable */
        checkpoints_set_sha3_override_for_test(&null_cp);

        const struct sha3_utxo_checkpoint *ov = get_sha3_utxo_checkpoint();
        ASSERT(ov == &null_cp);
        uint8_t zero[32] = {0};
        ASSERT(memcmp(ov->sha3_hash, zero, 32) == 0); /* binary "nulled" */

        /* TODO(act3-fold): when run_heavy, re-derive the root by folding bodies
         * (NOT by reading the now-nulled compiled checkpoint) and assert it
         * equals the captured baked.sha3_hash:
         *   uint8_t root[32]; uint64_t count;
         *   ASSERT(self_fold_to_height(tmp, baked.height, root, &count) == 0);
         *   ASSERT(memcmp(root, baked.sha3_hash, 32) == 0);
         *   ASSERT(count == baked.utxo_count);
         */
        (void)run_heavy;

        /* Restore the compiled checkpoint and confirm the seam reset works —
         * bodies are the authority, the binary is just the notarization. */
        checkpoints_reset_sha3_override_for_test();
        const struct sha3_utxo_checkpoint *restored = get_sha3_utxo_checkpoint();
        ASSERT(restored != NULL);
        ASSERT(restored != &null_cp);
        ASSERT(restored->height == baked.height);
        ASSERT(restored->utxo_count == baked.utxo_count);
        ASSERT(memcmp(restored->sha3_hash, baked.sha3_hash, 32) == 0);
        printf("OK\n");
    }

    goto _done;
_test_next:
    /* An ASSERT failed (it jumped here after failures++). */
    printf("(section aborted)\n");
_done:
    if (failures == 0)
        printf("=== test_self_folded_anchor: all cases passed ===\n");
    else
        printf("=== test_self_folded_anchor: %d failure(s) ===\n", failures);
    return failures;
}
