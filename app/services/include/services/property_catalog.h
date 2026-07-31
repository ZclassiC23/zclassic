/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SERVICES_PROPERTY_CATALOG_H
#define ZCL_SERVICES_PROPERTY_CATALOG_H

#include "base/result.h"
#include "metaverse/property_adapter.h"
#include "metaverse/property_id.h"
#include "metaverse/property_view.h"

#include <stdbool.h>
#include <stddef.h>

struct json_value;

/* The property catalog — one surface that states what sovereign digital
 * property this node's datadir holds, across every property kind, by asking
 * each kind's authoritative model.
 *
 * IT IS A PROJECTION, NOT A SOURCE OF TRUTH. There is no catalog table, no
 * catalog cache, and no catalog copy of an owner field. Every call rebuilds
 * every view from the authoritative bytes and throws the result away. That
 * is the whole design: a cached ownership fact is a second ownership truth,
 * and the recurring bug in this codebase is cloned ledgers. If the
 * underlying object changes or disappears, the next read reflects it,
 * because there is nothing here that could disagree.
 *
 * IT IS READ-ONLY, INCLUDING ON OPEN. The catalog reaches store bytes by
 * path and never opens a handle whose open() mutates the datadir (the ZCODE
 * package store's open runs a recovery sweep). `metaverse property list` is
 * a read command, and a read command must not rewrite the operator's
 * datadir.
 *
 * NO SILENT OMISSIONS. Every property kind produces a row in
 * `kinds[]` — including a kind whose reader is not wired, which reports
 * available = false plus the reason. A kind that vanished from the output
 * would be indistinguishable from a kind that owns nothing. */

/* Views rendered per call. A page is ~25 KB, so it is heap-allocated
 * through property_catalog_page_new() rather than placed on a stack. */
#define PROPERTY_CATALOG_PAGE_MAX 32u

/* Per-kind coverage of one list call. `total` is what the kind actually
 * holds; `written` is what fit. */
struct property_catalog_kind_row {
    enum metaverse_kind kind;
    const char *kind_name;
    const char *authority_source;
    bool available;              /* false => `unavailable_reason` says why */
    const char *unavailable_reason;
    size_t total;
    size_t written;
    bool truncated;
};

struct property_catalog_page {
    struct metaverse_property_view items[PROPERTY_CATALOG_PAGE_MAX];
    size_t count;

    /* One row per kind, always fully populated. */
    struct property_catalog_kind_row kinds[METAVERSE_KIND_COUNT];
    size_t kind_count;

    size_t total_across_kinds;   /* sum of every row's `total` */
    bool truncated;              /* any row truncated, or the page filled */
    size_t unavailable_kinds;    /* rows with available == false */

    /* False when a store under the datadir is PRESENT and unreadable, as
     * opposed to absent. An empty page then means "we could not look",
     * not "this node owns nothing", and `store_reason` says which store
     * and what the OS said. Rendered as the top-level "store" section so
     * a caller cannot miss it by reading only the item array. Any row
     * turned unavailable by this points its `unavailable_reason` at
     * `store_reason`, which lives as long as the page. */
    bool store_read;
    char store_reason[192];
};

struct property_catalog_query {
    /* METAVERSE_KIND_UNKNOWN lists every kind; a valid kind narrows to it. */
    enum metaverse_kind kind;
    /* 0 means PROPERTY_CATALOG_PAGE_MAX; larger values are clamped to it. */
    size_t limit;
};

struct property_catalog_page *property_catalog_page_new(void);
void property_catalog_page_free(struct property_catalog_page *page);

/* Project every (or one) kind's properties from `datadir`. Non-ok only for
 * a NULL/empty datadir, a NULL out, an invalid `kind` filter, or a kind
 * that produced no row at all — an unreadable kind is a populated row with
 * available = false, which is an answer, not an error. */
struct zcl_result property_catalog_list(const char *datadir,
                                        const struct property_catalog_query *q,
                                        struct property_catalog_page *out);

/* Project exactly one property. Non-ok for a NULL/empty datadir, a NULL
 * out, an invalid id, or a kind whose reader is not wired (the reason is in
 * the error message). An id the authority does not hold is ok with
 * status = absent — "not found" is a fact, not a failure. */
struct zcl_result property_catalog_show(const char *datadir,
                                        const struct metaverse_property_id *id,
                                        struct metaverse_property_view *out);

/* Render a page (items + the per-kind coverage rows) into `out`, set to an
 * object by this call. */
struct zcl_result property_catalog_page_to_json(
    const struct property_catalog_page *page, struct json_value *out);

#endif /* ZCL_SERVICES_PROPERTY_CATALOG_H */
