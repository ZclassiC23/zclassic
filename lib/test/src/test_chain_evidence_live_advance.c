/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression for TASK #33: a green at-tip node reported healthy=false because
 * the persisted chain-evidence active-tip froze at the last boot reconcile
 * while the live tip_finalize reducer advanced the served tip every block.
 * The health check's active_tip_hash_mismatch (live tip vs persisted hash)
 * then degraded further per block.
 *
 * These tests drive chain_evidence_controller_record_finalized_tip — the live
 * forward evidence follow the reducer's post-finalize side-effect path calls —
 * and assert the persisted evidence (and the snapshot mismatch flags) follow
 * the served tip, exactly as a real forward block does.
 */

#include "test/test_helpers.h"

#include "services/chain_evidence_authority_service.h"
#include "validation/chainstate.h"
#include "coins/coins_view.h"
#include "models/database.h"

#include <stdio.h>
#include <string.h>

struct live_fixture {
    struct node_db ndb;
    struct block_map bm;
    struct active_chain chain;
    struct block_index *header_tip;
    struct coins_view_cache coins_tip;
    struct chain_state_repository csr;
    struct chain_evidence_controller authority;
    struct uint256 hashes[4];
    struct block_index blocks[4];
};

static bool live_fixture_init(struct live_fixture *f)
{
    chain_evidence_pending_tip_test_reset();
    chain_evidence_controller_test_reset_startup_reconcile();
    memset(f, 0, sizeof(*f));
    if (!node_db_open(&f->ndb, ":memory:"))
        return false;
    block_map_init(&f->bm);
    active_chain_init(&f->chain);

    struct coins_view null_view;
    memset(&null_view, 0, sizeof(null_view));
    coins_view_cache_init(&f->coins_tip, &null_view);

    for (int i = 0; i < 4; i++) {
        memset(f->hashes[i].data, i + 1, 32);
        block_index_init(&f->blocks[i]);
        f->blocks[i].phashBlock = &f->hashes[i];
        f->blocks[i].nHeight = i;
        f->blocks[i].pprev = i ? &f->blocks[i - 1] : NULL;
        f->blocks[i].nStatus = BLOCK_VALID_TREE;
        arith_uint256_set_u64(&f->blocks[i].nChainWork, (uint64_t)i + 1);
        block_map_insert(&f->bm, &f->hashes[i], &f->blocks[i]);
        const struct block_index *canon =
            block_map_find(&f->bm, &f->hashes[i]);
        if (canon)
            f->blocks[i].phashBlock = canon->phashBlock;
    }

    csr_init(&f->csr, &f->bm, &f->chain, &f->header_tip,
             &f->coins_tip, &f->ndb, NULL);
    chain_evidence_controller_init(&f->authority, &f->ndb, &f->csr);
    return true;
}

static void live_fixture_free(struct live_fixture *f)
{
    chain_evidence_pending_tip_test_reset();
    csr_free(&f->csr);
    coins_view_cache_free(&f->coins_tip);
    active_chain_free(&f->chain);
    block_map_free(&f->bm);
    node_db_close(&f->ndb);
}

/* Promote the served tip to `idx` through the boot/import publication path
 * (the only writer that also drives csr_commit_tip + moves the active chain),
 * so the fixture starts with persisted evidence AND the in-memory tip both at
 * `idx` — the post-boot starting state. */
static bool promote_to(struct live_fixture *f, int idx)
{
    struct chain_evidence_controller_tip_request req = {
        .new_tip = &f->blocks[idx],
        .utxo_max_height = idx,
        .update_header_tip = true,
        .reason = "unit.bootstrap",
    };
    req.verified.header_ancestry_linked = true;
    req.verified.chainwork_recomputed = true;
    req.verified.nakamoto_selected_best_work = true;
    req.verified.block_bytes_hash_checked = true;
    return chain_evidence_controller_promote_tip(&f->authority, &req) == CEC_OK;
}

/* The live reducer moves the served tip WITHOUT re-entering promote_tip — the
 * exact path (active_chain_move_window_tip) that left the persisted evidence
 * frozen in production. */
static bool reducer_move_served_tip(struct live_fixture *f, int idx)
{
    return active_chain_move_window_tip(&f->chain, &f->blocks[idx]);
}

/* CORE REGRESSION: after the reducer advances the served tip past the
 * persisted evidence, the health snapshot must degrade (mismatch=true). The
 * live evidence follow then clears it and the persisted height tracks the
 * served tip — the green-node-reports-healthy fix. */
static int test_live_advance_clears_active_tip_hash_mismatch(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (!promote_to(&f, 1))
        failures++;

    /* Reducer advances the served tip to block 2; nothing wrote evidence. */
    if (!reducer_move_served_tip(&f, 2))
        failures++;

    struct chain_evidence_controller_view before;
    chain_evidence_controller_snapshot(&f.authority, &before);
    /* The live tip is block 2 but persisted evidence still names block 1 —
     * this is exactly the production degradation. */
    if (before.active_tip_height != 2 ||
        before.persisted_active_tip_height != 1 ||
        !before.active_tip_hash_mismatch ||
        before.health_reason[0] == '\0')
        failures++;

    /* The live evidence follow advances the persisted evidence. */
    if (!chain_evidence_controller_record_finalized_tip(
            &f.authority, &f.blocks[2], "unit.finalize"))
        failures++;

    struct chain_evidence_controller_view after;
    chain_evidence_controller_snapshot(&f.authority, &after);
    if (after.persisted_active_tip_height != 2 ||
        after.active_tip_hash_mismatch ||
        after.csr_cursor_mismatch ||
        after.health_reason[0] != '\0')
        failures++;
    /* The persisted hash now equals the served tip's hash. */
    if (!after.has_persisted_active_tip_hash ||
        memcmp(after.persisted_active_tip_hash.data,
               f.blocks[2].phashBlock->data, 32) != 0)
        failures++;
    /* coins_best_block_height twin follows too (not frozen at boot). */
    if (after.coins_best_block_height != 2)
        failures++;

    live_fixture_free(&f);
    return failures;
}

/* The follow is idempotent: re-finalizing the same served tip is a no-op that
 * still reports success and keeps the snapshot healthy (a steady at-tip node
 * calls post_finalize once per published block on a held frontier). */
static int test_live_advance_is_idempotent(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (!promote_to(&f, 1))
        failures++;
    if (!reducer_move_served_tip(&f, 2))
        failures++;
    if (!chain_evidence_controller_record_finalized_tip(
            &f.authority, &f.blocks[2], "unit.finalize"))
        failures++;
    /* Second call, same tip: still OK, still healthy. */
    if (!chain_evidence_controller_record_finalized_tip(
            &f.authority, &f.blocks[2], "unit.finalize.again"))
        failures++;

    struct chain_evidence_controller_view view;
    chain_evidence_controller_snapshot(&f.authority, &view);
    if (view.persisted_active_tip_height != 2 ||
        view.active_tip_hash_mismatch ||
        view.health_reason[0] != '\0')
        failures++;

    live_fixture_free(&f);
    return failures;
}

/* The live follow must NEVER paper over a genuine contradiction freeze: a
 * frozen controller declines (returns false) and stays frozen — the boot
 * reconcile owns lifting it, not the hot path. */
static int test_live_advance_declines_when_frozen(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (!promote_to(&f, 1))
        failures++;
    if (!reducer_move_served_tip(&f, 2))
        failures++;

    chain_evidence_controller_freeze(&f.authority, "unit.injected_contradiction");
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;

    if (chain_evidence_controller_record_finalized_tip(
            &f.authority, &f.blocks[2], "unit.finalize"))
        failures++;  /* must return false while frozen */
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;  /* and must NOT lift the freeze */

    live_fixture_free(&f);
    return failures;
}

/* THE PRODUCTION PATH (post-deadlock 2026-06-12): the reducer drive only
 * NOTES the published tip (leaf-mutex slot, no evidence machinery — the drive
 * holds coins_kv and taking csr->lock there was the ABBA deadlock); the
 * health-collect drain runs the actual record with the correct lock order.
 * Drives note -> degraded snapshot still visible pre-drain via direct
 * snapshot -> drain -> healthy; a second drain with nothing pending is a
 * cheap no-op success; a superseding note before drain records the NEWEST
 * tip. */
static int test_live_advance_note_then_drain(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (!promote_to(&f, 1))
        failures++;

    /* Reducer publishes block 2 then block 3 before any drain: the slot is
     * monotonic and must record the newest tip only. */
    if (!reducer_move_served_tip(&f, 2))
        failures++;
    chain_evidence_note_finalized_tip(&f.blocks[2]);
    if (!reducer_move_served_tip(&f, 3))
        failures++;
    chain_evidence_note_finalized_tip(&f.blocks[3]);

    /* Health drain (the node_health_collect call order: init -> drain ->
     * snapshot) advances the persisted evidence to the served tip. */
    if (!chain_evidence_drain_pending_tip(&f.authority))
        failures++;

    struct chain_evidence_controller_view after;
    chain_evidence_controller_snapshot(&f.authority, &after);
    if (after.persisted_active_tip_height != 3 ||
        after.active_tip_hash_mismatch ||
        after.health_reason[0] != '\0')
        failures++;
    if (!after.has_persisted_active_tip_hash ||
        memcmp(after.persisted_active_tip_hash.data,
               f.blocks[3].phashBlock->data, 32) != 0)
        failures++;

    /* Nothing pending: drain is a no-op success. */
    if (!chain_evidence_drain_pending_tip(&f.authority))
        failures++;

    /* Bad note args are ignored without crashing or corrupting the slot. */
    chain_evidence_note_finalized_tip(NULL);
    if (!chain_evidence_drain_pending_tip(&f.authority))
        failures++;

    live_fixture_free(&f);
    return failures;
}

/* Null / invalid args decline gracefully — no crash, no write, returns false. */
static int test_live_advance_rejects_bad_args(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (chain_evidence_controller_record_finalized_tip(NULL, &f.blocks[1],
                                                       "unit"))
        failures++;
    if (chain_evidence_controller_record_finalized_tip(&f.authority, NULL,
                                                       "unit"))
        failures++;

    live_fixture_free(&f);
    return failures;
}

/* (d2) A demonstrably-reconciled boot-transient tip divergence self-clears:
 * the freeze reason is the "active_tip_hash != csr_tip_hash" prefix, the reducer
 * finalized the SAME tip the live active chain holds, and csr agrees — so
 * record_finalized_tip lifts the freeze and publishes evidence. This is the
 * dev-lane false-page fix (un-pages without a reboot via the health drain). */
static int test_live_advance_clears_boot_tip_divergence_freeze(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (!promote_to(&f, 1))
        failures++;
    if (!reducer_move_served_tip(&f, 2))   /* live active tip = block 2 */
        failures++;
    chain_evidence_controller_freeze(&f.authority,
                                     "active_tip_hash != csr_tip_hash (h=1)");
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;

    /* Reducer finalized the SAME tip the live active chain holds -> resolved. */
    if (!chain_evidence_controller_record_finalized_tip(
            &f.authority, &f.blocks[2], "unit.finalize"))
        failures++;
    if (f.authority.state != CEC_EMPTY)
        failures++;
    if (f.authority.contradiction_reason[0] != '\0')
        failures++;

    struct chain_evidence_controller_view after;
    chain_evidence_controller_snapshot(&f.authority, &after);
    if (after.persisted_active_tip_height != 2 ||
        after.active_tip_hash_mismatch ||
        after.health_reason[0] != '\0')
        failures++;

    live_fixture_free(&f);
    return failures;
}

/* (d2) narrow gating: a freeze whose reason is NOT the boot-divergence prefix
 * (a genuine structural contradiction) is NEVER self-cleared, even when the live
 * tip is fully reconciled. It declines and stays frozen — the operator page
 * holds. */
static int test_live_advance_keeps_genuine_freeze(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (!promote_to(&f, 1))
        failures++;
    if (!reducer_move_served_tip(&f, 2))
        failures++;
    chain_evidence_controller_freeze(&f.authority,
                                     "active_tip_ancestry_unlinkable (h=1)");

    if (chain_evidence_controller_record_finalized_tip(
            &f.authority, &f.blocks[2], "unit.finalize"))
        failures++;   /* must decline */
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;   /* must stay frozen */

    live_fixture_free(&f);
    return failures;
}

/* (d2) fail-closed: even WITH the self-clearable reason, the freeze holds unless
 * the reducer-finalized tip PROVABLY equals the live csr/active tip. A finalize
 * for a height other than the live active tip (reorg-in-flight / stale) declines
 * and stays frozen — never a false clear. */
static int test_live_advance_keeps_unreconciled_divergence(void)
{
    int failures = 0;
    struct live_fixture f;
    if (!live_fixture_init(&f))
        return 1;

    if (!promote_to(&f, 1))
        failures++;
    if (!reducer_move_served_tip(&f, 2))   /* live active tip = block 2 */
        failures++;
    chain_evidence_controller_freeze(&f.authority,
                                     "active_tip_hash != csr_tip_hash (h=1)");

    /* Finalize names block 1, but the live active tip is block 2 -> NOT proven
     * reconciled -> declines, stays frozen. */
    if (chain_evidence_controller_record_finalized_tip(
            &f.authority, &f.blocks[1], "unit.finalize"))
        failures++;
    if (f.authority.state != CEC_CONTRADICTION_FROZEN)
        failures++;

    live_fixture_free(&f);
    return failures;
}

int test_chain_evidence_live_advance(void)
{
    int failures = 0;
    printf("\n=== chain_evidence_live_advance tests ===\n");
    failures += test_live_advance_clears_active_tip_hash_mismatch();
    failures += test_live_advance_note_then_drain();
    failures += test_live_advance_is_idempotent();
    failures += test_live_advance_declines_when_frozen();
    failures += test_live_advance_clears_boot_tip_divergence_freeze();
    failures += test_live_advance_keeps_genuine_freeze();
    failures += test_live_advance_keeps_unreconciled_divergence();
    failures += test_live_advance_rejects_bad_args();
    if (failures == 0)
        printf("  all chain_evidence_live_advance tests passed\n");
    else
        printf("  %d chain_evidence_live_advance failure(s)\n", failures);
    return failures;
}
