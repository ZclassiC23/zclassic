/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Liveness-tree membership regression: the never-stuck escalation
 * children must actually REGISTER into the supervisor tree as tickable
 * children. This was confirmed today only via live `zcl_state
 * subsystem=supervisor` — never a unit test. Dropping the
 * `supervisor_register_in_domain(g_chain_sup, ...)` call in
 * chain_tip_watchdog.c (app/services/src/chain_tip_watchdog.c:302)
 * silently disables the tip-stuck escalation rung with CI fully green:
 * the existing test_chain_tip_watchdog_bounded_restart only drives the
 * escalation seam (chain_tip_watchdog_test_escalate_restart) and never
 * calls chain_tip_watchdog_register(), so it cannot notice that the
 * watchdog is no longer on the supervisor thread.
 *
 * This test calls the REAL production entry point
 * chain_tip_watchdog_register(&ms) (which itself runs
 * supervisor_domains_init()), snapshots the whole supervisor tree, and
 * asserts that a child named "chain.chain_tip_watchdog" exists exactly
 * once and is supervisor-driven (period_secs > 0, the contract that the
 * supervisor thread will tick on_tick). The exact period (30 s) and the
 * disabled deadline gate (0) are pinned so a regression to the wiring or
 * the contract config fails loudly.
 *
 * Scope: this test covers the chain_tip_watchdog child only. The two
 * sibling never-stuck rungs ("net.outbound_floor" in net_supervisor.c
 * and "chain.coord_escalation" in chain_supervisor.c) register from
 * register entry points that need heavier subsystem setup (net /
 * coordinator state); they should get their own focused membership
 * assertions rather than be force-fit here. The watchdog is the rung
 * that can be exercised with just a main_state.
 *
 * Hermetic: test_parallel forks one process per X() test, so the
 * module-global g_id starts at SUPERVISOR_INVALID_ID and the idempotent
 * guard in chain_tip_watchdog_register lets the register fire. We still
 * supervisor_reset_for_testing() on both ends so the sequential runner
 * (test.c) stays side-effect free. No supervisor thread is started, so
 * nothing actually ticks on a real clock — we assert REGISTRATION, the
 * precondition for ticking. */

#include "test/test_helpers.h"

#include "services/chain_tip_watchdog.h"
#include "supervisors/staged_sync_supervisor.h"
#include "storage/progress_store.h"
#include "util/supervisor.h"
#include "validation/main_state.h"

#include <string.h>

#define SPT_CHECK(name, expr) do { \
    printf("supervisor_production_tree: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Find the named child in a full snapshot of the supervisor tree.
 * Returns the index (>=0) of the LAST match and writes the match count
 * to *out_count (so a duplicate registration is detectable). */
static int find_child_snapshot(const char *child_name,
                               struct supervisor_snapshot *out,
                               int *out_count)
{
    struct supervisor_snapshot snap[SUPERVISOR_CAP];
    int n = supervisor_snapshot_all(snap, SUPERVISOR_CAP);
    int matches = 0;
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].name, child_name) == 0) {
            if (out) *out = snap[i];
            matches++;
            found = i;
        }
    }
    if (out_count) *out_count = matches;
    return found;
}

static const char *const k_staged_children[] = {
    "staged.header_admit",
    "staged.validate_headers",
    "staged.body_fetch",
    "staged.body_persist",
    "staged.script_validate",
    "staged.proof_validate",
    "staged.utxo_apply",
    "staged.tip_finalize",
};

int test_supervisor_production_tree(void)
{
    printf("\n=== supervisor_production_tree tests ===\n");
    int failures = 0;

    supervisor_reset_for_testing();

    /* Sanity: the tree is empty before any production register runs, so
     * a found child below can only come from our register call. */
    SPT_CHECK("tree empty before register",
              supervisor_child_count_total() == 0);

    struct main_state ms;
    main_state_init(&ms);

    /* The REAL production entry point. Internally:
     *   - supervisor_domains_init() creates g_chain_sup
     *   - liveness_contract_init(&g_contract, "chain.chain_tip_watchdog")
     *   - period_secs=30, deadline_secs=0
     *   - supervisor_register_in_domain(g_chain_sup, &g_contract)
     * (app/services/src/chain_tip_watchdog.c:286-306). */
    chain_tip_watchdog_register(&ms);

    /* The stats accessor agrees the watchdog believes it registered. */
    struct chain_tip_watchdog_stats stats;
    chain_tip_watchdog_get_stats(&stats);
    SPT_CHECK("watchdog reports registered", stats.registered);

    struct supervisor_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    int count = 0;
    int idx = find_child_snapshot("chain.chain_tip_watchdog", &snap, &count);

    /* The load-bearing invariant: the never-stuck rung is ON the
     * supervisor tree, exactly once. Drop the register call → idx < 0. */
    SPT_CHECK("chain.chain_tip_watchdog registered exactly once",
              idx >= 0 && count == 1);

    /* Supervisor-driven: period_secs > 0 means the supervisor thread will
     * call on_tick on its clock. period_secs == 0 would silently make the
     * contract present-but-never-ticked (the exact failure mode this test
     * exists to catch). Pin the production value. */
    SPT_CHECK("watchdog is supervisor-driven (period_secs > 0)",
              snap.period_secs > 0);
    SPT_CHECK("watchdog period is 30 seconds",
              snap.period_secs == 30);

    /* The watchdog deliberately disables the supervisor's deadline stall
     * gate (its own escalation ladder is authoritative — see the on_tick
     * comment at chain_tip_watchdog.c:278-281). */
    SPT_CHECK("watchdog deadline stall gate disabled",
              snap.deadline_secs == 0);

    /* Idempotency: a second register must NOT create a duplicate rung. */
    chain_tip_watchdog_register(&ms);
    idx = find_child_snapshot("chain.chain_tip_watchdog", &snap, &count);
    SPT_CHECK("re-register does not duplicate the child",
              idx >= 0 && count == 1);

    main_state_free(&ms);
    chain_tip_watchdog_test_reset_runtime();
    supervisor_reset_for_testing();

    /* Failed init must be visible, not absent. With progress_store closed,
     * every staged child init fails; the production register path should
     * still leave one disabled child per core stage in the supervisor tree
     * with a named child_reported stall. */
    staged_sync_supervisor_test_reset_runtime();
    progress_store_close();
    supervisor_reset_for_testing();

    struct main_state staged_ms;
    main_state_init(&staged_ms);
    staged_sync_supervisor_register(&staged_ms);

    SPT_CHECK("staged failed-init children are visible",
              supervisor_child_count_total() ==
                  (int)(sizeof(k_staged_children) /
                        sizeof(k_staged_children[0])));

    for (size_t i = 0;
         i < sizeof(k_staged_children) / sizeof(k_staged_children[0]);
         i++) {
        memset(&snap, 0, sizeof(snap));
        count = 0;
        idx = find_child_snapshot(k_staged_children[i], &snap, &count);

        char label[96];
        snprintf(label, sizeof(label), "%s failed-init marker exists",
                 k_staged_children[i]);
        SPT_CHECK(label, idx >= 0 && count == 1);

        snprintf(label, sizeof(label), "%s failed-init marker disabled",
                 k_staged_children[i]);
        SPT_CHECK(label, snap.period_secs == 0 &&
                         snap.stall_reason ==
                             SUPERVISOR_STALL_CHILD_REPORTED &&
                         snap.stall_fires == 1);
    }

    main_state_free(&staged_ms);
    supervisor_reset_for_testing();
    staged_sync_supervisor_test_reset_runtime();

    printf("supervisor_production_tree: %d failures\n", failures);
    return failures;
}
