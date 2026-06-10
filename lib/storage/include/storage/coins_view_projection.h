/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_view_projection — B4: a read-only `struct coins_view` backed by
 * the UTXO **projection** instead of coins.db.
 *
 * This is the seam that closes the validation feedback loop (Prime
 * Directive): when the utxo_apply_stage is the authority
 * (`utxo_projection_get_author() == UTXO_AUTHOR_STAGE`), connect_block's
 * input lookups resolve through a `coins_view_cache` whose backing is
 * THIS view rather than `coins_view_sqlite`. The cache layer (RAM
 * read-cache) is unchanged; only the miss-resolution source moves to the
 * projection. The view is read-only: the stage authors UTXO state via
 * EV_UTXO_ADD/SPEND events, so `batch_write` here is a programming error.
 *
 * This view is the authoritative miss-resolution source whenever the stage
 * holds UTXO authority (the production default); parity with
 * coins_view_sqlite is proven by test_coins_view_projection.
 *
 * Mirrors coins_view_sqlite's vtable-embedding pattern. */

#ifndef ZCL_STORAGE_COINS_VIEW_PROJECTION_H
#define ZCL_STORAGE_COINS_VIEW_PROJECTION_H

#include "coins/coins_view.h"
#include "storage/utxo_projection.h"

#include <stdbool.h>

struct coins_view_projection {
    struct coins_view view;        /* vtable-based polymorphism (must be first) */
    utxo_projection_t *proj;       /* borrowed; not owned */
};

/* Bind `cvp` to `proj` and publish its vtable. `proj` is borrowed (the
 * caller keeps ownership). Returns false on NULL args. */
bool coins_view_projection_init(struct coins_view_projection *cvp,
                                utxo_projection_t *proj);

#endif /* ZCL_STORAGE_COINS_VIEW_PROJECTION_H */
