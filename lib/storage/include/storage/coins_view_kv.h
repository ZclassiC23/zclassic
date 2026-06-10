/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_view_kv — a read-only `struct coins_view` backed by **coins_kv**
 * (the canonical UTXO set living IN progress.kv) instead of the UTXO
 * projection or coins.db.
 *
 * This is the production miss-resolution source for the `g_coins_tip`
 * read cache. It supersedes coins_view_projection on the live read path:
 * the reducer authors the coins set into coins_kv INSIDE the same
 * stage_run_once BEGIN IMMEDIATE as the stage cursor + inverse-delta +
 * log row (utxo_apply_stage.c), so a read through this view sees the
 * in-txn-committed canonical set, and a kill-9 leaves the coins set
 * atomically consistent with the cursor. The projection's separate WAL
 * database lacked that atomic cross-commit (the entire tip-wedge tear
 * class — docs/work/tip-durability-collapse.md), which is why the live
 * read view moves here.
 *
 * The view holds NO database handle of its own: every vtable thunk fetches
 * the process-global progress.kv handle lazily via progress_store_db() (a
 * pointer load), so it binds to whatever handle is live at read time and
 * needs no per-call open. The view is read-only: the stage authors UTXO
 * state via coins_kv writes in the apply txn, so `batch_write` here is a
 * programming error (a second writer).
 *
 * Mirrors coins_view_projection's vtable-embedding pattern exactly; parity
 * with it is proven by test_coins_view_kv. */

#ifndef ZCL_STORAGE_COINS_VIEW_KV_H
#define ZCL_STORAGE_COINS_VIEW_KV_H

#include "coins/coins_view.h"

#include <stdbool.h>

struct coins_view_kv {
    struct coins_view view;        /* vtable-based polymorphism (must be first) */
};

/* Bind `ckv` and publish its vtable. No db/proj argument: each read fetches
 * progress_store_db() lazily, so the caller need only ensure progress_store
 * is open before the first read. Returns false on a NULL arg. */
bool coins_view_kv_init(struct coins_view_kv *ckv);

#endif /* ZCL_STORAGE_COINS_VIEW_KV_H */
