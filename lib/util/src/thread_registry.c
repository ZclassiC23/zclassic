/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * thread_registry: spawn, track, and drain zclassic23's
 * pthread population. See `util/thread_registry.h` for the API
 * contract and rationale. */

#define _GNU_SOURCE  /* pthread_timedjoin_np, pthread_setname_np */

#include "platform/time_compat.h"
#include "util/thread_registry.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Bounded fixed-size table.  Heap would be simpler but registering
 * from inside pthread creation paths is easier to reason about without
 * allocator round-trips, and the cap catches runaway thread growth. */
struct entry {
    pthread_t tid;
    char      name[48];
    bool      active;
};

static struct entry         g_entries[ZCL_THREAD_REGISTRY_CAP];
static pthread_mutex_t      g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool         g_shutdown = false;

/* Trampoline lets the registered thread unregister itself when it
 * returns normally, so join_all doesn't trip over exited entries.
 * Allocated per-spawn and freed in the trampoline. */
struct trampoline_args {
    void *(*entry)(void *);
    void  *arg;
};

static void *thread_registry_trampoline(void *raw)
{
    struct trampoline_args *ta = raw;
    void *(*entry)(void *) = ta->entry;
    void *arg = ta->arg;
    free(ta);
    void *ret = entry(arg);
    thread_registry_unregister_self();
    return ret;
}

int thread_registry_spawn(const char *name,
                          void *(*entry)(void *), void *arg,
                          pthread_t *out_tid)
{
    if (!entry) return EINVAL;

    struct trampoline_args *ta = zcl_malloc(sizeof(*ta),
                                             "thread_registry_trampoline");
    if (!ta) return ENOMEM;
    ta->entry = entry;
    ta->arg   = arg;

    /* Reserve a slot BEFORE pthread_create so we can't race with an
     * immediate exit + self-unregister. */
    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < ZCL_THREAD_REGISTRY_CAP; i++) {
        if (!g_entries[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_mu);
        free(ta);
        fprintf(stderr, "[thread_registry] capacity %d exceeded — "
                "cannot spawn '%s'\n",
                ZCL_THREAD_REGISTRY_CAP, name ? name : "?");
        return -1;
    }
    g_entries[slot].active = true;
    /* tid filled in after pthread_create succeeds. */
    strncpy(g_entries[slot].name, name ? name : "?",
            sizeof(g_entries[slot].name) - 1);
    g_entries[slot].name[sizeof(g_entries[slot].name) - 1] = '\0';
    pthread_mutex_unlock(&g_mu);

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, thread_registry_trampoline, ta);
    if (rc != 0) {
        pthread_mutex_lock(&g_mu);
        g_entries[slot].active = false;
        pthread_mutex_unlock(&g_mu);
        free(ta);
        return rc;
    }

    pthread_mutex_lock(&g_mu);
    g_entries[slot].tid = tid;
    pthread_mutex_unlock(&g_mu);

    if (out_tid) *out_tid = tid;

#ifdef __linux__
    /* pthread_setname_np accepts up to 15 chars + NUL; silently
     * truncate without propagating failure — diagnostics only. */
    char short_name[16];
    strncpy(short_name, name ? name : "zcl-thread", sizeof(short_name) - 1);
    short_name[sizeof(short_name) - 1] = '\0';
    (void)pthread_setname_np(tid, short_name);
#endif

    return 0;
}

bool thread_registry_shutdown_requested(void)
{
    return atomic_load_explicit(&g_shutdown, memory_order_acquire);
}

void thread_registry_request_shutdown(void)
{
    atomic_store_explicit(&g_shutdown, true, memory_order_release);
}

void thread_registry_unregister_self(void)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < ZCL_THREAD_REGISTRY_CAP; i++) {
        if (g_entries[i].active &&
            pthread_equal(g_entries[i].tid, self)) {
            g_entries[i].active = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

int thread_registry_join_all(int timeout_sec)
{
    if (timeout_sec < 0) timeout_sec = 0;
    int failed = 0;

    for (int i = 0; i < ZCL_THREAD_REGISTRY_CAP; i++) {
        pthread_mutex_lock(&g_mu);
        bool active = g_entries[i].active;
        pthread_t tid = g_entries[i].tid;
        char name[sizeof(g_entries[i].name)];
        if (active) memcpy(name, g_entries[i].name, sizeof(name));
        pthread_mutex_unlock(&g_mu);
        if (!active) continue;

        struct timespec ts;
        platform_time_realtime_timespec(&ts);
        ts.tv_sec += timeout_sec;

        int rc = pthread_timedjoin_np(tid, NULL, &ts);
        if (rc == 0) {
            pthread_mutex_lock(&g_mu);
            g_entries[i].active = false;
            pthread_mutex_unlock(&g_mu);
        } else {
            fprintf(stderr, "[thread_registry] straggler after %ds: "  // obs-ok:helper-context-logged
                    "'%s' (rc=%d: %s)\n",
                    timeout_sec, name, rc, strerror(rc));
            failed++;
        }
    }
    return failed;
}

int thread_registry_live_count(void)
{
    int n = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < ZCL_THREAD_REGISTRY_CAP; i++)
        if (g_entries[i].active) n++;
    pthread_mutex_unlock(&g_mu);
    return n;
}

void thread_registry_reset_for_test(void)
{
    pthread_mutex_lock(&g_mu);
    memset(g_entries, 0, sizeof(g_entries));
    atomic_store_explicit(&g_shutdown, false, memory_order_release);
    pthread_mutex_unlock(&g_mu);
}
