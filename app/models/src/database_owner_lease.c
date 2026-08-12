/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: exclusive process ownership of a canonical node database.
 * ar-validate-skip:connection-ownership-not-a-row */

#include "models/database_owner_lease.h"

#include "models/database.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

enum { NODE_DB_OWNER_LEASES = 128 };

struct node_db_owner_lease {
    char path[1024];
    int fd;
    unsigned refs;
    pid_t pid;
};

static pthread_mutex_t g_owner_lease_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct node_db_owner_lease g_owner_leases[NODE_DB_OWNER_LEASES];

void node_db_owner_lease_release(struct node_db *ndb)
{
    if (!ndb || ndb->lifetime_owner_lease_slot < 0) return;
    pthread_mutex_lock(&g_owner_lease_mutex);
    int slot = ndb->lifetime_owner_lease_slot;
    struct node_db_owner_lease *lease =
        slot < NODE_DB_OWNER_LEASES ? &g_owner_leases[slot] : NULL;
    if (lease && lease->pid == getpid() && lease->refs > 0 &&
        strcmp(lease->path, ndb->path) == 0) {
        lease->refs--;
        if (lease->refs == 0) {
            if (flock(lease->fd, LOCK_UN) != 0)
                LOG_WARN("db", "database owner lease unlock failed for %s: %s",
                         ndb->path, strerror(errno));
            if (close(lease->fd) != 0)
                LOG_WARN("db", "database owner lease close failed for %s: %s",
                         ndb->path, strerror(errno));
            memset(lease, 0, sizeof(*lease));
            lease->fd = -1;
        }
    }
    pthread_mutex_unlock(&g_owner_lease_mutex);
    ndb->lifetime_owner_lease_slot = -1;
}

bool node_db_owner_lease_acquire(struct node_db *ndb)
{
    if (!ndb || !ndb->path[0])
        LOG_FAIL("db", "database owner lease requires a path");
    if (strcmp(ndb->path, ":memory:") == 0) return true;
    pthread_mutex_lock(&g_owner_lease_mutex);
    int free_slot = -1;
    for (int i = 0; i < NODE_DB_OWNER_LEASES; i++) {
        struct node_db_owner_lease *lease = &g_owner_leases[i];
        if (lease->refs > 0 && lease->pid != getpid()) {
            (void)close(lease->fd);
            memset(lease, 0, sizeof(*lease));
            lease->fd = -1;
        }
        if (lease->refs > 0 && lease->pid == getpid() &&
            strcmp(lease->path, ndb->path) == 0) {
            lease->refs++;
            ndb->lifetime_owner_lease_slot = i;
            pthread_mutex_unlock(&g_owner_lease_mutex);
            return true;
        }
        if (lease->refs == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        pthread_mutex_unlock(&g_owner_lease_mutex);
        LOG_FAIL("db", "database owner lease registry is full for %s", ndb->path);
    }
    /* Lock node.db itself: no sidecar or directory metadata mutation, while
     * independent databases in one test/scratch directory remain independent. */
    int fd = open(ndb->path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) goto fail_locked;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        (void)close(fd);
        pthread_mutex_unlock(&g_owner_lease_mutex);
        LOG_FAIL("db", "DATABASE_OWNERSHIP_CONFLICT: canonical database owner "
                 "already holds path=%s (error=%s)", ndb->path,
                 strerror(saved));
    }
    struct node_db_owner_lease *lease = &g_owner_leases[free_slot];
    (void)snprintf(lease->path, sizeof(lease->path), "%s", ndb->path);
    lease->fd = fd;
    lease->refs = 1;
    lease->pid = getpid();
    ndb->lifetime_owner_lease_slot = free_slot;
    pthread_mutex_unlock(&g_owner_lease_mutex);
    return true;

fail_locked:
    {
        int saved = errno;
        pthread_mutex_unlock(&g_owner_lease_mutex);
        LOG_FAIL("db", "database owner lease open failed for %s: %s",
                 ndb->path, strerror(saved));
    }
}

bool node_db_owner_lease_rebind(struct node_db *ndb)
{
    if (!ndb || ndb->lifetime_owner_lease_slot < 0)
        LOG_FAIL("db", "database owner lease rebind requires ownership");
    int replacement = open(ndb->path,
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (replacement < 0)
        LOG_FAIL("db", "database owner rebind open failed for %s: %s",
                 ndb->path, strerror(errno));
    if (flock(replacement, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        (void)close(replacement);
        LOG_FAIL("db", "DATABASE_OWNERSHIP_CONFLICT: replacement path=%s "
                 "was acquired during quarantine (error=%s)", ndb->path,
                 strerror(saved));
    }
    pthread_mutex_lock(&g_owner_lease_mutex);
    int slot = ndb->lifetime_owner_lease_slot;
    struct node_db_owner_lease *lease =
        slot < NODE_DB_OWNER_LEASES ? &g_owner_leases[slot] : NULL;
    if (!lease || lease->pid != getpid() || lease->refs != 1 ||
        strcmp(lease->path, ndb->path) != 0) {
        pthread_mutex_unlock(&g_owner_lease_mutex);
        (void)flock(replacement, LOCK_UN);
        (void)close(replacement);
        LOG_FAIL("db", "database owner rebind found ambiguous ownership");
    }
    int retired = lease->fd;
    lease->fd = replacement;
    pthread_mutex_unlock(&g_owner_lease_mutex);
    (void)flock(retired, LOCK_UN);
    (void)close(retired);
    return true;
}
