/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * sync_controller — catchup / import job lifecycle.
 *
 * Thin pthread job wrappers around the two long bulk operations: the
 * block-index catchup loop and the snapshot UTXO import. Each job exposes
 * init / start / join / is_started plus its thread entry point. The core
 * catchup algorithm is node_db_sync_catchup in sync_controller_catchup.c;
 * the job structs and public prototypes are declared in
 * controllers/sync_controller.h. */

#include "platform/time_compat.h"
#include "controllers/sync_controller.h"
#include "sync_controller_internal.h"
#include "chain/chain.h"
#include "wallet/wallet.h"
#include "storage/coins_db.h"
#include "validation/chainstate.h"
#include "util/thread_registry.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct catchup_args {
    struct node_db *ndb;
    const struct active_chain *chain;
    const struct wallet *w;
    const char *datadir;
};

/* sync_wallet_inmemory removed: it passed height=0 for all wallet
 * transactions, causing INSERT OR REPLACE to overwrite correct heights
 * and clear spent_txid on already-spent UTXOs. The catchup block scan
 * already processes wallet transactions with correct heights. */

static void *node_db_sync_catchup_job_thread(void *arg)
{
    struct node_db_sync_catchup_job *job = arg;
    struct node_db catchup_db;
    struct node_db *work_db = NULL;
    bool private_open = false;
    bool owns_db = false;

    if (!job) {
        LOG_NULL("sync", "catchup_job_thread: job is NULL");
    }

    memset(&catchup_db, 0, sizeof(catchup_db));
    private_open = node_db_sync_open_private_db_like(job->args.ndb, &catchup_db);
    if (private_open) {
        work_db = &catchup_db;
        owns_db = true;
    } else if (job->args.datadir) {
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/node.db",
                     job->args.datadir) >= (int)sizeof(path)) {
            LOG_INFO("catchup", "catchup: datadir path too long: %s", job->args.datadir);
        } else {
            private_open = node_db_open(&catchup_db, path);
            if (private_open) {
                work_db = &catchup_db;
                owns_db = true;
            }
        }
    }

    if (!work_db && job->args.ndb && job->args.ndb->open) {
        work_db = job->args.ndb;
    }

    if (!work_db || !work_db->open) {
        fprintf(stderr, "catchup: no usable database handle\n");
        job->result = -1;
        atomic_store(&job->finished, true);
        return NULL;
    }

    job->result = node_db_sync_catchup(work_db, job->args.chain,
                                       job->args.w, job->args.datadir);

    if (owns_db)
        node_db_close(&catchup_db);
    atomic_store(&job->finished, true);
    return NULL;
}

void *node_db_sync_catchup_thread(void *arg)
{
    struct node_db_sync_catchup_job job;
    struct catchup_args *args = arg;

    node_db_sync_catchup_job_init(&job);
    if (!args)
        LOG_NULL("sync", "catchup_thread: args is NULL");
    job.args.ndb = args->ndb;
    job.args.chain = args->chain;
    job.args.w = args->w;
    job.args.datadir = args->datadir;
    if (!node_db_sync_catchup_job_start(&job, job.args.ndb, job.args.chain,
                                        job.args.w, job.args.datadir))
        LOG_NULL("sync", "catchup_thread: job_start failed");
    node_db_sync_catchup_job_join(&job, NULL);
    return NULL;
}

void node_db_sync_catchup_job_init(struct node_db_sync_catchup_job *job)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->result = -1;
    atomic_store(&job->finished, false);
}

bool node_db_sync_catchup_job_start(struct node_db_sync_catchup_job *job,
                                    struct node_db *ndb,
                                    const struct active_chain *chain,
                                    const struct wallet *w,
                                    const char *datadir)
{
    if (!job || job->started || !ndb || !chain)
        LOG_FAIL("sync", "catchup_job_start: invalid args (job=%p, ndb=%p, chain=%p)",
                 (void *)job, (void *)ndb, (void *)chain);

    job->args.ndb = ndb;
    job->args.chain = chain;
    job->args.w = w;
    job->args.datadir = datadir;
    job->result = -1;
    atomic_store(&job->finished, false);
    job->started = true;
    if (thread_registry_spawn_ex("zcl_catchup",
                                  node_db_sync_catchup_job_thread, job,
                                  &job->thread) != 0) {
        job->started = false;
        atomic_store(&job->finished, false);
        LOG_FAIL("sync", "catchup_job_start: thread_registry_spawn_ex failed");
    }
    return true;
}

bool node_db_sync_catchup_job_join(struct node_db_sync_catchup_job *job,
                                   int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        LOG_FAIL("sync", "catchup_job_join: invalid args (job=%p, started=%d)",
                 (void *)job, job ? job->started : 0);
    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        LOG_FAIL("sync", "catchup_job_join: pthread_join failed (rc=%d)", join_rc);
    job->started = false;
    atomic_store(&job->finished, false);
    if (result_out)
        *result_out = job->result;
    return true;
}

bool node_db_sync_catchup_job_is_started(
    const struct node_db_sync_catchup_job *job)
{
    return job && job->started;
}

static void *node_db_sync_import_job_thread(void *arg)
{
    struct node_db_sync_import_job *job = arg;

    if (!job) {
        LOG_NULL("sync", "import_job_thread: job is NULL");
    }

    job->result = node_db_sync_import_utxos(job->args.ndb, job->args.cvdb);
    return NULL;
}

void node_db_sync_import_job_init(struct node_db_sync_import_job *job)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->result = -1;
}

bool node_db_sync_import_job_start(struct node_db_sync_import_job *job,
                                   struct node_db *ndb,
                                   struct coins_view_db *cvdb)
{
    if (!job || job->started || !ndb || !cvdb)
        LOG_FAIL("sync", "import_job_start: invalid args (job=%p, ndb=%p, cvdb=%p)",
                 (void *)job, (void *)ndb, (void *)cvdb);

    job->args.ndb = ndb;
    job->args.cvdb = cvdb;
    job->result = -1;
    if (thread_registry_spawn_ex("zcl_db_import",
                                  node_db_sync_import_job_thread, job,
                                  &job->thread) != 0)
        LOG_FAIL("sync", "import_job_start: thread_registry_spawn_ex failed");
    job->started = true;
    return true;
}

bool node_db_sync_import_job_join(struct node_db_sync_import_job *job,
                                  int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        LOG_FAIL("sync", "import_job_join: invalid args (job=%p, started=%d)",
                 (void *)job, job ? job->started : 0);
    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        LOG_FAIL("sync", "import_job_join: pthread_join failed (rc=%d)", join_rc);
    job->started = false;
    if (result_out)
        *result_out = job->result;
    return true;
}

bool node_db_sync_import_job_is_started(
    const struct node_db_sync_import_job *job)
{
    return job && job->started;
}
