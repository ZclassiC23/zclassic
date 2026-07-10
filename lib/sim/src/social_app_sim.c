/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "sim/social_app_sim.h"

#include <stddef.h>
#include <string.h>

enum { SOCIAL_NODES = 5, SOCIAL_VALID_EVENT = 0, SOCIAL_INVALID_EVENT = 1 };

struct social_node {
    bool online;
    bool seen[2];
};

struct social_run {
    uint64_t rng;
    uint64_t transcript;
    uint32_t deliveries;
    uint32_t rejected;
    struct social_node nodes[SOCIAL_NODES];
};

static uint64_t mix(uint64_t hash, uint64_t value)
{
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t next_random(struct social_run *run)
{
    uint64_t x = run->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    run->rng = x;
    return x;
}

static bool linked(size_t a, size_t b, bool partitioned)
{
    static const bool edges[SOCIAL_NODES][SOCIAL_NODES] = {
        { false, true,  true,  false, false },
        { true,  false, false, true,  false },
        { true,  false, false, true,  false },
        { false, true,  true,  false, true  },
        { false, false, false, true,  false },
    };
    if (partitioned && ((a == 2 && b == 3) || (a == 3 && b == 2)))
        return false;
    return edges[a][b];
}

static bool accepts(size_t node, size_t event)
{
    if (event == SOCIAL_INVALID_EVENT)
        return false;
    /* Node 1 models a relay attempting to censor Alice's valid post. */
    return node != 1;
}

static bool deliver_round(struct social_run *run, size_t event,
                          bool partitioned)
{
    struct { uint8_t from, to; } pending[64];
    size_t count = 0;
    for (size_t from = 0; from < SOCIAL_NODES; from++) {
        if (!run->nodes[from].online || !run->nodes[from].seen[event])
            continue;
        for (size_t to = 0; to < SOCIAL_NODES; to++) {
            if (from == to || !run->nodes[to].online ||
                run->nodes[to].seen[event] || !linked(from, to, partitioned))
                continue;
            pending[count].from = (uint8_t)from;
            pending[count].to = (uint8_t)to;
            count++;
        }
    }
    bool progressed = false;
    while (count > 0) {
        size_t pick = (size_t)(next_random(run) % count);
        uint8_t from = pending[pick].from;
        uint8_t to = pending[pick].to;
        pending[pick] = pending[--count];
        run->transcript = mix(run->transcript,
                              ((uint64_t)event << 24) |
                              ((uint64_t)from << 8) | to);
        run->deliveries++;
        if (!accepts(to, event)) {
            run->rejected++;
            run->transcript = mix(run->transcript, UINT64_C(0xdead0000) | to);
            continue;
        }
        if (!run->nodes[to].seen[event]) {
            run->nodes[to].seen[event] = true;
            progressed = true;
        }
    }
    return progressed;
}

static void converge(struct social_run *run, size_t event, bool partitioned)
{
    for (size_t round = 0; round < 16; round++) {
        if (!deliver_round(run, event, partitioned))
            break;
    }
}

bool zcl_social_app_sim_run(uint64_t seed,
                            struct zcl_social_sim_report *out)
{
    if (!out || seed == 0)
        return false;
    struct social_run run;
    memset(&run, 0, sizeof(run));
    run.rng = seed;
    run.transcript = mix(UINT64_C(1469598103934665603), seed);

    /* Alice and the initial relays are online. A partition isolates honest
     * relay 2 from relay 3; censor 1 refuses Alice's event. */
    for (size_t i = 0; i < 4; i++)
        run.nodes[i].online = true;
    run.nodes[0].seen[SOCIAL_VALID_EVENT] = true;
    converge(&run, SOCIAL_VALID_EVENT, true);

    /* Partition heals and a brand-new peer joins after publication. Honest
     * anti-entropy must carry the event around the refusing relay. */
    run.nodes[4].online = true;
    converge(&run, SOCIAL_VALID_EVENT, false);

    /* A forged event enters at the late joiner. Every attempted receiver must
     * reject it; it can never become accepted state on an honest node. */
    run.nodes[4].seen[SOCIAL_INVALID_EVENT] = true;
    converge(&run, SOCIAL_INVALID_EVENT, false);

    memset(out, 0, sizeof(*out));
    out->seed = seed;
    out->transcript = run.transcript;
    out->deliveries = run.deliveries;
    out->rejected_invalid = run.rejected;
    out->censorship_bypassed = !run.nodes[1].seen[SOCIAL_VALID_EVENT] &&
        run.nodes[2].seen[SOCIAL_VALID_EVENT] &&
        run.nodes[3].seen[SOCIAL_VALID_EVENT];
    out->partition_rejoin_converged =
        run.nodes[0].seen[SOCIAL_VALID_EVENT] &&
        run.nodes[2].seen[SOCIAL_VALID_EVENT] &&
        run.nodes[3].seen[SOCIAL_VALID_EVENT] &&
        run.nodes[4].seen[SOCIAL_VALID_EVENT];
    out->late_joiner_caught_up = run.nodes[4].seen[SOCIAL_VALID_EVENT];
    out->invalid_signature_rejected =
        !run.nodes[0].seen[SOCIAL_INVALID_EVENT] &&
        !run.nodes[1].seen[SOCIAL_INVALID_EVENT] &&
        !run.nodes[2].seen[SOCIAL_INVALID_EVENT] &&
        !run.nodes[3].seen[SOCIAL_INVALID_EVENT];
    return out->censorship_bypassed && out->partition_rejoin_converged &&
           out->late_joiner_caught_up && out->invalid_signature_rejected;
}
