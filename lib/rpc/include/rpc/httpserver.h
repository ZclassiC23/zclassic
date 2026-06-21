/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_HTTPSERVER_H
#define ZCL_RPC_HTTPSERVER_H

#include "rpc/server.h"
#include "json/json.h"
#include <stdbool.h>
#include <stdint.h>

bool rpc_http_start(const struct rpc_table *table, uint16_t port,
                     const char *rpc_user, const char *rpc_password,
                     const char *datadir);
void rpc_http_stop(void);
bool rpc_http_is_running(void);

/* Return the live RPC dispatch table the HTTP server is executing against
 * (the exact pointer passed to rpc_http_start). NULL before the server
 * starts or after it stops. The in-process MCP transport calls
 * rpc_table_execute() on this same table so its results are byte-identical
 * to the out-of-process HTTP path. Read-only; do not mutate. */
const struct rpc_table *rpc_http_active_table(void);
bool rpc_http_tls_active(void);

/* Cookie rotation — call manually for testing; background thread calls
 * automatically every ZCL_RPC_COOKIE_ROTATE_SEC seconds (default 24h). */
void rpc_http_cookie_rotate(void);
int  rpc_http_cookie_rotate_sec(void);

/* test surface: builds the standard JSON-RPC response envelope
 * used by the HTTP server. Safe to call on stack-dirtied / previously
 * uninitialized `response` storage. Production code also routes through
 * this helper to avoid reintroducing stack-init regressions in the HTTP
 * response path. */
bool rpc_http_test_build_response_envelope(bool rpc_ok,
                                           const char *method,
                                           struct json_value *rpc_result,
                                           const struct json_value *id,
                                           struct json_value *response);

/* test surface: two-pass serialization of an RPC response. Sizes the
 * body with a zero-length json_write probe, rejects anything past the
 * internal cap, then allocates exactly len+1 and writes the body — so
 * the length sent to write() can never exceed the allocation (the heap
 * OOB-read fix). Production code routes through this same helper.
 *
 * On true: *out_buf owns a heap buffer the caller must free() and
 * *out_len is the exact body length. On false (OOM / over cap):
 * *out_buf == NULL and *out_len == 0. */
bool rpc_http_test_serialize_response(const struct json_value *response,
                                      char **out_buf, size_t *out_len);

#endif
