/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the have_data_unreadable Condition — the continuous self-healer
 * for the "torn HAVE_DATA block wedges the tip" class observed live:
 *
 *   find_most_work_chain: STUCK at tip h=N (best_header h=N+k, gap=k)
 *   read_block_pread_fail: h=N+1 file=-1 pos=0   (repeating)
 *
 * MECHANISM (confirmed in source):
 *   - A block above the tip carries BLOCK_HAVE_DATA in nStatus but its
 *     on-disk location is torn: nFile == -1 / nDataPos == 0 (a stale flag
 *     left by a quarantined synthetic tip; no actual body on disk).
 *   - gap_fill_service.c:168 skips any block with BLOCK_HAVE_DATA set, so the
 *     torn block is NEVER re-requested → the body never re-downloads.
 *   - connect_tip cannot read the body (read_block_from_disk_index_pread
 *     fails because nFile < 0), so the tip cannot advance → wedge.
 *
 * THE HEAL (app/conditions/src/have_data_unreadable.c):
 *   detect : tip stalled >=60s AND tip+1 is marked HAVE_DATA but
 *            block_index_have_data_readable() == false (file=-1 → read fails).
 *   remedy : CLEAR the provably-bogus HAVE_DATA flag (+ nFile=-1, nDataPos=0)
 *            so gap_fill re-requests the body. Never drops real data — the
 *            read genuinely failed AND file is already -1.
 *   witness: the tip advanced past the target (re-fetch + connect succeeded),
 *            or the block became readable.
 * Anti-thrash: the Condition engine bounds this at max_attempts=3 with a
 *   backoff_secs=30 window; after exhaustion it pages the operator instead
 *   of looping.
 *
 * Fixture pattern mirrors test_orphan_utxo_above_tip.c: a minimal in-RAM
 * main_state chain, the engine driven via condition_engine_tick(), tip
 * staleness injected via sync_monitor_test_set_tip_advance_ts(). The
 * torn-block readability gate needs NO disk file: a block with nFile=-1
 * makes block_index_have_data_readable() return false immediately.
 */

#include "test/test_helpers.h"

#include "conditions/have_data_unreadable.h"
#include "core/arith_uint256.h"
#include "framework/condition.h"
#include "platform/time_compat.h"
#include "services/sync_monitor.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdio.h>
#include <string.h>

#define HDU_CHECK(name, expr) do {                   \
    printf("have_data_unreadable: %s... ", (name));  \
    if (expr) printf("OK\n");                         \
    else { printf("FAIL\n"); failures++; }           \
} while (0)

/* Build a minimal linked chain in main_state with tip at height n-1. */
static struct block_index *hdu_build_chain(struct main_state *ms, int n,
                                           struct uint256 *hashes)
{
    struct block_index *tip = NULL;
    for (int h = 0; h < n; h++) {
        memset(&hashes[h], 0, sizeof(hashes[h]));
        hashes[h].data[0] = (uint8_t)(h & 0xFF);
        hashes[h].data[1] = (uint8_t)((h >> 8) & 0xFF);
        hashes[h].data[3] = 0xCD; /* distinct salt from other tests */

        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)ms, &hashes[h]);
        if (!pi) continue;

        pi->nHeight = h;
        pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx = 1;
        pi->nChainTx = (uint32_t)(h + 1);
        pi->nFile = 0;       /* on-disk (notional) */
        pi->nDataPos = 8;
        arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        if (h > 0)
            pi->pprev = block_map_find(&ms->map_block_index, &hashes[h - 1]);
        tip = pi;
    }
    if (tip)
        active_chain_move_window_tip(&ms->chain_active, tip);
    return tip;
}

/* Insert a torn next-block at height `h`: header-valid, HAVE_DATA flagged,
 * but its on-disk location is missing (nFile=-1). It is NOT linked into the
 * active chain (the tip is at h-1) — this is the would-be next tip. */
static struct block_index *hdu_insert_torn(struct main_state *ms, int h,
                                           struct uint256 *hash,
                                           struct block_index *prev)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(h & 0xFF);
    hash->data[1] = (uint8_t)((h >> 8) & 0xFF);
    hash->data[3] = 0xCD;

    struct block_index *pi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!pi)
        return NULL;
    pi->nHeight = h;
    pi->nStatus = BLOCK_VALID_TREE | BLOCK_HAVE_DATA; /* flag set... */
    pi->nFile = -1;                                   /* ...but torn */
    pi->nDataPos = 0;
    pi->nChainTx = 0;
    pi->pprev = prev;
    arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
    return pi;
}

int test_have_data_unreadable(void)
{
    printf("\n=== have_data_unreadable condition tests ===\n");
    int failures = 0;
    struct uint256 hashes[64];
    struct uint256 torn_hash;

    /* ── 1. Readable tip+1, fresh tip: detect false, no remedy. ── */
    {
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();

        struct main_state ms;
        main_state_init(&ms);
        hdu_build_chain(&ms, 10, hashes); /* tip h=9, all blocks readable */
        condition_engine_set_main_state(&ms);
        register_have_data_unreadable();

        /* Tip just advanced (age ~0 < 60) — even a torn block must not fire. */
        sync_monitor_test_set_tip_advance_ts(platform_time_wall_unix());

        struct block_index *tip = block_map_find(&ms.map_block_index,
                                                 &hashes[9]);
        hdu_insert_torn(&ms, 10, &torn_hash, tip);

        condition_engine_tick();

        bool ok = condition_engine_get_active_count() == 0;
        ok = ok && have_data_unreadable_test_remedy_calls() == 0;
        HDU_CHECK("fresh tip (age<60) -> detect false, no remedy", ok);

        condition_engine_set_main_state(NULL);
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();
        main_state_free(&ms);
    }

    /* ── 1b. Unknown tip age is not fresh. A restored/snapshot boot may not
     *      have observed a block-connected callback yet; if tip+1 is
     *      provably unreadable, clear the bogus HAVE_DATA flag anyway. ── */
    {
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();

        struct main_state ms;
        main_state_init(&ms);
        hdu_build_chain(&ms, 10, hashes); /* tip h=9 */
        condition_engine_set_main_state(&ms);
        register_have_data_unreadable();

        struct block_index *tip = block_map_find(&ms.map_block_index,
                                                 &hashes[9]);
        struct block_index *torn = hdu_insert_torn(&ms, 10, &torn_hash, tip);

        sync_monitor_test_set_tip_advance_ts(0); /* sentinel: age unknown */
        condition_engine_tick();

        bool ok = have_data_unreadable_test_remedy_calls() == 1;
        ok = ok && (torn->nStatus & BLOCK_HAVE_DATA) == 0;
        HDU_CHECK("unknown tip age + torn block -> remedy clears HAVE_DATA",
                  ok);

        condition_engine_set_main_state(NULL);
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();
        main_state_free(&ms);
    }

    /* ── 2. Stalled tip + torn HAVE_DATA next block: detect fires, remedy
     *      CLEARS the provably-bogus flag, and the engine witnesses the heal
     *      (the bogus flag is gone → the block is eligible for re-download).
     *
     *      TEETH: the remedy MUST clear BLOCK_HAVE_DATA. gap_fill_service.c
     *      skips any block with BLOCK_HAVE_DATA set, so leaving the flag set
     *      (disabling the remedy) means the torn block is never re-requested
     *      AND target_index() still finds it → the witness reports the
     *      symptom persists → the condition is NOT cleared and the assertions
     *      below fail. ── */
    {
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();

        struct main_state ms;
        main_state_init(&ms);
        hdu_build_chain(&ms, 10, hashes); /* tip h=9 */
        condition_engine_set_main_state(&ms);
        register_have_data_unreadable();

        struct block_index *tip = block_map_find(&ms.map_block_index,
                                                 &hashes[9]);
        struct block_index *torn = hdu_insert_torn(&ms, 10, &torn_hash, tip);

        /* Tip has been stuck for 120s (> 60s gate). tip_advance_age uses the
         * wall clock (now - last); set last to 120s ago. */
        int64_t now = platform_time_wall_unix();
        sync_monitor_test_set_tip_advance_ts(now - 120);

        /* Pre-state: torn block is flagged HAVE_DATA with file=-1. */
        bool pre_flagged = (torn->nStatus & BLOCK_HAVE_DATA) != 0 &&
                           torn->nFile == -1;

        struct condition_runtime_snapshot pre;
        int cleared_before = 0;
        if (condition_engine_get_registered_snapshot("have_data_unreadable",
                                                     &pre))
            cleared_before = pre.cleared_count;

        /* One tick: detect true → active; remedy clears the bogus flag; the
         * post-remedy witness sees the flag is gone → condition cleared. */
        condition_engine_tick();

        bool ok = pre_flagged;
        ok = ok && have_data_unreadable_test_remedy_calls() == 1;
        /* TEETH: the provably-bogus HAVE_DATA flag is cleared so the
         * downloader (gap_fill skips HAVE_DATA blocks) will re-request it. */
        ok = ok && (torn->nStatus & BLOCK_HAVE_DATA) == 0;
        ok = ok && torn->nFile == -1 && torn->nDataPos == 0;
        HDU_CHECK("stalled tip + torn block -> remedy clears HAVE_DATA", ok);

        struct condition_runtime_snapshot post;
        bool gotpost = condition_engine_get_registered_snapshot(
            "have_data_unreadable", &post);
        bool ok2 = gotpost;
        ok2 = ok2 && condition_engine_get_active_count() == 0;
        ok2 = ok2 && post.cleared_count == cleared_before + 1;
        HDU_CHECK("bogus flag cleared -> condition witnessed/cleared", ok2);

        condition_engine_set_main_state(NULL);
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();
        main_state_free(&ms);
    }

    /* ── 3. Anti-thrash: the remedy is self-limiting. Once the bogus flag is
     *      cleared, the condition resolves; further ticks (with the tip still
     *      stalled, but the block no longer falsely marked HAVE_DATA) do NOT
     *      re-fire the remedy. The remedy only ever acts on a block that is
     *      STILL falsely flagged — it cannot repeatedly hammer an already
     *      healed block. Combined with the engine's bounded backoff/
     *      max_attempts (asserted via the snapshot), this is the no-thrash
     *      guarantee. ── */
    {
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();

        struct main_state ms;
        main_state_init(&ms);
        hdu_build_chain(&ms, 10, hashes); /* tip h=9 */
        condition_engine_set_main_state(&ms);
        register_have_data_unreadable();

        struct block_index *tip = block_map_find(&ms.map_block_index,
                                                 &hashes[9]);
        hdu_insert_torn(&ms, 10, &torn_hash, tip);

        int64_t now = platform_time_wall_unix();
        sync_monitor_test_set_tip_advance_ts(now - 120); /* tip stalled */

        /* First tick heals the bogus flag (one remedy). */
        condition_engine_tick();
        int calls_after_heal = have_data_unreadable_test_remedy_calls();

        /* Drive more ticks WITHOUT re-arming the flag: the tip is still
         * stalled, but the block is no longer falsely HAVE_DATA, so detect is
         * false and the remedy must NOT fire again — no thrash. */
        for (int i = 0; i < 6; i++)
            condition_engine_tick();
        int calls_total = have_data_unreadable_test_remedy_calls();

        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(
            "have_data_unreadable", &snap);
        bool ok = got;
        ok = ok && calls_after_heal == 1;          /* healed in one remedy */
        ok = ok && calls_total == 1;               /* never re-fired: no thrash */
        ok = ok && snap.backoff_secs > 0;          /* backoff configured */
        ok = ok && snap.max_attempts > 0;          /* attempt cap configured */
        ok = ok && snap.attempts <= snap.max_attempts;
        HDU_CHECK("self-limiting -> remedy does not re-fire on healed block",
                  ok);

        condition_engine_set_main_state(NULL);
        condition_engine_reset_for_testing();
        have_data_unreadable_test_reset();
        sync_monitor_test_set_tip_advance_ts(0);
        main_state_free(&ms);
    }

    return failures;
}
