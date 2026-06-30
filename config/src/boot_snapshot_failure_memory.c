/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
#include "config/boot_snapshot_failure_memory.h"

#include "config/boot.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool marker_path_for_snapshot(char *out, size_t cap,
                                     const char *snapshot_path)
{
    if (!out || cap == 0 || !snapshot_path || !snapshot_path[0])
        return false;
    int n = snprintf(out, cap, "%s.failed", snapshot_path);
    return n > 0 && (size_t)n < cap;
}

static bool write_empty_marker(const char *path)
{
    FILE *mf = fopen(path, "we");
    if (!mf)
        return false;
    fclose(mf);
    return true;
}

static void forget_autodetected_seed(struct app_context *ctx,
                                     bool *from_autodetect,
                                     char *fail_marker)
{
    if (ctx && ctx->load_snapshot_at_own_height) {
        free((void *)ctx->load_snapshot_at_own_height);
        ctx->load_snapshot_at_own_height = NULL;
    }
    if (from_autodetect)
        *from_autodetect = false;
    if (fail_marker)
        fail_marker[0] = '\0';
}

static void forget_explicit_seed(struct app_context *ctx, char *fail_marker)
{
    if (ctx)
        ctx->load_snapshot_at_own_height = NULL;
    if (fail_marker)
        fail_marker[0] = '\0';
}

static void maybe_autodetect_seed(struct app_context *ctx,
                                  bool coins_kv_proven_authority,
                                  bool *from_autodetect,
                                  char *fail_marker,
                                  size_t fail_marker_cap)
{
    if (!ctx || ctx->load_snapshot_at_own_height || coins_kv_proven_authority)
        return;

    char *auto_snap = boot_autodetect_bundle_snapshot(ctx->datadir);
    if (!auto_snap)
        return;

    LOG_INFO("boot",
             "[boot] starter-pack bundle detected in datadir - auto-"
             "loading %s (no -load-snapshot-at-own-height flag needed)",
             auto_snap);
    ctx->load_snapshot_at_own_height = auto_snap;
    if (from_autodetect)
        *from_autodetect = true;

    if (!marker_path_for_snapshot(fail_marker, fail_marker_cap, auto_snap) ||
        !write_empty_marker(fail_marker)) {
        /* Could not write the failure-memory marker (read-only datadir /
         * ENOSPC / EACCES). Do not gamble on an autodetect seed whose failure
         * we cannot remember: a bad bundle _exit()s the loader, and the next
         * Restart=always boot would re-select the same bundle forever. */
        LOG_WARN("boot",
                 "[boot] autodetect seed marker unwritable (%s.failed) - "
                 "skipping snapshot seed, using P2P IBD",
                 auto_snap);
        forget_autodetected_seed(ctx, from_autodetect, fail_marker);
    }
}

static void prepare_explicit_seed_marker(struct app_context *ctx,
                                         bool from_autodetect,
                                         char *fail_marker,
                                         size_t fail_marker_cap)
{
    if (!ctx || !ctx->load_snapshot_at_own_height || from_autodetect)
        return;

    if (!marker_path_for_snapshot(fail_marker, fail_marker_cap,
                                  ctx->load_snapshot_at_own_height)) {
        /* Path too long to form a marker - we could not remember a failure, so
         * do not enter the fail-closed _exit path; degrade to P2P IBD. */
        LOG_WARN("boot",
                 "[boot] -load-snapshot-at-own-height marker path too long "
                 "for %s - skipping snapshot seed, using P2P IBD",
                 ctx->load_snapshot_at_own_height);
        forget_explicit_seed(ctx, fail_marker);
        return;
    }

    if (access(fail_marker, F_OK) == 0) {
        /* A prior boot crash-failed seeding this exact bundle. Skip it instead
         * of re-entering the _exit crash-loop. */
        LOG_WARN("boot",
                 "[boot] -load-snapshot-at-own-height bundle %s has a .failed "
                 "marker from a prior crashed seed - skipping it; using normal "
                 "P2P IBD. Delete %s to retry the bundle.",
                 ctx->load_snapshot_at_own_height, fail_marker);
        forget_explicit_seed(ctx, fail_marker);
        return;
    }

    if (!write_empty_marker(fail_marker)) {
        LOG_WARN("boot",
                 "[boot] -load-snapshot-at-own-height marker unwritable "
                 "(%s) - skipping snapshot seed, using P2P IBD",
                 fail_marker);
        forget_explicit_seed(ctx, fail_marker);
    }
}

bool boot_snapshot_failure_memory_prepare(struct app_context *ctx,
                                          bool coins_kv_proven_authority,
                                          bool *from_autodetect,
                                          char *fail_marker,
                                          size_t fail_marker_cap)
{
    bool local_from_autodetect = false;
    bool *autodetect_flag = from_autodetect ? from_autodetect
                                            : &local_from_autodetect;
    *autodetect_flag = false;
    if (fail_marker && fail_marker_cap > 0)
        fail_marker[0] = '\0';
    if (!ctx || !fail_marker || fail_marker_cap == 0)
        return false;

    maybe_autodetect_seed(ctx, coins_kv_proven_authority, autodetect_flag,
                          fail_marker, fail_marker_cap);
    prepare_explicit_seed_marker(ctx,
                                 *autodetect_flag,
                                 fail_marker,
                                 fail_marker_cap);

    return ctx->load_snapshot_at_own_height != NULL;
}

void boot_snapshot_failure_memory_clear(const char *fail_marker)
{
    if (fail_marker && fail_marker[0])
        (void)remove(fail_marker);
}
