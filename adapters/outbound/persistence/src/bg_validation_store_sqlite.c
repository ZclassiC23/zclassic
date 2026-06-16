/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * bg_validation_store_sqlite — sqlite implementation of
 * bg_validation_store_port.
 *
 * The two methods below are the crash-resume cursor reads/writes
 * (load_progress / save_progress over the node-DB state-kv path). The state
 * key ("bg_validation_height") and the get/set int semantics are part of the
 * contract: an existing on-disk cursor must read back bit-for-bit identical.
 */

#include "adapters/outbound/persistence/bg_validation_store_sqlite.h"

#include "models/database.h"

#include <stdint.h>

/* The single state key the cursor lives under: "bg_validation_height".
 * Fixed by contract so an existing on-disk cursor is read back unchanged. */
#define BGV_PROGRESS_KEY "bg_validation_height"

/* Separate key for the cumulative "non-coinbase tx not script-verified
 * because undo was missing/mismatched" tally. Keeps the verified claim
 * honest across restarts without disturbing the resume cursor. */
#define BGV_SKIPS_KEY "bg_validation_skipped_no_undo"

/* `self` aliases the node_db* directly — there is no wrapper struct. */
static inline struct node_db *ndb_of(void *self)
{
    return (struct node_db *)self;
}

static bool bgv_load_progress(void *self, int *out)
{
    struct node_db *ndb = ndb_of(self);
    if (!ndb || !ndb->open || !out)
        return false;
    int64_t val = 0;
    if (!node_db_state_get_int(ndb, BGV_PROGRESS_KEY, &val))
        return false;
    *out = (int)val;
    return true;
}

static bool bgv_save_progress(void *self, int height)
{
    struct node_db *ndb = ndb_of(self);
    if (!ndb || !ndb->open)
        return false;
    return node_db_state_set_int(ndb, BGV_PROGRESS_KEY, (int64_t)height);
}

static bool bgv_load_skips(void *self, int64_t *out)
{
    struct node_db *ndb = ndb_of(self);
    if (!ndb || !ndb->open || !out)
        return false;
    int64_t val = 0;
    if (!node_db_state_get_int(ndb, BGV_SKIPS_KEY, &val))
        return false;
    *out = val;
    return true;
}

static bool bgv_save_skips(void *self, int64_t skips)
{
    struct node_db *ndb = ndb_of(self);
    if (!ndb || !ndb->open)
        return false;
    return node_db_state_set_int(ndb, BGV_SKIPS_KEY, skips);
}

bool bg_validation_store_sqlite_bind(struct node_db *ndb,
                                     struct bg_validation_store_port *out_port)
{
    if (!out_port)
        return false;
    *out_port = (struct bg_validation_store_port){
        .self          = ndb,
        .load_progress = bgv_load_progress,
        .save_progress = bgv_save_progress,
        .load_skips    = bgv_load_skips,
        .save_skips    = bgv_save_skips,
    };
    return true;
}
