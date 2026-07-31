/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * property_adapter — the one interface a property kind implements so the
 * catalog can project it. One row per kind, dispatched centrally, exactly
 * the shape app/services/vault_read.c uses for asset classes and
 * app/controllers/diagnostics_dumpers.def uses for state dumpers.
 *
 * THE CONTRACT, and the whole reason this file is small: an adapter is a
 * READER. It calls the authoritative subsystem's existing read API and
 * fills a view. It may not write, may not open a handle that mutates the
 * datadir, may not cache between calls, and may not hold state of its own.
 * There is no put/update/transfer hook here on purpose — mutation belongs
 * to the capability engine and to the authority that already owns it.
 *
 * READ MEANS READ. Several node stores mutate on OPEN (a recovery sweep, a
 * staging commit, an orphan GC). An adapter must reach the same bytes by a
 * path that does not, because `metaverse property list` is a read command
 * and a read command that rewrites the operator's datadir is the defect
 * this project has already been bitten by. Adapters therefore take a
 * directory, not a store handle.
 *
 * A kind with no reader yet is still a ROW, carrying
 * `unavailable_reason`. The catalog emits it as an explicit unavailable
 * entry rather than dropping it — a kind that silently vanished from the
 * catalog is indistinguishable from a kind that owns nothing.
 */

#ifndef ZCL_METAVERSE_PROPERTY_ADAPTER_H
#define ZCL_METAVERSE_PROPERTY_ADAPTER_H

#include "metaverse/property_id.h"
#include "metaverse/property_view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Everything an adapter is allowed to know about where to read from.
 * Deliberately a directory + a height, never an open store handle: see
 * "READ MEANS READ" above. */
struct metaverse_adapter_ctx {
    const char *datadir;   /* datadir root; never NULL */
    const char *zcode_dir; /* "<datadir>/zcode"; never NULL */

    /* Chain height for chain-anchored kinds, or -1 when unknown. An
     * adapter over a non-chain authority MUST ignore this: stamping a tip
     * height onto a claim the tip does not commit is a false freshness
     * claim. */
    int64_t chain_height;
};

/* Fill `out` for exactly this id. Returns true when a view was written
 * (determined OR an honest gap — both are answers); false only when the
 * adapter could not even begin (NULL args, wrong kind). A view for an id
 * the authority does not hold is status = ABSENT and determined = true. */
typedef bool (*metaverse_adapter_show_fn)(
    const struct metaverse_adapter_ctx *ctx,
    const struct metaverse_property_id *id,
    struct metaverse_property_view *out);

/* Enumerate this kind's properties into out[0..out_cap). Returns the rows
 * written. *total_out (when non-NULL) receives the TOTAL this kind holds,
 * which may exceed the rows written; *truncated_out (when non-NULL) is set
 * when it does — a short page must never read as a short inventory.
 *
 * out_cap == 0 is a legal COUNT-ONLY call: no view is written, and
 * *total_out is still the real total. The catalog uses it once its page is
 * full, because "the page filled up" must not silently become "the node
 * owns exactly this many". */
typedef size_t (*metaverse_adapter_list_fn)(
    const struct metaverse_adapter_ctx *ctx,
    struct metaverse_property_view *out, size_t out_cap,
    size_t *total_out, bool *truncated_out);

/* Is this kind's authority READABLE from `ctx` right now, as opposed to
 * merely holding nothing? Optional; NULL means the question cannot arise
 * for this kind.
 *
 * It exists because `list` returns a count and `show` returns "absent",
 * and both of those answers are indistinguishable from "the store is
 * there and could not be opened". An operator told "you own nothing" over
 * a store that threw EACCES has been misinformed, and this project has
 * already paid for exactly that conflation once, on node.db. A row that
 * reads anything unlockable declares this hook; the catalog calls it
 * BEFORE list/show and turns a false into a stated unavailability, never
 * into an empty result. `reason` (never NULL, `reason_cap` > 0) receives
 * the operator-facing explanation on false. */
typedef bool (*metaverse_adapter_ready_fn)(
    const struct metaverse_adapter_ctx *ctx, char *reason,
    size_t reason_cap);

struct metaverse_adapter {
    enum metaverse_kind kind;
    /* NULL when this kind has a reader. Non-NULL names precisely why it
     * does not yet, and `list`/`show` are then NULL. */
    const char *unavailable_reason;
    metaverse_adapter_list_fn list;
    metaverse_adapter_show_fn show;
    /* Optional; see metaverse_adapter_ready_fn. */
    metaverse_adapter_ready_fn store_ready;
};

/* The registry. Exactly one row per kind in METAVERSE_KIND_TABLE order;
 * static_assert'd against METAVERSE_KIND_COUNT in adapter_registry.c. */
size_t metaverse_adapter_count(void);
const struct metaverse_adapter *metaverse_adapter_at(size_t i);
/* NULL only for an invalid kind — a valid kind always has a row, even if
 * that row is an unavailable one. */
const struct metaverse_adapter *metaverse_adapter_for(enum metaverse_kind k);

/* True when the row has a reader wired. */
bool metaverse_adapter_ready(const struct metaverse_adapter *adapter);

#endif /* ZCL_METAVERSE_PROPERTY_ADAPTER_H */
