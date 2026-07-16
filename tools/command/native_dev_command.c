/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the registry-owned `dev` tree
 * (docs/NATIVE_COMMAND_INTERFACE.md §7). These bind the registry catalog's dev
 * subtree to the checkout-local read-only producers: App manifest describe /
 * plan / simulate, source-change classification, the Core/App boundary law,
 * the latest native cycle verdict, and dev.vcs.revert. Source-only append-only
 * reverts remain available; generation relinking is contained until immutable
 * source epochs and complete publication proof receipts are transactional.
 *
 * The read-only checkout producers compile into both binaries.  Executors and
 * watcher/generation lifecycle handlers live below ZCL_DEV_BUILD, so a release
 * binary can describe the same grammar but cannot spawn, mutate, or activate
 * anything in the development lane. */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "dev_activation.h"
#include "dev_failure_store.h"
#include "devloop.h"
#include "kernel/command_registry.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "vcs/vcs.h"
#include "vcs/vcs_seal.h"

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
    char body[16384], why[160] = {0};
    size_t len = 0;
    enum zcl_devloop_state_lookup lookup = zcl_devloop_cycle_state_read(
        dev_source_root(request), body, sizeof(body), &len, NULL,
        why, sizeof(why));
    if (lookup == ZCL_DEVLOOP_STATE_FOUND) {
        struct json_value doc;
        json_init(&doc);
        if (json_read(&doc, body, len) && doc.type == JSON_OBJ) {
            json_free(&reply->data);
            json_init(&reply->data);
            json_copy(&reply->data, &doc);
            json_free(&doc);
            return;
        }
        json_free(&doc);
        lookup = ZCL_DEVLOOP_STATE_INVALID;
        (void)snprintf(why, sizeof(why), "%s", "cycle_state_decode_failed");
    }
    if (lookup == ZCL_DEVLOOP_STATE_INVALID) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
            "DEV_CYCLE_STATE_INVALID", "read", false, false,
            "workspace cycle state failed schema, SHA3, or inode validation",
            why[0] ? why : "cycle_state_invalid");
        return;
    }
    /* No durable verdict yet — a bounded, honest "unavailable" is passing. */
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_cycle.v1");
    (void)json_push_kv_str(&reply->data, "status", "unavailable");
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           "keep editing; the native watcher records verdicts");
}

/* ── dev.ff ────────────────────────────────────────────────────────────
 * Thin wrapper around `make ff` (Makefile: compile -> focused tests ->
 * lint-fast, cost-ordered and short-circuiting). Runs via the same
 * zcl_devloop_process_run() subprocess primitive the reload/redeploy path
 * uses (see dev_vcs_shell_fallback_activate() above), which is DEV_ONLY
 * linked (Makefile DEV_ONLY_SRCS) — so a release build never spawns this. */
void zcl_native_handle_dev_ff(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply)
{
#ifndef ZCL_DEV_BUILD
    (void)request;
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
        "DEV_BUILD_REQUIRED", "dispatch", false, false,
        "the fail-fast ladder requires a dev build",
        "make dev-bin, or zclassic23-dev dev ff");
#else
    char root[PATH_MAX];
    const char *src_root = dev_source_root(request);
    if (!realpath(src_root, root)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ROOT_RESOLVE_FAILED",
                               "normalize", false, false,
                               "could not resolve the checkout root", src_root);
        return;
    }
    const char *argv[] = { "make", "--no-print-directory", "ff", NULL };
    struct zcl_devloop_process_result result;
    if (!zcl_devloop_process_run(root, argv, 600000, &result)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "FF_EXEC_FAILED",
                               "execute", true, false,
                               "could not execute the fail-fast ladder", "");
        return;
    }
    bool ok = result.exit_code == 0 && result.term_signal == 0 &&
              !result.timed_out;
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_ff.v1");
    (void)json_push_kv_bool(&reply->data, "passed", ok);
    (void)json_push_kv_int(&reply->data, "elapsed_ms", result.elapsed_ms);
    (void)json_push_kv_int(&reply->data, "exit_code", result.exit_code);
    (void)json_push_kv_bool(&reply->data, "timed_out", result.timed_out);
    if (!ok) {
        const char *tail = result.output;
        if (result.output_len > 2048)
            tail += result.output_len - 2048;
        (void)json_push_kv_str(&reply->data, "output_tail", tail);
        /* The failing envelope drops reply->data (see serialize_reply()), so
         * carry the ladder's own dense "FIRST-ERROR[<rung>]: ..." line (see
         * tools/agent_fast_ci.sh) in the error evidence, where it is
         * actually rendered. Falls back to a short output tail if the
         * marker was not printed (e.g. a killed/timed-out subprocess). */
        char evidence[256];
        const char *marker = strstr(result.output, "FIRST-ERROR[");
        if (marker) {
            const char *eol = strchr(marker, '\n');
            size_t len = eol ? (size_t)(eol - marker) : strlen(marker);
            if (len >= sizeof(evidence))
                len = sizeof(evidence) - 1;
            memcpy(evidence, marker, len);
            evidence[len] = 0;
        } else {
            (void)snprintf(evidence, sizeof(evidence), "%s",
                           result.output_len > sizeof(evidence) - 1
                               ? result.output +
                                     (result.output_len - (sizeof(evidence) - 1))
                               : result.output);
        }
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "FF_LADDER_FAILED",
                               "prove", true, false,
                               "fail-fast ladder failed", evidence);
    }
#endif
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
    (void)request;
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
        "RUNTIME_PUBLICATION_CONTAINED", "authority", false, false,
        "development generation publication is contained until immutable "
        "source epochs, complete proof receipts, resident CAS, and rollback "
        "are one durable transaction",
        "use dev.change.plan and the verify-only watcher to produce candidate evidence");
    return;

    /* Future transaction body. The unconditional authority gate above reaches
     * no build, loader, service-control, or generation-activation side effect. */
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

static bool dev_watch_paths(const char *repo_root,
                            char lock[PATH_MAX], char log[PATH_MAX])
{
    char dir[PATH_MAX];
    if (!repo_root || !repo_root[0] || !dev_state_dir(dir) ||
        !zcl_devloop_watch_lock_path(repo_root, lock, PATH_MAX))
        return false;
    int on = snprintf(log, PATH_MAX, "%s/native-watch.log", dir);
    return on > 0 && on < PATH_MAX;
}

static bool dev_legacy_watch_lock_path(char lock[PATH_MAX])
{
    char dir[PATH_MAX];
    if (!dev_state_dir(dir))
        return false;
    int n = snprintf(lock, PATH_MAX, "%s/native-watch.lock", dir);
    return n > 0 && n < PATH_MAX;
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
    /* A live watcher whose on-disk binary was replaced (every rebuild of
     * build/bin/zclassic23-dev does exactly this while the watcher runs)
     * shows up as "<path> (deleted)" in /proc/<pid>/exe. Strip that suffix
     * before the basename compare so the watcher stays recognized across a
     * dev-binary rebuild — otherwise `dev loop status` reports active:false
     * and `dev loop ensure` tries to spawn a duplicate against the held
     * singleton lock. */
    static const char kDeleted[] = " (deleted)";
    size_t dlen = sizeof(kDeleted) - 1;
    if ((size_t)got >= dlen && strcmp(exe + got - dlen, kDeleted) == 0)
        exe[got - dlen] = 0;
    const char *base = strrchr(exe, '/');
    return base && strcmp(base + 1, "zclassic23-dev") == 0;
}

static bool dev_pid_cwd_matches_root(pid_t pid, const char *repo_root)
{
    if (pid <= 1 || !repo_root || !repo_root[0])
        return false;
    char proc[64], cwd[PATH_MAX], root[PATH_MAX];
    int n = snprintf(proc, sizeof(proc), "/proc/%ld/cwd", (long)pid);
    if (n <= 0 || n >= (int)sizeof(proc) || !realpath(repo_root, root))
        return false;
    ssize_t got = readlink(proc, cwd, sizeof(cwd) - 1);
    if (got <= 0 || (size_t)got >= sizeof(cwd))
        return false;
    cwd[got] = 0;
    return strcmp(cwd, root) == 0;
}

struct dev_watcher_info {
    pid_t pid;
    enum zcl_devloop_publish_mode publish_mode;
    char mode_name[16];
};

/* A busy advisory lock is the ownership proof; the PID is diagnostic and is
 * executable-checked only before stop ever sends a signal.  This deliberately
 * recognizes both the native watcher and the shell compatibility watcher so
 * either owner excludes the other.  New lock records bind the watcher mode
 * (`pid verify|auto`).  A pid-only record is an already-running
 * pre-containment watcher, whose historical behavior was auto publication, so
 * it is reported truthfully as legacy-auto. */
static bool dev_watcher_active_at(const char *lock,
                                  struct dev_watcher_info *info_out)
{
    char buf[64] = {0};
    if (info_out)
        memset(info_out, 0, sizeof(*info_out));
    if (!lock || !lock[0])
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
    if (!end || value <= 1)
        return false;
    while (*end == ' ' || *end == '\t')
        end++;
    enum zcl_devloop_publish_mode publish_mode = ZCL_DEVLOOP_PUBLISH_APPLY;
    const char *mode_name = "legacy-auto";
    if (*end != '\n' && *end != 0) {
        char *mode_end = end;
        while (*mode_end && *mode_end != '\n' && *mode_end != ' ' &&
               *mode_end != '\t')
            mode_end++;
        size_t mode_len = (size_t)(mode_end - end);
        if (mode_len == strlen("verify") &&
            memcmp(end, "verify", mode_len) == 0) {
            publish_mode = ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY;
            mode_name = "verify";
        } else if (mode_len == strlen("auto") &&
                   memcmp(end, "auto", mode_len) == 0) {
            publish_mode = ZCL_DEVLOOP_PUBLISH_APPLY;
            mode_name = "auto";
        } else {
            return false;
        }
        while (*mode_end == ' ' || *mode_end == '\t')
            mode_end++;
        if (*mode_end != '\n' && *mode_end != 0)
            return false;
    }
    pid_t pid = (pid_t)value;
    if (info_out) {
        info_out->pid = pid;
        info_out->publish_mode = publish_mode;
        (void)snprintf(info_out->mode_name, sizeof(info_out->mode_name), "%s",
                       mode_name);
    }
    return true;
}

static bool dev_watcher_active(const char *repo_root,
                               struct dev_watcher_info *info_out)
{
    char lock[PATH_MAX], log[PATH_MAX], legacy[PATH_MAX];
    bool have_worktree_lock = dev_watch_paths(repo_root, lock, log);
    if (have_worktree_lock && dev_watcher_active_at(lock, info_out))
        return true;
    /* Transitional compatibility for a watcher started by a pre-singleflight
     * binary.  Scope the legacy HOME-global lease back to that process's
     * actual cwd so it cannot serialize unrelated worktrees.  New watchers
     * never take the legacy lock. */
    struct dev_watcher_info old = {0};
    if (!dev_legacy_watch_lock_path(legacy) ||
        (have_worktree_lock && strcmp(legacy, lock) == 0) ||
        !dev_watcher_active_at(legacy, &old) ||
        !dev_pid_is_watcher(old.pid) ||
        !dev_pid_cwd_matches_root(old.pid, repo_root))
        return false;
    if (info_out)
        *info_out = old;
    return true;
}

static enum zcl_devloop_state_lookup dev_read_cycle(
    const char *repo_root, struct json_value *out, int64_t *epoch_out,
    char *why, size_t why_len)
{
    char body[16384];
    size_t len = 0;
    json_init(out);
    enum zcl_devloop_state_lookup lookup = zcl_devloop_cycle_state_read(
        repo_root, body, sizeof(body), &len, epoch_out, why, why_len);
    if (lookup != ZCL_DEVLOOP_STATE_FOUND)
        return lookup;
    if (!json_read(out, body, len) || out->type != JSON_OBJ) {
        json_free(out);
        if (why && why_len)
            (void)snprintf(why, why_len, "%s", "cycle_state_decode_failed");
        return ZCL_DEVLOOP_STATE_INVALID;
    }
    return ZCL_DEVLOOP_STATE_FOUND;
}

/* `dev.loop.wait` declares zcl.dev_cycle.v1, so return that cycle directly.
 * Loop identity belongs to `dev.loop.status`; nesting the cycle in a
 * zcl.dev_loop_status.v1 document made callers violate the declared schema
 * and spend an extra parse step to reach the verdict.  The file-generation
 * epoch is appended so the returned document can feed the next wait without
 * another status round trip. */
static void dev_emit_cycle_verdict(struct zcl_command_reply *reply,
                                   struct json_value *cycle, int64_t epoch)
{
    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, cycle);
    if (!json_get(&reply->data, "epoch"))
        (void)json_push_kv_int(&reply->data, "epoch", epoch);
}

static void dev_emit_loop_status(const char *repo_root,
                                 struct zcl_command_reply *reply)
{
    struct dev_watcher_info info = {0};
    bool active = dev_watcher_active(repo_root, &info);
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_loop_status.v1");
    (void)json_push_kv_bool(&reply->data, "active", active);
    (void)json_push_kv_int(&reply->data, "watcher_id", (int64_t)info.pid);
    (void)json_push_kv_str(&reply->data, "mode",
                           active ? info.mode_name : "");
    (void)json_push_kv_bool(
        &reply->data, "runtime_publication",
        active && zcl_devloop_publish_mode_applies(info.publish_mode));
    int64_t epoch = 0;
    struct json_value cycle;
    char why[160] = {0};
    enum zcl_devloop_state_lookup lookup =
        dev_read_cycle(repo_root, &cycle, &epoch, why, sizeof(why));
    if (lookup == ZCL_DEVLOOP_STATE_INVALID) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
            "DEV_CYCLE_STATE_INVALID", "read", false, false,
            "workspace cycle state failed schema, SHA3, or inode validation",
            why[0] ? why : "cycle_state_invalid");
        return;
    }
    (void)json_push_kv_int(&reply->data, "epoch", epoch);
    if (lookup == ZCL_DEVLOOP_STATE_FOUND) {
        (void)json_push_kv(&reply->data, "latest_verdict", &cycle);
        json_free(&cycle);
    } else
        json_free(&cycle);
}

static bool dev_requested_watch_mode(const struct json_value *input,
                                     enum zcl_devloop_publish_mode *mode_out,
                                     const char **name_out)
{
    const struct json_value *mode_v = json_get(input, "mode");
    const char *mode = mode_v && mode_v->type == JSON_STR
        ? json_get_str(mode_v) : "verify";
    if (strcmp(mode, "verify") == 0) {
        *mode_out = ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY;
        *name_out = "verify";
        return true;
    }
    if (strcmp(mode, "auto") == 0 || strcmp(mode, "apply") == 0) {
        *mode_out = ZCL_DEVLOOP_PUBLISH_APPLY;
        *name_out = "auto";
        return true;
    }
    return false;
}

void zcl_native_handle_dev_loop_ensure(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    enum zcl_devloop_publish_mode requested_mode;
    const char *requested_mode_name = NULL;
    if (!dev_requested_watch_mode(request->input, &requested_mode,
                                  &requested_mode_name)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_WATCH_MODE",
                               "normalize", false, false,
                               "mode must be verify",
                               "mode");
        return;
    }
    if (zcl_devloop_publish_mode_applies(requested_mode)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "RUNTIME_PUBLICATION_CONTAINED", "authority", false, false,
            "apply/auto watchers are contained until immutable source epochs, "
            "complete proof receipts, resident CAS, and rollback are one durable transaction",
            "start the watcher with mode=verify");
        return;
    }
    const struct json_value *root_v = json_get(request->input, "root");
    const char *requested = root_v && root_v->type == JSON_STR
        ? json_get_str(root_v) : dev_source_root(request);
    char root[PATH_MAX], makefile[PATH_MAX], lock[PATH_MAX], log[PATH_MAX];
    if (!requested || !realpath(requested, root) ||
        snprintf(makefile, sizeof(makefile), "%s/Makefile", root) <= 0 ||
        access(makefile, R_OK) != 0 || !dev_watch_paths(root, lock, log)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_WATCH_ROOT",
                               "confinement", false, false,
                               "watch root must be a zclassic23 checkout", "root");
        return;
    }
    struct dev_watcher_info existing = {0};
    if (dev_watcher_active(root, &existing)) {
        if (existing.publish_mode != requested_mode) {
            char evidence[96];
            (void)snprintf(evidence, sizeof(evidence),
                           "running_mode=%s requested_mode=%s watcher_id=%ld",
                           existing.mode_name, requested_mode_name,
                           (long)existing.pid);
            dev_emit_loop_status(root, reply);
            zcl_command_reply_fail(
                reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                "WATCHER_MODE_MISMATCH", "ownership", false, false,
                "existing watcher must be stopped before changing publication mode",
                evidence);
            return;
        }
        dev_emit_loop_status(root, reply);
        (void)json_push_kv_bool(&reply->data, "created", false);
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
        int rc = zcl_devloop_watch_mode(root, requested_mode);
        _exit(rc == 0 ? 0 : 1);
    }
    struct dev_watcher_info started = {0};
    for (int i = 0; i < 100 && !dev_watcher_active(root, &started); i++)
        usleep(20000);
    if (started.pid <= 1 || started.publish_mode != requested_mode ||
        !dev_pid_is_watcher(started.pid)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "WATCH_START_FAILED",
                               "start", true, false,
                               "watcher did not acquire its singleton lock", log);
        return;
    }
    dev_emit_loop_status(root, reply);
    (void)json_push_kv_bool(&reply->data, "created", true);
    (void)json_push_kv_str(&reply->data, "root", root);
}

void zcl_native_handle_dev_loop_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    dev_emit_loop_status(dev_source_root(request), reply);
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
    const char *repo_root = dev_source_root(request);
    int64_t current_epoch = 0;
    for (;;) {
        struct json_value cycle;
        char why[160] = {0};
        int64_t epoch = 0;
        enum zcl_devloop_state_lookup lookup =
            dev_read_cycle(repo_root, &cycle, &epoch, why, sizeof(why));
        current_epoch = epoch;
        if (lookup == ZCL_DEVLOOP_STATE_INVALID) {
            zcl_command_reply_fail(
                reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                "DEV_CYCLE_STATE_INVALID", "read", false, false,
                "workspace cycle state failed schema, SHA3, or inode validation",
                why[0] ? why : "cycle_state_invalid");
            return;
        }
        if (lookup == ZCL_DEVLOOP_STATE_FOUND && epoch > after) {
            dev_emit_cycle_verdict(reply, &cycle, epoch);
            json_free(&cycle);
            return;
        }
        json_free(&cycle);
        if (platform_time_monotonic_us() >= deadline_us)
            break;
        usleep(25000);
    }
    char evidence[128];
    (void)snprintf(evidence, sizeof(evidence), "current_epoch=%lld",
                   (long long)current_epoch);
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                           ZCL_COMMAND_EXIT_BLOCKED, "WAIT_TIMEOUT", "wait",
                           true, false, "no newer cycle verdict before timeout",
                           evidence);
    (void)zcl_command_reply_add_next(
        reply, "dev.loop.status", "{}",
        "inspect the latest epoch before deciding whether to wait again");
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
    struct dev_watcher_info active = {0};
    const char *repo_root = dev_source_root(request);
    if (!dev_watcher_active(repo_root, &active)) {
        dev_emit_loop_status(repo_root, reply);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "WATCHER_NOT_RUNNING",
                               "stop", false, false,
                               "no native watcher owns the singleton lock", "");
        return;
    }
    if ((int64_t)active.pid != requested || !dev_pid_is_watcher(active.pid)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "WATCHER_ID_MISMATCH",
                               "confinement", false, false,
                               "refusing to signal a different process", "watcher_id");
        return;
    }
    if (kill(active.pid, SIGTERM) != 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "WATCHER_STOP_FAILED",
                               "stop", true, false,
                               "SIGTERM could not be delivered", strerror(errno));
        return;
    }
    struct dev_watcher_info still = {0};
    for (int i = 0; i < 250; i++) {
        if (!dev_watcher_active(repo_root, &still))
            break;
        usleep(20000);
    }
    dev_emit_loop_status(repo_root, reply);
    if (dev_watcher_active(repo_root, &still))
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

#endif /* ZCL_DEV_BUILD */

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)

void zcl_native_handle_dev_diagnose_latest(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!reply)
        return;
    struct zcl_dev_failure_record record;
    char why[192] = {0};
    enum zcl_dev_failure_lookup lookup =
        zcl_dev_failure_read_latest(dev_source_root(request), &record,
                                    why, sizeof(why));
    if (lookup == ZCL_DEV_FAILURE_LOOKUP_ABSENT) {
        (void)json_push_kv_str(&reply->data, "schema",
                               "zcl.dev_failure_latest_result.v1");
        (void)json_push_kv_bool(&reply->data, "found", false);
        return;
    }
    if (lookup != ZCL_DEV_FAILURE_LOOKUP_FOUND) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
            "FAILURE_STORE_INVALID", "read", false, false,
            "latest failure state failed inode or SHA3 validation",
            why[0] ? why : "latest_failure_invalid");
        return;
    }
    (void)json_push_kv_str(&reply->data, "schema",
                           "zcl.dev_failure_latest_result.v1");
    (void)json_push_kv_bool(&reply->data, "found", true);
    (void)json_push_kv_str(&reply->data, "failure_id", record.failure_id);
    (void)json_push_kv_str(&reply->data, "phase", record.phase);
    (void)json_push_kv_str(&reply->data, "first_error", record.first_error);
    (void)json_push_kv_int(
        &reply->data, "repeat_count",
        record.repeat_count > INT64_MAX ? INT64_MAX
                                        : (int64_t)record.repeat_count);
    char input[96];
    (void)snprintf(input, sizeof(input), "{\"failure_id\":\"%s\"}",
                   record.failure_id);
    (void)zcl_command_reply_add_next(
        reply, "dev.diagnose.show", input,
        "inspect the most recently recorded deterministic compiler failure");
}

static bool dev_failure_id_valid(const char *failure_id)
{
    if (!failure_id || strlen(failure_id) != ZCL_DEV_FAILURE_HEX_LEN)
        return false;
    return strspn(failure_id, "0123456789abcdef") ==
           ZCL_DEV_FAILURE_HEX_LEN;
}

void zcl_native_handle_dev_diagnose_show(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!reply)
        return;
    const struct json_value *id_value =
        request && request->input ? json_get(request->input, "failure_id")
                                  : NULL;
    const char *failure_id =
        id_value && id_value->type == JSON_STR ? json_get_str(id_value) : NULL;
    if (!dev_failure_id_valid(failure_id)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_FAILURE_ID",
                               "normalize", false, false,
                               "failure_id must be 64 lowercase hex characters",
                               "failure_id");
        return;
    }
    struct zcl_dev_failure_record record;
    char why[192] = {0};
    enum zcl_dev_failure_lookup lookup =
        zcl_dev_failure_read(dev_source_root(request), failure_id, &record,
                             why, sizeof(why));
    if (lookup == ZCL_DEV_FAILURE_LOOKUP_ABSENT) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
            "FAILURE_NOT_FOUND", "read", false, false,
            "no durable failure exists for this workspace-scoped ID",
            failure_id);
        (void)snprintf(reply->error.failure_id,
                       sizeof(reply->error.failure_id), "%s", failure_id);
        (void)zcl_command_reply_add_next(
            reply, "dev.diagnose.latest", "{}",
            "inspect the most recently recorded failure for this workspace");
        return;
    }
    if (lookup != ZCL_DEV_FAILURE_LOOKUP_FOUND) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
            "FAILURE_STORE_INVALID", "read", false, false,
            "durable failure failed inode or SHA3 validation",
            why[0] ? why : "failure_record_invalid");
        (void)snprintf(reply->error.failure_id,
                       sizeof(reply->error.failure_id), "%s", failure_id);
        return;
    }
    const char *view = request && request->view && request->view[0]
                           ? request->view : "normal";
    bool summary = strcmp(view, "summary") == 0;
    bool full = strcmp(view, "full") == 0;
    (void)json_push_kv_str(&reply->data, "schema",
                           "zcl.dev_failure_show.v1");
    (void)json_push_kv_bool(&reply->data, "found", true);
    (void)json_push_kv_str(&reply->data, "failure_id", record.failure_id);
    (void)json_push_kv_str(&reply->data, "phase", record.phase);
    (void)json_push_kv_str(&reply->data, "first_error", record.first_error);
    (void)json_push_kv_int(
        &reply->data, "repeat_count",
        record.repeat_count > INT64_MAX ? INT64_MAX
                                        : (int64_t)record.repeat_count);
    if (!summary) {
        (void)json_push_kv_str(&reply->data, "record_sha3",
                               record.record_digest);
        (void)json_push_kv_str(&reply->data, "workspace_id",
                               record.workspace_id);
        (void)json_push_kv_str(&reply->data, "source_id_sha256",
                               record.source_id);
        (void)json_push_kv_str(&reply->data,
                               "first_source_mutation_sha256",
                               record.first_source_mutation);
        (void)json_push_kv_str(&reply->data, "first_execution_id_sha3",
                               record.first_execution_id);
        (void)json_push_kv_int(&reply->data, "first_seen_unix_ms",
                               record.first_seen_unix_ms);
        (void)json_push_kv_bool(&reply->data, "capsule_available",
                                record.capsule[0] != 0);
    }
    if (full) {
        (void)json_push_kv_str(&reply->data, "failure_capsule",
                               record.capsule);
        (void)json_push_kv_str(&reply->data, "retry_command",
                               record.retry_command);
    }
    (void)zcl_command_reply_add_next(
        reply, "dev.ff", "{}",
        "rerun the current checkout's fail-fast ladder without coalescing");
}

#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

/* ── dev.vcs.revert — relink activator seam ──────────────────────────
 * These activators are retained for the future transactional implementation,
 * but deliberately have no caller: relink_generation is refused before VCS
 * mutation below.
 *
 *   - dev_vcs_shell_fallback_activate() (wave 3.3, the long-standing
 *     default): rebuilds the binary from the just-reverted source tree and
 *     redeploys it via the same fixed argv devloop's transactional-reload
 *     path uses (tools/dev/devloop_cycle.c: `make agent-deploy-fast`) —
 *     never a shell string, never touching lib/vcs/. It cannot tell a full
 *     binary-generation hash apart from a bare hotswap .so hash, so it
 *     always issues a full rebuild+redeploy from the now-reverted source
 *     tree: always a safe way to activate ANY generation, just not the
 *     minimal one for a hotswap-only generation.
 *
 *   - dev_vcs_native_activate() (wave 3.2 engine, ZCL_DEV_NATIVE_ACTIVATION
 *     opt-in): calls dev_activation_activate_generation() directly against
 *     the already-staged gen-<sha> directory — no rebuild, no redeploy
 *     shell-out. Unlike the shell fallback it DOES tell the two hash kinds
 *     apart: dev_activation_activate_generation() requires
 *     gen_root/gen-<sha>/zclassic23-dev to already exist and match the
 *     requested sha, so a hotswap-anchored commit (whose generation_sha256
 *     addresses a standalone .so, never staged as a full binary directory)
 *     correctly fails staging and this function returns false — vcs_revert
 *     then reports VCS_EPARTIAL exactly per vcs.h's documented contract,
 *     rather than the shell fallback's blunter "always rebuild" guess.
 *
 * The dormant selector picks between them at call time via
 * dev_activation_native_enabled() (the same runtime env switch
 * devloop_cycle.c's transactional-reload site uses) — default OFF, so
 * today's shell-fallback behavior is unchanged unless the dev lane opts in. */
#ifdef ZCL_DEV_BUILD
static bool dev_capture_source_identity(const char *root, char out[65])
{
    char tool[PATH_MAX];
    int n = snprintf(tool, sizeof(tool), "%s/tools/dev/source-identity.sh",
                     root);
    if (n <= 0 || (size_t)n >= sizeof(tool))
        return false;
    const char *argv[] = { tool, "capture", NULL };
    struct zcl_devloop_process_result result = {0};
    if (!zcl_devloop_process_run(root, argv, 30000, &result) ||
        result.exit_code != 0 || result.timed_out || result.term_signal != 0 ||
        result.output_truncated)
        return false;
    size_t len = result.output_len;
    while (len > 0 && (result.output[len - 1] == '\n' ||
                       result.output[len - 1] == '\r'))
        len--;
    if (len != 64 || strspn(result.output, "0123456789abcdefABCDEF") < len)
        return false;
    memcpy(out, result.output, len);
    out[len] = 0;
    return true;
}

static bool dev_verify_source_identity(const char *root,
                                       const char identity[65])
{
    char tool[PATH_MAX];
    int n = snprintf(tool, sizeof(tool), "%s/tools/dev/source-identity.sh",
                     root);
    if (n <= 0 || (size_t)n >= sizeof(tool))
        return false;
    const char *argv[] = { tool, "verify", identity, NULL };
    struct zcl_devloop_process_result result = {0};
    return zcl_devloop_process_run(root, argv, 30000, &result) &&
           result.exit_code == 0 && !result.timed_out &&
           result.term_signal == 0;
}

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
    char identity[65], source_arg[96];
    if (!dev_capture_source_identity(root, identity))
        return false;
    int n = snprintf(source_arg, sizeof(source_arg), "ZCL_DEV_SOURCE_ID=%s",
                     identity);
    if (n <= 0 || (size_t)n >= sizeof(source_arg))
        return false;
    const char *argv[] = {
        "make", "--no-print-directory", "agent-deploy-fast", source_arg,
        NULL
    };
    struct zcl_devloop_process_result result;
    if (!zcl_devloop_process_run(root, argv, 900000, &result))
        return false;
    return result.exit_code == 0 && !result.timed_out &&
           result.term_signal == 0;
}

static bool dev_vcs_native_activate(const uint8_t gen_sha256[32], void *ctx)
{
    (void)ctx;
    const char *root = getenv("ZCL_DEV_SOURCE_ROOT");
    if (!root || !root[0])
        root = ".";
    /* No build/rebuild here — dev_activation_activate_generation() never
     * builds, it only relinks an already-staged generation. build_commit is
     * "" (not NULL): the staged generation's own manifest already carries
     * its build_commit, and dev_op_preflight() skips the expected-commit
     * comparison entirely when passed an empty string (see
     * tools/dev/dev_activation_ops.c). */
    struct dev_activation_cycle_request creq;
    if (!dev_activation_request_from_cycle(root, "", &creq))
        return false;
    char identity[65];
    if (!dev_capture_source_identity(root, identity))
        return false;
    creq.req.source_identity = identity;
    struct dev_activation_ops ops;
    dev_activation_default_ops(&creq.req, &ops);
    struct dev_activation_result result = {0};
    if (!dev_verify_source_identity(root, identity))
        return false;
    int rc = dev_activation_activate_generation(gen_sha256, &creq.req, &ops,
                                                &result);
    return rc == DEV_ACTIVATION_OK;
}

static __attribute__((unused)) struct vcs_revert_relink_ops
dev_vcs_revert_relink_ops(void)
{
    if (dev_activation_native_enabled())
        return (struct vcs_revert_relink_ops){
            .activate_generation = dev_vcs_native_activate,
            .ctx = NULL,
        };
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
    if (relink_generation) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "RUNTIME_PUBLICATION_CONTAINED", "authority", false, false,
            "source revert with generation relinking is contained until "
            "immutable source epochs, proof receipts, resident CAS, and "
            "rollback are one durable transaction",
            "retry with relink_generation=false to create only the append-only source revert");
        return;
    }

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

    uint8_t new_commit[32] = {0};
    int rc = vcs_revert(r, target, NULL, new_commit);
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

/* ── dev.vcs.seal.grant — owner-run ZVCS unseal-token ritual ─────────
 * ZVCS's seal pin (lib/vcs/src/vcs_seal.c: pin in index.kv, one-shot token
 * via vcs_seal_grant_unseal(), VCS_SEAL_TOKEN_KEY) has NO operator surface —
 * vcs_seal_grant_unseal() has zero callers outside lib/test. This executor
 * IS that surface, mirroring the core-unseal Makefile ritual's shape
 * (mandatory reason, append-only record, one-shot token, "no agent source
 * edit can produce this — owner make target") but for the ZVCS pin instead
 * of core/MANIFEST.sha3.
 *
 * lib/vcs/ stays git-free and process-spawn-free (the ZVCS sovereignty
 * gate): this file computes the CURRENT worktree's sealset with the exact
 * same primitives vcs_snapshot() itself uses (vcs_manifest_build +
 * vcs_seal_load_globs + vcs_sealset_hash), calls vcs_seal_grant_unseal() to
 * mint the one-shot token, then appends an audit record (reason + old/new
 * sealset hex + timestamp) into index.kv's meta table. meta is a flat
 * key->value store with no native append primitive (vcs_index.h), so the
 * append-safe idiom is one key per grant — "seal_grant_log_<N>" — with
 * "seal_grant_count" tracking N, written in one begin/commit transaction. */
#ifdef ZCL_DEV_BUILD
static void dev_vcs_seal_iso_utc_now(char out[32])
{
    time_t t = platform_time_wall_time_t();
    struct tm tmv;
    if (gmtime_r(&t, &tmv))
        strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    else
        snprintf(out, 32, "1970-01-01T00:00:00Z");
}

static uint64_t dev_vcs_seal_grant_count(struct vcs_index *idx)
{
    uint8_t buf[8] = {0};
    size_t len = 0;
    bool found = false;
    if (!vcs_index_meta_get(idx, "seal_grant_count", buf, sizeof(buf), &len,
                            &found) ||
        !found || len != sizeof(buf))
        return 0;
    uint64_t n = 0;
    for (int i = 0; i < 8; i++)
        n |= (uint64_t)buf[i] << (8 * i);
    return n;
}

/* Append one audit record and advance the counter in a single txn. Returns
 * false on any write failure (the token itself is already granted by this
 * point — a logging failure is reported to the caller as a partial-mutation
 * BLOCKED result, never silently dropped). */
static bool dev_vcs_seal_grant_log(struct vcs_index *idx, const char *reason,
                                   const char *old_hex, const char *new_hex,
                                   const char *ts, char *out_key,
                                   size_t out_key_sz)
{
    uint64_t n = dev_vcs_seal_grant_count(idx);
    if (snprintf(out_key, out_key_sz, "seal_grant_log_%llu",
                (unsigned long long)n) <= 0)
        return false;

    char record[1024];
    int rn = snprintf(record, sizeof(record),
                      "ts=%s\nreason=%s\nold_sealset=%s\nnew_sealset=%s\n",
                      ts, reason, old_hex, new_hex);
    if (rn <= 0)
        return false;
    size_t rlen = (size_t)rn < sizeof(record) ? (size_t)rn : sizeof(record) - 1;

    uint8_t next[8];
    uint64_t nn = n + 1;
    for (int i = 0; i < 8; i++)
        next[i] = (uint8_t)((nn >> (8 * i)) & 0xff);

    if (!vcs_index_begin(idx))
        return false;
    if (!vcs_index_meta_set_in_tx(idx, out_key, record, rlen) ||
        !vcs_index_meta_set_in_tx(idx, "seal_grant_count", next,
                                  sizeof(next))) {
        vcs_index_rollback(idx);
        return false;
    }
    return vcs_index_commit(idx);
}
#endif /* ZCL_DEV_BUILD */

void zcl_native_handle_dev_vcs_seal_grant(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
#ifndef ZCL_DEV_BUILD
    (void)request;
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
        "DEV_BUILD_REQUIRED", "dispatch", false, false,
        "granting a ZVCS unseal token requires a dev build",
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

    const char *reason = json_get_str(json_get(request->input, "reason"));
    bool confirm = json_get_bool(json_get(request->input, "confirm"));

    if (!reason || !reason[0]) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "REASON_REQUIRED", "normalize", true, false,
            "'reason' is required — record why this sealed-path change is "
            "authorized",
            "");
        return;
    }
    if (!confirm) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "CONFIRM_REQUIRED", "normalize", true, false,
            "granting a ZVCS unseal token requires 'confirm':true — this "
            "authorizes exactly the CURRENT tree's sealed content for the "
            "next green-cycle anchor",
            reason);
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
    struct vcs_index *idx = vcs_repo_index(r);

    /* Compute the sealset the worktree would produce right now — the exact
     * same computation vcs_snapshot() performs before its own seal check. */
    struct vcs_manifest m;
    if (!vcs_manifest_build(root, idx, &m)) {
        vcs_close(r);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "MANIFEST_BUILD_FAILED", "execute", false,
                               false,
                               "could not build the current worktree manifest",
                               "");
        return;
    }
    char **globs = NULL;
    size_t nglobs = 0;
    if (!vcs_seal_load_globs(root, &globs, &nglobs)) {
        vcs_manifest_free(&m);
        vcs_close(r);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "SEAL_GLOBS_FAILED", "execute", false, false,
                               "could not load the sealed-path glob set", "");
        return;
    }
    uint8_t new_sealset[32];
    bool sh = vcs_sealset_hash(&m, globs, nglobs, new_sealset);
    vcs_seal_free_globs(globs, nglobs);
    vcs_manifest_free(&m);
    if (!sh) {
        vcs_close(r);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "SEALSET_HASH_FAILED", "execute", false, false,
                               "could not compute the current sealset hash",
                               "");
        return;
    }

    uint8_t old_pin[32] = {0};
    bool have_old = false;
    (void)vcs_index_seal_pin_get(idx, old_pin, &have_old);

    if (!vcs_seal_grant_unseal(idx, new_sealset)) {
        vcs_close(r);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "GRANT_FAILED",
                               "execute", false, false,
                               "vcs_seal_grant_unseal failed", "");
        return;
    }

    char new_hex[65];
    HexStr(new_sealset, sizeof(new_sealset), false, new_hex, sizeof(new_hex));
    char old_hex[65];
    if (have_old)
        HexStr(old_pin, sizeof(old_pin), false, old_hex, sizeof(old_hex));
    else
        snprintf(old_hex, sizeof(old_hex), "none");

    char ts[32];
    dev_vcs_seal_iso_utc_now(ts);

    char log_key[64];
    bool logged = dev_vcs_seal_grant_log(idx, reason, old_hex, new_hex, ts,
                                         log_key, sizeof(log_key));
    vcs_close(r);

    (void)json_push_kv_str(&reply->data, "reason", reason);
    (void)json_push_kv_str(&reply->data, "old_sealset", old_hex);
    (void)json_push_kv_str(&reply->data, "granted_sealset", new_hex);
    (void)json_push_kv_str(&reply->data, "granted_at", ts);
    (void)json_push_kv_str(&reply->data, "log_key", logged ? log_key : "");
    (void)json_push_kv_str(&reply->data, "status", "granted");
    (void)json_push_kv_str(
        &reply->data, "note",
        "one-shot: the next green-cycle anchor (vcs_snapshot, e.g. via the "
        "dev change/apply cycle) consumes this token and re-pins the "
        "sealset; a FURTHER sealed-path change after that requires a new "
        "grant");

    if (!logged) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "LOG_WRITE_FAILED", "execute", true, true,
            "token was granted but the audit-log record failed to write",
            new_hex);
    }
#endif
}
