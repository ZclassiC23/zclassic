/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * event_emitter_port — publish a domain event.
 *
 * The domain announces facts ("block applied at h=N", "tip advanced",
 * "consensus reject", "peer banned") via this port. Adapters wire it
 * to the lock-free event ring in lib/event/ for production, and to a
 * deterministic recorder for the simulator.
 *
 * Emission is non-blocking and best-effort: if the consumer ring is
 * full, the event may be dropped and a drop counter incremented by
 * the adapter. The domain MUST NOT depend on delivery for
 * correctness — events are observations, not commands.
 */

#ifndef ZCL_PORTS_EVENT_EMITTER_PORT_H
#define ZCL_PORTS_EVENT_EMITTER_PORT_H

#include <stddef.h>
#include <stdint.h>

/* Stable identifier for event kinds. Specific values live in the
 * lib/event/ catalog and are not part of the port contract — the port
 * passes the kind through verbatim. */
typedef uint32_t event_kind_t;

struct event_emitter_port {
    void *self;

    /* Publish an event. payload may be NULL when payload_len == 0.
     * Returns the new ring sequence number on accept, or 0 on drop.
     * The domain treats 0 as a non-fatal signal and continues. */
    uint64_t (*emit)(void *self,
                     event_kind_t kind,
                     const void *payload,
                     size_t payload_len);
};

#endif /* ZCL_PORTS_EVENT_EMITTER_PORT_H */
