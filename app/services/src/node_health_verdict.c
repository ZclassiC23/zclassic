/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * node_health_verdict.c — the published health verdict atomics.
 *
 * Split out of node_health_service.c (E1 file-size ceiling: that unit sits
 * at the 800-line ceiling; this contract is self-contained). Two halves:
 *
 *   - node_health_verdict_publish(): called once at the end of every
 *     node_health_collect() — health ring, RPC handlers, soak service;
 *     last writer wins.
 *   - node_health_last_verdict(): read by the dedicated sd-watchdog pet
 *     thread (config/src/boot_sd_watchdog.c), which must decide
 *     ping/no-ping from CHEAP ATOMICS ONLY — never by running a collect
 *     inline, because a collect can block for minutes on reducer-held
 *     locks during bulk ingest, and that blocking was the pet-starvation
 *     source behind the 2026-08-02 systemd watchdog kill loop.
 *
 * The body-gap posture flag is the precise, bounded forgiveness for the
 * second half of that kill loop: "unhealthy SOLELY because the
 * body-history archive is still unproven". The sync FSM refuses
 * SYNC_AT_TIP while body_history_is_proven() is false (38fe0885b) — an
 * intentional, days-long posture on a snapshot-seeded datadir. healthy
 * MUST stay false there (serving must not open, /api/health stays 503),
 * but systemd-restarting cannot shorten the posture — it only interrupts
 * the backfill that ends it — so the sd pet forgives exactly this one
 * state and nothing else. Any named degradation (contradiction, operator
 * latch, mirror fatal, tip deadman, tip_lag growth, peer loss) clears the
 * posture and the pet stops. */

// one-result-type-ok:verdict-atomics-no-fallible-surface — E2 (one way
// out): this unit owns no fallible service surface. node_health_last_verdict's
// bool is a PRESENCE flag ("has any verdict been published yet"), read by
// the sd pet's pure decision function — there is no failure reason to carry
// (an absent verdict is data, not an error; the pet treats it as
// have_verdict=false and falls through to its boot-progress grace).
// node_health_verdict_publish returns void and cannot fail (atomic stores
// only). Nothing here can produce a zcl_result a caller could act on.

#include "platform/time_compat.h"
#include "services/node_health_service.h"
#include "storage/body_history.h"

#include <stdatomic.h>

static _Atomic bool    g_last_verdict_healthy = false;
static _Atomic bool    g_last_verdict_body_gap_posture = false;
static _Atomic int64_t g_last_verdict_us      = 0; /* 0 = never published */

bool node_health_last_verdict(bool *healthy_out, bool *body_gap_posture_out,
                              int64_t *publish_us_out)
{
    int64_t pub = atomic_load(&g_last_verdict_us);
    if (pub == 0)
        return false;
    if (healthy_out)
        *healthy_out = atomic_load(&g_last_verdict_healthy);
    if (body_gap_posture_out)
        *body_gap_posture_out = atomic_load(&g_last_verdict_body_gap_posture);
    if (publish_us_out)
        *publish_us_out = pub;
    return true;
}

void node_health_verdict_publish(const struct node_health_snapshot *snapshot)
{
    if (!snapshot)
        return;
    atomic_store(&g_last_verdict_healthy, snapshot->healthy);
    /* Deliberately strict: unhealthy with NO named degradation, archive
     * still unproven, at the network tip with peers. */
    atomic_store(&g_last_verdict_body_gap_posture,
                 !snapshot->healthy &&
                 snapshot->degraded_reason[0] == '\0' &&
                 !body_history_is_proven() &&
                 snapshot->has_peers &&
                 snapshot->tip_lag >= 0 &&
                 snapshot->tip_lag <= ZCL_NODE_HEALTH_LAG_WARN_BLOCKS);
    atomic_store(&g_last_verdict_us, platform_time_monotonic_us());
}
