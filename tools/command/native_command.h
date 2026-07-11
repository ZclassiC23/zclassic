/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_TOOLS_NATIVE_COMMAND_H
#define ZCL_TOOLS_NATIVE_COMMAND_H

#include "kernel/command_registry.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool zcl_native_command_is_root(const char *word);
int zcl_native_command_main(const char *root_word,
                            const char *const *args, int nargs);

void zcl_native_handle_discover_help(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_search(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_describe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_schema(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_core_boundary(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_describe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_simulate(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_change_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_app_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_app_inspect(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_NATIVE_COMMAND_H */
