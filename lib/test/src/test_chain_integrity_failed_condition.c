/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "core/arith_uint256.h"
#include "framework/condition.h"
#include "json/json.h"
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
void chain_integrity_failed_test_set_async(bool enabled);
void chain_integrity_failed_test_disable_spawn(bool disabled);
int chain_integrity_failed_test_queue_calls(void);

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

static const struct json_value *find_condition_json(
    const struct json_value *conditions,
    const char *name)
{
    if (!conditions || !name)
        return NULL;
    for (size_t i = 0; i < json_size(conditions); i++) {
        const struct json_value *cond = json_at(conditions, i);
        const struct json_value *n = cond ? json_get(cond, "name") : NULL;
        if (n && strcmp(json_get_str(n), name) == 0)
            return cond;
    }
    return NULL;
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
        ok = ok && before.active_chain_mismatches == 1;
        ok = ok && chain_integrity_classify(&before) ==
                         CHAIN_INTEGRITY_UNRECOVERABLE;

        condition_engine_tick();

        struct chain_integrity_result after;
        chain_integrity_check_post_restore(&after, &ms);
        ok = ok && chain_integrity_failed_test_remedy_calls() == 1;
        ok = ok && chain_integrity_failed_test_queue_calls() == 0;
        ok = ok && after.ok == true;
        ok = ok && after.active_chain_holes == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        CIF_CHECK("ancestor holes with active-chain mismatch trigger remedy", ok);
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

        chain_integrity_failed_test_set_async(true);
        chain_integrity_failed_test_disable_spawn(true);
        condition_engine_tick();

        struct chain_integrity_result after;
        chain_integrity_check_post_restore(&after, &ms);
        ok = ok && chain_integrity_failed_test_remedy_calls() == 1;
        ok = ok && chain_integrity_failed_test_queue_calls() == 1;
        ok = ok && after.ok == false;
        ok = ok && after.zero_nbits_count == 1;
        ok = ok && chain_integrity_classify(&after) ==
                         CHAIN_INTEGRITY_UNRECOVERABLE;
        ok = ok && condition_engine_get_active_count() == 1;
        CIF_CHECK("async remedy queues without inline finalize", ok);
        cleanup_cif(&ms);
    }

    {
        struct main_state ms;
        struct uint256 hashes[11];
        reset_cif(&ms);
        bool ok = true;
        register_chain_integrity_failed();

        ok = ok && seed_linked_chain(&ms, hashes, 10);
        ms.chain_active.chain[10] = NULL;

        chain_integrity_failed_test_set_async(true);
        chain_integrity_failed_test_disable_spawn(true);
        condition_engine_tick();
        ok = ok && chain_integrity_failed_test_remedy_calls() == 0;
        ok = ok && chain_integrity_failed_test_queue_calls() == 0;
        ok = ok && condition_engine_get_active_count() == 0;

        struct json_value out;
        json_init(&out);
        json_set_object(&out);
        ok = ok && condition_engine_dump_state_json(&out, NULL);
        const struct json_value *conditions = json_get(&out, "conditions");
        const struct json_value *cond =
            find_condition_json(conditions, "chain_integrity_failed");
        const struct json_value *detail = cond ? json_get(cond, "detail") : NULL;
        ok = ok && detail != NULL;
        ok = ok && strcmp(json_get_str(json_get(detail, "integrity_class")),
                          "reconcilable") == 0;
        ok = ok && json_get_int(json_get(detail, "tip_window_holes")) == 1;
        ok = ok && json_get_int(json_get(detail, "active_chain_holes")) == 1;
        ok = ok &&
             json_get_int(json_get(detail, "active_chain_mismatches")) == 0;
        ok = ok && !json_get_bool(json_get(detail,
                                           "restore_last_used_datadir"));
        json_free(&out);
        CIF_CHECK("detail exposes reconcilable integrity state", ok);
        cleanup_cif(&ms);
    }

    {
        struct main_state ms;
        struct uint256 hashes[5];
        reset_cif(&ms);
        bool ok = true;
        register_chain_integrity_failed();

        ok = ok && seed_linked_chain(&ms, hashes, 4);
        struct block_index *tip = active_chain_tip(&ms.chain_active);
        ok = ok && tip != NULL;
        if (tip)
            tip->nBits = 0;

        condition_engine_tick();

        struct json_value out;
        json_init(&out);
        json_set_object(&out);
        ok = ok && condition_engine_dump_state_json(&out, NULL);
        const struct json_value *conditions = json_get(&out, "conditions");
        const struct json_value *cond =
            find_condition_json(conditions, "chain_integrity_failed");
        const struct json_value *detail = cond ? json_get(cond, "detail") : NULL;
        ok = ok && detail != NULL;
        ok = ok && strcmp(json_get_str(json_get(detail, "integrity_class")),
                          "unrecoverable") == 0;
        ok = ok && json_get_int(json_get(detail, "zero_nbits_count")) == 1;
        ok = ok && json_get_int(json_get(detail,
                                         "first_nbits_zero_height")) == 4;
        json_free(&out);
        CIF_CHECK("detail exposes unrecoverable integrity state", ok);
        cleanup_cif(&ms);
    }

    return failures;
}
