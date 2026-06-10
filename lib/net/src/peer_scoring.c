/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Typed peer-offence scoring layered on top of peer_misbehaving().
 * See lib/net/include/net/peer_scoring.h for design notes. */

#include "platform/time_compat.h"
#include "net/peer_scoring.h"
#include "net/net.h"
#include "event/event.h"
#include "util/log_macros.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PEER_SCORING_DEFAULT_THRESHOLD 100
#define PEER_SCORING_DEFAULT_BAN_HOURS 24
#define PEER_SCORING_DEFAULT_DECAY     1   /* points per minute */

static _Atomic int g_ban_threshold   = PEER_SCORING_DEFAULT_THRESHOLD;
static _Atomic int g_ban_hours       = PEER_SCORING_DEFAULT_BAN_HOURS;
static _Atomic int g_decay_per_min   = PEER_SCORING_DEFAULT_DECAY;

static int read_env_int(const char *name, int fallback, int min, int max)
{
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    char *end = NULL;
    long parsed = strtol(v, &end, 10);
    if (end == v || parsed < (long)min || parsed > (long)max)
        return fallback;
    return (int)parsed;
}

void peer_scoring_init(void)
{
    /* Threshold range [1, 1_000_000] — an operator who sets 0 would get
     * instant-ban behaviour which is almost certainly a mistake, so we
     * fall back to the default instead of honouring it. */
    int threshold = read_env_int("ZCL_PEER_BAN_THRESHOLD",
                                 PEER_SCORING_DEFAULT_THRESHOLD,
                                 1, 1000000);
    int hours = read_env_int("ZCL_PEER_BAN_HOURS",
                             PEER_SCORING_DEFAULT_BAN_HOURS,
                             1, 24 * 365);
    /* Decay can legitimately be 0 (operator wants sticky scores). */
    int decay = read_env_int("ZCL_PEER_SCORE_DECAY_PER_MIN",
                             PEER_SCORING_DEFAULT_DECAY,
                             0, 10000);

    atomic_store(&g_ban_threshold, threshold);
    atomic_store(&g_ban_hours, hours);
    atomic_store(&g_decay_per_min, decay);
}

int peer_scoring_ban_threshold(void)
{
    return atomic_load(&g_ban_threshold);
}

int peer_scoring_ban_hours(void)
{
    return atomic_load(&g_ban_hours);
}

int peer_scoring_decay_rate(void)
{
    return atomic_load(&g_decay_per_min);
}

int64_t peer_scoring_now_ms(void)
{
    struct timespec ts;
    if (platform_time_realtime_timespec(&ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int peer_offence_weight(enum peer_offence offence)
{
    /* DoS-policy surface: these are the exact weights the raw
     * peer_misbehaving() call-sites used before typed adoption.
     * Changing one changes ban behaviour. */
    switch (offence) {
    case PEER_OFFENCE_NONE:               return 0;
    case PEER_OFFENCE_TIMEOUT:            return 5;
    case PEER_OFFENCE_INVALID_MESSAGE:    return 10;
    case PEER_OFFENCE_UNREQUESTED:        return 10;
    case PEER_OFFENCE_OFFER_REJECTED:     return 10;
    case PEER_OFFENCE_FLOOD:              return 20;
    case PEER_OFFENCE_INVALID_PAYLOAD:    return 20;
    case PEER_OFFENCE_INVALID_HEADER:     return 50;
    case PEER_OFFENCE_INVALID_CHUNK:      return 50;
    case PEER_OFFENCE_INVALID_BLOCK:      return 100;
    case PEER_OFFENCE_INVALID_PROOF:      return 100;
    case PEER_OFFENCE_PROTOCOL_VIOLATION: return 100;
    case PEER_OFFENCE_COUNT_:             break;
    }
    return 0;
}

const char *peer_offence_name(enum peer_offence offence)
{
    switch (offence) {
    case PEER_OFFENCE_NONE:               return "none";
    case PEER_OFFENCE_TIMEOUT:            return "timeout";
    case PEER_OFFENCE_INVALID_MESSAGE:    return "invalid_message";
    case PEER_OFFENCE_UNREQUESTED:        return "unrequested";
    case PEER_OFFENCE_OFFER_REJECTED:     return "offer_rejected";
    case PEER_OFFENCE_FLOOD:              return "flood";
    case PEER_OFFENCE_INVALID_PAYLOAD:    return "invalid_payload";
    case PEER_OFFENCE_INVALID_HEADER:     return "invalid_header";
    case PEER_OFFENCE_INVALID_CHUNK:      return "invalid_chunk";
    case PEER_OFFENCE_INVALID_BLOCK:      return "invalid_block";
    case PEER_OFFENCE_INVALID_PROOF:      return "invalid_proof";
    case PEER_OFFENCE_PROTOCOL_VIOLATION: return "protocol_violation";
    case PEER_OFFENCE_COUNT_:             break;
    }
    return "unknown";
}

void peer_scoring_record(struct net_manager *nm, struct p2p_node *node,
                         enum peer_offence offence, const char *context)
{
    if (!node || offence == PEER_OFFENCE_NONE)
        return;

    char reason[192];
    const char *name = peer_offence_name(offence);
    if (context && *context) {
        snprintf(reason, sizeof(reason), "%s: %s", name, context);
    } else {
        snprintf(reason, sizeof(reason), "%s", name);
    }

    /* peer_misbehaving() handles the is_trusted_peer() guard, the score
     * accumulation, the EV_PEER_MISBEHAVE event, the auto-ban at the
     * configured threshold (see peer_misbehaving() in net.c which now
     * reads peer_scoring_ban_threshold()), the EV_PEER_BANNED event, and
     * the disconnect flag. All we add here is the typed name prefix. */
    peer_misbehaving(nm, node, peer_offence_weight(offence), reason);
}

bool peer_scoring_should_ban(const struct p2p_node *node)
{
    if (!node) return false;
    /* Cast away const for atomic_load — p2p_node's misbehavior field is
     * _Atomic int, and the atomic load does not mutate observable state. */
    int score = atomic_load((_Atomic int *)&((struct p2p_node *)node)->misbehavior);
    return score >= peer_scoring_ban_threshold();
}

int peer_scoring_decay(struct p2p_node *node, int64_t now_ms)
{
    if (!node) return 0;

    int decay_per_min = peer_scoring_decay_rate();
    int score = atomic_load(&node->misbehavior);

    /* Score of 0 can't decay further. */
    if (score <= 0) {
        atomic_store(&node->peer_score_last_good_ms, (int_least64_t)now_ms);
        return 0;
    }

    /* Decay disabled — leave the score alone but keep tracking the
     * good-interaction timestamp so a later re-enable works correctly. */
    if (decay_per_min <= 0) {
        atomic_store(&node->peer_score_last_good_ms, (int_least64_t)now_ms);
        return score;
    }

    int_least64_t last_good = atomic_load(&node->peer_score_last_good_ms);
    if (last_good == 0) {
        /* First decay call — anchor the timer instead of awarding decay
         * back to the UNIX epoch. */
        atomic_store(&node->peer_score_last_good_ms, (int_least64_t)now_ms);
        return score;
    }

    int64_t delta_ms = now_ms - (int64_t)last_good;
    if (delta_ms <= 0) {
        /* Clock skew or same-millisecond call — no decay but still update
         * the anchor to the latest observation. */
        atomic_store(&node->peer_score_last_good_ms, (int_least64_t)now_ms);
        return score;
    }

    int minutes = (int)(delta_ms / 60000);
    if (minutes <= 0)
        return score; /* sub-minute — don't advance last_good so fractions accumulate */

    int decay = minutes * decay_per_min;
    int new_score = score - decay;
    if (new_score < 0) new_score = 0;
    atomic_store(&node->misbehavior, new_score);
    atomic_store(&node->peer_score_last_good_ms, (int_least64_t)now_ms);
    return new_score;
}

void peer_scoring_on_good_interaction(struct p2p_node *node, int64_t now_ms)
{
    if (!node) return;
    (void)peer_scoring_decay(node, now_ms);
}

void peer_scoring_reset(struct p2p_node *node)
{
    if (!node) return;
    atomic_store(&node->misbehavior, 0);
    atomic_store(&node->peer_score_last_good_ms,
                 (int_least64_t)peer_scoring_now_ms());
}
