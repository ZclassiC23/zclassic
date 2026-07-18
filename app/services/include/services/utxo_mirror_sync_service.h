/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_mirror_sync_service — keep node.db's explorer `utxos` mirror synced
 * to the authoritative coins_kv set in progress.kv.
 *
 * WHY
 * ---
 * The live consensus reducer (app/jobs/src/utxo_apply_stage.c) authors the
 * canonical UTXO set ONLY into coins_kv (the `coins` table in progress.kv) —
 * the store gettxoutsetinfo / the SHA3 commitment read. The node.db `utxos`
 * table is a DERIVED read model used by the block explorer, address balances,
 * the richlist, HodlWave, /explorer/stats and circulating supply. Its only
 * forward writer (process_block_flush_coins) is dead code, so after a cold
 * import seeds `utxos` once it FREEZES at the restore height while coins_kv
 * (correctly) tracks the tip. This service is the missing feeder.
 *
 * DESIGN — wholesale rebuild on drift (the simplest correct shape)
 * ----------------------------------------------------------------
 * coins_kv and the `utxos` mirror share the same canonical source columns
 * (txid, vout, value, height, is_coinbase, script); `utxos` adds only the
 * DERIVED script_type / address_hash / has_address, which utxo_classify_script
 * computes from a scriptPubKey. So rather than replay per-block deltas (pure
 * duplication of work coins_kv already captured, with far more failure
 * surface), each pass — when real drift is detected — rebuilds `utxos`
 * wholesale from coins_kv: iterate every coins_kv row, classify its script,
 * REPLACE it into `utxos`, then rebuild the derived wallet/address caches.
 *
 * This is:
 *   - node.db-ONLY: it never touches the coins_kv write path or its commit
 *     boundary. A mirror write failure LOG_FAILs/LOG_WARNs loudly but cannot
 *     roll back or fail anything on the consensus path. The mirror is
 *     rebuildable; the chain is not.
 *   - a DERIVED projection, not consensus state — no consensus logic.
 *   - idempotent + crash-safe: a torn pass leaves the cursor unadvanced and
 *     re-does the whole rebuild next tick (REPLACE makes that a no-op delta).
 *   - self-healing: on first run it rebuilds the entire post-import backlog.
 *
 * Drift is measured as |coins_kv_applied_height - mirror_cursor| (a durable
 * node.db state key) PLUS a row-count divergence guard, so a healthy mirror
 * that is exactly in step does no work.
 */

#ifndef ZCL_SERVICES_UTXO_MIRROR_SYNC_SERVICE_H
#define ZCL_SERVICES_UTXO_MIRROR_SYNC_SERVICE_H

#include "util/result.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

struct node_db;

/* ── Defaults ──────────────────────────────────────────────── */

#define UTXO_MIRROR_SYNC_DEFAULT_TICK_SECONDS 5
/* Persisted node.db state key: the height the `utxos` mirror is synced to
 * (== the coins_kv applied frontier reflected in the mirror). */
#define UTXO_MIRROR_SYNC_CURSOR_KEY "utxos_mirror_height"
/* While the applied frontier is more than this many blocks below the header
 * tip we are catching up (IBD / P2P body fetch), the frontier climbs every
 * tick, and a full ~1.3M-row rebuild per tick is quadratic against sync
 * progress. The mirror is an explorer/wallet read model nothing consumes
 * mid-sync, so defer the rebuild until within this window of the tip — the
 * first near-tip pass rebuilds it once. Fail-open: if the header tip is
 * unknown the gate no-ops and the pre-existing every-tick behavior stands. */
#define UTXO_MIRROR_SYNC_NEAR_TIP_BLOCKS 200

/* ── Status ────────────────────────────────────────────────── */

enum utxo_mirror_sync_state {
    UTXO_MIRROR_SYNC_IDLE = 0,
    UTXO_MIRROR_SYNC_RUNNING,
    UTXO_MIRROR_SYNC_STOPPED,
};

struct utxo_mirror_sync_service {
    /* References (not owned) */
    struct node_db *ndb;

    /* Config: written once by utxo_mirror_sync_init() before the background
     * thread is spawned and never mutated again — pthread_create's
     * happens-before guarantee makes a plain read from the background
     * thread (and from utxo_mirror_sync_dump_state_json on any thread)
     * race-free without atomics. */
    int tick_seconds;

    /* Thread management */
    pthread_t thread;
    /* Flipped by start()/stop() (boot/shutdown callers) and read from
     * utxo_mirror_sync_dump_state_json() on the diagnostics/native thread —
     * genuinely cross-thread, so _Atomic + atomic_store/atomic_load. */
    _Atomic bool thread_started;
    _Atomic bool stop_requested;

    /* Startup synchronization: start() blocks until the thread is live. */
    pthread_mutex_t ready_mutex;
    pthread_cond_t  ready_cond;
    bool            ready;

    /* Progress (atomics for lock-free reads). */
    _Atomic int     state;
    _Atomic int64_t rebuilds_run;       /* total wholesale rebuilds performed */
    _Atomic int64_t rows_written;       /* total mirror rows REPLACEd (cumulative) */
    _Atomic int64_t last_mirror_height; /* cursor after the last successful pass */
    _Atomic int64_t last_frontier;      /* coins_kv applied frontier last observed */
    _Atomic int64_t last_pass_unix;     /* wall time of last pass */
    _Atomic int64_t last_error_unix;    /* wall time of last failed pass (0 = none) */
};

/* Global pointer for RPC/native/diagnostics access. Set by boot, NULL before. */
extern struct utxo_mirror_sync_service *g_utxo_mirror_sync;

/* ── Lifecycle ─────────────────────────────────────────────── */

/* Initialize the service struct (does NOT start the thread). Reads
 * ZCL_UTXO_MIRROR_TICK_SECONDS env (optional override). */
void utxo_mirror_sync_init(struct utxo_mirror_sync_service *svc,
                           struct node_db *ndb);

/* Launch the background thread. Non-ok if already running or ndb is NULL. */
struct zcl_result utxo_mirror_sync_start(struct utxo_mirror_sync_service *svc);

/* Signal + join the background thread. Safe to call if not started. */
void utxo_mirror_sync_stop(struct utxo_mirror_sync_service *svc);

/* Run a single sync pass synchronously: if the mirror has drifted from the
 * coins_kv applied frontier, rebuild `utxos` wholesale from coins_kv and
 * advance the durable cursor. Returns the number of mirror rows written this
 * pass (0 when already in sync), or -1 on a (logged, non-fatal) error.
 * Exposed for tests and for an explicit one-shot from the diagnostics path. */
int64_t utxo_mirror_sync_run_once(struct utxo_mirror_sync_service *svc);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
struct json_value;
bool utxo_mirror_sync_dump_state_json(struct json_value *out,
                                      const char *key);

#endif /* ZCL_SERVICES_UTXO_MIRROR_SYNC_SERVICE_H */
