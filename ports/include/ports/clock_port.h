/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * clock_port — injectable monotonic and wall clock.
 *
 * The domain never calls clock_gettime() directly; it goes through a
 * clock_port. The production adapter resolves to CLOCK_MONOTONIC /
 * CLOCK_REALTIME; the simulator adapter resolves to a virtual time
 * line driven by the test harness. This is the discipline that makes
 * the whole node replayable under deterministic simulation.
 */

#ifndef ZCL_PORTS_CLOCK_PORT_H
#define ZCL_PORTS_CLOCK_PORT_H

#include <stdint.h>

struct clock_port {
    void *self;

    /* Strictly monotonic timestamp in microseconds. Used for timeouts,
     * rate limits, lease deadlines. Never goes backwards. */
    int64_t (*monotonic_us)(void *self);

    /* Wall-clock microseconds since the Unix epoch. Used only for
     * human-facing display and ledger timestamps that compare to
     * block header times. Subject to NTP step and skew. */
    int64_t (*wall_us)(void *self);
};

#endif /* ZCL_PORTS_CLOCK_PORT_H */
