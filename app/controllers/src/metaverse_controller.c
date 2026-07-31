/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the registry-owned `metaverse` tree — the read half:
 * `metaverse property list` and `metaverse property show`.
 *
 * THE INVARIANT OF THIS FILE: it holds no ownership logic and writes
 * nothing. Every field it renders comes from ONE call into the property
 * catalog projection (services/property_catalog.h), which in turn asks each
 * property kind's own authoritative model. There is no catalog table to
 * fall out of date with the chain, the wallet, or the package store,
 * because the projection keeps nothing between calls.
 *
 * These are READ leaves in the strict sense: the catalog reaches store
 * bytes by path and never opens a handle whose open() mutates the datadir.
 * A read command that rewrites the operator's datadir is a defect this
 * project has already paid for.
 *
 * `datadir` resolution follows the zcode precedent: explicit input.datadir
 * wins, else the CLI's --datadir, else a named MISSING_DATADIR refusal —
 * this surface never silently falls back to a global.
 *
 * Bound by config/commands/metaverse.def. */

#include "kernel/command_registry.h"
#include "command/native_command.h"

#include "json/json.h"
#include "metaverse/property_id.h"
#include "metaverse/property_view.h"
#include "services/property_catalog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MV_CMD_ITEM_CAP_DEFAULT 16u

static const char *mv_input_str(const struct json_value *input,
                                const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

/* Explicit input.datadir wins, else the CLI's --datadir; NULL when neither
 * is set (the zcode.package.show precedent). */
static const char *mv_datadir(const struct zcl_command_request *request)
{
    const char *dd = mv_input_str(request->input, "datadir");

    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

static bool mv_require_datadir(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply,
                               const char *leaf, const char **out)
{
    *out = mv_datadir(request);
    if (*out)
        return true;
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                           "normalize", false, false,
                           "no datadir given (input datadir or --datadir)",
                           leaf);
    return false;
}

void zcl_native_handle_metaverse_property_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *datadir = NULL;
    const char *kind_name;
    struct property_catalog_query q;
    struct property_catalog_page *page;
    struct zcl_result r;
    const struct json_value *limit_v;

    if (!request || !reply)
        return;
    if (!mv_require_datadir(request, reply, "metaverse.property.list",
                            &datadir))
        return;

    memset(&q, 0, sizeof(q));
    q.limit = MV_CMD_ITEM_CAP_DEFAULT;
    limit_v = json_get(request->input, "limit");
    if (limit_v) {
        int64_t want = json_get_int(limit_v);

        if (want > 0)
            q.limit = (size_t)want;
    }
    kind_name = mv_input_str(request->input, "kind");
    if (kind_name && kind_name[0]) {
        q.kind = metaverse_kind_from_name(kind_name);
        if (!metaverse_kind_valid(q.kind)) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "UNKNOWN_KIND",
                                   "normalize", false, false,
                                   "kind must be one of the property kinds "
                                   "the catalog enumerates",
                                   kind_name);
            return;
        }
    }

    page = property_catalog_page_new();
    if (!page) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ALLOC", "execute",
                               false, false,
                               "the catalog page could not be allocated",
                               "metaverse.property.list");
        return;
    }
    r = property_catalog_list(datadir, &q, page);
    if (!r.ok) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "CATALOG_FAILED",
                               "execute", false, false,
                               "the property catalog projection failed",
                               r.message);
        property_catalog_page_free(page);
        return;
    }
    r = property_catalog_page_to_json(page, &reply->data);
    property_catalog_page_free(page);
    if (!r.ok) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "RENDER_FAILED",
                               "render", false, false,
                               "the catalog page could not be rendered",
                               r.message);
        return;
    }
    (void)json_push_kv_str(&reply->data, "datadir", datadir);
}

void zcl_native_handle_metaverse_property_show(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *datadir = NULL;
    const char *id_text;
    struct metaverse_property_id id;
    struct metaverse_property_view view;
    struct zcl_result r;

    if (!request || !reply)
        return;
    if (!mv_require_datadir(request, reply, "metaverse.property.show",
                            &datadir))
        return;

    id_text = mv_input_str(request->input, "property_id");
    if (!metaverse_property_id_parse(id_text, &id)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_PROPERTY_ID",
                               "normalize", false, false,
                               "property_id must be "
                               "'<kind>:<64-hex immutable root>'",
                               id_text ? id_text : "");
        return;
    }
    r = property_catalog_show(datadir, &id, &view);
    if (!r.ok) {
        /* A kind with no reader wired is a distinct, named refusal — never
         * an empty result that reads as "this node owns nothing". */
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "KIND_UNAVAILABLE",
                               "execute", false, false,
                               "this property kind cannot be projected from "
                               "this datadir",
                               r.message);
        return;
    }
    if (!metaverse_view_to_json(&view, &reply->data)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "RENDER_FAILED",
                               "render", false, false,
                               "the property view could not be rendered",
                               view.id_text);
        return;
    }
    (void)json_push_kv_str(&reply->data, "datadir", datadir);
}
