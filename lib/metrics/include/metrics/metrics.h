/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_METRICS_H
#define ZCL_METRICS_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

struct main_state;
struct connman;
struct chain_params;

struct metrics_external_gauges {
    int64_t utxo_count;
    int sync_state;
    char sync_state_name[32];
    int64_t tip_advance_age_seconds;
    int64_t mirror_lag_blocks;
    int64_t mirror_lag_breach_seconds;
    int64_t mirror_lag_critical_seconds;
    int64_t magicbean_peer_count;
    int64_t zclassic_c23_peer_count;
};

typedef void (*metrics_external_gauges_fn)(
    struct metrics_external_gauges *out,
    void *ctx);

extern _Atomic uint64_t g_transactions_validated;
extern _Atomic uint64_t g_eh_solver_runs;

struct metrics_context {
    struct main_state *ms;
    struct connman *cm;
    const struct chain_params *params;
    bool mining;
    metrics_external_gauges_fn external_gauges;
    void *external_gauges_ctx;
    _Atomic bool running;
    bool thread_started;
};

void metrics_print_art(void);
bool metrics_start(struct metrics_context *ctx);
void metrics_stop(struct metrics_context *ctx);

/* O(1) atomic counter bump; thread-safe, callable from any thread. */
static inline void metrics_increment_tx_validated(void)
{
    atomic_fetch_add(&g_transactions_validated, 1);
}

/* O(1) atomic counter bump; thread-safe, callable from any thread. */
static inline void metrics_increment_eh_solver_runs(void)
{
    atomic_fetch_add(&g_eh_solver_runs, 1);
}

#endif
