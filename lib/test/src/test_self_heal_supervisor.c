/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_self_heal_supervisor -- self_heal_register() must fail LOUD, not
 * with a bare LOG_WARN, when supervisor_register_in_domain() fails. That
 * registration is the only cadence the condition engine, blocker escape
 * sweep, and remedy ladder ride on; losing it silently loses the whole
 * spine. Drives the real failure path by filling the supervisor registry
 * to capacity first (mirrors test_supervisor.c's "registry capacity"
 * case), then asserts a PERMANENT "self_heal.unsupervised" blocker +
 * EV_OPERATOR_NEEDED fired. A sibling case proves the healthy path stays
 * silent (no blocker, no page). */

#include "test/test_helpers.h"

#include "event/event.h"
#include "supervisors/self_heal.h"
#include "util/blocker.h"
#include "util/supervisor.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SHS_CHECK(name, expr) do {                               \
    printf("self_heal_supervisor: %s... ", (name));              \
    if ((expr)) printf("OK\n");                                  \
    else { printf("FAIL\n"); failures++; }                       \
} while (0)

static _Atomic int g_shs_operator_events;

static void shs_operator_observer(enum event_type type, uint32_t peer_id,
                                  const void *payload, uint32_t payload_len,
                                  void *ctx)
{
    (void)type; (void)peer_id; (void)payload; (void)payload_len; (void)ctx;
    atomic_fetch_add(&g_shs_operator_events, 1);
}

static void shs_fill_registry(struct liveness_contract pool[SUPERVISOR_CAP])
{
    for (int i = 0; i < SUPERVISOR_CAP; i++) {
        char nm[SUPERVISOR_NAME_MAX];
        snprintf(nm, sizeof(nm), "shs.filler.%d", i);
        liveness_contract_init(&pool[i], nm);
        (void)supervisor_register(&pool[i]);
    }
}

int test_self_heal_supervisor(void)
{
    printf("\n=== self_heal_supervisor tests ===\n");
    int failures = 0;

    /* ── registration failure fails LOUD ─────────────────────────── */
    supervisor_reset_for_testing();
    blocker_reset_for_testing();
    self_heal_test_reset();
    atomic_store(&g_shs_operator_events, 0);
    event_observe(EV_OPERATOR_NEEDED, shs_operator_observer, NULL);

    {
        static struct liveness_contract pool[SUPERVISOR_CAP];
        shs_fill_registry(pool);

        struct main_state ms;
        main_state_init(&ms);
        self_heal_register(&ms);

        SHS_CHECK("registry full -> self_heal.unsupervised blocker set",
                  blocker_exists("self_heal.unsupervised"));
        SHS_CHECK("blocker class is PERMANENT (operator clears, no auto-retry)",
                  blocker_class_for("self_heal.unsupervised") ==
                  (int)BLOCKER_PERMANENT);
        SHS_CHECK("EV_OPERATOR_NEEDED paged the operator",
                  atomic_load(&g_shs_operator_events) > 0);

        main_state_free(&ms);
    }

    event_clear_observers(EV_OPERATOR_NEEDED);
    supervisor_reset_for_testing();
    blocker_reset_for_testing();
    self_heal_test_reset();

    /* ── healthy registration stays silent ───────────────────────── */
    atomic_store(&g_shs_operator_events, 0);
    event_observe(EV_OPERATOR_NEEDED, shs_operator_observer, NULL);

    {
        struct main_state ms;
        main_state_init(&ms);
        self_heal_register(&ms);

        SHS_CHECK("healthy registration sets NO self_heal.unsupervised blocker",
                  !blocker_exists("self_heal.unsupervised"));
        SHS_CHECK("healthy registration pages nobody",
                  atomic_load(&g_shs_operator_events) == 0);

        main_state_free(&ms);
    }

    event_clear_observers(EV_OPERATOR_NEEDED);
    supervisor_reset_for_testing();
    blocker_reset_for_testing();
    self_heal_test_reset();

    printf("=== self_heal_supervisor: %d failure(s) ===\n", failures);
    return failures;
}
