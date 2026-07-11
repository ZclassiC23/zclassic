/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the registry-owned `dev` tree
 * (docs/NATIVE_COMMAND_INTERFACE.md §7). These bind the registry catalog's dev
 * subtree to the checkout-local read-only producers: App manifest describe /
 * plan / simulate, source-change classification, the Core/App boundary law,
 * the latest native cycle verdict, and dev.vcs.revert — the one-command
 * source+binary revert (vcs_revert(), lib/vcs/) that, when relink_generation
 * is requested, activates the reverted commit's bound binary generation so
 * the RUNNING node matches the reverted source. lib/vcs/ itself stays
 * git-free and process-spawn-free (the ZVCS sovereignty gate,
 * check-vcs-no-git); the relink activator that closes the binary half lives
 * here, entirely outside lib/vcs/.
 *
 * The read-only checkout producers compile into both binaries.  Executors and
 * watcher/generation lifecycle handlers live below ZCL_DEV_BUILD, so a release
 * binary can describe the same grammar but cannot spawn, mutate, or activate
 * anything in the development lane. */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "devloop.h"
#include "kernel/command_registry.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "vcs/vcs.h"

#include "encoding/utilstrencodings.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZCL_DEV_BUILD
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

/* Copy a produced JSON document (buffer producer output) into reply->data.
 * On any failure, fail the reply with an INTERNAL contract error. */
static void dev_reply_from_json(struct zcl_command_reply *reply,
                                const char *body, size_t n, const char *what)
{
    struct json_value doc;
    json_init(&doc);
    if (n == 0 || !json_read(&doc, body, n) || doc.type != JSON_OBJ) {
        json_free(&doc);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "DEV_RENDER_FAILED",
                               "serialize", false, false,
                               "read-only dev producer returned no document",
                               what ? what : "");
        return;
    }
    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &doc);
    json_free(&doc);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

static const char *dev_source_root(const struct zcl_command_request *request)
{
    if (request && request->context && request->context->source_root &&
        request->context->source_root[0])
        return request->context->source_root;
    const char *env = getenv("ZCL_DEV_SOURCE_ROOT");
    return env && env[0] ? env : ".";
}

/* ── dev.status ────────────────────────────────────────────────────────── */
void zcl_native_handle_dev_status(const struct zcl_command_request *request,
                                  struct zcl_command_reply *reply)
{
    (void)request;
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (home && home[0] &&
        snprintf(path, sizeof(path),
                 "%s/.local/state/zclassic23-dev/native-cycle.json", home) > 0) {
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[16384];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            struct json_value doc;
            if (n > 0 && json_read(&doc, buf, n) && doc.type == JSON_OBJ) {
                json_free(&reply->data);
                json_init(&reply->data);
                json_copy(&reply->data, &doc);
                json_free(&doc);
                return;
            }
            json_free(&doc);
        }
    }
    /* No durable verdict yet — a bounded, honest "unavailable" is passing. */
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_cycle.v1");
    (void)json_push_kv_str(&reply->data, "status", "unavailable");
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           "keep editing; the native watcher records verdicts");
}

/* ── dev.core.boundary ─────────────────────────────────────────────────── */
void zcl_native_handle_dev_core_boundary(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    static const char *const core[] = {
        "consensus", "validation", "chain_mutation", "wallet_keys",
        "raw_storage", "sockets", "boot"
    };
    static const char *const apps[] = {
        "resources", "signed_events", "services", "projections", "web",
        "onion", "znam", "p2p_topics"
    };
    (void)json_push_kv_str(&reply->data, "schema", "zcl.core_app_boundary.v1");
    (void)json_push_kv_str(&reply->data, "rule",
                           "core_owns_truth_apps_consume_capabilities");
    struct json_value core_arr, app_arr;
    json_init(&core_arr);
    json_init(&app_arr);
    json_set_array(&core_arr);
    json_set_array(&app_arr);
    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++) {
        struct json_value it;
        json_init(&it);
        json_set_str(&it, core[i]);
        (void)json_push_back(&core_arr, &it);
        json_free(&it);
    }
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        struct json_value it;
        json_init(&it);
        json_set_str(&it, apps[i]);
        (void)json_push_back(&app_arr, &it);
        json_free(&it);
    }
    (void)json_push_kv(&reply->data, "core", &core_arr);
    (void)json_push_kv(&reply->data, "apps", &app_arr);
    (void)json_push_kv_str(&reply->data, "core_change", "guarded_reload");
    (void)json_push_kv_str(&reply->data, "app_change",
                           "simulate_then_atomic_publish");
    json_free(&core_arr);
    json_free(&app_arr);
}

/* ── dev.app.describe ──────────────────────────────────────────────────── */
void zcl_native_handle_dev_app_describe(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *app_id = json_get_str(json_get(request->input, "app_id"));
    if (!app_id || !app_id[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_APP_ID",
                               "normalize", false, false,
                               "app_id is required", "");
        return;
    }
    char body[8192];
    size_t n = zcl_devloop_app_describe_json(dev_source_root(request), app_id,
                                             body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_APP",
                               "resolve", false, false,
                               "unknown App or checkout root", app_id);
        return;
    }
    dev_reply_from_json(reply, body, n, app_id);
}

/* ── dev.app.plan ──────────────────────────────────────────────────────── */
void zcl_native_handle_dev_app_plan(const struct zcl_command_request *request,
                                    struct zcl_command_reply *reply)
{
    const char *app_id = json_get_str(json_get(request->input, "app_id"));
    const char *resource = json_get_str(json_get(request->input, "resource"));
    if (!app_id || !app_id[0] || !resource || !resource[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_ARGS",
                               "normalize", false, false,
                               "app_id and resource are required", "");
        return;
    }
    char body[4096];
    size_t n = zcl_devloop_app_plan_json(dev_source_root(request), app_id,
                                         resource, body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_ARGS",
                               "resolve", false, false,
                               "invalid App, resource, or checkout root",
                               app_id);
        return;
    }
    dev_reply_from_json(reply, body, n, app_id);
}

/* ── dev.app.simulate ──────────────────────────────────────────────────── */
void zcl_native_handle_dev_app_simulate(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *app_id = json_get_str(json_get(request->input, "app_id"));
    if (!app_id || !app_id[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_APP_ID",
                               "normalize", false, false,
                               "app_id is required", "");
        return;
    }
    uint64_t seed = UINT64_C(0x534f4349414c0001);
    const struct json_value *seed_v = json_get(request->input, "seed");
    if (seed_v && !json_is_null(seed_v)) {
        if (seed_v->type == JSON_STR) {
            char *end = NULL;
            errno = 0;
            seed = strtoull(json_get_str(seed_v), &end, 0);
            if (errno != 0 || !end || *end || seed == 0) {
                zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                       ZCL_COMMAND_EXIT_INVALID, "INVALID_SEED",
                                       "normalize", false, false,
                                       "seed must be a nonzero uint64 integer or string",
                                       "seed");
                return;
            }
        } else if (seed_v->type == JSON_INT && json_get_int(seed_v) > 0) {
            seed = (uint64_t)json_get_int(seed_v);
        } else {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "INVALID_SEED",
                                   "normalize", false, false,
                                   "seed must be a nonzero uint64 integer or string",
                                   "seed");
            return;
        }
    }
    const struct json_value *scenario_v = json_get(request->input, "scenario");
    if (scenario_v && !json_is_null(scenario_v) &&
        (scenario_v->type != JSON_STR ||
         (strcmp(json_get_str(scenario_v), "default") != 0 &&
          strcmp(json_get_str(scenario_v), "network") != 0))) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_SCENARIO",
                               "normalize", false, false,
                               "scenario must be 'default' or 'network'",
                               "scenario");
        return;
    }
    char body[4096];
    size_t n = zcl_devloop_app_simulate_json(app_id, seed, body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_SIM",
                               "resolve", false, false,
                               "unknown App or invalid seed", app_id);
        return;
    }
    dev_reply_from_json(reply, body, n, app_id);
}

/* ── dev.change.plan ───────────────────────────────────────────────────── */
static bool dev_request_files(const struct json_value *input, bool allow_empty,
                              const char **files, size_t *count,
                              char *why, size_t why_size)
{
    const struct json_value *array = json_get(input, "files");
    *count = 0;
    if (!array || array->type == JSON_NULL)
        return allow_empty;
    if (array->type != JSON_ARR ||
        (!allow_empty && array->num_children == 0) ||
        array->num_children > ZCL_DEVLOOP_MAX_FILES) {
        (void)snprintf(why, why_size,
                       "files must be a bounded string array%s",
                       allow_empty ? "" : " with at least one item");
        return false;
    }
    for (size_t i = 0; i < array->num_children; i++) {
        const struct json_value *item = &array->children[i];
        const char *path = json_get_str(item);
        if (item->type != JSON_STR || !path || !path[0] ||
            strlen(path) >= ZCL_DEVLOOP_PATH_MAX || path[0] == '/' ||
            strstr(path, "..")) {
            (void)snprintf(why, why_size,
                           "files[%zu] must be a confined relative path", i);
            return false;
        }
        files[i] = path;
    }
    *count = array->num_children;
    return true;
}

void zcl_native_handle_dev_change_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *file_ptrs[ZCL_DEVLOOP_MAX_FILES];
    size_t count = 0;
    char why[160];
    if (!dev_request_files(request->input, true, file_ptrs, &count,
                           why, sizeof(why))) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_FILE_SET",
                               "normalize", false, false, why, "files");
        return;
    }
    char body[16384];
    size_t n = zcl_devloop_plan_json(file_ptrs, count, body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_FILE_SET",
                               "normalize", false, false,
                               "invalid or oversized file set", "");
        return;
    }
    dev_reply_from_json(reply, body, n, "change.plan");
}

#ifdef ZCL_DEV_BUILD

/* The registry is the public grammar.  These helpers adapt the existing
 * bounded native devloop engine without letting its legacy stdout document
 * escape ahead of the registry's single zcl.result.v1 envelope. */
typedef int (*dev_captured_fn)(void *arg);

static bool dev_capture_stdout(dev_captured_fn fn, void *arg, char *out,
                               size_t out_size, int *call_rc)
{
    if (!fn || !out || out_size < 2 || !call_rc)
        return false;
    FILE *tmp = tmpfile();
    if (!tmp)
        return false;
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) {
        fclose(tmp);
        return false;
    }
    fflush(stdout);
    if (dup2(fileno(tmp), STDOUT_FILENO) < 0) {
        close(saved);
        fclose(tmp);
        return false;
    }
    *call_rc = fn(arg);
    fflush(stdout);
    bool restored = dup2(saved, STDOUT_FILENO) >= 0;
    close(saved);
    if (!restored || fseek(tmp, 0, SEEK_SET) != 0) {
        fclose(tmp);
        return false;
    }
    size_t n = fread(out, 1, out_size - 1, tmp);
    bool complete = !ferror(tmp) && fgetc(tmp) == EOF;
    fclose(tmp);
    out[n] = 0;
    return complete && n > 0;
}

struct dev_cycle_call {
    const char *root;
    const char *const *files;
    size_t count;
};

static int dev_call_cycle(void *opaque)
{
    struct dev_cycle_call *call = opaque;
    return zcl_devloop_run_cycle(call->root, call->files, call->count);
}

static int dev_call_sim(void *opaque)
{
    return zcl_devloop_run_sim((const char *)opaque);
}

static void dev_fail_with_data(struct zcl_command_reply *reply, int rc,
                               const char *code, const char *phase,
                               bool mutated, const char *message)
{
    enum zcl_command_status status = rc == ZCL_COMMAND_EXIT_BLOCKED
        ? ZCL_COMMAND_STATUS_BLOCKED : ZCL_COMMAND_STATUS_FAILED;
    enum zcl_command_exit exit_code = rc == ZCL_COMMAND_EXIT_BLOCKED
        ? ZCL_COMMAND_EXIT_BLOCKED : ZCL_COMMAND_EXIT_FAILED;
    zcl_command_reply_fail(reply, status, exit_code, code, phase, false,
                           mutated, message,
                           "run dev status for the persisted bounded verdict");
    (void)zcl_command_reply_add_next(
        reply, "dev.status", "{}",
        "read the persisted cycle verdict and executable next action");
}

void zcl_native_handle_dev_change_apply(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *files[ZCL_DEVLOOP_MAX_FILES];
    size_t count = 0;
    char why[160];
    if (!dev_request_files(request->input, false, files, &count,
                           why, sizeof(why))) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_FILE_SET",
                               "normalize", false, false, why, "files");
        return;
    }
    struct dev_cycle_call call = {
        .root = dev_source_root(request), .files = files, .count = count,
    };
    char body[16384];
    int rc = ZCL_COMMAND_EXIT_INTERNAL;
    if (!dev_capture_stdout(dev_call_cycle, &call, body, sizeof(body), &rc)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "CYCLE_CAPTURE_FAILED",
                               "execute", false, true,
                               "native cycle produced no bounded verdict", "");
        return;
    }
    dev_reply_from_json(reply, body, strlen(body), "change.apply");
    if (reply->exit_code == ZCL_COMMAND_EXIT_OK && rc != 0)
        dev_fail_with_data(reply, rc, "DEV_CYCLE_REJECTED", "prove_publish",
                           true, "development cycle did not publish");
}

static bool dev_state_dir(char out[PATH_MAX])
{
    const char *home = getenv("HOME");
    int n = home && home[0]
        ? snprintf(out, PATH_MAX, "%s/.local/state/zclassic23-dev", home) : -1;
    return n > 0 && n < PATH_MAX;
}

static bool dev_mkdirs(const char *path)
{
    char copy[PATH_MAX];
    if (!path || !path[0] || strlen(path) >= sizeof(copy))
        return false;
    (void)snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(copy, 0700) == 0 || errno == EEXIST;
}

static bool dev_watch_paths(char lock[PATH_MAX], char log[PATH_MAX])
{
    char dir[PATH_MAX];
    if (!dev_state_dir(dir))
        return false;
    int ln = snprintf(lock, PATH_MAX, "%s/native-watch.lock", dir);
    int on = snprintf(log, PATH_MAX, "%s/native-watch.log", dir);
    return ln > 0 && ln < PATH_MAX && on > 0 && on < PATH_MAX;
}

static bool dev_pid_is_watcher(pid_t pid)
{
    if (pid <= 1 || (kill(pid, 0) != 0 && errno != EPERM))
        return false;
    char proc[64], exe[PATH_MAX];
    int n = snprintf(proc, sizeof(proc), "/proc/%ld/exe", (long)pid);
    if (n <= 0 || n >= (int)sizeof(proc))
        return false;
    ssize_t got = readlink(proc, exe, sizeof(exe) - 1);
    if (got <= 0 || (size_t)got >= sizeof(exe))
        return false;
    exe[got] = 0;
    const char *base = strrchr(exe, '/');
    return base && strcmp(base + 1, "zclassic23-dev") == 0;
}

/* A busy advisory lock is the ownership proof; the PID is diagnostic and is
 * additionally executable-checked before stop ever sends a signal. */
static bool dev_watcher_active(pid_t *pid_out)
{
    char lock[PATH_MAX], log[PATH_MAX], buf[64] = {0};
    if (pid_out)
        *pid_out = 0;
    if (!dev_watch_paths(lock, log))
        return false;
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        (void)flock(fd, LOCK_UN);
        close(fd);
        return false;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        close(fd);
        return false;
    }
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0)
        return false;
    char *end = NULL;
    long value = strtol(buf, &end, 10);
    if (!end || (*end != '\n' && *end != 0) || value <= 1)
        return false;
    pid_t pid = (pid_t)value;
    if (!dev_pid_is_watcher(pid))
        return false;
    if (pid_out)
        *pid_out = pid;
    return true;
}

static int64_t dev_cycle_epoch(void)
{
    char dir[PATH_MAX], path[PATH_MAX];
    struct stat st;
    if (!dev_state_dir(dir) ||
        snprintf(path, sizeof(path), "%s/native-cycle.json", dir) <= 0 ||
        stat(path, &st) != 0)
        return 0;
    return (int64_t)st.st_mtim.tv_sec * INT64_C(1000000000) +
           (int64_t)st.st_mtim.tv_nsec;
}

static bool dev_read_cycle(struct json_value *out)
{
    char dir[PATH_MAX], path[PATH_MAX], body[16384];
    json_init(out);
    if (!dev_state_dir(dir) ||
        snprintf(path, sizeof(path), "%s/native-cycle.json", dir) <= 0)
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t n = fread(body, 1, sizeof(body) - 1, f);
    bool complete = !ferror(f) && fgetc(f) == EOF;
    fclose(f);
    body[n] = 0;
    return complete && n > 0 && json_read(out, body, n) && out->type == JSON_OBJ;
}

static void dev_emit_loop_status(struct zcl_command_reply *reply)
{
    pid_t pid = 0;
    bool active = dev_watcher_active(&pid);
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_loop_status.v1");
    (void)json_push_kv_bool(&reply->data, "active", active);
    (void)json_push_kv_int(&reply->data, "watcher_id", (int64_t)pid);
    (void)json_push_kv_int(&reply->data, "epoch", dev_cycle_epoch());
    struct json_value cycle;
    if (dev_read_cycle(&cycle)) {
        (void)json_push_kv(&reply->data, "latest_verdict", &cycle);
        json_free(&cycle);
    }
}

void zcl_native_handle_dev_loop_ensure(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    pid_t existing = 0;
    if (dev_watcher_active(&existing)) {
        dev_emit_loop_status(reply);
        (void)json_push_kv_bool(&reply->data, "created", false);
        return;
    }
    const struct json_value *root_v = json_get(request->input, "root");
    const char *requested = root_v && root_v->type == JSON_STR
        ? json_get_str(root_v) : dev_source_root(request);
    char root[PATH_MAX], makefile[PATH_MAX], lock[PATH_MAX], log[PATH_MAX];
    if (!requested || !realpath(requested, root) ||
        snprintf(makefile, sizeof(makefile), "%s/Makefile", root) <= 0 ||
        access(makefile, R_OK) != 0 || !dev_watch_paths(lock, log)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_WATCH_ROOT",
                               "confinement", false, false,
                               "watch root must be a zclassic23 checkout", "root");
        return;
    }
    char state_dir[PATH_MAX];
    if (!dev_state_dir(state_dir) || !dev_mkdirs(state_dir)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "STATE_DIR_FAILED",
                               "start", false, false,
                               "could not prepare watcher state directory", "");
        return;
    }
    pid_t child = fork();
    if (child < 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "WATCH_FORK_FAILED",
                               "start", false, false,
                               "could not start native watcher", strerror(errno));
        return;
    }
    if (child == 0) {
        (void)setsid();
        int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        int log_fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        if (log_fd >= 0) {
            (void)dup2(log_fd, STDOUT_FILENO);
            (void)dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        int rc = zcl_devloop_watch(root);
        _exit(rc == 0 ? 0 : 1);
    }
    pid_t started = 0;
    for (int i = 0; i < 100 && !dev_watcher_active(&started); i++)
        usleep(20000);
    if (started <= 1) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "WATCH_START_FAILED",
                               "start", true, false,
                               "watcher did not acquire its singleton lock", log);
        return;
    }
    dev_emit_loop_status(reply);
    (void)json_push_kv_bool(&reply->data, "created", true);
    (void)json_push_kv_str(&reply->data, "root", root);
}

void zcl_native_handle_dev_loop_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    dev_emit_loop_status(reply);
}

static bool dev_input_int(const struct json_value *input, const char *key,
                          int64_t default_value, int64_t *out)
{
    const struct json_value *v = json_get(input, key);
    if (!v || v->type == JSON_NULL) {
        *out = default_value;
        return true;
    }
    if (v->type != JSON_INT)
        return false;
    *out = json_get_int(v);
    return true;
}

void zcl_native_handle_dev_loop_wait(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    int64_t after = 0, timeout_ms = 30000;
    if (!dev_input_int(request->input, "after_epoch", 0, &after) || after < 0 ||
        !dev_input_int(request->input, "timeout_ms", 30000, &timeout_ms) ||
        timeout_ms < 1 || timeout_ms > 300000) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_WAIT",
                               "normalize", false, false,
                               "after_epoch must be nonnegative and timeout_ms 1..300000",
                               "after_epoch,timeout_ms");
        return;
    }
    int64_t deadline_us = platform_time_monotonic_us() + timeout_ms * 1000;
    for (;;) {
        int64_t epoch = dev_cycle_epoch();
        if (epoch > after) {
            dev_emit_loop_status(reply);
            return;
        }
        if (platform_time_monotonic_us() >= deadline_us)
            break;
        usleep(25000);
    }
    int64_t current_epoch = dev_cycle_epoch();
    char evidence[96], next_input[160];
    (void)snprintf(evidence, sizeof(evidence), "current_epoch=%lld",
                   (long long)current_epoch);
    (void)snprintf(next_input, sizeof(next_input),
                   "{\"after_epoch\":%lld,\"timeout_ms\":30000}",
                   (long long)current_epoch);
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                           ZCL_COMMAND_EXIT_BLOCKED, "WAIT_TIMEOUT", "wait",
                           true, false, "no newer source verdict before timeout",
                           evidence);
    (void)zcl_command_reply_add_next(reply, "dev.loop.wait", next_input,
                                     "wait from the latest observed epoch");
}

void zcl_native_handle_dev_loop_stop(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    int64_t requested = 0;
    if (!dev_input_int(request->input, "watcher_id", 0, &requested) ||
        requested <= 1) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_WATCHER_ID",
                               "normalize", false, false,
                               "watcher_id must be the positive id returned by status",
                               "watcher_id");
        return;
    }
    pid_t active = 0;
    if (!dev_watcher_active(&active)) {
        dev_emit_loop_status(reply);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "WATCHER_NOT_RUNNING",
                               "stop", false, false,
                               "no native watcher owns the singleton lock", "");
        return;
    }
    if ((int64_t)active != requested || !dev_pid_is_watcher(active)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "WATCHER_ID_MISMATCH",
                               "confinement", false, false,
                               "refusing to signal a different process", "watcher_id");
        return;
    }
    if (kill(active, SIGTERM) != 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "WATCHER_STOP_FAILED",
                               "stop", true, false,
                               "SIGTERM could not be delivered", strerror(errno));
        return;
    }
    pid_t still = 0;
    for (int i = 0; i < 250; i++) {
        if (!dev_watcher_active(&still))
            break;
        usleep(20000);
    }
    dev_emit_loop_status(reply);
    if (dev_watcher_active(&still))
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "WATCHER_STOP_TIMEOUT",
                               "stop", true, false,
                               "watcher retained its lock after SIGTERM", "");
    else
        (void)json_push_kv_bool(&reply->data, "stopped", true);
}

static bool dev_group_valid(const char *group)
{
    return group && group[0] && strlen(group) < 128 &&
        strspn(group,
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
            strlen(group);
}

void zcl_native_handle_dev_test_run(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const struct json_value *group_v = json_get(request->input, "group");
    const char *group = group_v && group_v->type == JSON_STR
        ? json_get_str(group_v) : NULL;
    if (!dev_group_valid(group)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_TEST_GROUP",
                               "normalize", false, false,
                               "group must be one exact alphanumeric test group",
                               "group");
        return;
    }
    char root[PATH_MAX], bin[PATH_MAX], selector[160];
    if (!realpath(dev_source_root(request), root) ||
        snprintf(bin, sizeof(bin), "%s/build/bin/test_parallel_fast", root) <= 0 ||
        snprintf(selector, sizeof(selector), "--only=%s", group) <= 0 ||
        access(bin, X_OK) != 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "TEST_RUNNER_MISSING",
                               "precondition", true, false,
                               "prebuilt focused test runner is unavailable",
                               "run make test-parallel-fast");
        return;
    }
    const char *argv[] = {bin, selector, NULL};
    struct zcl_devloop_process_result result;
    if (!zcl_devloop_process_run(root, argv, 300000, &result)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "TEST_EXEC_FAILED",
                               "execute", true, false,
                               "could not execute focused test runner", group);
        return;
    }
    bool ok = result.exit_code == 0 && result.term_signal == 0 &&
              !result.timed_out;
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_focused_test.v1");
    (void)json_push_kv_str(&reply->data, "group", group);
    (void)json_push_kv_bool(&reply->data, "passed", ok);
    (void)json_push_kv_int(&reply->data, "elapsed_ms", result.elapsed_ms);
    (void)json_push_kv_int(&reply->data, "exit_code", result.exit_code);
    (void)json_push_kv_bool(&reply->data, "timed_out", result.timed_out);
    if (!ok) {
        const char *tail = result.output;
        if (result.output_len > 2048)
            tail += result.output_len - 2048;
        (void)json_push_kv_str(&reply->data, "output_tail", tail);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "FOCUSED_TEST_FAILED",
                               "prove", true, false,
                               "focused test group failed", group);
    }
}

void zcl_native_handle_dev_test_sim(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    char body[8192];
    int rc = ZCL_COMMAND_EXIT_INTERNAL;
    if (!dev_capture_stdout(dev_call_sim, (void *)dev_source_root(request),
                            body, sizeof(body), &rc)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "SIM_CAPTURE_FAILED",
                               "execute", false, false,
                               "native simulation produced no bounded result", "");
        return;
    }
    dev_reply_from_json(reply, body, strlen(body), "test.sim");
    if (reply->exit_code == ZCL_COMMAND_EXIT_OK && rc != 0)
        dev_fail_with_data(reply, rc, "SIM_FAILED", "prove", false,
                           "hot-swap simulation failed");
}

static bool dev_generation_root(char out[PATH_MAX])
{
    const char *override = getenv("ZCL_DEV_GENERATION_ROOT");
    const char *home = getenv("HOME");
    int n = override && override[0]
        ? snprintf(out, PATH_MAX, "%s", override)
        : home && home[0]
            ? snprintf(out, PATH_MAX, "%s/.local/lib/zclassic23-dev", home)
            : -1;
    return n > 0 && n < PATH_MAX && out[0] == '/' && !strstr(out, "..");
}

static bool dev_generation_name_valid(const char *name)
{
    if (!name || strchr(name, '/') || strlen(name) >= 96)
        return false;
    const char *hex = NULL;
    if (strncmp(name, "gen-", 4) == 0)
        hex = name + 4;
    else if (strncmp(name, "legacy-", 7) == 0)
        hex = name + 7;
    if (!hex || strlen(hex) != 64)
        return false;
    return strspn(hex, "0123456789abcdef") == 64;
}

static bool dev_read_generation_link(const char *root, const char *link_name,
                                     char out[96])
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", root, link_name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    ssize_t got = readlink(path, out, 95);
    if (got <= 0 || got >= 95)
        return false;
    out[got] = 0;
    return dev_generation_name_valid(out);
}

void zcl_native_handle_dev_generation_current(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    char root[PATH_MAX], current[96] = {0}, last_good[96] = {0}, staged[96] = {0};
    if (!dev_generation_root(root)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "GENERATION_ROOT_INVALID",
                               "read", false, false,
                               "development generation root is unavailable", "");
        return;
    }
    (void)dev_read_generation_link(root, "current", current);
    (void)dev_read_generation_link(root, "last-good", last_good);
    (void)dev_read_generation_link(root, "staged", staged);
    (void)json_push_kv_str(&reply->data, "schema",
                           "zcl.dev_generation_status.v1");
    (void)json_push_kv_str(&reply->data, "root", root);
    (void)json_push_kv_str(&reply->data, "current_generation", current);
    (void)json_push_kv_str(&reply->data, "last_good_generation", last_good);
    (void)json_push_kv_str(&reply->data, "staged_generation", staged);
    (void)json_push_kv_bool(&reply->data, "rollback_available",
                            last_good[0] && strcmp(current, last_good) != 0);
}

struct dev_generation_entry {
    char name[96];
    char disposition[16];
};

static int dev_generation_entry_cmp(const void *a, const void *b)
{
    const struct dev_generation_entry *ea = a;
    const struct dev_generation_entry *eb = b;
    int by_name = strcmp(eb->name, ea->name);
    return by_name ? by_name : strcmp(ea->disposition, eb->disposition);
}

static void dev_scan_generation_markers(const char *root, const char *subdir,
                                        const char *disposition,
                                        struct dev_generation_entry *entries,
                                        size_t capacity, size_t *count)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", root, subdir) <= 0)
        return;
    DIR *dir = opendir(path);
    if (!dir)
        return;
    struct dirent *item;
    while (*count < capacity && (item = readdir(dir)) != NULL) {
        size_t len = strlen(item->d_name);
        if (len <= 5 || strcmp(item->d_name + len - 5, ".json") != 0 ||
            len - 5 >= sizeof(entries[*count].name))
            continue;
        memcpy(entries[*count].name, item->d_name, len - 5);
        entries[*count].name[len - 5] = 0;
        if (!dev_generation_name_valid(entries[*count].name))
            continue;
        (void)snprintf(entries[*count].disposition,
                       sizeof(entries[*count].disposition), "%s", disposition);
        (*count)++;
    }
    closedir(dir);
}

void zcl_native_handle_dev_generation_history(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    char root[PATH_MAX];
    if (!dev_generation_root(root)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "GENERATION_ROOT_INVALID",
                               "read", false, false,
                               "development generation root is unavailable", "");
        return;
    }
    struct dev_generation_entry entries[512];
    size_t count = 0;
    dev_scan_generation_markers(root, "accepted", "accepted", entries,
                                512, &count);
    dev_scan_generation_markers(root, "rejected", "rejected", entries,
                                512, &count);
    qsort(entries, count, sizeof(entries[0]), dev_generation_entry_cmp);
    size_t offset = 0;
    if (request->cursor && request->cursor[0]) {
        char *end = NULL;
        unsigned long long parsed = strtoull(request->cursor, &end, 10);
        if (!end || *end || parsed > count) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "INVALID_CURSOR",
                                   "normalize", false, false,
                                   "cursor must be a valid numeric history offset",
                                   request->cursor);
            return;
        }
        offset = (size_t)parsed;
    }
    size_t limit = request->max_items ? request->max_items : 50;
    if (limit > 100)
        limit = 100;
    size_t end = offset + limit < count ? offset + limit : count;
    struct json_value rows;
    json_init(&rows);
    json_set_array(&rows);
    for (size_t i = offset; i < end; i++) {
        struct json_value row;
        json_init(&row);
        json_set_object(&row);
        (void)json_push_kv_str(&row, "generation", entries[i].name);
        (void)json_push_kv_str(&row, "disposition", entries[i].disposition);
        (void)json_push_back(&rows, &row);
        json_free(&row);
    }
    (void)json_push_kv_str(&reply->data, "schema",
                           "zcl.dev_generation_history.v1");
    (void)json_push_kv(&reply->data, "generations", &rows);
    json_free(&rows);
    (void)json_push_kv_int(&reply->data, "total", (int64_t)count);
    (void)json_push_kv_bool(&reply->data, "has_more", end < count);
    if (end < count) {
        char next[32];
        (void)snprintf(next, sizeof(next), "%zu", end);
        (void)json_push_kv_str(&reply->data, "next_cursor", next);
    }
}

void zcl_native_handle_dev_diagnose_latest(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    struct json_value cycle;
    if (!dev_read_cycle(&cycle)) {
        (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_failure.v1");
        (void)json_push_kv_str(&reply->data, "status", "unavailable");
        return;
    }
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_failure.v1");
    (void)json_push_kv(&reply->data, "latest_cycle", &cycle);
    json_free(&cycle);
}

#endif /* ZCL_DEV_BUILD */

/* ── dev.vcs.revert — relink activator seam ──────────────────────────
 * This wave (3.3) ships a SHELL FALLBACK: activation rebuilds the binary
 * from the just-reverted source tree and redeploys it via the same fixed
 * argv devloop's transactional-reload path uses
 * (tools/dev/devloop_cycle.c: `make agent-deploy-fast`) — never a shell
 * string, never touching lib/vcs/. It cannot yet tell a full
 * binary-generation hash apart from a bare hotswap .so hash (that
 * classification belongs to the not-yet-built generation store / native
 * dev_activation engine), so it always issues a full rebuild+redeploy from
 * the now-reverted source tree: always a safe way to activate ANY
 * generation, just not the minimal one for a hotswap-only generation.
 *
 * dev_vcs_revert_relink_ops() is the ONE seam every caller goes through —
 * swapping the shell fallback for the native
 * dev_activation_activate_generation() engine (once it lands) touches only
 * this function's body. */
#ifdef ZCL_DEV_BUILD
static bool dev_vcs_shell_fallback_activate(const uint8_t gen_sha256[32],
                                            void *ctx)
{
    (void)ctx;
    (void)gen_sha256; /* vcs_revert() already skips an all-zero hash; the
                       * shell fallback rebuilds from source regardless of
                       * which non-zero generation is bound. */
    const char *root = getenv("ZCL_DEV_SOURCE_ROOT");
    if (!root || !root[0])
        root = ".";
    const char *argv[] = {
        "make", "--no-print-directory", "agent-deploy-fast", NULL
    };
    struct zcl_devloop_process_result result;
    if (!zcl_devloop_process_run(root, argv, 900000, &result))
        return false;
    return result.exit_code == 0 && !result.timed_out &&
           result.term_signal == 0;
}

static struct vcs_revert_relink_ops dev_vcs_revert_relink_ops(void)
{
    return (struct vcs_revert_relink_ops){
        .activate_generation = dev_vcs_shell_fallback_activate,
        .ctx = NULL,
    };
}
#endif /* ZCL_DEV_BUILD */

void zcl_native_handle_dev_vcs_revert(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
#ifndef ZCL_DEV_BUILD
    (void)request;
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
        "DEV_BUILD_REQUIRED", "dispatch", false, false,
        "one-command source+binary revert requires a dev build",
        "make dev-bin, or zclassic23-dev");
#else
    if (!reply)
        return;
    if (!request || !request->input) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_REQUEST",
                               "normalize", false, false,
                               "missing request input", "");
        return;
    }

    const char *to_hex = json_get_str(json_get(request->input, "to"));
    bool relink_generation =
        json_get_bool(json_get(request->input, "relink_generation"));

    uint8_t target[32];
    if (!to_hex || strlen(to_hex) != 64 || !IsHex(to_hex) ||
        ParseHex(to_hex, target, sizeof(target)) != sizeof(target)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_COMMIT_ID", "normalize", false, false,
            "'to' must be a 64-char hex ZVCS commit id",
            to_hex ? to_hex : "");
        return;
    }

    const char *root = (request->context && request->context->source_root &&
                        request->context->source_root[0])
                           ? request->context->source_root
                           : ".";
    struct vcs_repo *r = vcs_open(root);
    if (!r) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "VCS_OPEN_FAILED",
                               "execute", false, false,
                               "could not open the ZVCS repo at source_root",
                               root);
        return;
    }

    struct vcs_revert_relink_ops ops = dev_vcs_revert_relink_ops();
    uint8_t new_commit[32] = {0};
    int rc = vcs_revert(r, target, relink_generation ? &ops : NULL,
                        new_commit);
    vcs_close(r);

    /* Only VCS_OK / VCS_EPARTIAL actually write out_new_commit (vcs_revert
     * forwards VCS_REFUSED / VCS_ERR before the forward commit lands), so
     * the hex form is computed lazily per-branch below, never over an
     * unwritten buffer. */
    char new_hex[65];
    static const char hexd[] = "0123456789abcdef";

    switch (rc) {
    case VCS_OK:
        for (int i = 0; i < 32; i++) {
            new_hex[2 * i] = hexd[(new_commit[i] >> 4) & 0xf];
            new_hex[2 * i + 1] = hexd[new_commit[i] & 0xf];
        }
        new_hex[64] = '\0';
        (void)json_push_kv_str(&reply->data, "to", to_hex);
        (void)json_push_kv_str(&reply->data, "forward_commit", new_hex);
        (void)json_push_kv_bool(&reply->data, "relink_generation",
                                relink_generation);
        (void)json_push_kv_str(&reply->data, "status", "reverted");
        return;
    case VCS_EPARTIAL:
        for (int i = 0; i < 32; i++) {
            new_hex[2 * i] = hexd[(new_commit[i] >> 4) & 0xf];
            new_hex[2 * i + 1] = hexd[new_commit[i] & 0xf];
        }
        new_hex[64] = '\0';
        (void)json_push_kv_str(&reply->data, "to", to_hex);
        (void)json_push_kv_str(&reply->data, "forward_commit", new_hex);
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "RELINK_ACTIVATION_FAILED", "execute", true, true,
            "source revert + forward commit landed (append-only, never "
            "undone), but binary-generation activation failed",
            new_hex);
        return;
    case VCS_REFUSED:
        (void)json_push_kv_str(&reply->data, "to", to_hex);
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "SEALED_PATH_REFUSED", "execute", false, true,
            "revert would change a sealed path; run the owner-gated "
            "unseal ritual first",
            to_hex);
        return;
    default:
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "REVERT_FAILED",
                               "execute", false, false,
                               "vcs_revert failed (bad commit id or a "
                               "worktree I/O error)",
                               to_hex);
        return;
    }
#endif
}
