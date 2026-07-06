/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_AGENT_H
#define ZCL_CONTROLLERS_AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct json_value;

/* Set once from node-mode main after CLI/env parsing and before app_init()
 * starts RPC-serving threads. Tests may reset the context directly. */
void rpc_agent_set_boot_context(const char *operator_lane,
                                const char *runtime_profile,
                                const char *datadir,
                                int rpc_port, int p2p_port,
                                int https_port, int fs_port);
void agent_fill_operator_lane_contract_json(struct json_value *lane_obj,
                                            const char *operator_lane,
                                            const char *runtime_profile,
                                            const char *datadir,
                                            int rpc_port, int p2p_port,
                                            int https_port, int fs_port);
void agent_push_operator_lane_fields_json(struct json_value *out);
void agent_push_operator_lane_json(struct json_value *out,
                                   const char *key);
void agent_push_runtime_build_json(struct json_value *out,
                                   const char *key);
void agent_push_runtime_services_json(struct json_value *out,
                                      const char *key);
void agent_runtime_availability_reset(void);
void agent_runtime_availability_begin_probe(const char *source,
                                            const char *datadir,
                                            int rpc_port,
                                            const char *status);
void agent_runtime_availability_set_probe_status(const char *status);
void agent_runtime_availability_record_method(const char *method,
                                              const char *support,
                                              int64_t rpc_error_code,
                                              const char *error_message);
void agent_runtime_availability_set_target_build_commit(
    const char *build_commit);
size_t agent_runtime_probe_method_count(void);
const char *agent_runtime_probe_method_name(size_t index);
void agent_push_runtime_availability_json(struct json_value *out,
                                          const char *key);

bool rpc_agent_map(const struct json_value *params, bool help,
                   struct json_value *result);
bool rpc_agent_lanes(const struct json_value *params, bool help,
                     struct json_value *result);
bool rpc_agent_impact(const struct json_value *params, bool help,
                      struct json_value *result);
bool rpc_agent_contracts(const struct json_value *params, bool help,
                         struct json_value *result);
bool rpc_agent_build(const struct json_value *params, bool help,
                     struct json_value *result);
bool rpc_agent_interface(const struct json_value *params, bool help,
                         struct json_value *result);
bool rpc_agent_deploy_guard(const struct json_value *params, bool help,
                            struct json_value *result);
bool rpc_agent_ops(const struct json_value *params, bool help,
                   struct json_value *result);

#endif
