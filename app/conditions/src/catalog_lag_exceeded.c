/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * catalog_lag_exceeded — self-heal condition that fires when an enabled
 * chain-data index (catalog_completeness) falls too far behind the reducer's
 * provable served height H*, and raises a typed named blocker naming the
 * lagging index's backfill as the dependency to advance. */

#include "conditions/catalog_lag_exceeded.h"

#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "storage/catalog_completeness.h"
#include "util/blocker.h"
#include "util/log_macros.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Lag past H* (blocks) at which an ENABLED index counts as "exceeded". Chosen
 * generously so a normal catch-up burst (which the backfill services clear on
 * their own) never trips the condition — only a genuinely stuck index does. */
#define CATALOG_LAG_EXCEEDED_BLOCKS 1000

/* The sustain state: an over-threshold index must survive TWO consecutive
 * detect passes before the symptom is declared, so a single-pass blip during a
 * legitimate reorg/catch-up never fires. g_lagging_name aliases the static row
 * name literal in catalog_completeness.c (stable lifetime), so storing the
 * pointer across passes is safe. */
static _Atomic bool          g_over_last_pass;
static _Atomic(const char *) g_lagging_name;      /* NULL until a firing pass */
static _Atomic int64_t       g_cursor_at_detect;  /* offending cursor at detect */

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
#endif

/* Build the typed blocker id for an index. */
/* blocker-id: catalog.*.lag_exceeded */
static void lag_blocker_id(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "catalog.%s.lag_exceeded", name ? name : "unknown");
}

/* Core sustain evaluator over a completeness snapshot. Returns true only on the
 * SECOND consecutive pass in which some enabled index is over the threshold,
 * latching that index (name + cursor) for the remedy/witness. */
static bool eval_over(const struct catalog_index_status *rows, size_t n)
{
    const struct catalog_index_status *w =
        catalog_completeness_worst_over(rows, n, CATALOG_LAG_EXCEEDED_BLOCKS);
    if (!w) {
        atomic_store(&g_over_last_pass, false);
        return false;
    }
    bool prev = atomic_exchange(&g_over_last_pass, true);
    if (!prev)
        return false;                 /* first over-pass: arm, wait to confirm */
    atomic_store(&g_lagging_name, w->name);
    atomic_store(&g_cursor_at_detect, w->cursor);
    return true;
}

/* Snapshot the live catalog against the reducer's provable tip. */
static size_t snapshot_live(struct catalog_index_status *rows, size_t cap)
{
    int64_t hstar = (int64_t)reducer_frontier_provable_tip_cached();
    return catalog_completeness_snapshot(rows, cap, hstar);
}

static bool detect_catalog_lag_exceeded(void)
{
    struct catalog_index_status rows[CATALOG_COMPLETENESS_MAX_INDEXES];
    size_t n = snapshot_live(rows, CATALOG_COMPLETENESS_MAX_INDEXES);
    return eval_over(rows, n);
}

static enum condition_remedy_result remedy_catalog_lag_exceeded(void)
{
    const char *name = atomic_load(&g_lagging_name);
    if (!name)
        return COND_REMEDY_SKIP;

    int64_t cursor = atomic_load(&g_cursor_at_detect);
    char id[BLOCKER_ID_MAX];
    lag_blocker_id(name, id, sizeof(id));

    /* Non-destructive: raise/refresh a typed DEPENDENCY blocker (waiting on that
     * index's own backfill service). No store is touched, no cursor rewound. The
     * blocker names the exact dependency an operator/agent can act on. There is
     * no per-index "kick tick" API to nudge here (the backfill services drive
     * themselves on their own supervised cadence), so the remedy is: name the
     * dependency loudly + let the service advance. */
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "index %s is > %d blocks behind H* (cursor=%lld); its backfill "
             "service must advance",
             name, CATALOG_LAG_EXCEEDED_BLOCKS, (long long)cursor);
    struct blocker_record r;
    if (blocker_init(&r, id, "catalog_completeness", BLOCKER_DEPENDENCY,
                     reason)) {
        r.escape_deadline_secs = 0;   /* no auto-escape; witness clears it */
        (void)blocker_set(&r);
    }

    LOG_INFO("condition",
             "[condition:catalog_lag_exceeded] index=%s cursor=%lld raised "
             "blocker %s",
             name, (long long)cursor, id);

#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    return COND_REMEDY_OK;
}

static bool witness_catalog_lag_exceeded(int64_t target_at_detect)
{
    (void)target_at_detect;
    const char *name = atomic_load(&g_lagging_name);
    if (!name)
        return true;                  /* nothing latched -> nothing to witness */

    /* Honest witness: read the offending index's cursor LIVE against the
     * reducer's provable served height (reducer_frontier_provable_tip_cached —
     * the same H* detect measured against) and require it to have ADVANCED past
     * the cursor recorded at detect. Counting anything else (blocker state, a
     * frozen FSM flag) would let the symptom self-certify cleared while the
     * index stays wedged. */
    int64_t hstar = (int64_t)reducer_frontier_provable_tip_cached();
    struct catalog_index_status rows[CATALOG_COMPLETENESS_MAX_INDEXES];
    size_t n = catalog_completeness_snapshot(
        rows, CATALOG_COMPLETENESS_MAX_INDEXES, hstar);
    int64_t cursor_now = -1;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].name && strcmp(rows[i].name, name) == 0) {
            if (rows[i].enabled)
                cursor_now = rows[i].cursor;
            break;
        }
    }
    bool advanced = cursor_now > atomic_load(&g_cursor_at_detect);
    if (advanced) {
        char id[BLOCKER_ID_MAX];
        lag_blocker_id(name, id, sizeof(id));
        blocker_clear(id);            /* advanced -> the dependency cleared */
    }
    return advanced;                  /* real cursor movement, nothing else */
}

static struct condition c_catalog_lag_exceeded = {
    .name = "catalog_lag_exceeded",
    .severity = COND_WARN,
    .poll_secs = 5,
    .backoff_secs = 60,
    .max_attempts = 5,
    /* Rearm-forever (peer_floor's posture): a chain-data index catching up is
     * an external-progress dependency — after the page ladder, keep nudging
     * every 10 min, unbounded, until the backfill advances. The episode resets
     * when detect() goes false (the index caught back within threshold). */
    .cooldown_secs = 600,
    .cooldown_max_rearms = 0,
    .detect = detect_catalog_lag_exceeded,
    .remedy = remedy_catalog_lag_exceeded,
    .witness = witness_catalog_lag_exceeded,
    .witness_window_secs = 60,
};

void register_catalog_lag_exceeded(void)
{
    (void)condition_register(&c_catalog_lag_exceeded);
}

#ifdef ZCL_TESTING
void catalog_lag_exceeded_test_reset(void)
{
    atomic_store(&g_over_last_pass, false);
    atomic_store(&g_lagging_name, NULL);
    atomic_store(&g_cursor_at_detect, 0);
    atomic_store(&g_test_remedy_calls, 0);
}

bool catalog_lag_exceeded_test_feed(const struct catalog_index_status *rows,
                                    size_t n)
{
    return eval_over(rows, n);
}

int catalog_lag_exceeded_test_remedy(void)
{
    return (int)remedy_catalog_lag_exceeded();
}

int catalog_lag_exceeded_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}

const char *catalog_lag_exceeded_test_lagging_name(void)
{
    return atomic_load(&g_lagging_name);
}
#endif
