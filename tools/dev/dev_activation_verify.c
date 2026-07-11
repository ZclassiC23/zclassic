/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * dev_activation_verify.c — bounded post-restart verification, quarantine, and
 * rollback for the native dev-lane activation engine. Split from
 * dev_activation.c along the verify/rollback seam (file-size ceiling). Uses the
 * ops vtable for every service action, so it too is exercisable with a fake
 * ops implementation and carries no direct process exec.
 */

#define _GNU_SOURCE

#include "dev_activation.h"
#include "dev_activation_internal.h"

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Sleep for `ms` milliseconds (bounded verify poll cadence). */
static void dev_sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

bool dev_activation_verify_running(struct dev_activation_txn *txn,
                                   const char *expected)
{
    const struct dev_activation_ops *ops = txn->ops;
    char expected_bin[PATH_MAX], expected_canon[PATH_MAX];
    int n = snprintf(expected_bin, sizeof(expected_bin),
                     "%s/%s/zclassic23-dev", txn->gen_root, expected);
    if (n <= 0 || (size_t)n >= sizeof(expected_bin))
        LOG_FAIL("dev-activation", "verify expected bin path overflow");
    if (!dev_activation_canon(expected_bin, expected_canon,
                              sizeof(expected_canon)))
        return false;

    char commit[128];
    dev_activation_generation_commit(txn, expected, commit, sizeof(commit));

    /* The verify window defaults to DEV_ACTIVATION_VERIFY_TIMEOUT_S; a
     * hermetic-test override (mirroring the shell's ZCL_DEV_ACTIVATION_TIMEOUT)
     * lets failure-path tests fail fast instead of burning the full 60 s. */
    long timeout_s = DEV_ACTIVATION_VERIFY_TIMEOUT_S;
    const char *ev = getenv("ZCL_DEV_ACTIVATION_VERIFY_TIMEOUT_S");
    if (ev && *ev) {
        long v = strtol(ev, NULL, 10);
        if (v >= 0)
            timeout_s = v;
    }

    txn->result->running_generation[0] = 0;
    int64_t deadline = platform_time_monotonic_us() +
        (int64_t)timeout_s * 1000000;
    for (;;) {
        if (ops->service_active(ops->ctx) == 0) {
            long pid = 0;
            char exe[PATH_MAX], exe_canon[PATH_MAX];
            if (ops->service_main_pid(ops->ctx, &pid) == 0 && pid > 0 &&
                ops->running_exe(ops->ctx, pid, exe, sizeof(exe)) == 0 &&
                dev_activation_canon(exe, exe_canon, sizeof(exe_canon)) &&
                strcmp(exe_canon, expected_canon) == 0) {
                snprintf(txn->result->running_generation,
                         sizeof(txn->result->running_generation), "%s",
                         expected);
                if (ops->activation_probe(ops->ctx, expected, commit) == 0)
                    return true;
            }
        }
        if (platform_time_monotonic_us() >= deadline)
            break;
        dev_sleep_ms(DEV_ACTIVATION_VERIFY_INTERVAL_MS);
    }
    return false;
}

void dev_activation_quarantine(const struct dev_activation_txn *txn,
                               const char *reason)
{
    char marker_tmp[PATH_MAX], marker[PATH_MAX];
    int n = snprintf(marker, sizeof(marker), "%s/%s.json", txn->rejected_dir,
                     txn->candidate_generation);
    if (n <= 0 || (size_t)n >= sizeof(marker)) {
        fprintf(stderr, "[dev-activation] quarantine marker path overflow\n");
        return;
    }
    n = snprintf(marker_tmp, sizeof(marker_tmp), "%s/.%s.%ld", txn->rejected_dir,
                 txn->candidate_generation, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(marker_tmp)) {
        fprintf(stderr, "[dev-activation] quarantine tmp path overflow\n");
        return;
    }
    FILE *f = fopen(marker_tmp, "w");
    if (!f) {
        fprintf(stderr, "[dev-activation] quarantine open %s: %s\n", marker_tmp,
                strerror(errno));
        return;
    }
    time_t t = platform_time_wall_time_t();
    struct tm tmv;
    char now[32];
    if (gmtime_r(&t, &tmv))
        strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    else
        snprintf(now, sizeof(now), "1970-01-01T00:00:00Z");
    /* one-line the reason to keep the value on a single JSON line */
    char clean[256];
    size_t o = 0;
    for (const char *p = reason ? reason : ""; *p && o + 2 < sizeof(clean); p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')
            clean[o++] = '\\';
        clean[o++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
    }
    clean[o] = 0;
    fprintf(f, "{\"schema\":\"zcl.dev_rejected_generation.v1\","
               "\"generation\":\"%s\",\"sha256\":\"%s\","
               "\"rejected_at_utc\":\"%s\",\"reason\":\"%s\"}\n",
            txn->candidate_generation, txn->candidate_sha_hex, now, clean);
    fclose(f);
    if (rename(marker_tmp, marker) != 0) {
        (void)unlink(marker_tmp);
        fprintf(stderr, "[dev-activation] quarantine rename failed: %s\n",
                strerror(errno));
    }
}

bool dev_activation_rollback_previous(struct dev_activation_txn *txn,
                                      const char *reason)
{
    struct dev_activation_result *r = txn->result;
    const struct dev_activation_ops *ops = txn->ops;
    snprintf(r->rollback_status, sizeof(r->rollback_status), "started");

    (void)ops->service_stop(ops->ctx);

    if (txn->previous_generation[0] == 0 ||
        !dev_activation_link_generation(txn, "current",
                                        txn->previous_generation)) {
        (void)unlink(txn->current_link);
        (void)unlink(txn->compat_bin);
        snprintf(r->rollback_status, sizeof(r->rollback_status), "unavailable");
        snprintf(r->failure_capsule, sizeof(r->failure_capsule),
                 "%s; no prior generation available", reason ? reason : "");
        txn->activation_in_progress = false;
        return false;
    }

    (void)dev_activation_refresh_compat_link(txn);
    dev_activation_write_build_identity(txn, txn->previous_generation);
    (void)ops->service_daemon_reload(ops->ctx);
    (void)ops->service_reset_failed(ops->ctx);

    if (ops->service_start(ops->ctx) != 0) {
        snprintf(r->rollback_status, sizeof(r->rollback_status),
                 "restart_failed");
        snprintf(r->failure_capsule, sizeof(r->failure_capsule),
                 "%s; last-good restart command failed", reason ? reason : "");
        txn->activation_in_progress = false;
        return false;
    }

    if (dev_activation_verify_running(txn, txn->previous_generation)) {
        (void)dev_activation_link_generation(txn, "last-good",
                                             txn->previous_generation);
        snprintf(r->rollback_status, sizeof(r->rollback_status), "verified");
        txn->activation_in_progress = false;
        return true;
    }

    snprintf(r->rollback_status, sizeof(r->rollback_status),
             "verification_failed");
    snprintf(r->failure_capsule, sizeof(r->failure_capsule),
             "%s; last-good recovery probe failed", reason ? reason : "");
    txn->activation_in_progress = false;
    return false;
}

#endif /* ZCL_DEV_BUILD || ZCL_TESTING */
