/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The adapter registry: exactly one row per property kind, in
 * METAVERSE_KIND_TABLE order, dispatched centrally.
 *
 * A kind with no reader yet is a row carrying `unavailable_reason`, never a
 * missing row. The static_assert below makes that mechanical: the table
 * must hold METAVERSE_KIND_COUNT - 1 rows (every kind except UNKNOWN), so
 * adding a kind to the vocabulary without deciding what happens when
 * someone asks for it is a compile error, not a silently absent line of
 * catalog output.
 *
 * The `unavailable_reason` strings are the honest current state of each
 * unwired kind. They name what is missing, not "TODO" — a caller reading
 * the catalog learns why a kind cannot be projected and where the read API
 * would have to come from. */

#include "metaverse_priv.h"

#include "metaverse/property_adapter.h"

#include <stddef.h>

static const struct metaverse_adapter k_unwired[] = {
    { .kind = METAVERSE_KIND_ZNAM_NAME,
      .unavailable_reason =
          "no read-only ZNAM registration projection is wired: the name "
          "records live behind db_znam_* (node.db), which this read path "
          "may not open",
      .list = NULL, .show = NULL },
    { .kind = METAVERSE_KIND_ZSLP_ASSET,
      .unavailable_reason =
          "no read-only ZSLP genesis projection is wired: the ledger lives "
          "behind db_zslp_ledger_* (node.db), which this read path may not "
          "open",
      .list = NULL, .show = NULL },
    { .kind = METAVERSE_KIND_HOSTED_SERVICE,
      .unavailable_reason =
          "hosted services are enumerated by the live diagnostics registry, "
          "which requires a running node; no datadir-only read exists",
      .list = NULL, .show = NULL },
    { .kind = METAVERSE_KIND_ENDPOINT_ONION,
      .unavailable_reason =
          "onion endpoints are runtime state of the embedded Tor service; no "
          "datadir-only read exists",
      .list = NULL, .show = NULL },
    { .kind = METAVERSE_KIND_STOREFRONT_PRODUCT,
      .unavailable_reason =
          "storefront products live in db_store_product (node.db), which "
          "this read path may not open",
      .list = NULL, .show = NULL },
    { .kind = METAVERSE_KIND_CONTRACT_SWAP,
      .unavailable_reason =
          "swap contracts live in db_swap_contract (node.db), which this "
          "read path may not open",
      .list = NULL, .show = NULL },
};

enum {
    MV_UNWIRED_COUNT = (int)(sizeof(k_unwired) / sizeof(k_unwired[0])),
    /* The two kinds with readers: CONTENT and ZCODE_PACKAGE. */
    MV_WIRED_COUNT = 2,
};

_Static_assert(MV_WIRED_COUNT + MV_UNWIRED_COUNT ==
                   (int)METAVERSE_KIND_COUNT - 1,
               "every kind in METAVERSE_KIND_TABLE needs exactly one adapter "
               "row (wired or explicitly unavailable): a kind with no row "
               "would drop out of the catalog silently");

size_t metaverse_adapter_count(void)
{
    /* Every kind except UNKNOWN has a row. */
    return (size_t)METAVERSE_KIND_COUNT - 1u;
}

/* Resolved per call from immutable data: no mutable registry state, so this
 * is reentrant and safe to call from any thread without a lock. */
const struct metaverse_adapter *metaverse_adapter_for(enum metaverse_kind k)
{
    if (!metaverse_kind_valid(k))
        return NULL;
    switch (k) {
    case METAVERSE_KIND_CONTENT:
        return metaverse_adapter_content();
    case METAVERSE_KIND_ZCODE_PACKAGE:
        return metaverse_adapter_zcode_package();
    default:
        break;
    }
    for (int i = 0; i < MV_UNWIRED_COUNT; i++) {
        if (k_unwired[i].kind == k)
            return &k_unwired[i];
    }
    return NULL;
}

const struct metaverse_adapter *metaverse_adapter_at(size_t i)
{
    if (i >= metaverse_adapter_count())
        return NULL;
    /* Row i is kind i+1: UNKNOWN owns no row. */
    return metaverse_adapter_for((enum metaverse_kind)(i + 1u));
}

bool metaverse_adapter_ready(const struct metaverse_adapter *adapter)
{
    return adapter && !adapter->unavailable_reason && adapter->list &&
           adapter->show;
}
