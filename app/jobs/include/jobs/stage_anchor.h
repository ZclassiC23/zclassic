/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_anchor — trusted reducer-anchor cursor alignment helpers.
 * utxo_apply alignment is capped by coins_applied_height when that durable
 * frontier exists, so a trusted tip cannot fake coin application. */

#ifndef ZCL_JOBS_STAGE_ANCHOR_H
#define ZCL_JOBS_STAGE_ANCHOR_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

bool stage_anchor_upstream_cursors_to(sqlite3 *db, uint64_t target,
                                      const char *owner,
                                      const char *reason);

#endif /* ZCL_JOBS_STAGE_ANCHOR_H */
