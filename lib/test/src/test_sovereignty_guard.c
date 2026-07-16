/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit test for the sovereignty guard (controllers/sovereignty_controller.h):
 * the operational-vs-sovereign trust split enforced at the mint/mining entry
 * (mining_controller.c) and the wallet-spend entry (wallet_shielded_send.c
 * rpc_z_sendmany), per docs/work/fast-sync-to-tip-plan-2026-07-16.md §5.
 *
 * Three scenarios, same shape as test_coins_kv_sovereign_gate.c:
 *   - bare (no migration stamp): G-SOV part 3's first disjunct
 *     (!proven_authority) holds regardless of self_folded — mint/spend
 *     allowed, trust_mode == "bare".
 *   - borrowed (migration stamped, no self_folded marker): the
 *     release_assisted trap — mint refused, wallet spend refused,
 *     tip-following (the dumper's tip_following_allowed) stays true,
 *     trust_mode == "release_assisted".
 *   - sovereign (self_folded marker present): mint allowed, wallet spend
 *     allowed, snapshot/bundle export allowed, trust_mode == "sovereign". */

#include "test/test_helpers.h"

#include "controllers/sovereignty_controller.h"
#include "core/uint256.h"
#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define SG_CHECK(name, expr) do {                                           \
    if (expr) { printf("  sovereignty_guard: %s... OK\n", (name)); }         \
    else { printf("  sovereignty_guard: %s... FAIL\n", (name)); failures++; }\
} while (0)

/* Mirrors what the borrowed-seed path stamps (coins_kv_seed_from_node_db) —
 * see test_coins_kv_sovereign_gate.c's identical helper. */
static bool sg_stamp_migration(sqlite3 *db)
{
    uint8_t one = 1;
    return progress_meta_set(db, COINS_KV_MIGRATION_COMPLETE_KEY, &one, 1);
}

static bool sg_set_applied(sqlite3 *db, int32_t h)
{
    char *err = NULL;
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) == SQLITE_OK
              && coins_kv_set_applied_height_in_tx(db, h)
              && sqlite3_exec(db, "COMMIT", NULL, NULL, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    return ok;
}

int test_sovereignty_guard(void);
int test_sovereignty_guard(void)
{
    printf("\n=== sovereignty guard (mint/spend release_assisted gate) tests ===\n");
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "sovereignty_guard", "main");

    SG_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    SG_CHECK("db handle", db != NULL);
    SG_CHECK("coins_kv schema", db && coins_kv_ensure_schema(db));

    /* Pin H*=50 for the whole test — a real boot only ever advances this
     * cached atomic under progress_store_tx_lock at the tip_finalize sites;
     * test_parallel forks one child process per group, so no cross-test
     * leakage, but reset at the end anyway (hygiene, matches
     * test_refold_retro_validate.c's convention). */
    reducer_frontier_provable_tip_set(50);

    struct uint256 t1;
    uint256_set_null(&t1);
    t1.data[0] = 0x51;
    t1.data[31] = 0x77;
    unsigned char sc[4] = {0xE0, 0xE0, 0xE0, 0xE0};
    SG_CHECK("add t1.0", coins_kv_add(db, t1.data, 0, 1234, 50, true, sc,
                                      sizeof(sc)));
    SG_CHECK("seed applied_height=51 (== hstar+1)", sg_set_applied(db, 51));

    /* ── bare: applied==hstar+1, no migration stamp yet. */
    {
        char reason[96] = {0};
        SG_CHECK("bare: mint allowed",
                 sovereignty_guard_allow("mint", reason, sizeof(reason)));
        SG_CHECK("bare: wallet_spend allowed",
                 sovereignty_guard_allow("wallet_spend", reason,
                                         sizeof(reason)));
    }
    {
        struct json_value out = {0};
        SG_CHECK("bare: dumper ok", sovereignty_dump_state_json(&out, NULL));
        const struct json_value *tm = json_get(&out, "trust_mode");
        SG_CHECK("bare: trust_mode == bare",
                 tm && strcmp(json_get_str(tm), "bare") == 0);
        const struct json_value *gated = json_get(&out, "gated");
        SG_CHECK("bare: tip_following_allowed",
                 gated &&
                 json_get_bool(json_get(gated, "tip_following_allowed")));
        json_free(&out);
    }

    /* ── borrowed: stamp migration (proven_authority=true), no self_folded
     * marker — the release_assisted trap this guard exists to close. */
    SG_CHECK("stamp migration", sg_stamp_migration(db));
    {
        char reason[96] = {0};
        SG_CHECK("borrowed: mint refused",
                 !sovereignty_guard_allow("mint", reason, sizeof(reason)));
        SG_CHECK("borrowed: mint reason names release_assisted",
                 strstr(reason, "release_assisted") != NULL);
        SG_CHECK("borrowed: wallet_spend refused",
                 !sovereignty_guard_allow("wallet_spend", reason,
                                          sizeof(reason)));
        SG_CHECK("borrowed: wallet_spend reason names release_assisted",
                 strstr(reason, "release_assisted") != NULL);
    }
    {
        struct json_value out = {0};
        SG_CHECK("borrowed: dumper ok", sovereignty_dump_state_json(&out, NULL));
        const struct json_value *tm = json_get(&out, "trust_mode");
        SG_CHECK("borrowed: trust_mode == release_assisted",
                 tm && strcmp(json_get_str(tm), "release_assisted") == 0);
        const struct json_value *posture =
            json_get(&out, "authority_posture");
        SG_CHECK("borrowed: authority_posture == proven_but_not_self_folded",
                 posture && strcmp(json_get_str(posture),
                                   "proven_but_not_self_folded") == 0);
        const struct json_value *gated = json_get(&out, "gated");
        SG_CHECK("borrowed: mint_mining_allowed == false",
                 gated &&
                 !json_get_bool(json_get(gated, "mint_mining_allowed")));
        SG_CHECK("borrowed: wallet_spend_allowed == false",
                 gated &&
                 !json_get_bool(json_get(gated, "wallet_spend_allowed")));
        SG_CHECK("borrowed: snapshot_bundle_export_allowed == false",
                 gated && !json_get_bool(json_get(gated,
                                         "snapshot_bundle_export_allowed")));
        SG_CHECK("borrowed: tip_following_allowed stays true (not gated)",
                 gated &&
                 json_get_bool(json_get(gated, "tip_following_allowed")));
        json_free(&out);
    }

    /* ── sovereign: stamp self_folded — everything unlocks. */
    SG_CHECK("mark_self_folded", coins_kv_mark_self_folded(db));
    {
        char reason[96] = {0};
        SG_CHECK("sovereign: mint allowed",
                 sovereignty_guard_allow("mint", reason, sizeof(reason)));
        SG_CHECK("sovereign: wallet_spend allowed",
                 sovereignty_guard_allow("wallet_spend", reason,
                                         sizeof(reason)));
    }
    {
        struct json_value out = {0};
        SG_CHECK("sovereign: dumper ok",
                 sovereignty_dump_state_json(&out, NULL));
        const struct json_value *tm = json_get(&out, "trust_mode");
        SG_CHECK("sovereign: trust_mode == sovereign",
                 tm && strcmp(json_get_str(tm), "sovereign") == 0);
        const struct json_value *gated = json_get(&out, "gated");
        SG_CHECK("sovereign: mint_mining_allowed == true",
                 gated &&
                 json_get_bool(json_get(gated, "mint_mining_allowed")));
        SG_CHECK("sovereign: wallet_spend_allowed == true",
                 gated &&
                 json_get_bool(json_get(gated, "wallet_spend_allowed")));
        SG_CHECK("sovereign: snapshot_bundle_export_allowed == true",
                 gated && json_get_bool(json_get(gated,
                                        "snapshot_bundle_export_allowed")));
        json_free(&out);
    }

    /* ── NULL-input hardening. */
    SG_CHECK("dump_state_json(NULL) -> false",
             !sovereignty_dump_state_json(NULL, NULL));

    reducer_frontier_provable_tip_reset();
    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}
