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
 * The pet consumes only the publication timestamp. The verdict itself stays
 * authoritative for serving, conditions, remedies, and operator action, but
 * a fresh negative verdict is evidence of a live collector—not a hung process.
 * This separation prevents restart-proof named degradations from becoming
 * systemd crash loops. */

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
#include <stdatomic.h>

static _Atomic int64_t g_last_verdict_us      = 0; /* 0 = never published */

bool node_health_last_verdict(int64_t *publish_us_out)
{
    int64_t pub = atomic_load(&g_last_verdict_us);
    if (pub == 0)
        return false;
    if (publish_us_out)
        *publish_us_out = pub;
    return true;
}

void node_health_verdict_publish(const struct node_health_snapshot *snapshot)
{
    if (!snapshot)
        return;
    atomic_store(&g_last_verdict_us, platform_time_monotonic_us());
}
