/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: One join recipe for C23 Commons package hosting and compile work. */

#ifndef ZCL_TOOLS_NATIVE_ZCODE_JOIN_H
#define ZCL_TOOLS_NATIVE_ZCODE_JOIN_H

#include "json/json.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCL_ZCODE_JOIN_FLAGS "-packagehost=1 -buildworker=1"
#define ZCL_ZCODE_HOSTING_REQUIREMENT \
    "run the full node with -packagehost=1 -buildworker=1"

struct zcl_zcode_join_posture {
    bool package_hosting;
    bool build_worker;
    bool joined;
    const char *join_flags;
    const char *hosting_requirement;
    const char *offline_next_command;
};

/* Reads this process's -packagehost / -buildworker flags and whether the
 * in-process work node has local compile capability. One-shot CLI processes
 * report joined=false unless those flags were passed to this process. */
bool zcl_zcode_join_posture_fill(struct zcl_zcode_join_posture *out);

/* Emits join_flags, package_hosting, build_worker, joined, and
 * hosting_requirement. */
bool zcl_zcode_join_posture_push_json(struct json_value *data,
                                      const struct zcl_zcode_join_posture *join);

#ifdef __cplusplus
}
#endif

#endif
