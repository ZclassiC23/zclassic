/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Debug bundle — one-shot diagnostic capture for postmortems.
 *
 * One call (or one supervisor stall) writes ONE JSON document to
 * <datadir>/debug-bundle-<utc>.json containing:
 *
 *   - format / captured_at / captured_at_utc / trigger metadata;
 *   - build: the binary identity block (version, subversion,
 *     source_id_sha256, build_commit) shared with the other agent
 *     surfaces (node_binary_identity_json.c);
 *   - supervisor_stalls: every liveness child with stall_fires > 0 or a
 *     non-NONE stall_reason (name, reason, fires, tick age, progress);
 *   - subsystems: EVERY registered g_dumpers[] entry's state, keyed by
 *     subsystem name (the same bodies `dumpstate <name>` returns).
 *
 * Two entry points share debug_bundle_write():
 *
 *   1. The `debugbundle` RPC (ops.debug.bundle native command) — trigger
 *      "manual".
 *   2. The supervisor-stall auto-capture — debug_bundle_on_stall is
 *      registered as the process-wide supervisor stall observer at boot
 *      (diagnostics_controller_set_state). It runs on the DETECTING
 *      thread (for sweep stalls: the supervisor thread that drives every
 *      child's on_tick), so it never writes the bundle inline: it
 *      rate-limits (one capture per DEBUG_BUNDLE_AUTO_MIN_INTERVAL_SECS,
 *      one in flight at a time) and hands off to a detached helper
 *      thread. Every failure on that path is logged and swallowed —
 *      auto-capture can never crash, wedge, or spam the supervisor.
 *
 * Lives here (not diagnostics_registry.c) so that file stays a routing
 * table — same precedent as diagnostics_registry_bundle.c and
 * diagnostics_health_rollup.c, whose "unhealthy" rollup already walks
 * every dumper with a NULL key on RPC threads; this walk has the same
 * reentrancy profile (dumpers are required to be reentrant-safe, see
 * AGENTS.md "Adding state introspection"). No log scraping, no DB writes;
 * state dumpers only. */

#include "controllers/diagnostics_internal.h"

#include "controllers/node_binary_identity_json.h"
#include "controllers/strong_params.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DBB_SUBSYS "debug_bundle"

/* Auto-capture rate limit (monotonic clock): at most one supervisor-stall
 * bundle per interval, so a flapping child cannot fill the datadir.
 * Manual ops.debug.bundle calls are never rate-limited. */
#define DEBUG_BUNDLE_AUTO_MIN_INTERVAL_SECS 300

/* ── Bundle document sections ──────────────────────────────────────── */

static void debug_bundle_push_supervisor_stalls(struct json_value *root)
{
    struct supervisor_snapshot snap[SUPERVISOR_CAP];
    int n = supervisor_snapshot_all(snap, SUPERVISOR_CAP);

    struct json_value summary;
    json_init(&summary);
    json_set_object(&summary);
    json_push_kv_int(&summary, "child_count", n);

    struct json_value children;
    json_init(&children);
    json_set_array(&children);
    int fired = 0;
    for (int i = 0; i < n; i++) {
        if (snap[i].stall_fires == 0 &&
            snap[i].stall_reason == SUPERVISOR_STALL_NONE)
            continue;   /* quiet child: no stall history, not stalled now */
        fired++;
        struct json_value child;
        json_init(&child);
        json_set_object(&child);
        json_push_kv_str(&child, "name", snap[i].name);
        json_push_kv_str(&child, "stall_reason",
                         supervisor_stall_reason_name(
                             (enum supervisor_stall_reason)
                             snap[i].stall_reason));
        json_push_kv_int(&child, "stall_fires", (int64_t)snap[i].stall_fires);
        json_push_kv_int(&child, "last_tick_age_us",
                         snap[i].last_tick_age_us);
        json_push_kv_int(&child, "progress_marker", snap[i].progress_marker);
        json_push_back(&children, &child);
        json_free(&child);
    }
    json_push_kv_int(&summary, "stalled_or_fired_count", fired);
    json_push_kv(&summary, "children", &children);
    json_free(&children);
    json_push_kv(root, "supervisor_stalls", &summary);
    json_free(&summary);
}

static void debug_bundle_push_subsystems(struct json_value *root,
                                         struct debug_bundle_result *res)
{
    struct json_value subs;
    json_init(&subs);
    json_set_object(&subs);

    size_t total = diagnostics_dumper_count();
    for (size_t i = 0; i < total; i++) {
        const struct diagnostics_dump_entry *e = diagnostics_dumper_at(i);
        if (!e || !e->name || !e->fn)
            continue;

        struct json_value scratch;
        json_init(&scratch);
        json_set_object(&scratch);
        if (e->fn(&scratch, NULL)) {
            json_push_kv(&subs, e->name, &scratch);
            res->subsystems_captured++;
        } else {
            /* A failing dump is diagnostic signal, not a bundle failure:
             * degrade that one entry to an error marker and continue. */
            json_free(&scratch);
            json_init(&scratch);
            json_set_object(&scratch);
            json_push_kv_str(&scratch, "error",
                             "dump function returned false");
            json_push_kv(&subs, e->name, &scratch);
            res->subsystems_failed++;
        }
        json_free(&scratch);
    }

    json_push_kv(root, "subsystems", &subs);
    json_free(&subs);
}

/* ── The writer (shared by manual RPC + stall auto-capture) ────────── */

bool debug_bundle_write(const char *trigger, const char *trigger_child,
                        int trigger_reason,
                        struct debug_bundle_result *res)
{
    if (!res)
        LOG_FAIL(DBB_SUBSYS, "result out-param is NULL");
    memset(res, 0, sizeof(*res));

    const char *datadir = diag_datadir();
    if (!datadir || !datadir[0])
        LOG_FAIL(DBB_SUBSYS, "cannot write debug bundle: datadir not set");

    int64_t wall = (int64_t)platform_time_wall_time_t();
    time_t wall_t = (time_t)wall;
    struct tm tm_utc;
    char utc_iso[24] = {0};
    char utc_name[24] = {0};
    if (!gmtime_r(&wall_t, &tm_utc) ||
        strftime(utc_iso, sizeof(utc_iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0 ||
        strftime(utc_name, sizeof(utc_name), "%Y%m%dT%H%M%SZ", &tm_utc) == 0)
        LOG_FAIL(DBB_SUBSYS, "UTC timestamp formatting failed");

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_str(&root, "format", "zcl.debug_bundle.v1");
    json_push_kv_int(&root, "captured_at", wall);
    json_push_kv_str(&root, "captured_at_utc", utc_iso);
    json_push_kv_str(&root, "trigger",
                     trigger && trigger[0] ? trigger : "manual");
    if (trigger_child && trigger_child[0]) {
        json_push_kv_str(&root, "trigger_child", trigger_child);
        json_push_kv_str(&root, "trigger_stall_reason",
                         supervisor_stall_reason_name(
                             (enum supervisor_stall_reason)trigger_reason));
    }
    node_binary_identity_push_json(&root, "build", true);
    debug_bundle_push_supervisor_stalls(&root);
    debug_bundle_push_subsystems(&root, res);
    json_push_kv_int(&root, "subsystems_captured",
                     res->subsystems_captured);
    json_push_kv_int(&root, "subsystems_failed", res->subsystems_failed);

    /* Serialize (sizing probe, then exact buffer). */
    size_t need = json_write(&root, NULL, 0);
    char *buf = zcl_malloc(need + 1, "debug_bundle_json");
    if (!buf) {
        json_free(&root);
        LOG_FAIL(DBB_SUBSYS, "allocating %zu bytes for bundle JSON failed",
                 need + 1);
    }
    size_t wrote = json_write(&root, buf, need + 1);
    json_free(&root);
    if (wrote >= need + 1) {   /* truncated — unreachable with exact sizing */
        free(buf);
        LOG_FAIL(DBB_SUBSYS, "bundle JSON truncated (%zu >= %zu)",
                 wrote, need + 1);
    }

    /* Create <datadir>/debug-bundle-<utc>.json (O_EXCL; -N suffix on
     * collision, same convention as self_backtrace's backtrace-<ts>.log). */
    int fd = -1;
    for (int k = 0; k < 1000; k++) {
        if (k == 0)
            snprintf(res->path, sizeof(res->path),
                     "%s/debug-bundle-%s.json", datadir, utc_name);
        else
            snprintf(res->path, sizeof(res->path),
                     "%s/debug-bundle-%s-%d.json", datadir, utc_name, k);
        fd = open(res->path,
                  O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0)
            break;
        if (errno != EEXIST) {
            free(buf);
            LOG_FAIL(DBB_SUBSYS, "open %s failed: %s", res->path,
                     strerror(errno));
        }
    }
    if (fd < 0) {
        free(buf);
        LOG_FAIL(DBB_SUBSYS, "no free debug-bundle filename under %s",
                 datadir);
    }

    int io_errno = 0;
    size_t off = 0;
    while (off < wrote) {
        ssize_t n = write(fd, buf + off, wrote - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            io_errno = errno;
            break;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0 && io_errno == 0)
        io_errno = errno;
    free(buf);
    if (io_errno != 0) {
        (void)unlink(res->path);   /* never leave a half-written bundle */
        res->path[0] = '\0';
        LOG_FAIL(DBB_SUBSYS, "writing bundle file failed: %s",
                 strerror(io_errno));
    }

    res->bytes = (int64_t)wrote;
    return true;
}

/* ── RPC: debugbundle ────────────────────────────────────────────────
 *
 * Backs the `ops.debug.bundle` native command: capture everything the node
 * knows about its own state into one JSON file and return the path. The
 * typed answer to "collect complete node state for a postmortem". */
bool diag_rpc_debugbundle(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "debugbundle\n"
        "\nWrite a one-shot debug bundle (every registered state dumper, the\n"
        "build identity, and the supervisor stall summary) as ONE JSON\n"
        "document to <datadir>/debug-bundle-<utc>.json. Also written\n"
        "automatically (rate-limited) when the supervisor detects a stall.\n"
        "\nResult: { path, bytes, subsystems_captured, subsystems_failed, "
        "trigger }.");

    struct debug_bundle_result res;
    if (!debug_bundle_write("manual", NULL, SUPERVISOR_STALL_NONE, &res)) {
        json_set_object(result);
        json_push_kv_str(result, "error",
                         "debug bundle write failed (see node log)");
        return false;
    }
    json_set_object(result);
    json_push_kv_str(result, "path", res.path);
    json_push_kv_int(result, "bytes", res.bytes);
    json_push_kv_int(result, "subsystems_captured",
                     (int64_t)res.subsystems_captured);
    json_push_kv_int(result, "subsystems_failed",
                     (int64_t)res.subsystems_failed);
    json_push_kv_str(result, "trigger", "manual");
    return true;
}

/* ── Supervisor-stall auto-capture ─────────────────────────────────── */

static _Atomic int64_t g_last_auto_us = 0;
static atomic_flag     g_auto_in_progress = ATOMIC_FLAG_INIT;
/* Hand-off slots, written by the observer while it holds
 * g_auto_in_progress (the flag serializes: only one capture can be
 * pending or running at a time, so the helper thread reads these before
 * any subsequent observer call could overwrite them). */
static char            g_pending_child[SUPERVISOR_NAME_MAX];
static int             g_pending_reason;

static void *debug_bundle_auto_thread(void *arg)
{
    (void)arg;
    char child[SUPERVISOR_NAME_MAX];
    memcpy(child, g_pending_child, sizeof(child));
    int reason = g_pending_reason;

    /* A capture requested during shutdown is dropped: bundle writes are
     * diagnostics, not state, and must not extend the shutdown path. */
    if (!thread_registry_shutdown_requested()) {
        struct debug_bundle_result res;
        if (debug_bundle_write("supervisor_stall", child, reason, &res)) {
            LOG_INFO(DBB_SUBSYS,
                     "stall auto-capture wrote %s (child=%s reason=%s "
                     "bytes=%lld failed_dumpers=%d)",
                     res.path, child,
                     supervisor_stall_reason_name(
                         (enum supervisor_stall_reason)reason),
                     (long long)res.bytes, res.subsystems_failed);
        }
        /* The failure path already logged with context inside
         * debug_bundle_write — swallowed here: best-effort by contract. */
    }
    atomic_flag_clear_explicit(&g_auto_in_progress, memory_order_release);
    return NULL;
}

void debug_bundle_on_stall(const char *child_name,
                           enum supervisor_stall_reason reason)
{
    /* Runs on the detecting thread — for sweep-detected stalls that is the
     * supervisor thread, which dispatches EVERY child's on_tick. Everything
     * up to the pthread_create is O(1) atomic work; the bundle write itself
     * happens on the detached helper. */
    if (atomic_flag_test_and_set_explicit(&g_auto_in_progress,
                                          memory_order_acquire))
        return;   /* a capture is already pending or running */

    int64_t now = platform_time_monotonic_us();
    int64_t last = atomic_load_explicit(&g_last_auto_us,
                                        memory_order_acquire);
    if (last != 0 &&
        now - last <
            (int64_t)DEBUG_BUNDLE_AUTO_MIN_INTERVAL_SECS * 1000000) {
        atomic_flag_clear_explicit(&g_auto_in_progress,
                                   memory_order_release);
        return;   /* rate-limited */
    }

    const char *datadir = diag_datadir();
    if (!datadir || !datadir[0]) {
        atomic_flag_clear_explicit(&g_auto_in_progress,
                                   memory_order_release);
        LOG_WARN(DBB_SUBSYS,
                 "stall auto-capture skipped: datadir not set (child=%s)",
                 child_name ? child_name : "?");
        return;
    }

    snprintf(g_pending_child, sizeof(g_pending_child), "%s",
             child_name ? child_name : "");
    g_pending_reason = (int)reason;

    pthread_attr_t attr;
    pthread_t tid;
    bool spawned = false;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0) {
            /* raw-pthread-ok: one-shot detached capture helper, rate-limited best-effort — must not join the supervised tree it observes. */
            spawned = pthread_create(&tid, &attr,
                                     debug_bundle_auto_thread, NULL) == 0;
        }
        pthread_attr_destroy(&attr);
    }
    if (!spawned) {
        atomic_flag_clear_explicit(&g_auto_in_progress,
                                   memory_order_release);
        LOG_WARN(DBB_SUBSYS,
                 "stall auto-capture: helper spawn failed (child=%s) — "
                 "capture dropped", g_pending_child);
        return;
    }
    atomic_store_explicit(&g_last_auto_us, now, memory_order_release);
}

void debug_bundle_register_stall_observer(void)
{
    supervisor_set_stall_observer(debug_bundle_on_stall);
}
