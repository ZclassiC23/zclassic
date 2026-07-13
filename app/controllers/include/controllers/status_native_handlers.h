/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral operator read compositions.
 *
 * Body functions for the operator read tools: each takes the tool's
 * argument object and returns one heap-allocated JSON body (caller frees),
 * exactly the bytes the legacy MCP handler set as res->body. On failure it
 * returns NULL and fills struct zcl_native_body_err (see
 * controllers/native_handler_body.h) with the legacy MCP error tier and the
 * byte-identical error_message text, having already logged the failure.
 * Both the MCP wrapper handlers and the native command bridge call these.
 */

#ifndef ZCL_CONTROLLERS_STATUS_NATIVE_HANDLERS_H
#define ZCL_CONTROLLERS_STATUS_NATIVE_HANDLERS_H

#include "controllers/native_handler_body.h"

#ifdef __cplusplus
extern "C" {
#endif

char *zcl_native_status_body(const struct json_value *args,
                             struct zcl_native_body_err *err);
char *zcl_native_status_brief_body(const struct json_value *args,
                                   struct zcl_native_body_err *err);
char *zcl_native_kpi_body(const struct json_value *args,
                          struct zcl_native_body_err *err);
char *zcl_native_syncdiag_body(const struct json_value *args,
                               struct zcl_native_body_err *err);
char *zcl_native_blockers_body(const struct json_value *args,
                               struct zcl_native_body_err *err);
char *zcl_native_timeline_body(const struct json_value *args,
                               struct zcl_native_body_err *err);
char *zcl_native_agent_diagnose_body(const struct json_value *args,
                                     struct zcl_native_body_err *err);
char *zcl_native_postmortem_list_body(const struct json_value *args,
                                      struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_STATUS_NATIVE_HANDLERS_H */
