/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_drive_watchdog — makes the synchronous reducer/mint drive
 * observable and named when it wedges. See the header for the SYMPTOM /
 * REMEDY / WITNESSED contract. Also owns the "reducer_drive" dumpstate
 * subsystem (reducer_drive_dump_state_json) — this file is the state, so it
 * is the natural owner rather than a bespoke command (CLAUDE.md "Adding
 * state introspection"). */

#include "conditions/reducer_drive_watchdog.h"
#include "conditions/condition_registry.h"
#include "framework/condition.h"
#include "jobs/utxo_apply_stage.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/reducer_drive_guard.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Typed blocker id — see util/blocker.h. TRANSIENT: a wedged drive is
 * recoverable the instant the drive itself exits or advances (there is no
 * escape action a supervisor sweep can dispatch that would be safe — the
 * drive owns the write path). */
#define REDUCER_DRIVE_WATCHDOG_BLOCKER_ID "reducer_drive_stuck"
#define REDUCER_DRIVE_WATCHDOG_DEFAULT_SEC 60

/* Baseline cursor tracking (single-writer: the condition engine tick
 * thread calls detect/remedy/witness for one condition serially, never
 * concurrently with itself). UINT64_MAX is the "no observation yet"
 * sentinel — a real utxo_apply cursor never reaches it. */
static _Atomic uint64_t g_last_cursor_seen = UINT64_MAX;

/* Fields captured at the detect() tick that tripped the condition; remedy()
 * and the dump function read them back. */
static _Atomic int64_t g_age_at_detect_us;
static _Atomic uint64_t g_cursor_at_detect;

/* Unix time of the last blocker_set fired by remedy(); 0 = never. Read by
 * the dumpstate subsystem. */
static _Atomic int64_t g_last_fire_unix;

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
/* -1 = unset (use the real env-or-default lookup); >= 0 = forced value,
 * including 0 (trip the instant any age > 0 is observed). */
static _Atomic int g_test_threshold_override = -1;
/* -1 = unset (use the real utxo_apply_stage_cursor()); >= 0 = forced
 * cursor, so tests never need to spin up the real utxo_apply stage. */
static _Atomic int64_t g_test_cursor_override = -1;
#endif

static int reducer_drive_watchdog_threshold_secs(void)
{
#ifdef ZCL_TESTING
    int ov = atomic_load(&g_test_threshold_override);
    if (ov >= 0)
        return ov;
#endif
    const char *env = getenv("ZCL_DRIVE_WATCHDOG_SEC");
    if (env && env[0]) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end != env && v > 0 && v < 1000000000L)
            return (int)v;
    }
    return REDUCER_DRIVE_WATCHDOG_DEFAULT_SEC;
}

/* The single reader for the durable utxo_apply cursor — lock-free (see
 * utxo_apply_stage_cursor()/stage_cursor(): guarded field, torn 64-bit read
 * tolerated by design, no sqlite/mutex on this path), safe to call from the
 * condition engine thread without racing a long-running drive. Tests inject
 * a fixed value instead of spinning up the real utxo_apply stage module. */
static uint64_t reducer_drive_watchdog_read_cursor(void)
{
#ifdef ZCL_TESTING
    int64_t ov = atomic_load(&g_test_cursor_override);
    if (ov >= 0)
        return (uint64_t)ov;
#endif
    return utxo_apply_stage_cursor();
}

static bool detect_reducer_drive_watchdog(void)
{
    if (!reducer_drive_active()) {
        /* Not driving — nothing to track. Reset the baseline so the next
         * drive starts with a clean "no observation yet" state. */
        atomic_store(&g_last_cursor_seen, UINT64_MAX);
        return false;
    }

    uint64_t cursor = reducer_drive_watchdog_read_cursor();
    uint64_t last = atomic_exchange(&g_last_cursor_seen, cursor);

    /* First observation of this drive episode, or the cursor moved since
     * the previous detect tick — forward progress observed, not stuck. */
    if (last == UINT64_MAX || cursor != last)
        return false;

    int64_t age_us = reducer_drive_age_us();
    int64_t threshold_us =
        (int64_t)reducer_drive_watchdog_threshold_secs() * 1000000;
    if (age_us <= threshold_us)
        return false;

    atomic_store(&g_age_at_detect_us, age_us);
    atomic_store(&g_cursor_at_detect, cursor);
    return true;
}

static enum condition_remedy_result remedy_reducer_drive_watchdog(void)
{
    int64_t age_us = atomic_load(&g_age_at_detect_us);
    uint64_t cursor = atomic_load(&g_cursor_at_detect);
    const char *label = reducer_drive_label();
    if (!label[0])
        label = "unlabeled";
    int threshold_secs = reducer_drive_watchdog_threshold_secs();

    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "reducer drive '%s' active %llds, utxo_apply cursor frozen at "
             "height %llu (threshold=%ds) — no automated repair seam, "
             "clears when the drive advances or exits",
             label, (long long)(age_us / 1000000),
             (unsigned long long)cursor, threshold_secs);

    struct blocker_record r;
    if (blocker_init(&r, REDUCER_DRIVE_WATCHDOG_BLOCKER_ID, "reducer_drive",
                     BLOCKER_TRANSIENT, reason)) {
        (void)blocker_set(&r);
        atomic_store(&g_last_fire_unix, platform_time_wall_unix());
    }

    LOG_WARN("condition",
             "[condition:reducer_drive_watchdog] label=%s age_s=%lld "
             "cursor=%llu threshold_s=%d action=name_blocker",
             label, (long long)(age_us / 1000000),
             (unsigned long long)cursor, threshold_secs);

#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif

    /* Honest "cannot self-heal": there is no safe automated action against a
     * synchronous drive owned by another thread. FAILED pages the operator
     * on the normal ladder; the cooldown re-arm (see the condition struct)
     * keeps re-notifying without ever latching a recoverable stall forever. */
    return COND_REMEDY_FAILED;
}

static bool witness_reducer_drive_watchdog(int64_t target_at_detect)
{
    (void)target_at_detect;
    // honest-witness-ok: cleared iff the drive actually exited OR the
    // utxo_apply cursor genuinely advanced past the height frozen at
    // detect() — both read live, non-cached state.
    bool active = reducer_drive_active();
    uint64_t cursor = reducer_drive_watchdog_read_cursor();
    uint64_t frozen = atomic_load(&g_cursor_at_detect);
    bool resolved = !active || cursor != frozen;
    if (resolved)
        blocker_clear(REDUCER_DRIVE_WATCHDOG_BLOCKER_ID);
    return resolved;
}

static bool detail_reducer_drive_watchdog(struct json_value *out)
{
    if (!out)
        return false;
    const char *label = reducer_drive_label();
    bool ok = true;
    ok = ok && json_push_kv_bool(out, "drive_active", reducer_drive_active());
    ok = ok && json_push_kv_str(out, "label", label[0] ? label : "");
    ok = ok && json_push_kv_int(out, "age_at_detect_us",
                                atomic_load(&g_age_at_detect_us));
    ok = ok && json_push_kv_int(out, "cursor_at_detect",
                                (int64_t)atomic_load(&g_cursor_at_detect));
    ok = ok && json_push_kv_int(out, "threshold_secs",
                                reducer_drive_watchdog_threshold_secs());
    ok = ok && json_push_kv_int(out, "last_fire_unix",
                                atomic_load(&g_last_fire_unix));
    return ok;
}

static struct condition c_reducer_drive_watchdog = {
    .name = "reducer_drive_watchdog",
    .severity = COND_CRITICAL,
    .poll_secs = 15,
    .backoff_secs = 60,
    /* No automated repair seam (see remedy) — page fast, then keep
     * re-notifying on cooldown rather than spinning remedy attempts that
     * can never change the outcome. */
    .max_attempts = 1,
    /* Continue-with-cooldown (sticky-node plan #7): a wedged synchronous
     * drive is not a local deterministic-unrecoverable fault in the
     * consensus sense — it may be a long but legitimate fold that briefly
     * looks frozen, or a genuine hang. Either way latching operator_needed
     * forever is a human dead-end; re-arm every 10 minutes, unbounded,
     * while it stays wedged. The episode itself clears instantly (via
     * witness) the moment the drive advances or exits. */
    .cooldown_secs = 600,
    .cooldown_max_rearms = 0,
    .detect = detect_reducer_drive_watchdog,
    .remedy = remedy_reducer_drive_watchdog,
    .witness = witness_reducer_drive_watchdog,
    .detail = detail_reducer_drive_watchdog,
    .witness_window_secs = 60,
};

void register_reducer_drive_watchdog(void)
{
    (void)condition_register(&c_reducer_drive_watchdog);
}

bool reducer_drive_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    const char *label = reducer_drive_label();
    bool ok = true;
    ok = ok && json_push_kv_bool(out, "active", reducer_drive_active());
    ok = ok && json_push_kv_str(out, "label", label[0] ? label : "");
    ok = ok && json_push_kv_int(out, "age_us", reducer_drive_age_us());
    ok = ok && json_push_kv_int(out, "watchdog_threshold_secs",
                                reducer_drive_watchdog_threshold_secs());
    ok = ok && json_push_kv_int(out, "last_watchdog_fire_unix",
                                atomic_load(&g_last_fire_unix));
    ok = ok && json_push_kv_int(out, "utxo_apply_cursor",
                                (int64_t)reducer_drive_watchdog_read_cursor());

    /* coins_applied_height is the LAGGING measure (the durable coins_kv
     * frontier vs. the immediate in-memory utxo_apply cursor above) — the
     * gap between the two is exactly the -fold-inram batching lag. Read
     * under progress_store_tx_trylock: NEVER a blocking coins/progress lock
     * from an observational surface (LOCK-ORDER LAW) — a synchronous drive
     * can hold this lock for a long stretch, and this dump function may be
     * called from an MCP/RPC thread that must not stall behind it. */
    sqlite3 *db = progress_store_db();
    if (!db || !progress_store_tx_trylock()) {
        ok = ok && json_push_kv_bool(out, "coins_applied_read_ok", false);
        ok = ok && json_push_kv_int(out, "coins_applied_height", -1);
        return ok;
    }
    int32_t coins_applied = -1;
    bool found = false;
    bool read_ok = coins_kv_get_applied_height(db, &coins_applied, &found);
    progress_store_tx_unlock();
    ok = ok && json_push_kv_bool(out, "coins_applied_read_ok", read_ok);
    ok = ok && json_push_kv_int(out, "coins_applied_height",
                                (read_ok && found) ? coins_applied : -1);
    return ok;
}

#ifdef ZCL_TESTING
void reducer_drive_watchdog_test_reset(void)
{
    atomic_store(&g_last_cursor_seen, UINT64_MAX);
    atomic_store(&g_age_at_detect_us, 0);
    atomic_store(&g_cursor_at_detect, 0);
    atomic_store(&g_last_fire_unix, 0);
    atomic_store(&g_test_remedy_calls, 0);
    atomic_store(&g_test_threshold_override, -1);
    atomic_store(&g_test_cursor_override, -1);
    blocker_clear(REDUCER_DRIVE_WATCHDOG_BLOCKER_ID);
    condition_reset_state(&c_reducer_drive_watchdog);
}

int reducer_drive_watchdog_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}

void reducer_drive_watchdog_test_set_threshold_secs(int secs)
{
    atomic_store(&g_test_threshold_override, secs);
}

void reducer_drive_watchdog_test_set_cursor_override(int64_t cursor)
{
    atomic_store(&g_test_cursor_override, cursor);
}
#endif
