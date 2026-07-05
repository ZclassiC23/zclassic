/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_EVENT_AGENT_PEERS_H
#define ZCL_EVENT_AGENT_PEERS_H

#include "net/connman.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct agent_peer_snapshot {
    size_t peer_count;
    size_t magicbean_peer_count;
    size_t zclassic_c23_peer_count;
    int peer_best_height;
    int64_t age_seconds;
    bool available;
    bool stale;
    const char *warning_reason;
};

void agent_peer_snapshot_collect(struct agent_peer_snapshot *out,
                                 struct connman *cm);

#endif /* ZCL_EVENT_AGENT_PEERS_H */
