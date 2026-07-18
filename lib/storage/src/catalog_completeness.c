/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * catalog_completeness — implementation. See storage/catalog_completeness.h
 * for the design contract (REPORT ONLY, static table, forward-declared
 * app/-layer accessors).
 *
 * Cursor semantics per row:
 *   - op_return_index / address_index / view_integrity / explorer_projection
 *     report the ordinary "highest height folded contiguously from
 *     genesis" cursor their owning subsystem already exposes. lag =
 *     max(0, target - cursor) is the normal catch-up gap.
 *   - sprout_anchor / sapling_anchor / nullifier_history are activation-
 *     cursor stores (lib/storage/anchor_kv.h, lib/storage/nullifier_kv.h):
 *     a POSITIVE activation cursor means the prefix [0, activation_cursor)
 *     is a permanent, unbackfilled GAP, regardless of how far forward the
 *     store has since advanced. Reporting THAT forward position as
 *     "cursor" would hide the gap behind a healthy-looking lag. Instead,
 *     when the activation cursor is positive these rows report cursor=0 —
 *     "nothing from genesis is proven complete" — so lag = target (the
 *     harshest honest number), surfacing the exact
 *     utxo_apply.anchor_backfill_gap / nullifier_backfill_gap condition as
 *     a strongly positive lag rather than a small one. Only when the
 *     activation cursor is 0 (a true from-genesis store) do these rows
 *     report their real forward frontier height via anchor_kv_latest_tree.
 */

#include "storage/catalog_completeness.h"

#include "storage/anchor_kv.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "sapling/incremental_merkle_tree.h"
#include "core/uint256.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* ── app/-layer accessors, reached by FORWARD DECLARATION ONLY ──────────
 * Every symbol below is a pre-existing, already-shipped READ accessor
 * owned by its app/ home file (named per symbol). None is #include'd —
 * see catalog_completeness.h's layering note and
 * tools/scripts/check_lib_layering.sh's own "Fix option 2". This module
 * adds no new mutation surface: it only composes reads that already ship
 * elsewhere. */

struct node_db;

/* config/src/runtime.c — process-wide explorer/node.db handle, or NULL
 * before boot wires it. Zero-arg singleton load, same shape as
 * progress_store_db() below. */
extern struct node_db *app_runtime_node_db(void);

/* app/models/src/op_return_index.c (models/op_return_index.h) */
extern bool op_return_index_get_cursor(struct node_db *ndb,
                                       int32_t *out_height,
                                       uint8_t out_digest[32]);

/* app/models/src/explorer_index.c (models/explorer_index.h) — added
 * alongside this module for exactly this read (see that header). */
extern int64_t db_view_integrity_max_height(struct node_db *ndb);

/* app/controllers/src/sync_controller_writers.c
 * (controllers/sync_controller.h) */
extern int node_db_sync_get_tip_height(struct node_db *ndb);

/* app/jobs/src/address_index.c (jobs/address_index.h) */
extern bool address_index_enabled(void);
extern bool address_index_get_cursor(sqlite3 *db, int64_t *cursor_out);

/* app/jobs/src/txindex_projection.c (jobs/txindex_projection.h) — the
 * txindex fold reads the shared progress.kv kernel handle (progress_store_db),
 * exactly like address_index above. */
extern bool txindex_projection_enabled(void);
extern bool txindex_projection_get_cursor(sqlite3 *db, int64_t *cursor_out);

/* app/models/src/zslp_ledger.c (models/zslp_ledger.h) — the ZSLP ledger folds
 * into node.db (app_runtime_node_db), like op_return_index above. */
extern bool zslp_ledger_get_cursor(struct node_db *ndb, int32_t *out_height,
                                   uint8_t out_digest[32]);

/* ── per-index get_cursor() wrappers ─────────────────────────────────── */

static int64_t cc_get_op_return_cursor(void)
{
    struct node_db *ndb = app_runtime_node_db();
    if (!ndb) return CATALOG_CURSOR_UNAVAILABLE;
    int32_t h = -1;
    uint8_t digest[32];
    if (!op_return_index_get_cursor(ndb, &h, digest)) {
        LOG_WARN("catalog_completeness", "op_return_index_get_cursor failed");
        return CATALOG_CURSOR_UNAVAILABLE;
    }
    return (int64_t)h;    /* -1 == "nothing folded yet", a legit low cursor */
}

static int64_t cc_get_address_index_cursor(void)
{
    if (!address_index_enabled()) return CATALOG_CURSOR_UNAVAILABLE;
    sqlite3 *db = progress_store_db();
    if (!db) return CATALOG_CURSOR_UNAVAILABLE;
    int64_t cursor = -1;
    if (!address_index_get_cursor(db, &cursor)) {
        LOG_WARN("catalog_completeness", "address_index_get_cursor failed");
        return CATALOG_CURSOR_UNAVAILABLE;
    }
    return cursor;
}

static int64_t cc_get_txindex_cursor(void)
{
    if (!txindex_projection_enabled()) return CATALOG_CURSOR_UNAVAILABLE;
    sqlite3 *db = progress_store_db();
    if (!db) return CATALOG_CURSOR_UNAVAILABLE;
    int64_t cursor = -1;
    if (!txindex_projection_get_cursor(db, &cursor)) {
        LOG_WARN("catalog_completeness", "txindex_projection_get_cursor failed");
        return CATALOG_CURSOR_UNAVAILABLE;
    }
    return cursor;    /* -1 == "nothing folded yet", a legit low cursor */
}

static int64_t cc_get_zslp_ledger_cursor(void)
{
    struct node_db *ndb = app_runtime_node_db();
    if (!ndb) return CATALOG_CURSOR_UNAVAILABLE;
    int32_t h = -1;
    uint8_t digest[32];
    if (!zslp_ledger_get_cursor(ndb, &h, digest)) {
        LOG_WARN("catalog_completeness", "zslp_ledger_get_cursor failed");
        return CATALOG_CURSOR_UNAVAILABLE;
    }
    return (int64_t)h;    /* -1 == "nothing folded yet", a legit low cursor */
}

static int64_t cc_get_view_integrity_cursor(void)
{
    struct node_db *ndb = app_runtime_node_db();
    if (!ndb) return CATALOG_CURSOR_UNAVAILABLE;
    return db_view_integrity_max_height(ndb);
}

static int64_t cc_get_explorer_projection_cursor(void)
{
    struct node_db *ndb = app_runtime_node_db();
    if (!ndb) return CATALOG_CURSOR_UNAVAILABLE;
    return (int64_t)node_db_sync_get_tip_height(ndb);
}

/* Shared by both anchor rows: activation-cursor semantics (see the file
 * header comment). `pool` is ANCHOR_POOL_SPROUT / ANCHOR_POOL_SAPLING. */
static int64_t cc_anchor_cursor(int pool)
{
    sqlite3 *db = progress_store_db();
    if (!db) return CATALOG_CURSOR_UNAVAILABLE;

    int64_t activation = 0;
    bool found = false;
    if (!anchor_kv_activation_cursor(db, pool, &activation, &found)) {
        LOG_WARN("catalog_completeness",
                 "anchor_kv_activation_cursor failed pool=%d", pool);
        return CATALOG_CURSOR_UNAVAILABLE;
    }
    if (!found) return CATALOG_CURSOR_UNAVAILABLE;    /* never adopted */
    if (activation > 0) return 0;                     /* known genesis gap */

    struct incremental_merkle_tree tree;
    struct uint256 root;
    int64_t height = -1;
    enum anchor_kv_lookup_result r =
        anchor_kv_latest_tree(db, pool, &tree, &root, &height);
    return (r == ANCHOR_KV_FOUND) ? height : 0;
}

static int64_t cc_get_sprout_anchor_cursor(void)
{
    return cc_anchor_cursor(ANCHOR_POOL_SPROUT);
}

static int64_t cc_get_sapling_anchor_cursor(void)
{
    return cc_anchor_cursor(ANCHOR_POOL_SAPLING);
}

static int64_t cc_get_nullifier_cursor(void)
{
    sqlite3 *db = progress_store_db();
    if (!db) return CATALOG_CURSOR_UNAVAILABLE;

    int64_t activation = 0;
    bool found = false;
    if (!nullifier_kv_activation_cursor(db, &activation, &found)) {
        LOG_WARN("catalog_completeness", "nullifier_kv_activation_cursor failed");
        return CATALOG_CURSOR_UNAVAILABLE;
    }
    if (!found) return CATALOG_CURSOR_UNAVAILABLE;
    if (activation > 0) return 0;

    /* nullifier_kv exposes no forward "latest height" of its own (only a
     * row COUNT) — but it commits in the SAME utxo_apply_stage transaction
     * as the Sapling anchor frontier (see nullifier_kv.h / anchor_kv.h:
     * "nullifiers commit or roll back atomically with coins + cursor + log
     * row, exactly like coins_kv"), so the Sapling anchor's latest frontier
     * height is a faithful proxy for how far forward the nullifier set has
     * actually been folded. */
    struct incremental_merkle_tree tree;
    struct uint256 root;
    int64_t height = -1;
    enum anchor_kv_lookup_result r =
        anchor_kv_latest_tree(db, ANCHOR_POOL_SAPLING, &tree, &root, &height);
    return (r == ANCHOR_KV_FOUND) ? height : 0;
}

/* ── the static registry table — adding an index is one row + one
 * wrapper above ─────────────────────────────────────────────────────── */

struct catalog_index_entry {
    const char *name;
    int64_t (*get_cursor)(void);
    bool always_on;
};

static const struct catalog_index_entry g_catalog_indexes[] = {
    { "op_return_index",     cc_get_op_return_cursor,          true  },
    { "address_index",       cc_get_address_index_cursor,      false },
    { "txindex",             cc_get_txindex_cursor,            false },
    { "zslp_ledger",         cc_get_zslp_ledger_cursor,        false },
    { "sprout_anchor",       cc_get_sprout_anchor_cursor,       true  },
    { "sapling_anchor",      cc_get_sapling_anchor_cursor,      true  },
    { "nullifier_history",   cc_get_nullifier_cursor,          true  },
    { "view_integrity",      cc_get_view_integrity_cursor,      true  },
    { "explorer_projection", cc_get_explorer_projection_cursor, true  },
};
#define CATALOG_INDEX_COUNT \
    (sizeof(g_catalog_indexes) / sizeof(g_catalog_indexes[0]))

/* ── public API ───────────────────────────────────────────────────────── */

size_t catalog_completeness_snapshot(struct catalog_index_status *out,
                                     size_t max, int64_t target_height)
{
    if (!out || max == 0) {
        LOG_WARN("catalog_completeness",
                 "snapshot: no output buffer (out=%p max=%zu)",
                 (void *)out, max);
        return 0;
    }

    size_t n = CATALOG_INDEX_COUNT < max ? CATALOG_INDEX_COUNT : max;
    for (size_t i = 0; i < n; i++) {
        const struct catalog_index_entry *e = &g_catalog_indexes[i];
        struct catalog_index_status *row = &out[i];

        memset(row, 0, sizeof(*row));
        row->name = e->name;
        row->always_on = e->always_on;
        row->target = target_height;

        int64_t cursor = e->get_cursor();
        if (cursor == CATALOG_CURSOR_UNAVAILABLE) {
            row->enabled = false;
            row->cursor = 0;
            row->lag = 0;
            continue;
        }

        row->enabled = true;
        row->cursor = cursor;
        int64_t lag = target_height - cursor;
        row->lag = lag > 0 ? lag : 0;
    }
    return n;
}

int64_t catalog_completeness_worst_lag(const struct catalog_index_status *rows,
                                       size_t n)
{
    if (!rows) {
        LOG_WARN("catalog_completeness", "worst_lag: NULL rows (n=%zu)", n);
        return 0;
    }
    int64_t worst = 0;
    for (size_t i = 0; i < n; i++) {
        if (!rows[i].enabled) continue;
        if (rows[i].lag > worst) worst = rows[i].lag;
    }
    return worst;
}

const struct catalog_index_status *catalog_completeness_worst_over(
    const struct catalog_index_status *rows, size_t n, int64_t threshold)
{
    if (!rows) return NULL;
    const struct catalog_index_status *worst = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!rows[i].enabled) continue;
        if (rows[i].lag <= threshold) continue;
        if (!worst || rows[i].lag > worst->lag) worst = &rows[i];
    }
    return worst;
}

enum catalog_verdict catalog_completeness_verdict(
    const struct catalog_index_status *rows, size_t n,
    int handshaked_peers, int peer_floor,
    int64_t census_age_s, int64_t census_max_age_s,
    char *out, size_t out_cap)
{
    /* BLOCKED dominates: an enabled index lagging at all (threshold 0) is the
     * primary omniscience deficit — a peer/census gap is secondary. */
    const struct catalog_index_status *lag =
        catalog_completeness_worst_over(rows, n, 0);
    if (lag) {
        if (out && out_cap)
            snprintf(out, out_cap, "blocked:%s@%lld",
                     lag->name ? lag->name : "?", (long long)lag->cursor);
        return CATALOG_VERDICT_BLOCKED;
    }
    if (handshaked_peers < peer_floor) {
        if (out && out_cap)
            snprintf(out, out_cap, "degraded:peers");
        return CATALOG_VERDICT_DEGRADED;
    }
    if (census_age_s < 0 || census_age_s > census_max_age_s) {
        if (out && out_cap)
            snprintf(out, out_cap, "degraded:census");
        return CATALOG_VERDICT_DEGRADED;
    }
    if (out && out_cap)
        snprintf(out, out_cap, "omniscient");
    return CATALOG_VERDICT_OMNISCIENT;
}
