/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_root_ladder_verify — the live tripwire half of the golden-height
 * UTXO root ladder (lib/chain/include/chain/utxo_root_ladder.h /
 * tools/gen_utxo_root_ladder.c). The generated table is pure data with no
 * storage dependency (lib/chain never includes lib/storage — see the
 * LIB_MODULES layering in the top-level Makefile); this hand-written
 * companion lives in app/models because it is the one piece that reads
 * THIS node's own coins_kv boundary-root store
 * (coins_kv_boundary_root_get, lib/storage/src/coins_kv.c:571) and compares
 * it against the locked ladder.
 *
 * The comparison is a pure read — no AR_BEGIN_SAVE/AR_FINISH_SAVE lifecycle
 * applies (nothing is written), mirroring app/models/src/mmb_leaf_store.c's
 * read-side helpers.
 */

#ifndef ZCL_MODELS_UTXO_ROOT_LADDER_VERIFY_H
#define ZCL_MODELS_UTXO_ROOT_LADDER_VERIFY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct sqlite3;
struct mmb_leaf_store;

enum utxo_root_ladder_verify_status {
    /* This node's own boundary-root store has no entry at the rung's
     * height yet (the live fold hasn't passed it under non-deferred
     * conditions — see tip_finalize_post_step.c's ibd_defer gate). Not a
     * failure: most nodes will show this for most rungs most of the time. */
    UTXO_ROOT_LADDER_VERIFY_NOT_YET_REACHED = 0,
    /* This node's boundary root at the rung's height matches the locked
     * ladder value bit-for-bit. */
    UTXO_ROOT_LADDER_VERIFY_MATCH = 1,
    /* This node's boundary root at the rung's height DIFFERS from the
     * locked ladder value — the detective's "state-wrong coin detected"
     * tripwire. Named at an exact height; never a silent stop. */
    UTXO_ROOT_LADDER_VERIFY_DIVERGENT = 2,
};

struct utxo_root_ladder_verify_result {
    int32_t height;
    enum utxo_root_ladder_verify_status status;
};

/* Compares every rung in g_utxo_root_ladder against `db`'s OWN
 * coins_kv_boundary_root_get() at that height. Writes up to `out_cap`
 * per-rung results into `out_results` (in ladder order) and the number
 * written into `*out_count`. `out_results`/`out_count` may be NULL if the
 * caller only wants the aggregate verdict.
 *
 * Returns false iff at least one rung is DIVERGENT (a real mismatch —
 * refuse to treat this as healthy). NOT_YET_REACHED rungs never cause a
 * false return; they are normal, not an error. Returns false (and logs)
 * on a NULL `db`. */
bool utxo_root_ladder_verify_against_store(
        struct sqlite3 *db,
        struct utxo_root_ladder_verify_result *out_results, size_t out_cap,
        size_t *out_count);

/* Dense-layer companion: recomputes mmb_root() from `store`'s own leaf
 * hashes up to and including g_utxo_root_ladder_dense_height (inclusive)
 * and compares it to the locked g_utxo_root_ladder_dense_mmb_root.
 *
 * Returns true when the dense anchor is absent (height==-1 — nothing to
 * check) OR `store` doesn't yet have enough leaves to cover it (not yet
 * reached — same "not a failure" doctrine as the per-rung check above).
 * Returns false ONLY on a genuine byte mismatch (writes the recomputed
 * root into `out_mismatch_root` when non-NULL) — the same tripwire
 * doctrine as utxo_root_ladder_verify_against_store(). */
bool utxo_root_ladder_verify_dense_anchor(struct mmb_leaf_store *store,
                                          uint8_t out_mismatch_root[32]);

#endif /* ZCL_MODELS_UTXO_ROOT_LADDER_VERIFY_H */
