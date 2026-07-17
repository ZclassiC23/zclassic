/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Safe JSON-RPC params builder.
 *
 * Hand-rolled snprintf(payload, ..., "[\"%s\"]", user_input) is vulnerable
 * to JSON injection when user_input contains `"`, `\`, or control chars:
 * a caller can punch through the string context and rewrite the params
 * array.  All MCP controller handlers must build RPC param arrays through
 * this builder — it routes every string through the project's JSON
 * encoder, which escapes the dangerous characters.
 *
 * Usage (flat case):
 *   struct mcp_params p;
 *   mcp_params_init(&p);
 *   mcp_params_push_str(&p, untrusted_addr);
 *   mcp_params_push_real(&p, amount);
 *   char *params = mcp_params_to_json(&p);   // consumes p
 *   char *out = mcp_node_rpc("sendtoaddress", params);
 *   free(params);
 *
 * Usage (nested case — z_sendmany's [from, [{address, amount}]]):
 *   struct mcp_params p;
 *   mcp_params_init(&p);
 *   mcp_params_push_str(&p, from);
 *
 *   struct json_value recip; json_init(&recip); json_set_object(&recip);
 *   json_push_kv_str(&recip,  "address", to);
 *   json_push_kv_real(&recip, "amount",  amount);
 *   struct json_value recip_arr; json_init(&recip_arr);
 *   json_set_array(&recip_arr); json_push_back(&recip_arr, &recip);
 *   mcp_params_push_value(&p, &recip_arr);
 *   json_free(&recip); json_free(&recip_arr);
 *
 *   char *params = mcp_params_to_json(&p);
 */

#ifndef ZCL_MCP_RPC_PARAMS_H
#define ZCL_MCP_RPC_PARAMS_H

#include "json/json.h"

#include <stdbool.h>
#include <stdint.h>

struct mcp_params {
    struct json_value arr;  /* JSON_ARR — do not touch directly */
};

void mcp_params_init(struct mcp_params *p);
void mcp_params_free(struct mcp_params *p);

/* Append a typed value. NULL strings are treated as the empty string. */
void mcp_params_push_str(struct mcp_params *p, const char *s);
void mcp_params_push_int(struct mcp_params *p, int64_t i);
void mcp_params_push_real(struct mcp_params *p, double d);
void mcp_params_push_bool(struct mcp_params *p, bool b);

/* Append a pre-built json_value (object, array, nested — anything).
 * Copies v; caller retains ownership and must json_free() it. */
void mcp_params_push_value(struct mcp_params *p, const struct json_value *v);

/* Serialize to a malloc'd, NUL-terminated JSON array string ready to
 * pass as the params_json argument to mcp_node_rpc(). Also frees the
 * builder's internal storage (equivalent to mcp_params_free(p)).
 * Returns NULL on allocation failure. */
char *mcp_params_to_json(struct mcp_params *p);

#endif
