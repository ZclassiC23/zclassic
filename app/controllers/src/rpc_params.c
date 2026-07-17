/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * JSON-RPC parameter builder: accumulates positional args into a JSON array
 * for the node RPC client. See rpc_params.h for the public API. */

#include "controllers/rpc_params.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdlib.h>

void mcp_params_init(struct mcp_params *p)
{
    json_init(&p->arr);
    json_set_array(&p->arr);
}

void mcp_params_free(struct mcp_params *p)
{
    json_free(&p->arr);
}

void mcp_params_push_str(struct mcp_params *p, const char *s)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, s ? s : "");
    json_push_back(&p->arr, &v);
    json_free(&v);
}

void mcp_params_push_int(struct mcp_params *p, int64_t i)
{
    struct json_value v;
    json_init(&v);
    json_set_int(&v, i);
    json_push_back(&p->arr, &v);
    json_free(&v);
}

void mcp_params_push_real(struct mcp_params *p, double d)
{
    struct json_value v;
    json_init(&v);
    json_set_real(&v, d);
    json_push_back(&p->arr, &v);
    json_free(&v);
}

void mcp_params_push_bool(struct mcp_params *p, bool b)
{
    struct json_value v;
    json_init(&v);
    json_set_bool(&v, b);
    json_push_back(&p->arr, &v);
    json_free(&v);
}

void mcp_params_push_value(struct mcp_params *p, const struct json_value *v)
{
    json_push_back(&p->arr, v);
}

char *mcp_params_to_json(struct mcp_params *p)
{
    /* Two-pass: size then allocate. json_write is safe with NULL/0 —
     * every write is guarded by a bounds check, so it just counts. */
    size_t need = json_write(&p->arr, NULL, 0);
    char *buf = zcl_malloc(need + 1, "mcp_params json");
    if (!buf) {
        mcp_params_free(p);
        LOG_NULL("mcp.params", "malloc failed for %zu byte json array", need + 1);
    }
    size_t wrote = json_write(&p->arr, buf, need + 1);
    buf[wrote] = '\0';
    mcp_params_free(p);
    return buf;
}
