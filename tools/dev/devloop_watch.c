/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE
#include "devloop.h"

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
};

static volatile sig_atomic_t g_watch_stop;

static void watch_signal(int sig)
{
    (void)sig;
    g_watch_stop = 1;
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
    return !name || !name[0] || name[0] == '.' ||
           strcmp(name, "build") == 0 || strcmp(name, "vendor") == 0 ||
           strcmp(name, "target") == 0 || strcmp(name, "node_modules") == 0 ||
           strcmp(name, "test-tmp") == 0 ||
           strcmp(name, "test-tmp-reclaim") == 0;
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
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
        IN_DELETE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
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
                add_changed(ctx, "Makefile");
                saw = true;
                continue;
            }
            struct watched_dir *dir = find_watch(ctx, ev->wd);
            if (!dir || ev->len == 0)
                continue;
            char rel[ZCL_DEVLOOP_PATH_MAX];
            int rn = dir->rel[0]
                ? snprintf(rel, sizeof(rel), "%s/%s", dir->rel, ev->name)
                : snprintf(rel, sizeof(rel), "%s", ev->name);
            if (rn <= 0 || (size_t)rn >= sizeof(rel))
                continue;
            if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                (void)add_watch_recursive(ctx, rel);
                continue;
            }
            if (!(ev->mask & IN_ISDIR) &&
                (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM |
                             IN_CREATE | IN_DELETE | IN_ATTRIB))) {
                size_t before = ctx->changed_count;
                add_changed(ctx, rel);
                saw = saw || ctx->changed_count != before;
            }
        }
    }
    return saw;
}

static int open_singleton_lock(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    char dir[PATH_MAX], path[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.local/state/zclassic23-dev", home);
    snprintf(path, sizeof(path), "%s/native-watch.lock", dir);
    if (!mkdirs(dir))
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) == 0)
        dprintf(fd, "%ld\n", (long)getpid());
    return fd;
}

int zcl_devloop_watch(const char *repo_root)
{
    struct watch_context ctx = {0};
    const char *root = repo_root && repo_root[0] ? repo_root : ".";
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
    int lock_fd = open_singleton_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "[devloop] watch: another native watcher owns the lane\n");
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

    g_watch_stop = 0;
    signal(SIGINT, watch_signal);
    signal(SIGTERM, watch_signal);
    printf("{\"schema\":\"zcl.dev_watch_heartbeat.v1\","
           "\"status\":\"watching\",\"pid\":%ld,\"directories\":%zu,"
           "\"root\":\"%s\",\"agent_next_action\":\"edit code\"}\n",
           (long)getpid(), ctx.dir_count, ctx.root);
    fflush(stdout);

    while (!g_watch_stop) {
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

        /* Coalesce editor temp-file renames and multi-file saves into one
         * source epoch. Each newly observed event extends the quiet window. */
        int64_t quiet_until = platform_time_monotonic_us() + 150000;
        while (!g_watch_stop) {
            int64_t remain_us = quiet_until - platform_time_monotonic_us();
            if (remain_us <= 0)
                break;
            int wait_ms = (int)((remain_us + 999) / 1000);
            struct pollfd debounce = { .fd = ctx.fd, .events = POLLIN };
            int drc = poll(&debounce, 1, wait_ms);
            if (drc > 0 && collect_events(&ctx))
                quiet_until = platform_time_monotonic_us() + 150000;
            else if (drc < 0 && errno != EINTR)
                break;
        }

        const char *files[ZCL_DEVLOOP_MAX_FILES];
        for (size_t i = 0; i < ctx.changed_count; i++)
            files[i] = ctx.changed[i];
        (void)zcl_devloop_run_cycle(ctx.root, files, ctx.changed_count);
        ctx.changed_count = 0;
    }

    printf("{\"schema\":\"zcl.dev_watch_heartbeat.v1\","
           "\"status\":\"stopped\",\"pid\":%ld}\n", (long)getpid());
    close(ctx.fd);
    close(lock_fd);
    return 0;
}

#else

int zcl_devloop_watch(const char *repo_root)
{
    (void)repo_root;
    fprintf(stderr, "[devloop] watch is compiled out of release builds\n");
    return 2;
}

#endif
