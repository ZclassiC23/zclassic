/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * consensus_log_port — append-only audit log of accept/reject decisions.
 *
 * Every block the mutator evaluates produces one consensus-log entry:
 * either ACCEPTED (the block is now durable in block_log) or REJECTED
 * with a typed reason. The log is the forensic record: zcl_consensus_report,
 * zcl_reorg_history, zcl_diff_with_legacy are all tails of this log.
 *
 * The log is append-only, content-immutable, and rotated by the
 * adapter when configured size is exceeded.
 */

#ifndef ZCL_PORTS_CONSENSUS_LOG_PORT_H
#define ZCL_PORTS_CONSENSUS_LOG_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ports/block_log_port.h"   /* struct block_hash */
#include "util/result.h"

enum consensus_decision {
    CONSENSUS_DECISION_ACCEPTED       = 1,
    CONSENSUS_DECISION_REJECTED       = 2,
    CONSENSUS_DECISION_REORG_DISCARD  = 3,   /* block evicted by deeper-work fork */
};

/* Reasons rejected blocks were rejected. Stable identifiers; new
 * reasons are added at the end. The full catalog lives alongside the
 * domain validators in domain/consensus/. */
typedef uint32_t consensus_reject_reason_t;

struct consensus_log_entry {
    int64_t  ts_us;                         /* monotonic timestamp via clock_port */
    uint32_t height;
    struct block_hash hash;
    enum consensus_decision decision;
    consensus_reject_reason_t reason;       /* 0 when decision == ACCEPTED */
    uint32_t source_peer_id;                /* 0 when not from a peer (mined locally, mirror, snapshot) */
    /* Optional human-readable detail for diagnostics; adapter may
     * truncate to a fixed buffer length. */
    const char *detail;
};

typedef bool (*consensus_log_iter_fn)(const struct consensus_log_entry *entry,
                                      void *user_data);

enum consensus_log_err {
    CONSENSUS_LOG_ERR_IO       = 1,
    CONSENSUS_LOG_ERR_CORRUPT  = 2,
    CONSENSUS_LOG_ERR_CLOSED   = 3,
};

struct consensus_log_port {
    void *self;

    /* Record one decision. Durable when this returns OK. */
    struct zcl_result (*record)(void *self,
                                const struct consensus_log_entry *entry);

    /* Iterate entries newest-first, up to limit. */
    struct zcl_result (*iter_recent)(void *self,
                                     size_t limit,
                                     consensus_log_iter_fn cb,
                                     void *user_data);

    /* Iterate entries for a specific (height range, decision filter). */
    struct zcl_result (*iter_filtered)(void *self,
                                       uint32_t height_min,
                                       uint32_t height_max,
                                       enum consensus_decision decision_filter,
                                       consensus_log_iter_fn cb,
                                       void *user_data);
};

#endif /* ZCL_PORTS_CONSENSUS_LOG_PORT_H */
