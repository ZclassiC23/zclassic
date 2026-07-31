/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* The property catalog projection. See services/property_catalog.h for the
 * contract and the three rules it enforces (projection not truth, read-only
 * including on open, no silent omissions).
 *
 * Layout: context construction, then the per-kind sweep over the adapter
 * registry, then the JSON rendering. The kind list appears nowhere in this
 * file — it is walked through metaverse_adapter_at(), so a kind added to
 * METAVERSE_KIND_TABLE shows up here automatically (as an unavailable row
 * until its adapter is wired) and can never be forgotten. */

#include "services/property_catalog.h"

#include "base/log_macros.h"
#include "base/safe_alloc.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_LOG "services.property_catalog"
#define PC_ZCODE_DIR_MAX 4400

struct property_catalog_page *property_catalog_page_new(void)
{
    struct property_catalog_page *page =
        zcl_calloc(1, sizeof(*page), "property_catalog_page");

    if (!page)
        LOG_NULL(PC_LOG, "catalog page of %zu bytes", sizeof(*page));
    return page;
}

void property_catalog_page_free(struct property_catalog_page *page)
{
    free(page);
}

/* Build the adapter context. The chain anchor (`chain_height`,
 * `chain_work`) is left UNKNOWN — -1 and NULL — and that is a decision, not
 * an omission:
 *
 *   1. No wired kind is proof-of-work settled. CONTENT and ZCODE_PACKAGE
 *      are content-addressed; their roots are checkable without any tip,
 *      and handing them one would only invite a renderer to print it.
 *   2. This projection is datadir-only and node-free by contract (see
 *      services/property_catalog.h). The live tip lives in the in-process
 *      block index, and reading it here would make `metaverse property
 *      list` depend on a running node — a different command from the one
 *      documented.
 *
 * When a ZNAM or ZSLP adapter lands it needs a real tip, and the honest
 * shape is for the CALLER that has one to supply both fields together:
 * `bi->nChainWork` beside `bi->nHeight` from the same block index entry,
 * never a height with the work left NULL and never work recomputed here.
 * Until then, unknown is the true value and metaverse_work_measure() turns
 * it into a stated gap rather than a zero. */
static bool pc_ctx_init(struct metaverse_adapter_ctx *ctx, const char *datadir,
                        char *zcode_dir, size_t zcode_dir_cap)
{
    int n = snprintf(zcode_dir, zcode_dir_cap, "%s/zcode", datadir);

    if (n < 0 || (size_t)n >= zcode_dir_cap)
        return false;
    ctx->datadir      = datadir;
    ctx->zcode_dir    = zcode_dir;
    ctx->chain_height = -1;
    ctx->chain_work   = NULL;
    return true;
}

static void pc_row_begin(struct property_catalog_kind_row *row,
                         const struct metaverse_adapter *adapter)
{
    memset(row, 0, sizeof(*row));
    row->kind             = adapter->kind;
    row->kind_name        = metaverse_kind_name(adapter->kind);
    row->authority_source = metaverse_kind_authority(adapter->kind);
    row->settlement       = metaverse_kind_settlement(adapter->kind);
    row->available        = metaverse_adapter_ready(adapter);
    row->unavailable_reason =
        row->available ? "" : (adapter->unavailable_reason
                                   ? adapter->unavailable_reason
                                   : "adapter row has no reader and no reason");
}

struct zcl_result property_catalog_list(const char *datadir,
                                        const struct property_catalog_query *q,
                                        struct property_catalog_page *out)
{
    struct metaverse_adapter_ctx ctx;
    char zcode_dir[PC_ZCODE_DIR_MAX];
    enum metaverse_kind filter = METAVERSE_KIND_UNKNOWN;
    size_t limit = PROPERTY_CATALOG_PAGE_MAX;
    size_t rows;

    if (!out)
        return ZCL_ERR(-1, "property_catalog_list: NULL out");
    memset(out, 0, sizeof(*out));
    out->store_read = true;
    if (!datadir || !*datadir)
        return ZCL_ERR(-2, "property_catalog_list: datadir is %s",
                       datadir ? "empty" : "NULL");
    if (q) {
        if (q->kind != METAVERSE_KIND_UNKNOWN &&
            !metaverse_kind_valid(q->kind))
            return ZCL_ERR(-3, "property_catalog_list: kind %d is not a "
                               "property kind", (int)q->kind);
        filter = q->kind;
        if (q->limit && q->limit < limit)
            limit = q->limit;
    }
    if (!pc_ctx_init(&ctx, datadir, zcode_dir, sizeof(zcode_dir)))
        return ZCL_ERR(-4, "property_catalog_list: datadir path too long "
                           "(%zu bytes)", strlen(datadir));

    rows = metaverse_adapter_count();
    for (size_t i = 0; i < rows; i++) {
        const struct metaverse_adapter *adapter = metaverse_adapter_at(i);
        struct property_catalog_kind_row *row;
        size_t room;
        size_t total = 0;
        bool truncated = false;

        if (!adapter)
            return ZCL_ERR(-5, "property_catalog_list: adapter row %zu is "
                               "missing — a kind may not drop out of the "
                               "catalog silently", i);
        if (out->kind_count >= METAVERSE_KIND_COUNT)
            return ZCL_ERR(-6, "property_catalog_list: more adapter rows "
                               "than property kinds");
        row = &out->kinds[out->kind_count++];
        pc_row_begin(row, adapter);

        /* A filtered-out kind still gets its row, marked as not scanned:
         * the reader must be able to tell "we did not look" from "there is
         * nothing there". */
        if (filter != METAVERSE_KIND_UNKNOWN && adapter->kind != filter) {
            row->available = false;
            row->unavailable_reason = "not scanned: excluded by the kind "
                                      "filter";
            continue;
        }
        if (!row->available)
            continue;

        /* Ask whether the authority can be READ before asking what it
         * holds. A store that is present and unopenable must not come
         * back through the counting path, because every count there is
         * zero and zero is the same number an empty node reports. */
        if (adapter->store_ready &&
            !adapter->store_ready(&ctx, out->store_reason,
                                  sizeof(out->store_reason))) {
            out->store_read         = false;
            row->available          = false;
            row->unavailable_reason = out->store_reason;
            continue;
        }

        room = limit > out->count ? limit - out->count : 0;
        if (room == 0) {
            /* The page is full but the inventory question is still owed an
             * honest answer, so ask for the total with a zero-width page. */
            (void)adapter->list(&ctx, NULL, 0, &total, &truncated);
            row->total     = total;
            row->written   = 0;
            row->truncated = total > 0;
            out->total_across_kinds += total;
            if (row->truncated)
                out->truncated = true;
            continue;
        }
        row->written = adapter->list(&ctx, &out->items[out->count], room,
                                     &total, &truncated);
        row->total     = total;
        row->truncated = truncated;
        out->count += row->written;
        out->total_across_kinds += total;
        if (truncated)
            out->truncated = true;
    }

    for (size_t i = 0; i < out->kind_count; i++) {
        if (!out->kinds[i].available)
            out->unavailable_kinds++;
    }
    if (out->kind_count != rows)
        return ZCL_ERR(-7, "property_catalog_list: %zu of %zu kinds produced "
                           "a row", out->kind_count, rows);
    return ZCL_OK;
}

struct zcl_result property_catalog_show(const char *datadir,
                                        const struct metaverse_property_id *id,
                                        struct metaverse_property_view *out)
{
    struct metaverse_adapter_ctx ctx;
    char zcode_dir[PC_ZCODE_DIR_MAX];
    const struct metaverse_adapter *adapter;

    if (!out)
        return ZCL_ERR(-1, "property_catalog_show: NULL out");
    memset(out, 0, sizeof(*out));
    if (!datadir || !*datadir)
        return ZCL_ERR(-2, "property_catalog_show: datadir is %s",
                       datadir ? "empty" : "NULL");
    if (!metaverse_property_id_valid(id))
        return ZCL_ERR(-3, "property_catalog_show: property id is not "
                           "well-formed (kind and non-zero root required)");
    adapter = metaverse_adapter_for(id->kind);
    if (!adapter)
        return ZCL_ERR(-4, "property_catalog_show: kind '%s' has no adapter "
                           "row", metaverse_kind_name(id->kind));
    if (!metaverse_adapter_ready(adapter))
        return ZCL_ERR(-5, "property_catalog_show: kind '%s' is not "
                           "projectable: %s", metaverse_kind_name(id->kind),
                       adapter->unavailable_reason
                           ? adapter->unavailable_reason
                           : "no reader wired");
    if (!pc_ctx_init(&ctx, datadir, zcode_dir, sizeof(zcode_dir)))
        return ZCL_ERR(-6, "property_catalog_show: datadir path too long "
                           "(%zu bytes)", strlen(datadir));

    /* An unreadable store must refuse here rather than reach the adapter,
     * whose honest "the authority holds nothing at this root" would be
     * indistinguishable from "we could not open the authority". */
    if (adapter->store_ready) {
        char reason[192];

        if (!adapter->store_ready(&ctx, reason, sizeof(reason)))
            return ZCL_ERR(-9, "property_catalog_show: %s", reason);
    }

    if (!adapter->show(&ctx, id, out))
        return ZCL_ERR(-7, "property_catalog_show: the '%s' adapter refused "
                           "the request", metaverse_kind_name(id->kind));
    /* An adapter that returned true without writing anything would emit a
     * silent all-zero view. Refuse it instead. */
    if (!out->populated)
        return ZCL_ERR(-8, "property_catalog_show: the '%s' adapter wrote no "
                           "view", metaverse_kind_name(id->kind));
    return ZCL_OK;
}

struct zcl_result property_catalog_page_to_json(
    const struct property_catalog_page *page, struct json_value *out)
{
    struct json_value arr;

    if (!page || !out)
        return ZCL_ERR(-1, "property_catalog_page_to_json: NULL %s",
                       page ? "out" : "page");
    json_set_object(out);

    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < page->count; i++) {
        struct json_value row;

        json_init(&row);
        if (metaverse_view_to_json(&page->items[i], &row))
            (void)json_push_back(&arr, &row);
        json_free(&row);
    }
    (void)json_push_kv(out, "properties", &arr);
    json_free(&arr);

    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < page->kind_count; i++) {
        const struct property_catalog_kind_row *k = &page->kinds[i];
        struct json_value row;

        json_init(&row);
        json_set_object(&row);
        (void)json_push_kv_str(&row, "kind", k->kind_name);
        (void)json_push_kv_str(&row, "authority_source",
                               k->authority_source);
        /* Beside the authority, what kind of answer it gives. A kind whose
         * reader is not wired still states this — the class is a property
         * of the mechanism, knowable without reading a single record. */
        (void)json_push_kv_str(&row, "settlement",
                               metaverse_settlement_name(k->settlement));
        (void)json_push_kv_str(&row, "settlement_means",
                               metaverse_settlement_means(k->settlement));
        (void)json_push_kv_bool(&row, "available", k->available);
        (void)json_push_kv_str(&row, "unavailable_reason",
                               k->unavailable_reason ? k->unavailable_reason
                                                     : "");
        (void)json_push_kv_int(&row, "total", (int64_t)k->total);
        (void)json_push_kv_int(&row, "rendered", (int64_t)k->written);
        (void)json_push_kv_bool(&row, "items_truncated", k->truncated);
        (void)json_push_back(&arr, &row);
        json_free(&row);
    }
    (void)json_push_kv(out, "kinds", &arr);
    json_free(&arr);

    (void)json_push_kv_int(out, "rendered", (int64_t)page->count);
    (void)json_push_kv_int(out, "total", (int64_t)page->total_across_kinds);
    (void)json_push_kv_bool(out, "items_truncated", page->truncated);
    (void)json_push_kv_int(out, "kinds_scanned", (int64_t)page->kind_count);
    (void)json_push_kv_int(out, "kinds_unavailable",
                           (int64_t)page->unavailable_kinds);

    /* The disclosure section. "read": false says the emptiness above is a
     * failure to look, not an inventory — a distinction an operator
     * cannot recover from the item array alone. */
    {
        struct json_value store;

        json_init(&store);
        json_set_object(&store);
        (void)json_push_kv_bool(&store, "read", page->store_read);
        (void)json_push_kv_str(&store, "reason",
                               page->store_read ? "" : page->store_reason);
        (void)json_push_kv(out, "store", &store);
        json_free(&store);
    }
    return ZCL_OK;
}
