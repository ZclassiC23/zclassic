/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE
#include "devloop.h"

#include "codeindex/codeindex_merkle.h"
#include "hotswap/hotswap_service.h"
#include "json/json.h"
#include "platform/time_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ZCL_DEV_BUILD

#define DEVLOOP_MAX_WATCHES 512
#define DEVLOOP_MUTATION_MASK                                                \
    (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE)

struct watched_dir {
    int wd;
    char rel[ZCL_DEVLOOP_PATH_MAX];
};

struct watch_context {
    int fd;
    char root[PATH_MAX];
    struct watched_dir dirs[DEVLOOP_MAX_WATCHES];
    size_t dir_count;
    char changed[ZCL_DEVLOOP_MAX_FILES][ZCL_DEVLOOP_PATH_MAX];
    size_t changed_count;
    struct zcl_devloop_restart_source_set restart_sources;
    bool force_full_source_rescan;
    uint64_t mutation_sequence;
};

static volatile sig_atomic_t g_watch_stop;

static bool watch_c_source(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n > 2 && path[n - 2] == '.' && path[n - 1] == 'c';
}

static bool watch_epoch_all_c(const char *const *paths, size_t path_count)
{
    if (!paths || path_count == 0)
        return false;
    for (size_t i = 0; i < path_count; i++)
        if (!watch_c_source(paths[i]))
            return false;
    return true;
}

/* A public service contract header is intentionally outside the live island:
 * changing ABI/schema/wire/KAT bytes invalidates the resident frozen contract.
 * Persist a typed DEV_RESTART selection without falling through to the legacy
 * make/shell proof path.  No proof is claimed and no node is restarted here. */
static int service_contract_restart_event(
    const char *root, const char *const *files, size_t count)
{
    const char *contract_path = NULL;
    const char *service_source = NULL;
    for (size_t i = 0; i < count && !service_source; i++) {
        service_source =
            zcl_hotswap_service_contract_source_for_path(files[i]);
        if (service_source) contract_path = files[i];
    }
    if (!service_source)
        return 0;
    char body[4096];
    int n = snprintf(
        body, sizeof(body),
        "{\"schema\":\"zcl.dev_cycle.v1\","
        "\"producer\":\"resident-build-authority\","
        "\"status\":\"blocked\",\"action\":\"reload\","
        "\"reload_lane\":\"DEV_RESTART\","
        "\"reason\":\"service_contract_changed\","
        "\"phase\":\"dev_restart_selected\","
        "\"runtime_published\":false,\"dev_restart\":true,"
        "\"proof_complete\":false,\"immediate_proof_complete\":false,"
        "\"integration_proof_deferred\":true,"
        "\"bounded_proof_deferred\":true,"
        "\"make_processes\":0,\"shell_processes\":0,"
        "\"lto_processes\":0,\"compiler_processes\":0,"
        "\"linker_processes\":0,\"test_processes\":0,"
        "\"contract_path\":\"%s\",\"service_source\":\"%s\","
        "\"failure_capsule\":\"frozen ABI/schema/wire/KAT contract changed; live service publication refused\","
        "\"why_not_live\":\"frozen ABI/schema/wire/KAT contract changed; live service publication refused\","
        "\"agent_next_action\":\"run make dev-bin to refresh the bounded DEV_RESTART plan, then rerun mapped proofs\"}",
        contract_path, service_source);
    if (n <= 0 || n >= (int)sizeof(body))
        return -1;
    char why[160] = {0};
    if (!zcl_devloop_cycle_state_write(root, body, (size_t)n, why,
                                       sizeof(why))) {
        fprintf(stderr, "[devloop] contract restart receipt failed: %s\n",
                why[0] ? why : "unknown");
        return -1;
    }
    (void)fwrite(body, 1, (size_t)n, stdout);
    (void)fputc('\n', stdout);
    (void)fflush(stdout);
    return 1;
}

static void mutation_sequence_advance(struct watch_context *ctx)
{
    if (ctx && ctx->mutation_sequence < UINT64_MAX)
        ctx->mutation_sequence++;
}

static void watch_signal(int sig)
{
    (void)sig;
    g_watch_stop = 1;
    zcl_devloop_process_cancel_request();
}

static void print_json_string(FILE *stream, const char *value)
{
    (void)fputc('"', stream);
    for (const unsigned char *p =
             (const unsigned char *)(value ? value : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            (void)fprintf(stream, "\\%c", *p);
        else if (*p < 0x20)
            (void)fprintf(stream, "\\u%04x", *p);
        else
            (void)fputc(*p, stream);
    }
    (void)fputc('"', stream);
}

static bool watch_emit_edit_seen(const struct watch_context *ctx)
{
    if (!ctx || ctx->changed_count == 0)
        return false;
    struct json_value doc, files;
    json_init(&doc); json_set_object(&doc);
    json_init(&files); json_set_array(&files);
    bool ok = json_push_kv_str(&doc, "schema", "zcl.dev_cycle.v1") &&
        json_push_kv_str(&doc, "producer", "reflex-reactor") &&
        json_push_kv_str(&doc, "status", "edit_seen") &&
        json_push_kv_str(&doc, "action", "reflex") &&
        json_push_kv_str(&doc, "reason", "source_mutation_observed") &&
        json_push_kv_str(&doc, "phase", "EDIT_SEEN") &&
        json_push_kv_bool(&doc, "runtime_published", false) &&
        json_push_kv_bool(&doc, "proof_complete", false) &&
        json_push_kv_int(&doc, "elapsed_us", 0) &&
        json_push_kv_int(&doc, "file_count",
                         (int64_t)ctx->changed_count);
    for (size_t i = 0; ok && i < ctx->changed_count; i++) {
        struct json_value item;
        json_init(&item); json_set_str(&item, ctx->changed[i]);
        ok = json_push_back(&files, &item);
        json_free(&item);
    }
    ok = ok && json_push_kv(&doc, "files", &files) &&
        json_push_kv_str(&doc, "agent_next_action",
                         "impact analysis is running in the resident reactor");
    json_free(&files);
    char body[16384];
    size_t n = ok ? json_write(&doc, body, sizeof(body) - 1) : 0;
    json_free(&doc);
    if (!n)
        return false;
    body[n] = 0;
    char why[160] = {0};
    if (!zcl_devloop_cycle_state_write(ctx->root, body, n, why,
                                       sizeof(why))) {
        fprintf(stderr, "[devloop] EDIT_SEEN persistence failed: %s\n",
                why[0] ? why : "unknown");
        return false;
    }
    (void)fwrite(body, 1, n, stdout);
    (void)fputc('\n', stdout);
    (void)fflush(stdout);
    return true;
}

static bool mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    if (!path || !path[0] || strlen(path) >= sizeof(tmp))
        return false;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static bool ignored_dir(const char *name)
{
    return zcl_devloop_watch_dir_is_ignored(name);
}

static bool relevant_file(const char *path)
{
    /* Shared with the dev-platform unit test — see
     * zcl_devloop_path_is_relevant() in devloop_plan.c. Keeps the watcher's
     * change filter (including the transient lint-fixture exclusion) in one
     * testable, pure place. */
    return zcl_devloop_path_is_relevant(path);
}

static struct watched_dir *find_watch(struct watch_context *ctx, int wd)
{
    for (size_t i = 0; i < ctx->dir_count; i++) {
        if (ctx->dirs[i].wd == wd)
            return &ctx->dirs[i];
    }
    return NULL;
}

static bool add_watch_recursive(struct watch_context *ctx, const char *rel)
{
    if (!ctx || ctx->dir_count >= DEVLOOP_MAX_WATCHES)
        return false;
    char full[PATH_MAX];
    int n = rel && rel[0]
        ? snprintf(full, sizeof(full), "%s/%s", ctx->root, rel)
        : snprintf(full, sizeof(full), "%s", ctx->root);
    if (n <= 0 || (size_t)n >= sizeof(full))
        return false;

    int wd = inotify_add_watch(ctx->fd, full,
        DEVLOOP_MUTATION_MASK | IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd < 0)
        return false;
    struct watched_dir *slot = &ctx->dirs[ctx->dir_count++];
    slot->wd = wd;
    snprintf(slot->rel, sizeof(slot->rel), "%s", rel ? rel : "");

    DIR *dir = opendir(full);
    if (!dir)
        return true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (ignored_dir(entry->d_name))
            continue;
        char child_full[PATH_MAX], child_rel[ZCL_DEVLOOP_PATH_MAX];
        int fn = snprintf(child_full, sizeof(child_full), "%s/%s",
                          full, entry->d_name);
        int rn = rel && rel[0]
            ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel,
                       entry->d_name)
            : snprintf(child_rel, sizeof(child_rel), "%s", entry->d_name);
        if (fn <= 0 || (size_t)fn >= sizeof(child_full) ||
            rn <= 0 || (size_t)rn >= sizeof(child_rel))
            continue;
        struct stat st;
        if (lstat(child_full, &st) == 0 && S_ISDIR(st.st_mode) &&
            !S_ISLNK(st.st_mode)) {
            if (!add_watch_recursive(ctx, child_rel)) {
                closedir(dir);
                return false;
            }
        }
    }
    closedir(dir);
    return true;
}

static void add_changed(struct watch_context *ctx, const char *path)
{
    if (!ctx || !relevant_file(path))
        return;
    for (size_t i = 0; i < ctx->changed_count; i++) {
        if (strcmp(ctx->changed[i], path) == 0)
            return;
    }
    if (ctx->changed_count >= ZCL_DEVLOOP_MAX_FILES) {
        /* A broad/overflowing edit must fail toward reload, never silently
         * drop paths and accidentally qualify for hot-swap. */
        ctx->changed_count = 1;
        snprintf(ctx->changed[0], sizeof(ctx->changed[0]), "%s", "Makefile");
        return;
    }
    snprintf(ctx->changed[ctx->changed_count],
             sizeof(ctx->changed[ctx->changed_count]), "%s", path);
    ctx->changed_count++;
}

static bool collect_events(struct watch_context *ctx)
{
    char buffer[64 * 1024];
    bool saw = false;
    for (;;) {
        ssize_t n = read(ctx->fd, buffer, sizeof(buffer));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        for (char *p = buffer; p < buffer + n; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            p += sizeof(*ev) + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) {
                mutation_sequence_advance(ctx);
                ctx->force_full_source_rescan = true;
                add_changed(ctx, "Makefile");
                saw = true;
                continue;
            }
            struct watched_dir *dir = find_watch(ctx, ev->wd);
            if (!dir)
                continue;
            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                mutation_sequence_advance(ctx);
                ctx->force_full_source_rescan = true;
                add_changed(ctx, "Makefile");
                saw = true;
            }
            if (ev->len == 0)
                continue;
            char rel[ZCL_DEVLOOP_PATH_MAX];
            int rn = dir->rel[0]
                ? snprintf(rel, sizeof(rel), "%s/%s", dir->rel, ev->name)
                : snprintf(rel, sizeof(rel), "%s", ev->name);
            if (rn <= 0 || (size_t)rn >= sizeof(rel))
                continue;
            if (ev->mask & IN_ISDIR) {
                /* The recursive watch deliberately never enters generated,
                 * dependency, or dot-prefixed scratch directories. Their
                 * create/remove traffic is equally irrelevant: treating it
                 * as a synthetic Makefile edit cancels the exact proof that
                 * created a test scratch directory in the first place. */
                if (ignored_dir(ev->name))
                    continue;
                mutation_sequence_advance(ctx);
                add_changed(ctx, "Makefile");
                saw = true;
                if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) &&
                    !add_watch_recursive(ctx, rel)) {
                    ctx->force_full_source_rescan = true;
                }
                if (ev->mask & (IN_DELETE | IN_MOVED_FROM))
                    ctx->force_full_source_rescan = true;
                continue;
            }
            if (!(ev->mask & IN_ISDIR) &&
                zcl_devloop_watch_event_is_mutation(ev->mask)) {
                mutation_sequence_advance(ctx);
                size_t before = ctx->changed_count;
                add_changed(ctx, rel);
                saw = saw || ctx->changed_count != before;
            }
        }
    }
    return saw;
}

/* Watches are armed before this runs. The reconciled refresh validates the
 * SHA3 seal, enumerates and stats the current policy inventory, and performs a
 * complete byte pass only when the prior image is absent/invalid or its
 * inventory moved. Any mutation racing that work is already queued in inotify
 * and is drained before the image can be described as trusted. */
static bool prime_source_snapshot(struct watch_context *ctx)
{
    int64_t started_us = platform_time_monotonic_us();
    struct ci_merkle_cost cost = {0};
    struct ci_merkle *tree =
        ci_merkle_refresh_reconciled(ctx->root, &cost);
    if (!tree)
        return false;
    struct ci_merkle_node root = {0};
    bool ok = ci_merkle_root(tree, &root);
    char root_hex[65] = {0};
    if (ok)
        ci_merkle_hex(&root.digest, root_hex);
    ci_merkle_free(tree);
    if (!ok)
        return false;

    (void)collect_events(ctx);
    int64_t elapsed_us = platform_time_monotonic_us() - started_us;
    printf("{\"schema\":\"zcl.dev_source_snapshot.v1\","
           "\"status\":\"reconciled\",\"inotify_armed\":true,"
           "\"seal_verified\":%s,\"snapshot_used\":%s,"
           "\"full_rescan\":%s,\"inventory_changed\":%s,"
           "\"files_total\":%u,\"files_read\":%u,"
           "\"bytes_read\":%llu,\"queued_paths\":%zu,"
           "\"mutation_sequence\":%llu,\"elapsed_us\":%lld,"
           "\"source_root\":\"%s\"}\n",
           cost.snapshot_used ? "true" : "false",
           cost.snapshot_used ? "true" : "false",
           cost.full_rescan ? "true" : "false",
           cost.inventory_changed ? "true" : "false",
           (unsigned)cost.files_total, (unsigned)cost.files_read,
           (unsigned long long)cost.bytes_read, ctx->changed_count,
           (unsigned long long)ctx->mutation_sequence,
           (long long)elapsed_us, root_hex);
    fflush(stdout);
    return true;
}

static bool watch_cancel_poll(void *opaque)
{
    struct watch_context *ctx = opaque;
    if (g_watch_stop)
        return true;
    return collect_events(ctx) && ctx->changed_count > 0;
}

static int open_singleton_lock(const char *repo_root,
                               enum zcl_devloop_publish_mode publish_mode)
{
    char dir[PATH_MAX], path[PATH_MAX];
    int dn = snprintf(dir, sizeof(dir), "%s/.cache", repo_root);
    if (dn <= 0 || (size_t)dn >= sizeof(dir) ||
        !zcl_devloop_watch_lock_path(repo_root, path, sizeof(path)))
        return -1;
    if (!mkdirs(dir))
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) == 0)
        dprintf(fd, "%ld %s starting\n", (long)getpid(),
                zcl_devloop_publish_mode_name(publish_mode));
    return fd;
}

static bool mark_singleton_ready(
    int fd, enum zcl_devloop_publish_mode publish_mode)
{
    if (fd < 0 || ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0)
        return false;
    return dprintf(fd, "%ld %s ready\n", (long)getpid(),
                   zcl_devloop_publish_mode_name(publish_mode)) > 0;
}

int zcl_devloop_watch_mode(const char *repo_root,
                           enum zcl_devloop_publish_mode publish_mode)
{
    struct watch_context ctx = {0};
    const char *root = repo_root && repo_root[0] ? repo_root : ".";
    const char *mode_name = zcl_devloop_publish_mode_name(publish_mode);
    if (!mode_name) {
        fprintf(stderr, "[devloop] watch: invalid publication mode\n");
        return 2;
    }
    if (!realpath(root, ctx.root)) {
        fprintf(stderr, "[devloop] watch: cannot resolve repository root: %s\n",
                strerror(errno));
        return 2;
    }
    char makefile[PATH_MAX];
    snprintf(makefile, sizeof(makefile), "%s/Makefile", ctx.root);
    if (access(makefile, R_OK) != 0) {
        fprintf(stderr, "[devloop] watch: root has no readable Makefile\n");
        return 2;
    }
    int lock_fd = open_singleton_lock(ctx.root, publish_mode);
    if (lock_fd < 0) {
        fprintf(stderr,
                "[devloop] watch: another watcher owns this worktree lane\n");
        return 1;
    }
    ctx.fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (ctx.fd < 0 || !add_watch_recursive(&ctx, "")) {
        fprintf(stderr, "[devloop] watch: recursive inotify setup failed: %s\n",
                strerror(errno));
        if (ctx.fd >= 0)
            close(ctx.fd);
        close(lock_fd);
        return 1;
    }
    if (!prime_source_snapshot(&ctx)) {
        fprintf(stderr,
                "[devloop] watch: source snapshot reconciliation failed\n");
        close(ctx.fd);
        close(lock_fd);
        return 1;
    }
    if (!mark_singleton_ready(lock_fd, publish_mode)) {
        fprintf(stderr,
                "[devloop] watch: could not publish ready ownership\n");
        close(ctx.fd);
        close(lock_fd);
        return 1;
    }

    g_watch_stop = 0;
    zcl_devloop_process_cancel_clear();
    zcl_devloop_process_cancel_poll_clear();
    signal(SIGINT, watch_signal);
    signal(SIGTERM, watch_signal);
    printf("{\"schema\":\"zcl.dev_watch_heartbeat.v1\","
           "\"status\":\"watching\",\"pid\":%ld,\"directories\":%zu,"
           "\"root\":\"%s\",\"mode\":\"%s\","
           "\"runtime_publication\":%s,"
           "\"agent_next_action\":\"edit code\"}\n",
           (long)getpid(), ctx.dir_count, ctx.root, mode_name,
           zcl_devloop_publish_mode_applies(publish_mode) ? "true" : "false");
    fflush(stdout);

    while (!g_watch_stop) {
        if (ctx.changed_count == 0) {
            struct pollfd pfd = { .fd = ctx.fd, .events = POLLIN };
            int prc = poll(&pfd, 1, 1000);
            if (prc < 0 && errno == EINTR)
                continue;
            if (prc < 0) {
                fprintf(stderr, "[devloop] watch: poll failed: %s\n",
                        strerror(errno));
                break;
            }
            if (prc == 0)
                continue;
            if (!collect_events(&ctx) || ctx.changed_count == 0)
                continue;
        }

        if (!watch_emit_edit_seen(&ctx))
            break;

        /* Coalesce editor temp-file renames and multi-file saves into one
         * source epoch. Each newly observed event extends the quiet window. */
        int64_t quiet_until = platform_time_monotonic_us() + 15000;
        while (!g_watch_stop) {
            int64_t remain_us = quiet_until - platform_time_monotonic_us();
            if (remain_us <= 0)
                break;
            int wait_ms = (int)((remain_us + 999) / 1000);
            struct pollfd debounce = { .fd = ctx.fd, .events = POLLIN };
            int drc = poll(&debounce, 1, wait_ms);
            if (drc > 0 && collect_events(&ctx))
                quiet_until = platform_time_monotonic_us() + 15000;
            else if (drc < 0 && errno != EINTR)
                break;
        }

        char epoch_changed[ZCL_DEVLOOP_MAX_FILES][ZCL_DEVLOOP_PATH_MAX];
        const char *files[ZCL_DEVLOOP_MAX_FILES];
        size_t epoch_count = ctx.changed_count;
        for (size_t i = 0; i < epoch_count; i++) {
            snprintf(epoch_changed[i], sizeof(epoch_changed[i]), "%s",
                     ctx.changed[i]);
            files[i] = epoch_changed[i];
        }
        ctx.changed_count = 0;
        bool full_rescan = ctx.force_full_source_rescan;
        ctx.force_full_source_rescan = false;
        if (full_rescan) {
            (void)ci_merkle_forget(ctx.root);
        }
        printf("{\"schema\":\"zcl.dev_source_epoch.v1\","
               "\"mutation_sequence\":%llu,\"full_rescan\":%s,"
               "\"changed_paths\":%zu,\"first_path\":",
               (unsigned long long)ctx.mutation_sequence,
               full_rescan ? "true" : "false", epoch_count);
        print_json_string(stdout, epoch_changed[0]);
        printf("}\n");
        fflush(stdout);
        zcl_devloop_process_cancel_poll_set(watch_cancel_poll, &ctx);
        bool restart_union_ok = zcl_devloop_restart_source_set_add(
            &ctx.restart_sources, files, epoch_count);
        int fast = zcl_devloop_hotswap_batch_event(
            ctx.root, files, epoch_count, publish_mode);
        if (fast == 0)
            fast = service_contract_restart_event(ctx.root, files,
                                                  epoch_count);
        if (fast == 0) {
            const char *restart_files[ZCL_DEVLOOP_RESTART_SOURCE_MAX];
            const char *const *selected_files = files;
            size_t selected_count = epoch_count;
            if (restart_union_ok && watch_epoch_all_c(files, epoch_count) &&
                ctx.restart_sources.count > 0) {
                selected_count = ctx.restart_sources.count;
                for (size_t i = 0; i < selected_count; i++)
                    restart_files[i] = ctx.restart_sources.sources[i];
                selected_files = restart_files;
            }
            fast = zcl_devloop_restart_event(
                ctx.root, selected_files, selected_count, publish_mode);
            /* A green resident restart event is intentionally only the
             * low-latency affected-test receipt.  Keep the same warm owner
             * moving through the conservative complete proof so a stable
             * edit reaches ZVCS/publication without requiring the operator
             * to notice the deferred flag and launch a second command.
             * New filesystem activity cancels this proof through the poll
             * callback already armed above; stale epochs never anchor. */
            if (fast == ZCL_DEVLOOP_RESTART_EVENT_PROOF_PENDING ||
                fast == ZCL_DEVLOOP_RESTART_EVENT_FALLBACK_PENDING) {
                (void)zcl_devloop_run_cycle_mode(
                    ctx.root, selected_files, selected_count,
                    ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY);
                fast = ZCL_DEVLOOP_RESTART_EVENT_FINAL;
            }
        }
        if (fast == 0) {
            /* APPLY authority is intentionally narrower than the generic
             * cycle: only one compiled-allowlist island may publish live.
             * Storage/reducers/network/consensus and ordinary reload edits
             * remain on the verify-only contained path. */
            (void)zcl_devloop_run_cycle_mode(
                ctx.root, files, epoch_count,
                ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY);
        }
        zcl_devloop_process_cancel_poll_clear();
        if (g_watch_stop)
            break;
        bool superseded = ctx.changed_count > 0;
        zcl_devloop_process_cancel_clear();
        if (superseded) {
            printf("{\"schema\":\"zcl.dev_source_epoch.v1\","
                   "\"status\":\"superseded\","
                   "\"queued_paths\":%zu,\"first_queued_path\":",
                   ctx.changed_count);
            print_json_string(stdout, ctx.changed[0]);
            printf(",\"agent_next_action\":\"wait for latest verdict\"}\n");
            fflush(stdout);
        }
    }

    zcl_devloop_process_cancel_poll_clear();
    printf("{\"schema\":\"zcl.dev_watch_heartbeat.v1\","
           "\"status\":\"stopped\",\"pid\":%ld}\n", (long)getpid());
    close(ctx.fd);
    close(lock_fd);
    return 0;
}

int zcl_devloop_watch(const char *repo_root)
{
    return zcl_devloop_watch_mode(repo_root,
                                  zcl_devloop_default_watch_publish_mode());
}

#else

int zcl_devloop_watch_mode(const char *repo_root,
                           enum zcl_devloop_publish_mode publish_mode)
{
    (void)repo_root;
    (void)publish_mode;
    fprintf(stderr, "[devloop] watch is compiled out of release builds\n");
    return 2;
}

int zcl_devloop_watch(const char *repo_root)
{
    return zcl_devloop_watch_mode(repo_root,
                                  zcl_devloop_default_watch_publish_mode());
}

#endif
