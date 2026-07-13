/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral handler-body contract.
 *
 * A *body function* is the re-homed composition of one operator read tool:
 * it takes the tool's argument object and returns one heap-allocated JSON
 * body (caller frees), exactly the bytes the legacy MCP handler used to set
 * as res->body. Both remaining transports call the same body function:
 *
 *   - the MCP controller handler (tools/mcp/controllers/) is a thin wrapper
 *     that maps a failure onto its historical MCP error code + message, so
 *     the MCP surface stays byte-identical (dual-run through W2);
 *   - the native command bridge (tools/command/native_command.c) calls the
 *     body function directly and wraps the body in the zcl.result.v1
 *     envelope — the MCP router/middleware is never entered.
 *
 * Contract:
 *   - success: return the body; err->status = ZCL_NATIVE_BODY_OK. A body
 *     that itself carries an RPC-level {"error":...} object is still a
 *     SUCCESS return (forwarded verbatim, exactly like the legacy
 *     mcp_return_rpc_body tail) — the caller decides how to surface it.
 *   - failure: return NULL; set err->status + err->message. The message
 *     text must be byte-identical to the legacy handler's
 *     res->error_message so the MCP wrapper reproduces today's envelope.
 *     All LOG_* calls stay inside the body function (same tag, same text).
 */

#ifndef ZCL_CONTROLLERS_NATIVE_HANDLER_BODY_H
#define ZCL_CONTROLLERS_NATIVE_HANDLER_BODY_H

#include "json/json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum zcl_native_body_status {
    ZCL_NATIVE_BODY_OK = 0,
    /* Caller-supplied input failed validation before any side effect. */
    ZCL_NATIVE_BODY_INVALID = 1,
    /* An upstream RPC/service returned nothing (legacy handlers surfaced
     * this as MCP_ERR_HANDLER_FAILED, "RPC <method> returned null"). */
    ZCL_NATIVE_BODY_UNAVAILABLE = 2,
    /* Allocation or serialization failed (legacy MCP_ERR_INTERNAL). */
    ZCL_NATIVE_BODY_INTERNAL = 3,
};

struct zcl_native_body_err {
    enum zcl_native_body_status status;
    /* Byte-identical to the legacy handler's res->error_message text. */
    char message[192];
};

typedef char *(*zcl_native_body_fn)(const struct json_value *args,
                                    struct zcl_native_body_err *err);

/* ── Defaulted JSON accessors ─────────────────────────────────────
 * Shared by the body functions and the MCP controllers (controllers.h
 * includes this header rather than defining its own copies). */
static inline int64_t
json_get_int_or(const struct json_value *obj, const char *key, int64_t dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_int(v) : dflt;
}

static inline bool
json_get_bool_or(const struct json_value *obj, const char *key, bool dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_bool(v) : dflt;
}

static inline const char *
json_get_str_or(const struct json_value *obj, const char *key, const char *dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_str(v) : dflt;
}

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_NATIVE_HANDLER_BODY_H */
