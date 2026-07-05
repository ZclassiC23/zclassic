/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot.h"

#include <string.h>

void app_context_defaults(struct app_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->datadir = NULL;
    ctx->params_dir = NULL;
    ctx->rpc_port = 18232;
    ctx->p2p_port = 8033;
    ctx->https_port = 8443;
    ctx->fs_port = 18034;
    ctx->listen = true;   /* accept inbound by default — be a good peer */
    ctx->checkpoints_enabled = true;
    ctx->runtime_profile = ZCL_RUNTIME_FULL;
    ctx->operator_lane = ZCL_OPERATOR_LANE_UNKNOWN;
    ctx->par_workers = 0;   /* 0 => verify engine sizes to GetNumCores()-1 */
}

const char *app_runtime_profile_name(enum zcl_runtime_profile profile)
{
    switch (profile) {
    case ZCL_RUNTIME_FULL:
        return "full";
    case ZCL_RUNTIME_ZCLASSIC_ONLY:
        return "zclassic-only";
    case ZCL_RUNTIME_EXPLORER:
        return "explorer";
    case ZCL_RUNTIME_ONION_NODE:
        return "onion-node";
    case ZCL_RUNTIME_LEGACY_COMPAT:
        return "legacy-compat";
    default:
        return "unknown";
    }
}

bool app_runtime_profile_parse(const char *name,
                               enum zcl_runtime_profile *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "full") == 0) {
        *out = ZCL_RUNTIME_FULL;
        return true;
    }
    if (strcmp(name, "zclassic-only") == 0 ||
        strcmp(name, "zclassic") == 0) {
        *out = ZCL_RUNTIME_ZCLASSIC_ONLY;
        return true;
    }
    if (strcmp(name, "explorer") == 0) {
        *out = ZCL_RUNTIME_EXPLORER;
        return true;
    }
    if (strcmp(name, "onion-node") == 0 ||
        strcmp(name, "onion") == 0) {
        *out = ZCL_RUNTIME_ONION_NODE;
        return true;
    }
    if (strcmp(name, "legacy-compat") == 0 ||
        strcmp(name, "legacy") == 0) {
        *out = ZCL_RUNTIME_LEGACY_COMPAT;
        return true;
    }
    return false;
}

bool app_runtime_profile_has_explorer(enum zcl_runtime_profile profile)
{
    return profile == ZCL_RUNTIME_FULL ||
           profile == ZCL_RUNTIME_EXPLORER ||
           profile == ZCL_RUNTIME_ONION_NODE ||
           profile == ZCL_RUNTIME_LEGACY_COMPAT;
}

bool app_runtime_profile_has_store(enum zcl_runtime_profile profile)
{
    return profile == ZCL_RUNTIME_FULL ||
           profile == ZCL_RUNTIME_ONION_NODE ||
           profile == ZCL_RUNTIME_LEGACY_COMPAT;
}

bool app_runtime_profile_has_onion(enum zcl_runtime_profile profile,
                                   bool tor_flag)
{
    return tor_flag || profile == ZCL_RUNTIME_ONION_NODE;
}

bool app_runtime_profile_has_file_service(enum zcl_runtime_profile profile)
{
    return profile == ZCL_RUNTIME_FULL ||
           profile == ZCL_RUNTIME_ONION_NODE ||
           profile == ZCL_RUNTIME_LEGACY_COMPAT;
}

const char *app_operator_lane_name(enum zcl_operator_lane lane)
{
    switch (lane) {
    case ZCL_OPERATOR_LANE_CANONICAL:
        return "canonical";
    case ZCL_OPERATOR_LANE_SOAK:
        return "soak";
    case ZCL_OPERATOR_LANE_DEV:
        return "dev";
    case ZCL_OPERATOR_LANE_TEST:
        return "test";
    case ZCL_OPERATOR_LANE_COPY:
        return "copy";
    case ZCL_OPERATOR_LANE_UNKNOWN:
    default:
        return "unknown";
    }
}

bool app_operator_lane_parse(const char *name,
                             enum zcl_operator_lane *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "canonical") == 0 ||
        strcmp(name, "live") == 0 ||
        strcmp(name, "main") == 0) {
        *out = ZCL_OPERATOR_LANE_CANONICAL;
        return true;
    }
    if (strcmp(name, "soak") == 0) {
        *out = ZCL_OPERATOR_LANE_SOAK;
        return true;
    }
    if (strcmp(name, "dev") == 0 ||
        strcmp(name, "development") == 0) {
        *out = ZCL_OPERATOR_LANE_DEV;
        return true;
    }
    if (strcmp(name, "test") == 0 ||
        strcmp(name, "ci") == 0) {
        *out = ZCL_OPERATOR_LANE_TEST;
        return true;
    }
    if (strcmp(name, "copy") == 0 ||
        strcmp(name, "repro") == 0) {
        *out = ZCL_OPERATOR_LANE_COPY;
        return true;
    }
    if (strcmp(name, "unknown") == 0) {
        *out = ZCL_OPERATOR_LANE_UNKNOWN;
        return true;
    }
    return false;
}
