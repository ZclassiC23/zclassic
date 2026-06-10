/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "core/arith_uint256.h"
#include "framework/condition.h"
#include "services/chain_restore_integrity.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <string.h>

#define CIF_CHECK(name, expr) do { \
    printf("chain_integrity_failed_condition: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

void register_chain_integrity_failed(void);
void chain_integrity_failed_test_reset(void);
int chain_integrity_failed_test_remedy_calls(void);

static void reset_cif(struct main_state *ms)
{
    condition_engine_reset_for_testing();
    chain_integrity_failed_test_reset();
    main_state_init(ms);
    condition_engine_set_main_state(ms);
}

static void cleanup_cif(struct main_state *ms)
{
    condition_engine_set_main_state(NULL);
    condition_engine_reset_for_testing();
    chain_integrity_failed_test_reset();
    main_state_free(ms);
}

static struct block_index *insert_test_block(struct main_state *ms,
                                             struct uint256 *hashes,
                                             int h)
{
    memset(&hashes[h], 0, sizeof(hashes[h]));
    hashes[h].data[0] = (uint8_t)(h & 0xFF);
    hashes[h].data[1] = (uint8_t)((h >> 8) & 0xFF);
    hashes[h].data[3] = 0xC1;
    struct block_index *pi = chainstate_insert_block_index(
        (struct chainstate *)ms, &hashes[h]);
    if (!pi) return NULL;
    pi->nHeight = h;
    pi->nBits = 0x1f07ffff;
    pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
    pi->nTx = 1;
    pi->nChainTx = (uint32_t)(h + 1);
    arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
    if (h > 0)
        pi->pprev = block_map_find(&ms->map_block_index, &hashes[h - 1]);
    return pi;
}

static bool seed_linked_chain(struct main_state *ms,
                              struct uint256 *hashes,
                              int tip_h)
{
    for (int h = 0; h <= tip_h; h++) {
        if (!insert_test_block(ms, hashes, h))
            return false;
    }
    struct block_index *tip = block_map_find(&ms->map_block_index,
                                             &hashes[tip_h]);
    return tip && active_chain_move_window_tip(&ms->chain_active, tip);
}

int test_chain_integrity_failed_condition(void)
{
    printf("\n=== chain_integrity_failed condition tests ===\n");
    int failures = 0;

    {
        struct main_state ms;
        struct uint256 hashes[11];
        reset_cif(&ms);
        bool ok = true;
        register_chain_integrity_failed();

        ok = ok && seed_linked_chain(&ms, hashes, 10);
        for (int h = 0; h < 10; h++)
            ms.chain_active.chain[h] = NULL;

        struct chain_integrity_result before;
        chain_integrity_check_post_restore(&before, &ms);
        ok = ok && before.ok == false;
        ok = ok && before.active_chain_holes == 10;

        condition_engine_tick();

        struct chain_integrity_result after;
        chain_integrity_check_post_restore(&after, &ms);
        ok = ok && chain_integrity_failed_test_remedy_calls() == 1;
        ok = ok && after.ok == true;
        ok = ok && after.active_chain_holes == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        CIF_CHECK("near-tip active-chain holes trigger remedy", ok);
        cleanup_cif(&ms);
    }

    {
        struct main_state ms;
        struct uint256 hashes[11];
        reset_cif(&ms);
        bool ok = true;
        register_chain_integrity_failed();

        ok = ok && seed_linked_chain(&ms, hashes, 10);
        struct block_index *tip = active_chain_tip(&ms.chain_active);
        ok = ok && tip != NULL;
        if (tip)
            tip->nBits = 0;

        struct chain_integrity_result before;
        chain_integrity_check_post_restore(&before, &ms);
        ok = ok && before.ok == false;
        ok = ok && before.zero_nbits_count == 1;

        condition_engine_tick();

        struct chain_integrity_result after;
        chain_integrity_check_post_restore(&after, &ms);
        ok = ok && chain_integrity_failed_test_remedy_calls() == 1;
        ok = ok && after.ok == false;
        ok = ok && condition_engine_get_active_count() == 1;
        CIF_CHECK("fatal nBits zero triggers restore finalize remedy", ok);
        cleanup_cif(&ms);
    }

    {
        struct main_state ms;
        struct uint256 hashes[5];
        reset_cif(&ms);
        bool ok = true;
        register_chain_integrity_failed();

        ok = ok && seed_linked_chain(&ms, hashes, 4);
        condition_engine_tick();

        ok = ok && chain_integrity_failed_test_remedy_calls() == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        CIF_CHECK("clean chain is ignored", ok);
        cleanup_cif(&ms);
    }

    return failures;
}
