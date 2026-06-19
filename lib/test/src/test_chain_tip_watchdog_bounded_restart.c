/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * P0 resilience test for chain_tip_watchdog's bounded-restart logic.
 *
 * The watchdog used to call thread_registry_request_shutdown() every time
 * the tip stayed stuck past the restart threshold. systemd Restart=always
 * then brought the node back, and against a DETERMINISTIC wedge that loop
 * ran forever (~every 20 min). The fix: persist (stuck_height,
 * consecutive_no_progress_restarts) in progress.kv so the counter SURVIVES
 * the restart it triggers; after CHAIN_TIP_WD_MAX_RESTARTS it stops
 * restarting and pages a human (EV_OPERATOR_NEEDED) instead, staying up
 * degraded.
 *
 * Coverage:
 *   (a) K no-progress restarts at the SAME height across simulated process
 *       boots → after the cap, NO further shutdown request, operator_needed
 *       fired exactly once at the cap.
 *   (b) only SUSTAINED progress (CHAIN_TIP_WD_EPISODE_CLEAR blocks past the
 *       episode anchor) resets the counter; a 1-block creep does NOT.
 *   (c) the persisted counter is what carries across boots — wiping ONLY
 *       in-memory state (not progress.kv) and reloading reproduces the count.
 *   (d) a creeping wedge (tip gains 1-2 blocks per restart, re-wedges within
 *       CHAIN_TIP_WD_EPISODE_MARGIN of the anchor) is ONE episode: the count
 *       carries, the cap still bites; outside the margin is a new episode. */

#include "test/test_helpers.h"

#include "services/chain_tip_watchdog.h"
#include "storage/progress_store.h"
#include "event/event.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WD_CHECK(name, expr) do { \
    printf("chain_tip_wd_bounded: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static _Atomic int g_operator_events;

static void wd_operator_observer(enum event_type type, uint32_t peer_id,
                                 const void *payload, uint32_t payload_len,
                                 void *ctx)
{
    (void)type; (void)peer_id; (void)payload; (void)payload_len; (void)ctx;
    atomic_fetch_add(&g_operator_events, 1);
}

static void wd_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/chain_tip_wd_%d_%s", (int)getpid(), tag);
}

static int wd_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* Model one process boot: zero in-memory state, then reload the persisted
 * counters from the open progress.kv (exactly what register() does). */
static void wd_sim_boot(void)
{
    chain_tip_watchdog_test_reset_runtime();
    chain_tip_watchdog_test_load_persisted();
}

int test_chain_tip_watchdog_bounded_restart(void)
{
    printf("\n=== chain_tip_watchdog bounded-restart tests ===\n");
    int failures = 0;
    const int CAP = CHAIN_TIP_WD_MAX_RESTARTS;

    /* Observers only fire once the event log is initialized. */
    event_log_init();
    event_clear_all_observers();
    atomic_store(&g_operator_events, 0);
    event_observe(EV_OPERATOR_NEEDED, wd_operator_observer, NULL);

    /* ── (a) deterministic wedge: K restarts then STOP + page ───────── */
    {
        char dir[256];
        wd_tmpdir(dir, sizeof(dir), "wedge");
        wd_mkdir_p(dir);
        WD_CHECK("open progress.kv", progress_store_open(dir));
        atomic_store(&g_operator_events, 0);

        const int64_t STUCK_H = 3123688;  /* the live wedge height */
        bool all_restarts_requested_shutdown = true;
        int operator_pages = 0;

        /* Simulate restarts 1..CAP+2. Each iteration is a fresh process at
         * the SAME stuck height: boot (reload persisted count), then hit the
         * restart escalation. Below the cap it requests shutdown (true);
         * at/after the cap it pages instead (false). */
        for (int restart = 1; restart <= CAP + 2; restart++) {
            wd_sim_boot();
            bool requested = chain_tip_watchdog_test_escalate_restart(STUCK_H);

            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);

            if (restart <= CAP) {
                /* restarts 1..CAP: shutdown requested, counter climbs. */
                if (!requested) all_restarts_requested_shutdown = false;
                char label[96];
                snprintf(label, sizeof(label),
                         "restart %d <= cap requests shutdown", restart);
                WD_CHECK(label, requested);
                snprintf(label, sizeof(label),
                         "restart %d persisted count == %d", restart, restart);
                WD_CHECK(label, s.no_progress_restarts == restart);
                WD_CHECK("not yet operator_needed below cap",
                         !s.operator_needed);
            } else {
                /* restart CAP+1, CAP+2: NO shutdown, operator paged. */
                char label[96];
                snprintf(label, sizeof(label),
                         "restart %d > cap does NOT request shutdown", restart);
                WD_CHECK(label, !requested);
                WD_CHECK("operator_needed flag set past cap",
                         s.operator_needed);
                operator_pages++;
            }
        }

        WD_CHECK("all sub-cap restarts requested shutdown",
                 all_restarts_requested_shutdown);
        /* We paged on CAP+1 and CAP+2 (two over-cap escalations). */
        WD_CHECK("paged operator on each over-cap escalation",
                 operator_pages == 2);
        /* The EV_OPERATOR_NEEDED event is emitted live on every page,
         * regardless of the per-process counter resets, so the observer
         * sees BOTH pages — this is the cross-restart paging signal a
         * human/MCP would actually receive. */
        WD_CHECK("EV_OPERATOR_NEEDED emitted once per over-cap page",
                 atomic_load(&g_operator_events) == 2);

        struct chain_tip_watchdog_stats s;
        chain_tip_watchdog_get_stats(&s);
        /* The fires_* counters are per-process (reset each simulated boot).
         * The final boot (restart CAP+2) paged exactly once and requested
         * no shutdown — so for THAT process: 0 restart fires, 1 page. */
        WD_CHECK("final process fired no shutdown request",
                 s.fires_restart == 0);
        WD_CHECK("final process paged exactly once",
                 s.fires_operator_needed == 1);
        WD_CHECK("persisted_stuck_height recorded",
                 s.persisted_stuck_height == STUCK_H);
        WD_CHECK("max_restarts reported", s.max_restarts == CAP);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── (b) transient hang: 1 restart, then tip advances → reset ───── */
    {
        char dir[256];
        wd_tmpdir(dir, sizeof(dir), "transient");
        wd_mkdir_p(dir);
        WD_CHECK("open progress.kv (transient)", progress_store_open(dir));

        const int64_t STUCK_H = 1000000;

        /* Restart #1 at STUCK_H — a transient hang. */
        wd_sim_boot();
        bool r1 = chain_tip_watchdog_test_escalate_restart(STUCK_H);
        WD_CHECK("transient restart 1 requests shutdown", r1);
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("transient count == 1", s.no_progress_restarts == 1);
        }

        /* Process comes back AND the tip advances. A 1-block creep is
         * exactly the creeping-wedge signature and must NOT clear the
         * budget — clearing on any advance made the restart loop
         * infinite. Only sustained progress past the anchor clears. */
        wd_sim_boot();
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("after reboot, count still 1 (loaded from kv)",
                     s.no_progress_restarts == 1);
        }
        chain_tip_watchdog_test_observe_advance(STUCK_H + 1);
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("1-block creep keeps count",
                     s.no_progress_restarts == 1);
            WD_CHECK("1-block creep keeps episode anchor",
                     s.persisted_stuck_height == STUCK_H);
        }
        chain_tip_watchdog_test_observe_advance(
            STUCK_H + CHAIN_TIP_WD_EPISODE_CLEAR);
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("sustained advance resets count to 0",
                     s.no_progress_restarts == 0);
            WD_CHECK("sustained advance clears persisted_stuck_height",
                     s.persisted_stuck_height == -1);
            WD_CHECK("sustained advance clears operator_needed",
                     !s.operator_needed);
        }

        /* The reset must SURVIVE a reboot (was it persisted?). A LATER hang
         * at a new height then gets a fresh full restart budget. */
        wd_sim_boot();
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("reset persisted across reboot (count 0)",
                     s.no_progress_restarts == 0);
        }
        const int64_t NEW_STUCK = 2000000;
        bool r_new = chain_tip_watchdog_test_escalate_restart(NEW_STUCK);
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("new-height hang gets fresh restart (shutdown requested)",
                     r_new);
            WD_CHECK("new-height count starts at 1",
                     s.no_progress_restarts == 1);
            WD_CHECK("new-height tracked as stuck",
                     s.persisted_stuck_height == NEW_STUCK);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── (c) persistence carries the count across an in-memory wipe ─── */
    {
        char dir[256];
        wd_tmpdir(dir, sizeof(dir), "persist");
        wd_mkdir_p(dir);
        WD_CHECK("open progress.kv (persist)", progress_store_open(dir));

        const int64_t STUCK_H = 42;

        wd_sim_boot();
        (void)chain_tip_watchdog_test_escalate_restart(STUCK_H);
        wd_sim_boot();
        (void)chain_tip_watchdog_test_escalate_restart(STUCK_H);
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("two boots at same height → count 2",
                     s.no_progress_restarts == 2);
        }

        /* Hard close + reopen the store: the count must come back from disk,
         * not from a lingering in-memory atomic. */
        progress_store_close();
        WD_CHECK("reopen progress.kv", progress_store_open(dir));
        wd_sim_boot();
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("count survives close+reopen of progress.kv",
                     s.no_progress_restarts == 2);
            WD_CHECK("stuck height survives close+reopen",
                     s.persisted_stuck_height == STUCK_H);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── (d) creeping wedge: re-stuck within the margin = SAME episode ── */
    {
        char dir[256];
        wd_tmpdir(dir, sizeof(dir), "creep");
        wd_mkdir_p(dir);
        WD_CHECK("open progress.kv (creep)", progress_store_open(dir));

        /* Each restart "helps" by 2 blocks, then the tip re-wedges. The
         * old exact-height keying saw a NEW stuck height every time and
         * reset the budget = infinite restart loop. Episode keying must
         * carry the count and keep the original anchor. */
        const int64_t ANCHOR = 3132687;
        int64_t h = ANCHOR;
        for (int restart = 1; restart <= CAP; restart++) {
            wd_sim_boot();
            bool requested = chain_tip_watchdog_test_escalate_restart(h);
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            char label[96];
            snprintf(label, sizeof(label),
                     "creep restart %d <= cap requests shutdown", restart);
            WD_CHECK(label, requested);
            snprintf(label, sizeof(label),
                     "creep restart %d carries count across heights", restart);
            WD_CHECK(label, s.no_progress_restarts == restart);
            WD_CHECK("creep keeps episode anchor",
                     s.persisted_stuck_height == ANCHOR);
            h += 2;  /* tip creeps, stays inside the episode margin */
        }

        /* Over the cap, still inside the margin: page, do NOT restart. */
        wd_sim_boot();
        bool over_cap = chain_tip_watchdog_test_escalate_restart(h);
        WD_CHECK("creep over cap does NOT request shutdown", !over_cap);
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("creep over cap pages operator", s.operator_needed);
        }

        /* A wedge OUTSIDE the margin is a genuinely new episode. */
        wd_sim_boot();
        const int64_t NEW_ANCHOR = ANCHOR + CHAIN_TIP_WD_EPISODE_MARGIN + 1;
        bool r_new = chain_tip_watchdog_test_escalate_restart(NEW_ANCHOR);
        {
            struct chain_tip_watchdog_stats s;
            chain_tip_watchdog_get_stats(&s);
            WD_CHECK("outside-margin wedge requests shutdown", r_new);
            WD_CHECK("outside-margin wedge count restarts at 1",
                     s.no_progress_restarts == 1);
            WD_CHECK("outside-margin wedge re-anchors",
                     s.persisted_stuck_height == NEW_ANCHOR);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── (e) deterministic stall: page on the FIRST escalation, 0 restarts ─
     *
     * A successor pinned on a deterministic precondition (the live tick reads
     * tip_finalize's TF_BLOCKED_SUCCESSOR_PENDING class — a persisted ok=0
     * script row) is byte-identical every boot, so a restart cannot clear it.
     * The cause-probe must page immediately and burn ZERO restarts, instead
     * of power-cycling the full CAP first (the live 2026-06-19 wedge spent 3
     * restarts on exactly this deterministic condition before paging). */
    {
        char dir[256];
        wd_tmpdir(dir, sizeof(dir), "deterministic");
        wd_mkdir_p(dir);
        WD_CHECK("open progress.kv (deterministic)", progress_store_open(dir));
        atomic_store(&g_operator_events, 0);

        const int64_t STUCK_H = 3151411;  /* the live deterministic wedge */
        wd_sim_boot();
        bool requested = chain_tip_watchdog_test_escalate_deterministic(STUCK_H);

        struct chain_tip_watchdog_stats s;
        chain_tip_watchdog_get_stats(&s);
        WD_CHECK("deterministic stall does NOT request shutdown", !requested);
        WD_CHECK("deterministic stall pages operator on first escalation",
                 s.operator_needed);
        WD_CHECK("deterministic stall burns ZERO restarts",
                 s.no_progress_restarts == 0);
        WD_CHECK("deterministic stall fired exactly one page",
                 s.fires_operator_needed == 1);
        WD_CHECK("deterministic stall requested no shutdown (0 restart fires)",
                 s.fires_restart == 0);
        WD_CHECK("deterministic EV_OPERATOR_NEEDED emitted once",
                 atomic_load(&g_operator_events) == 1);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* Leave global state clean for the next test. */
    chain_tip_watchdog_test_reset_runtime();
    event_clear_all_observers();

    printf("chain_tip_wd_bounded: %d failures\n", failures);
    return failures;
}
