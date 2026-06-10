/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_internal — sibling-private declarations shared between
 * validate_headers_stage.c (the Job) and its helper translation units
 * (validate_headers_validator.c, validate_headers_report.c). This is not a
 * public header. */

#ifndef ZCL_VALIDATE_HEADERS_INTERNAL_H
#define ZCL_VALIDATE_HEADERS_INTERNAL_H

#include "jobs/validate_headers_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block_index;
struct node_db;

/* ── Default validator (validate_headers_validator.c) ───────────────
 * Full PoW target + Equihash-from-nSolution verification, sourced from
 * the in-memory block index when it carries the solution, else from the
 * persisted compact block-index record, else from the node.db
 * blocks.solution BLOB. Matches vh_validator_fn. */
bool validate_headers_default_validator(const struct block_index *bi,
                                        const char *datadir,
                                        char *out_reason,
                                        size_t out_reason_size,
                                        void *user);

/* Override the node.db handle used by the default validator's SQLite
 * solution fallback. NULL (the default) resolves the live runtime handle
 * via app_runtime_node_db(); tests inject a fixture handle. Wiring this
 * does NOT relax validation — the solution bytes loaded from node.db are
 * verified by the identical Equihash/PoW path. */
void validate_headers_validator_set_node_db(struct node_db *ndb);

/* ── Failure summary + window report (validate_headers_report.c) ────
 * Read-only SQL reporting over validate_headers_log. Off the
 * advance-or-block path. */
struct validate_headers_failure_summary {
    int64_t count;
    int64_t first_height;
    int64_t last_height;
    char first_reason[VH_MAX_REASON];
    char last_reason[VH_MAX_REASON];
};

void validate_headers_failure_summary_load(
    struct validate_headers_failure_summary *out);

#endif /* ZCL_VALIDATE_HEADERS_INTERNAL_H */
