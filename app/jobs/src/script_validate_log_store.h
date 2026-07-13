/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * script_validate_log_store — durable script_validate_log schema (+ idempotent
 * migrations) and read/write helpers, split out of script_validate_stage.c to
 * keep that file under the framework file-size ceiling. Pure sqlite kernel
 * helpers: they take a sqlite3 handle and touch no script_validate module
 * state. */

#ifndef ZCL_JOBS_SCRIPT_VALIDATE_LOG_STORE_H
#define ZCL_JOBS_SCRIPT_VALIDATE_LOG_STORE_H

#include "core/uint256.h"
#include "jobs/mint_skip_crypto.h"
#include "script/script_error.h"

#include <stdbool.h>
#include <stddef.h>

struct sqlite3;

/* One upstream body_persist_log row (source + ok-flag) at a given height. */
struct body_persist_row {
    int ok;
    char source[64];
};

/* One script_validate_log verdict row at a given height: the ok flag plus
 * the (possibly absent) block-hash binding. Rows predating the block_hash
 * column report has_block_hash == false. */
struct script_validate_verdict_row {
    int ok;
    enum mint_validation_evidence evidence;
    bool has_block_hash;
    struct uint256 block_hash;  /* valid only when has_block_hash */
};

bool script_validate_log_ensure_schema(struct sqlite3 *db);

/* Read the upstream body_persist_log {source, ok} at `height`. Returns 1 if a
 * row was found, 0 if not, -1 on a query error. */
int script_validate_body_persist_log_at(struct sqlite3 *db, int height,
                                        struct body_persist_row *out);

/* Read the script_validate_log {ok, block_hash} at `height` — the hash-bound
 * verdict consumers (tip_finalize's self-heal gate, utxo_apply's label-splice
 * gate) use to prove a height-keyed row belongs to the block they are about
 * to act on. Returns 1 if a row was found, 0 if not, -1 on a query error
 * (logged). */
int script_validate_log_verdict_at(struct sqlite3 *db, int height,
                                   struct script_validate_verdict_row *out);

bool script_validate_log_insert(struct sqlite3 *db, int height,
                                const char *status, bool ok,
                                size_t tx_count, size_t input_count,
                                const struct uint256 *first_failure_txid,
                                int first_failure_vin,
                                ScriptError first_failure_serror,
                                const struct uint256 *block_hash);

#endif /* ZCL_JOBS_SCRIPT_VALIDATE_LOG_STORE_H */
