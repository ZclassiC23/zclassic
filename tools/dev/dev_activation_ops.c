/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * dev_activation_ops.c — the REAL service-action ops for the native dev-lane
 * activation engine: exec `systemctl --user ...` through the fixed-argv runner
 * zcl_devloop_process_run(), and read /proc/<pid>/exe. Every entry point here
 * does process exec, so the whole TU is confined to ZCL_DEV_BUILD and is absent
 * from both the release binary and the (ZCL_TESTING) test harness — tests drive
 * a fake ops vtable instead. Symbol absence from release is proven by
 * tools/lint/check_release_no_dev_symbols.sh.
 */

#define _GNU_SOURCE

#include "dev_activation.h"
#include "dev_activation_internal.h"

#ifdef ZCL_DEV_BUILD

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "devloop.h"

/* The default ops treat the request as their context so the real systemctl /
 * proc calls can read unit / datadir / rpcport / repo_root. */

static int dev_run_argv(const char *cwd, const char *const argv[],
                        int timeout_ms, struct zcl_devloop_process_result *out)
{
    if (!zcl_devloop_process_run(cwd, argv, timeout_ms, out))
        return -1;
    if (out->timed_out || out->term_signal != 0)
        return -1;
    return out->exit_code;
}

static int dev_op_stop(void *ctx)
{
    const struct dev_activation_request *req = ctx;
    const char *argv[] = { "systemctl", "--user", "stop", req->unit, NULL };
    struct zcl_devloop_process_result res = {0};
    return dev_run_argv(req->repo_root, argv,
                        DEV_ACTIVATION_STOP_START_TIMEOUT_S * 1000, &res);
}

static int dev_op_start(void *ctx)
{
    const struct dev_activation_request *req = ctx;
    const char *argv[] = { "systemctl", "--user", "start", req->unit, NULL };
    struct zcl_devloop_process_result res = {0};
    return dev_run_argv(req->repo_root, argv,
                        DEV_ACTIVATION_STOP_START_TIMEOUT_S * 1000, &res);
}

static int dev_op_daemon_reload(void *ctx)
{
    const struct dev_activation_request *req = ctx;
    const char *argv[] = { "systemctl", "--user", "daemon-reload", NULL };
    struct zcl_devloop_process_result res = {0};
    return dev_run_argv(req->repo_root, argv, 30000, &res);
}

static int dev_op_reset_failed(void *ctx)
{
    const struct dev_activation_request *req = ctx;
    const char *argv[] = { "systemctl", "--user", "reset-failed", req->unit,
                           NULL };
    struct zcl_devloop_process_result res = {0};
    (void)dev_run_argv(req->repo_root, argv, 30000, &res);
    return 0; /* best-effort, mirrors the shell's `|| true` */
}

static int dev_op_active(void *ctx)
{
    const struct dev_activation_request *req = ctx;
    const char *argv[] = { "systemctl", "--user", "is-active", "--quiet",
                           req->unit, NULL };
    struct zcl_devloop_process_result res = {0};
    return dev_run_argv(req->repo_root, argv, 30000, &res);
}

static int dev_op_main_pid(void *ctx, long *pid_out)
{
    const struct dev_activation_request *req = ctx;
    const char *argv[] = { "systemctl", "--user", "show", req->unit, "-p",
                           "MainPID", "--value", NULL };
    struct zcl_devloop_process_result res = {0};
    if (dev_run_argv(req->repo_root, argv, 30000, &res) != 0)
        return -1;
    *pid_out = strtol(res.output, NULL, 10);
    return 0;
}

static int dev_op_running_exe(void *ctx, long pid, char *out, size_t out_sz)
{
    (void)ctx;
    if (pid <= 0)
        return -1;
    char link[64];
    snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
    char target[PATH_MAX];
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0)
        return -1;
    target[n] = 0;
    char canon[PATH_MAX];
    if (!dev_activation_canon(target, canon, sizeof(canon)))
        return -1;
    n = snprintf(out, out_sz, "%s", canon);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

static int dev_op_preflight(void *ctx, const char *cand_bin,
                            const char *source_id_sha256)
{
    const struct dev_activation_request *req = ctx;
    struct zcl_devloop_process_result res = {0};
    const char *ab[] = { cand_bin, "agentbuild", NULL };
    if (dev_run_argv(req->repo_root, ab, 30000, &res) != 0)
        return -1;
    if (!strstr(res.output, "zcl.agent_build.v2"))
        return -1;
    char observed[65];
    if (!dev_activation_json_first_string(res.output, "source_id_sha256",
                                          observed, sizeof(observed)) ||
        !dev_activation_source_id_valid(observed))
        return -1;
    if (!source_id_sha256 ||
        strcmp(observed, source_id_sha256) != 0)
        return -1;
    /* Native registry self-test is the resident command-catalog proof
     * preflight seam: a deterministic, node-free well-formedness sweep of the
     * command catalog. Fail closed unless the candidate reports fail == 0. */
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "-rpcport=%d", req->rpcport);
    char ddbuf[PATH_MAX];
    snprintf(ddbuf, sizeof(ddbuf), "-datadir=%s", req->datadir);
    struct zcl_devloop_process_result st = {0};
    const char *sv[] = { cand_bin, ddbuf, portbuf, "ops", "selftest", NULL };
    if (dev_run_argv(req->repo_root, sv, 30000, &st) != 0)
        return -1;
    if (!strstr(st.output, "\"mode\":\"registry\"") ||
        !strstr(st.output, "\"fail\":0"))
        return -1;
    return 0;
}

static int dev_op_source_epoch_cas(void *ctx)
{
    const struct dev_activation_request *req = ctx;
    if (!dev_activation_source_id_valid(req->source_identity))
        return -1;
    char tool[PATH_MAX];
    int n = snprintf(tool, sizeof(tool), "%s/tools/dev/source-identity.sh",
                     req->repo_root);
    if (n <= 0 || (size_t)n >= sizeof(tool))
        return -1;
    const char *argv[] = { tool, "verify", req->source_identity, NULL };
    struct zcl_devloop_process_result res = {0};
    return dev_run_argv(req->repo_root, argv, 30000, &res);
}

static int dev_op_activation_probe(void *ctx, const char *gen_id,
                                   const char *expected_source_id_sha256)
{
    const struct dev_activation_request *req = ctx;
    char cli[PATH_MAX];
    snprintf(cli, sizeof(cli), "%s/build/bin/zclassic-cli", req->repo_root);
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "-rpcport=%d", req->rpcport);
    char ddbuf[PATH_MAX];
    snprintf(ddbuf, sizeof(ddbuf), "-datadir=%s", req->datadir);
    struct zcl_devloop_process_result res = {0};
    const char *hv[] = { cli, ddbuf, portbuf, "getblockcount", NULL };
    if (dev_run_argv(req->repo_root, hv, 12000, &res) != 0)
        return -1;
    const char *av[] = { cli, ddbuf, portbuf, "agent", NULL };
    if (dev_run_argv(req->repo_root, av, 12000, &res) != 0)
        return -1;
    if (!strstr(res.output, "zcl.public_status.v2"))
        return -1;
    char observed[65];
    if (!dev_activation_source_id_valid(expected_source_id_sha256) ||
        !dev_activation_json_first_string(res.output, "source_id_sha256",
                                          observed, sizeof(observed)) ||
        !dev_activation_source_id_valid(observed) ||
        strcmp(observed, expected_source_id_sha256) != 0)
        return -1;
    (void)gen_id;
    return 0;
}

void dev_activation_default_ops(const struct dev_activation_request *req,
                                struct dev_activation_ops *out)
{
    memset(out, 0, sizeof(*out));
    out->service_stop = dev_op_stop;
    out->service_start = dev_op_start;
    out->service_daemon_reload = dev_op_daemon_reload;
    out->service_reset_failed = dev_op_reset_failed;
    out->service_active = dev_op_active;
    out->service_main_pid = dev_op_main_pid;
    out->running_exe = dev_op_running_exe;
    out->preflight = dev_op_preflight;
    out->source_epoch_cas = dev_op_source_epoch_cas;
    out->activation_probe = dev_op_activation_probe;
    out->ctx = (void *)req;
}

#endif /* ZCL_DEV_BUILD */
