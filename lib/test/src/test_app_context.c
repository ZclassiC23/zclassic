/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "config/boot.h"

#include <stdio.h>
#include <string.h>

#define APPCTX_CHECK(name, expr) do {                                  \
    printf("app_context: %s... ", (name));                            \
    if (expr) printf("OK\n");                                         \
    else { printf("FAIL\n"); failures++; }                            \
} while (0)

int test_app_context(void)
{
    int failures = 0;

    {
        struct app_context ctx;
        memset(&ctx, 0xff, sizeof(ctx));

        app_context_defaults(&ctx);

        APPCTX_CHECK("defaults select full public node profile",
                     ctx.datadir == NULL &&
                     ctx.params_dir == NULL &&
                     ctx.rpc_port == 18232 &&
                     ctx.p2p_port == 8033 &&
                     ctx.https_port == 8443 &&
                     ctx.fs_port == 18034 &&
                     ctx.listen &&
                     ctx.checkpoints_enabled &&
                     ctx.runtime_profile == ZCL_RUNTIME_FULL &&
                     ctx.par_workers == 0 &&
                     !ctx.no_services &&
                     !ctx.no_legacy_auto_import &&
                     !ctx.load_verify_boot &&
                     ctx.load_snapshot_at_own_height == NULL);
    }

    {
        enum zcl_runtime_profile profile = ZCL_RUNTIME_FULL;
        bool ok = true;

        ok = ok && app_runtime_profile_parse("full", &profile) &&
             profile == ZCL_RUNTIME_FULL;
        ok = ok && app_runtime_profile_parse("zclassic", &profile) &&
             profile == ZCL_RUNTIME_ZCLASSIC_ONLY;
        ok = ok && app_runtime_profile_parse("zclassic-only", &profile) &&
             profile == ZCL_RUNTIME_ZCLASSIC_ONLY;
        ok = ok && app_runtime_profile_parse("explorer", &profile) &&
             profile == ZCL_RUNTIME_EXPLORER;
        ok = ok && app_runtime_profile_parse("onion", &profile) &&
             profile == ZCL_RUNTIME_ONION_NODE;
        ok = ok && app_runtime_profile_parse("onion-node", &profile) &&
             profile == ZCL_RUNTIME_ONION_NODE;
        ok = ok && app_runtime_profile_parse("legacy", &profile) &&
             profile == ZCL_RUNTIME_LEGACY_COMPAT;
        ok = ok && app_runtime_profile_parse("legacy-compat", &profile) &&
             profile == ZCL_RUNTIME_LEGACY_COMPAT;
        ok = ok && !app_runtime_profile_parse("unknown", &profile);
        ok = ok && !app_runtime_profile_parse(NULL, &profile);
        ok = ok && !app_runtime_profile_parse("full", NULL);

        APPCTX_CHECK("runtime profile names and aliases parse", ok);
    }

    APPCTX_CHECK("runtime profile names are stable",
                 strcmp(app_runtime_profile_name(ZCL_RUNTIME_FULL), "full") == 0 &&
                 strcmp(app_runtime_profile_name(ZCL_RUNTIME_ZCLASSIC_ONLY),
                        "zclassic-only") == 0 &&
                 strcmp(app_runtime_profile_name(ZCL_RUNTIME_EXPLORER),
                        "explorer") == 0 &&
                 strcmp(app_runtime_profile_name(ZCL_RUNTIME_ONION_NODE),
                        "onion-node") == 0 &&
                 strcmp(app_runtime_profile_name(ZCL_RUNTIME_LEGACY_COMPAT),
                        "legacy-compat") == 0 &&
                 strcmp(app_runtime_profile_name((enum zcl_runtime_profile)999),
                        "unknown") == 0);

    APPCTX_CHECK("zclassic-only profile stays lean",
                 !app_runtime_profile_has_explorer(ZCL_RUNTIME_ZCLASSIC_ONLY) &&
                 !app_runtime_profile_has_store(ZCL_RUNTIME_ZCLASSIC_ONLY) &&
                 !app_runtime_profile_has_file_service(
                     ZCL_RUNTIME_ZCLASSIC_ONLY) &&
                 !app_runtime_profile_has_onion(
                     ZCL_RUNTIME_ZCLASSIC_ONLY, false) &&
                 app_runtime_profile_has_onion(
                     ZCL_RUNTIME_ZCLASSIC_ONLY, true));

    APPCTX_CHECK("application profiles expose expected surfaces",
                 app_runtime_profile_has_explorer(ZCL_RUNTIME_FULL) &&
                 app_runtime_profile_has_store(ZCL_RUNTIME_FULL) &&
                 app_runtime_profile_has_file_service(ZCL_RUNTIME_FULL) &&
                 app_runtime_profile_has_explorer(ZCL_RUNTIME_EXPLORER) &&
                 !app_runtime_profile_has_store(ZCL_RUNTIME_EXPLORER) &&
                 !app_runtime_profile_has_file_service(ZCL_RUNTIME_EXPLORER) &&
                 app_runtime_profile_has_explorer(ZCL_RUNTIME_ONION_NODE) &&
                 app_runtime_profile_has_store(ZCL_RUNTIME_ONION_NODE) &&
                 app_runtime_profile_has_file_service(ZCL_RUNTIME_ONION_NODE) &&
                 app_runtime_profile_has_onion(ZCL_RUNTIME_ONION_NODE, false) &&
                 app_runtime_profile_has_explorer(ZCL_RUNTIME_LEGACY_COMPAT) &&
                 app_runtime_profile_has_store(ZCL_RUNTIME_LEGACY_COMPAT) &&
                 app_runtime_profile_has_file_service(ZCL_RUNTIME_LEGACY_COMPAT));

    return failures;
}
