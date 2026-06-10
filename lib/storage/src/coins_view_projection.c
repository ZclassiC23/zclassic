/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_view_projection — implementation. See coins_view_projection.h.
 *
 * Read-only coins_view backed by utxo_projection. Mirrors the
 * coins_view_sqlite vtable-thunk pattern. */

#include "storage/coins_view_projection.h"

#include "coins/coins.h"
#include "core/uint256.h"
#include "util/log_macros.h"

/* ── vtable implementations ──────────────────────────────────────── */

static bool cvp_get_coins_impl(void *self, const struct uint256 *txid,
                               struct coins *out)
{
    struct coins_view_projection *cvp = (struct coins_view_projection *)self;
    if (!cvp || !txid || !out) return false;
    return utxo_projection_get_coins(cvp->proj, txid->data, out);
}

static bool cvp_have_coins_impl(void *self, const struct uint256 *txid)
{
    struct coins_view_projection *cvp = (struct coins_view_projection *)self;
    if (!cvp || !txid) return false;
    /* "have" = any live output of txid exists. Reuse get_coins (the extra
     * reconstruction is acceptable on this read-only miss path) and release
     * the coins. */
    struct coins c;
    bool found = utxo_projection_get_coins(cvp->proj, txid->data, &c);
    if (found) coins_free(&c);
    return found;
}

static bool cvp_get_best_block_impl(void *self, struct uint256 *hash)
{
    /* The projection tracks its consume offset, not a best-block hash, so
     * it cannot name a tip: the tip_finalize cursor is the definitional
     * tip for the authoritative read path. Report "unknown". */
    (void)self;
    if (hash) uint256_set_null(hash);
    return false;
}

static bool cvp_batch_write_impl(void *self, struct coins_map *map_coins,
                                 const struct uint256 *hash_block)
{
    /* Read-only: the stage authors UTXO state via EV_UTXO_ADD/SPEND, so
     * flushing the legacy cache back into the projection would be a
     * second writer — exactly what B3's single-writer authority forbids.
     * Reaching here means the cache was wired to flush to the projection
     * backing, which is a programming error. */
    (void)self; (void)map_coins; (void)hash_block;
    LOG_FAIL("coins_view_projection",
             "batch_write called: projection is read-only (stage authors "
             "UTXO via events); cache must not flush here");
}

static bool cvp_get_stats_impl(void *self, struct coins_stats *stats)
{
    (void)self; (void)stats;
    return false;
}

static struct coins_view_vtable cvp_vtable = {
    .get_coins      = cvp_get_coins_impl,
    .have_coins     = cvp_have_coins_impl,
    .get_best_block = cvp_get_best_block_impl,
    .batch_write    = cvp_batch_write_impl,
    .get_stats      = cvp_get_stats_impl,
};

bool coins_view_projection_init(struct coins_view_projection *cvp,
                                utxo_projection_t *proj)
{
    if (!cvp || !proj)
        LOG_FAIL("coins_view_projection",
                 "init: NULL arg (cvp=%p proj=%p)",
                 (const void *)cvp, (const void *)proj);
    cvp->view.vtable = &cvp_vtable;
    cvp->view.impl   = cvp;
    cvp->proj        = proj;
    return true;
}
