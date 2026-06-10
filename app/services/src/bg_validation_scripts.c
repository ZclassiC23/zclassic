/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright (c) 2014-2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */
// one-result-type-ok:hot-script-verifier
// Internal background-validation verifier, not a new service surface. The
// bg_validation service walks the chain and converts a false return here into
// a logged per-block validation failure with the offending height/tx; the bool
// keeps the hot per-input verifier branch-free.
/*
 * bg_validation_scripts: parallel ECDSA script-signature verification for the
 * background full-validation pass. */

#include "bg_validation_internal.h"

#include "validation/tx_verifier.h"
#include "script/interpreter.h"
#include "primitives/transaction.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdlib.h>

/* ── Parallel script verification ────────────────────────────── */

static bool verify_script_item(void *item)
{
    struct script_check_item *sc = item;
    struct tx_sig_checker tsc;
    tx_sig_checker_init(&tsc, sc->tx, sc->input_index,
                        sc->amount, sc->branch_id, &sc->txdata);
    struct sig_checker checker = tx_make_sig_checker(&tsc);

    ScriptError serror = SCRIPT_ERR_OK;
    return verify_script(&sc->tx->vin[sc->input_index].script_sig,
                         &sc->script_pub_key,
                         sc->flags, &checker,
                         sc->branch_id, &serror);
}

/* ── Worker thread for parallel verification ─────────────────── */

struct worker_ctx {
    struct script_check_item *items;
    size_t start;
    size_t count;
    bool result;
};

static void *worker_thread(void *arg)
{
    struct worker_ctx *w = arg;
    w->result = true;
    for (size_t i = 0; i < w->count; i++) {
        if (!verify_script_item(&w->items[w->start + i])) {
            w->result = false;
            return NULL;
        }
    }
    return NULL;
}

/* Verify all script items in parallel using num_workers threads.
 * Falls back to serial for small batches. */
bool bg_validation_verify_scripts_parallel(struct script_check_item *items,
                                           size_t count, int num_workers)
{
    if (count == 0)
        return true;

    /* Serial path for small batches or single-threaded mode */
    if (num_workers <= 1 || count <= 4) {
        for (size_t i = 0; i < count; i++) {
            if (!verify_script_item(&items[i]))
                return false;
        }
        return true;
    }

    /* Parallel: split work across threads */
    int nthreads = num_workers;
    if ((size_t)nthreads > count)
        nthreads = (int)count;

    struct worker_ctx *workers = zcl_calloc((size_t)nthreads,
                                        sizeof(struct worker_ctx), "bg_valid workers");
    pthread_t *threads = zcl_calloc((size_t)nthreads, sizeof(pthread_t), "bg_valid threads");
    if (!workers || !threads) {
        free(workers);
        free(threads);
        /* Fallback to serial */
        for (size_t i = 0; i < count; i++) {
            if (!verify_script_item(&items[i]))
                return false;
        }
        return true;
    }

    size_t per_thread = count / (size_t)nthreads;
    size_t remainder = count % (size_t)nthreads;
    size_t offset = 0;

    for (int t = 0; t < nthreads; t++) {
        workers[t].items = items;
        workers[t].start = offset;
        workers[t].count = per_thread + (t < (int)remainder ? 1 : 0);
        workers[t].result = true;
        offset += workers[t].count;
        /* raw-pthread-ok: short-burst-joined-immediately */
        pthread_create(&threads[t], NULL, worker_thread, &workers[t]);
    }

    bool all_ok = true;
    for (int t = 0; t < nthreads; t++) {
        pthread_join(threads[t], NULL);
        if (!workers[t].result)
            all_ok = false;
    }

    free(workers);
    free(threads);
    return all_ok;
}
