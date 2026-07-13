/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Reducer drain core — the bounded round loop that drives the eight staged-Job
 * step bodies to convergence, plus the two public kick entry points
 * (reducer_kick for the supervisor/FSM path, reducer_kick_unbudgeted for the
 * dedicated -mint-anchor driver). Split out of reducer_ingest_service.c (which
 * keeps the synchronous block-intake path) so each file holds one seam.
 *
 * LIVELOCK GUARD (2026-07-13): reducer_kick_unbudgeted used to run with NO
 * wall-clock budget and NO frontier-progress check, so one call could drain
 * hard_cap(64) * ZCL_REFOLD_DRAIN_BATCH(2000) = 128k blocks back-to-back —
 * HOURS under the fsync-bound fold rate — while the boot_mint_anchor drive
 * loop (which logs progress and runs the stall detector BETWEEN kicks) never
 * regained control: the forbidden quiet spin. Two bounds close it:
 *   (1) converge_on_frontier_stall — a round that advances upstream stages but
 *       not the utxo_apply frontier returns immediately (a walled fold hands
 *       control back so the driver fails closed with a named blocker);
 *   (2) a generous wall-clock budget (ZCL_MINT_KICK_BUDGET_MS, default 3000)
 *       checked at ROUND boundaries only, so the per-batch fsync cadence (the
 *       fold's throughput lever) is untouched.
 * Guarded by lib/test/src/test_reducer_step_drain_harness.c
 * (test_mint_fold_livelock). */

// one-result-type-ok:reducer-drive-counts
/* The reducer entry points return advance-counts; a failure surfaces via the
 * stage FATAL latch + EV_OPERATOR_NEEDED, not a return-value reason (same
 * rationale as reducer_ingest_service.c). */

#include "services/chain_activation_service.h"
#include "services/reducer_ingest_service.h"
#include "services/reducer_drain.h"

#include "event/event.h"
#include "core/utiltime.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/reducer_drive_guard.h"
#include "util/stage.h"
#include "util/thread_registry.h"  /* thread_registry_shutdown_requested */

#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/refold_cadence.h"   /* refold_cadence_drain_batch (mint/refold) */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* getenv, strtoll */
#include <string.h>   /* memset */

/* Clamped env int; `def` when unset/empty/unparsable (mirrors
 * refold_cadence.c's cadence_env_int — local so this TU owns no cross-TU
 * dependency for one knob). */
static int64_t env_int_default(const char *name, int64_t def, int64_t lo,
                               int64_t hi)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return def;
    char *end = NULL;
    long long n = strtoll(v, &end, 10);
    if (end == v)
        return def;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return (int64_t)n;
}

/* ── The eight staged-Job stages, in pipeline order ──────────────────────
 * One descriptor per stage: the bounded drain entry point (returns the count
 * of advancing steps) and the cheap in-memory cursor reader (a lock-free
 * stage_cursor(g_stage) read — NOT the SQL stage_cursor table, per the
 * hot-path rule). Both the drain loop and the advance-or-blocker
 * reconciliation walk this one table so their stage indices never diverge. */
struct drain_stage_desc {
    const char *name;
    int      (*drain)(int max_steps);
    uint64_t (*cursor)(void);
};

static const struct drain_stage_desc g_drain_stages[REDUCER_DRAIN_NUM_STAGES] = {
    { "header_admit",     header_admit_stage_drain,     header_admit_stage_cursor },
    { "validate_headers", validate_headers_stage_drain, validate_headers_stage_cursor },
    { "body_fetch",       body_fetch_stage_drain,       body_fetch_stage_cursor },
    { "body_persist",     body_persist_stage_drain,     body_persist_stage_cursor },
    { "script_validate",  script_validate_stage_drain,  script_validate_stage_cursor },
    { "proof_validate",   proof_validate_stage_drain,   proof_validate_stage_cursor },
    { "utxo_apply",       utxo_apply_stage_drain,       utxo_apply_stage_cursor },
    { "tip_finalize",     tip_finalize_stage_drain,     tip_finalize_stage_cursor },
};

/* Index of the utxo_apply stage in g_drain_stages — the fold's real
 * forward-progress metric (the converge_on_frontier_stall break reads it). */
#define REDUCER_DRAIN_UTXO_APPLY_IDX 6

/* ── Advance-or-blocker reconciliation state ─────────────────────────────
 * Per-stage consecutive rounds where the stage reported advances>0 but its own
 * cursor did NOT move, plus the steps it reported across that streak, plus a
 * flag tracking whether a "stage_spin_<name>" blocker is currently set for the
 * stage. Written only from reducer_drain_core, which every entry point reaches
 * while holding chain_activation_controller::mutex, so the writer is
 * single-threaded; the reducer_drive dumpstate reads them from another thread,
 * hence atomics. */
static _Atomic uint32_t g_spin_rounds_frozen[REDUCER_DRAIN_NUM_STAGES];
static _Atomic uint64_t g_spin_steps_reported[REDUCER_DRAIN_NUM_STAGES];
static _Atomic bool     g_spin_blocker_active[REDUCER_DRAIN_NUM_STAGES];

/* Consecutive-round spin threshold K (compile-time default, env override). */
static int reducer_stage_spin_rounds(void)
{
    return (int)env_int_default("ZCL_STAGE_SPIN_ROUNDS",
                                ZCL_STAGE_SPIN_ROUNDS_DEFAULT, 1, 1000000);
}

/* Build the "stage_spin_<name>" blocker id into `buf` (>= BLOCKER_ID_MAX). */
static void reducer_spin_blocker_id(char *buf, size_t buflen, const char *name)
{
    snprintf(buf, buflen, "stage_spin_%s", name ? name : "unknown");
}

void reducer_drain_spin_observe(int stage_idx, const char *stage_name,
                                int advance, uint64_t cursor_before,
                                uint64_t cursor_after, int spin_k)
{
    if (stage_idx < 0 || stage_idx >= REDUCER_DRAIN_NUM_STAGES || !stage_name)
        return;
    if (spin_k < 1)
        spin_k = 1;

    /* Real forward progress: the spin (if any) is resolved. Reset the streak
     * and clear the standing blocker exactly once on the movement edge. */
    if (cursor_after != cursor_before) {
        atomic_store(&g_spin_rounds_frozen[stage_idx], 0u);
        atomic_store(&g_spin_steps_reported[stage_idx], 0u);
        if (atomic_exchange(&g_spin_blocker_active[stage_idx], false)) {
            char id[BLOCKER_ID_MAX];
            reducer_spin_blocker_id(id, sizeof(id), stage_name);
            blocker_clear(id);
        }
        return;
    }

    /* No advance reported and the cursor is frozen: idle / converged, not a
     * spin. Break the consecutive-round streak (re-arming needs K again). A
     * blocker that was already standing stays until real cursor movement clears
     * it — the named "no forward progress" fact is still true. */
    if (advance <= 0) {
        atomic_store(&g_spin_rounds_frozen[stage_idx], 0u);
        atomic_store(&g_spin_steps_reported[stage_idx], 0u);
        return;
    }

    /* advance > 0 AND the stage's own cursor did not move — the spin signal. */
    uint32_t rounds =
        atomic_fetch_add(&g_spin_rounds_frozen[stage_idx], 1u) + 1u;
    uint64_t steps = atomic_fetch_add(&g_spin_steps_reported[stage_idx],
                                      (uint64_t)advance) + (uint64_t)advance;
    if ((int)rounds < spin_k)
        return;

    char id[BLOCKER_ID_MAX];
    reducer_spin_blocker_id(id, sizeof(id), stage_name);
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "stage=%s reported advance for %u consecutive drain rounds "
             "(steps_reported=%llu) but its cursor is frozen at height %llu — "
             "steps-reported and durable-cursor-moved diverged; observational, "
             "clears when the cursor advances",
             stage_name, rounds, (unsigned long long)steps,
             (unsigned long long)cursor_after);

    struct blocker_record r;
    if (blocker_init(&r, id, "reducer_drain", BLOCKER_TRANSIENT, reason)) {
        (void)blocker_set(&r);
        atomic_store(&g_spin_blocker_active[stage_idx], true);
    }
}

int reducer_drain_spin_snapshot(struct reducer_stage_spin_entry *out, int max)
{
    if (!out || max <= 0)
        return 0;
    int n = max < REDUCER_DRAIN_NUM_STAGES ? max : REDUCER_DRAIN_NUM_STAGES;
    for (int i = 0; i < n; i++) {
        out[i].name = g_drain_stages[i].name;
        out[i].rounds_frozen = atomic_load(&g_spin_rounds_frozen[i]);
        out[i].steps_reported = atomic_load(&g_spin_steps_reported[i]);
    }
    return n;
}

#ifdef ZCL_TESTING
void reducer_drain_spin_reset_for_testing(void)
{
    for (int i = 0; i < REDUCER_DRAIN_NUM_STAGES; i++) {
        atomic_store(&g_spin_rounds_frozen[i], 0u);
        atomic_store(&g_spin_steps_reported[i], 0u);
        if (atomic_exchange(&g_spin_blocker_active[i], false)) {
            char id[BLOCKER_ID_MAX];
            reducer_spin_blocker_id(id, sizeof(id), g_drain_stages[i].name);
            blocker_clear(id);
        }
    }
}
#endif

/* Drain the eight stage step bodies once, in pipeline order — the SAME
 * *_stage_drain functions the per-stage supervisor children tick
 * (staged_sync_supervisor.c). One pass; caller loops to convergence. Fills
 * `adv_per_stage` (length REDUCER_DRAIN_NUM_STAGES) with each stage's own
 * advance count so the caller can reconcile per-stage advance vs cursor
 * movement; returns the total across all stages. */
static int reducer_drain_all_stages(int max_steps_per_stage,
                                    int *adv_per_stage)
{
    int total = 0;
    for (int i = 0; i < REDUCER_DRAIN_NUM_STAGES; i++) {
        int a = g_drain_stages[i].drain(max_steps_per_stage);
        if (adv_per_stage)
            adv_per_stage[i] = a;
        total += a;
    }
    return total;
}

/* Shared drain core. `budget_us <= 0` means NO latency budget: keep draining
 * until convergence (a no-advance pass) or the round hard cap, whichever first.
 * The supervisor/FSM path passes a 2s budget so it yields its 2s stage ticks.
 * `per_stage_batch` sets how many blocks each stage folds under ONE batch
 * transaction (one COMMIT / fsync / ext4 journal barrier per stage per round —
 * see STAGE_DRAIN_IMPL). A larger batch drops the fsync cadence, the genesis
 * fold's dominant wait (jbd2_log_wait_commit). Full validation is identical
 * for any batch size — only the commit cadence and latency differ, never WHAT
 * a stage checks.
 *
 * `converge_on_frontier_stall` (the -mint-anchor unbudgeted path ONLY): break
 * the moment a round advances SOME stage but NOT the utxo_apply frontier (the
 * fold's real forward-progress metric). Without it, a fold walled at a low
 * height keeps `adv > 0` every round while header_admit/validate_headers grind
 * the whole upstream backlog toward the mint ceiling inside ONE call — the
 * silent multi-hour kick of the 2026-07-13 mint livelock. A healthy fold
 * advances the frontier every round (the stages run in pipeline order within a
 * round), so this fires only when the frontier is genuinely walled. */
static int reducer_drain_core(int64_t budget_us, int hard_cap,
                              int per_stage_batch,
                              bool converge_on_frontier_stall)
{
    int64_t   start_us        = GetTimeMicros();
    uint64_t  fatal_gen0      = stage_fatal_generation();
    int       total           = 0;
    const int spin_k          = reducer_stage_spin_rounds();
    if (per_stage_batch <= 0) per_stage_batch = 100;
    for (int round = 0; round < hard_cap; round++) {
        /* On shutdown, return at this round boundary (a safe, committed point —
         * each stage's batch has already COMMITted) so the P2P message thread's
         * reducer activation exits promptly and connman_join succeeds instead of
         * timing out and detaching the thread under the frees that follow. The
         * fold is resumable, so stopping mid-drain loses no state. */
        if (thread_registry_shutdown_requested())
            break;
        /* Snapshot every stage's cursor at the round boundary (cheap in-memory
         * reads). Each stage only ever moves its OWN cursor, so a round-boundary
         * before/after diff is correctly attributed per stage even though the
         * eight drains run back-to-back inside the round. */
        uint64_t cur_before[REDUCER_DRAIN_NUM_STAGES];
        for (int i = 0; i < REDUCER_DRAIN_NUM_STAGES; i++)
            cur_before[i] = g_drain_stages[i].cursor();
        uint64_t frontier_before =
            converge_on_frontier_stall
                ? cur_before[REDUCER_DRAIN_UTXO_APPLY_IDX] : 0;

        int adv_per_stage[REDUCER_DRAIN_NUM_STAGES] = {0};
        int adv = reducer_drain_all_stages(per_stage_batch, adv_per_stage);
        total += adv;

        /* Advance-or-blocker contract: reconcile each stage's REPORTED advance
         * against its own cursor movement. A stage that reports advances while
         * its cursor stays frozen for K consecutive rounds is named as a
         * "stage_spin_<name>" blocker (observational — this never breaks the
         * drain; the drive-age watchdog and the round budget own termination).
         * The per-stage own-cursor predicate is robust to the legitimate
         * feed-downstream case: a stage that reports advance moves its OWN
         * cursor (stage-runner contract), so reporting advance while a
         * DOWNSTREAM cursor moves instead cannot mislabel it. */
        for (int i = 0; i < REDUCER_DRAIN_NUM_STAGES; i++) {
            uint64_t cur_after = g_drain_stages[i].cursor();
            reducer_drain_spin_observe(i, g_drain_stages[i].name,
                                       adv_per_stage[i], cur_before[i],
                                       cur_after, spin_k);
        }

        if (adv == 0)
            break;
        /* Frontier-stall convergence (mint drive only): return NOW so the
         * boot_mint_anchor drive loop re-reads the frontier, logs, and runs
         * its stall detector instead of spinning the upstream backlog. */
        if (converge_on_frontier_stall &&
            g_drain_stages[REDUCER_DRAIN_UTXO_APPLY_IDX].cursor() ==
                frontier_before)
            break;
        if (budget_us > 0 && GetTimeMicros() - start_us > budget_us)
            break;
    }
    /* Page the operator on a FATAL latched during this drain regardless of
     * which exit fired — convergence (adv==0) OR the budget timeout. A stage
     * can return JOB_FATAL every pass while another keeps advancing, so
     * total>0 and the loop exits on the budget, not on adv==0; gating the page
     * on the adv==0 break alone let that masked-FATAL recur unpaged. */
    {
        char st[STAGE_NAME_MAX] = {0}, why[128] = {0};
        if (stage_fatal_generation() != fatal_gen0 &&
            stage_last_fatal(st, sizeof(st), why, sizeof(why)))
            event_emitf(EV_OPERATOR_NEEDED, 0,
                        "condition=reducer_stage_fatal stage=%s reason=%s",
                        st, why);
    }
    return total;
}

int reducer_drain_to_convergence(void)
{
    const int64_t drain_budget_us = 2000 * 1000; /* 2s, same as legacy */
    const int     drain_hard_cap  = 4096;
    const int     per_stage_batch = 100;          /* legacy cadence, unchanged */
    return reducer_drain_core(drain_budget_us, drain_hard_cap, per_stage_batch,
                              /*converge_on_frontier_stall=*/false);
}

int reducer_drain_to_convergence_unbudgeted(void)
{
    /* Drain back-to-back, NOT in 2s slices like the supervisor path — but with
     * a GENEROUS wall-clock budget so the call still returns periodically to
     * the -mint-anchor drive loop (progress logging + the stall detector run
     * BETWEEN kicks; see the file header for the livelock this closes). The
     * budget is checked only at ROUND boundaries (committed points), so the
     * per-batch fsync cadence — the fold's throughput lever — is untouched. */
    const int64_t budget_ms =
        env_int_default("ZCL_MINT_KICK_BUDGET_MS", 3000, 100, 600000);
    const int drain_hard_cap   = 64;   /* backstop cap; budget returns first */
    /* Per-stage batch: one COMMIT/fsync per this many blocks/stage. Reached
     * ONLY via reducer_kick_unbudgeted (the -mint-anchor driver), where the
     * mint fold ceiling is set, so refold_cadence_active() is true and the
     * accelerated ZCL_REFOLD_DRAIN_BATCH default (2000) applies; 1000 is the
     * fallback if this path is ever reached with the gate inactive. */
    const int per_stage_batch  = refold_cadence_drain_batch(1000);
    return reducer_drain_core(/*budget_us=*/budget_ms * 1000, drain_hard_cap,
                              per_stage_batch,
                              /*converge_on_frontier_stall=*/true);
}

int reducer_kick(struct chain_activation_controller *ctl)
{
    if (!ctl)
        return 0;
    zcl_mutex_lock(&ctl->mutex);
    int advanced = reducer_drain_to_convergence();
    zcl_mutex_unlock(&ctl->mutex);
    return advanced;
}

int reducer_kick_unbudgeted(struct chain_activation_controller *ctl)
{
    if (!ctl)
        return 0;
    /* The dedicated -mint-anchor driver's tight drain: same locking + drive
     * marking as reducer_kick, but the inner drain is budgeted in seconds (not
     * 2s slices) and converges on a frontier stall. Held under ctl->mutex for
     * the whole drain — the same serialization point the supervisor takes — so
     * no concurrent supervisor drain races the active-chain window. Full
     * validation is unchanged. */
    zcl_mutex_lock(&ctl->mutex);
    reducer_drive_enter();
    reducer_enter_batched_body_sync();
    int advanced = reducer_drain_to_convergence_unbudgeted();
    reducer_exit_batched_body_sync();
    reducer_drive_exit();
    zcl_mutex_unlock(&ctl->mutex);
    return advanced;
}
