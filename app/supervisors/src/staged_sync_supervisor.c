/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Staged-sync supervisor children. Owns liveness contracts for the
 * authoritative eight-stage reducer pipeline in the chain domain.
 *
 * staged_sync_supervisor_register() registers them in pipeline order:
 *   header_admit → validate_headers → body_fetch → body_persist →
 *   script_validate → proof_validate → utxo_apply → tip_finalize.
 *
 * Every stage shares one wiring (period=2s, the 30-min progress-quiet
 * window, the chain domain, idempotent registration, the register-failure
 * WARN) and differs only by its symbol names, batch size, and stall
 * message. That common wiring is expressed once: the stages live in a
 * single `struct staged_stage_desc` table in pipeline order, and one
 * generic tick/stall/register helper reads a desc. The only genuinely
 * per-stage code — the on_stall LOG_WARN bodies and the init-failure
 * message — stays as tiny named functions/strings the table points at. */

#include "supervisors/staged_sync_supervisor.h"
#include "util/log_macros.h"
#include "util/reducer_drive_guard.h"
#include "supervisors/domains.h"
#include "storage/progress_store.h"
#include "validation/main_logic.h"

#include "util/supervisor.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Generous progress-quiet window shared by all eight stages:
 * 1800s (30 min) of IDLE before emitting a progress warning.
 * Stages legitimately idle for long stretches when the live chain is
 * waiting on input, so this is intentionally longer than the 900s
 * COORD_ESC_QUIET_US escalation window in chain_supervisor.c. */
#define STAGED_STAGE_QUIET_US ((int64_t)1800 * 1000 * 1000)

/* One stage of the eight-stage reducer pipeline. The function pointers
 * match the stage Job ABI exactly: init(struct main_state*)->bool,
 * drain(int max_steps)->int, cursor(void)->uint64_t. `log_stall` is the
 * one genuinely per-stage piece — the cursor/counter LOG_WARN body.
 *
 * The mutable runtime fields (contract / id / ms) live in the desc so a
 * single generic tick/stall handler can recover its stage from the
 * contract's `ctx` pointer (set once at register time, never mutated). */
struct staged_stage_desc {
    /* immutable wiring */
    const char *name;               /* supervisor child name */
    const char *init_fail_msg;      /* WARN text when init() returns false */
    bool      (*init)(struct main_state *ms);
    int       (*drain)(int max_steps);
    uint64_t  (*cursor)(void);
    int         batch;              /* max steps drained per tick */
    void      (*log_stall)(void);   /* per-stage stall LOG_WARN body */

    /* mutable runtime state, owned by this translation unit */
    struct liveness_contract contract;
    supervisor_child_id      id;    /* SUPERVISOR_INVALID_ID until registered */
    struct main_state       *ms;
};

/* ---- per-stage stall bodies (the only genuinely per-stage code) ---- */

static void header_admit_log_stall(void)
{
    /* A stall here means header admission is not moving; the condition
     * layer decides whether this is missing input or a live-chain stall. */
    LOG_WARN("supervisor", "[supervisor] staged.header_admit stalled " "(cursor=%llu admitted=%llu) — stage log behind live chain", (unsigned long long)header_admit_stage_cursor(), (unsigned long long)header_admit_stage_admitted_total());
}

static void validate_headers_log_stall(void)
{
    /* Validate can fall behind admit or wait on a stalled live chain; surface
     * the cursor gap and let conditions classify the cause. */
    LOG_WARN("supervisor", "[supervisor] staged.validate_headers stalled " "(cursor=%llu passed=%llu failed=%llu) — validator behind admit", (unsigned long long)validate_headers_stage_cursor(), (unsigned long long)validate_headers_stage_passed_total(), (unsigned long long)validate_headers_stage_failed_total());
}

static void body_fetch_log_stall(void)
{
    /* Stall = body_fetch falling behind validate OR bodies
     * not arriving on disk. Either way, surface but do nothing
     * destructive. */
    LOG_WARN("supervisor", "[supervisor] staged.body_fetch stalled " "(cursor=%llu observed=%llu skipped=%llu) — fetch behind validate", (unsigned long long)body_fetch_stage_cursor(), (unsigned long long)body_fetch_stage_observed_total(), (unsigned long long)body_fetch_stage_skipped_total());
}

static void body_persist_log_stall(void)
{
    LOG_WARN("supervisor", "[supervisor] staged.body_persist stalled " "(cursor=%llu verified=%llu upstream_failed=%llu read_failed=%llu) " "— persist behind body_fetch", (unsigned long long)body_persist_stage_cursor(), (unsigned long long)body_persist_stage_verified_total(), (unsigned long long)body_persist_stage_upstream_failed_total(), (unsigned long long)body_persist_stage_read_failed_total());
}

static void script_validate_log_stall(void)
{
    LOG_WARN("supervisor", "[supervisor] staged.script_validate stalled " "(cursor=%llu verified=%llu upstream_failed=%llu internal_error=%llu) " "— script validation behind body_persist", (unsigned long long)script_validate_stage_cursor(), (unsigned long long)script_validate_stage_verified_total(), (unsigned long long)script_validate_stage_upstream_failed_total(), (unsigned long long)script_validate_stage_internal_error_total());
}

static void proof_validate_log_stall(void)
{
    LOG_WARN("supervisor", "[supervisor] staged.proof_validate stalled " "(cursor=%llu verified=%llu upstream_failed=%llu internal_error=%llu) " "— proof validation behind script_validate", (unsigned long long)proof_validate_stage_cursor(), (unsigned long long)proof_validate_stage_verified_total(), (unsigned long long)proof_validate_stage_upstream_failed_total(), (unsigned long long)proof_validate_stage_internal_error_total());
}

static void utxo_apply_log_stall(void)
{
    LOG_WARN("supervisor", "[supervisor] staged.utxo_apply stalled " "(cursor=%llu verified=%llu upstream_failed=%llu internal_error=%llu) " "— UTXO apply behind proof_validate", (unsigned long long)utxo_apply_stage_cursor(), (unsigned long long)utxo_apply_stage_verified_total(), (unsigned long long)utxo_apply_stage_upstream_failed_total(), (unsigned long long)utxo_apply_stage_internal_error_total());
}

static void tip_finalize_log_stall(void)
{
    LOG_WARN("supervisor", "[supervisor] staged.tip_finalize stalled "
             "(cursor=%llu finalized=%llu upstream_failed=%llu reorg=%llu "
             "last_blocked=%s) — tip finalize behind utxo_apply or live tip",
             (unsigned long long)tip_finalize_stage_cursor(),
             (unsigned long long)tip_finalize_stage_finalized_total(),
             (unsigned long long)tip_finalize_stage_upstream_failed_total(),
             (unsigned long long)tip_finalize_stage_reorg_detected_total(),
             tip_finalize_stage_last_blocked_reason());
}

/* The eight stages in EXACT pipeline order (header_admit → … →
 * tip_finalize). Each INIT_DESC row binds one stage's name, init/drain/
 * cursor entry points, batch macro, init-failure WARN text, and stall
 * body. The register-failure WARN, period=2s, and the shared quiet window
 * are uniform and applied by the generic helper below. */
#define INIT_DESC(NM, FAILMSG, PFX, BATCH)              \
    { .name = (NM), .init_fail_msg = (FAILMSG),         \
      .init = PFX##_stage_init, .drain = PFX##_stage_drain, \
      .cursor = PFX##_stage_cursor, .batch = (BATCH),   \
      .log_stall = PFX##_log_stall,                     \
      .id = SUPERVISOR_INVALID_ID, .ms = NULL }

static struct staged_stage_desc g_stages[] = {
    INIT_DESC("staged.header_admit",
              "[supervisor] WARN staged.header_admit init failed — " "stage not running this boot",
              header_admit, HEADER_ADMIT_BATCH_PER_TICK),
    INIT_DESC("staged.validate_headers",
              "[supervisor] WARN staged.validate_headers init failed — " "validator not running this boot",
              validate_headers, VH_BATCH_PER_TICK),
    INIT_DESC("staged.body_fetch",
              "[supervisor] WARN staged.body_fetch init failed — " "fetch not running this boot",
              body_fetch, BODY_FETCH_BATCH_PER_TICK),
    INIT_DESC("staged.body_persist",
              "[supervisor] WARN staged.body_persist init failed — " "persist not running this boot",
              body_persist, BODY_PERSIST_BATCH_PER_TICK),
    INIT_DESC("staged.script_validate",
              "[supervisor] WARN staged.script_validate init failed — " "script validation not running this boot",
              script_validate, SCRIPT_VALIDATE_BATCH_PER_TICK),
    INIT_DESC("staged.proof_validate",
              "[supervisor] WARN staged.proof_validate init failed — " "proof validation not running this boot",
              proof_validate, PROOF_VALIDATE_BATCH_PER_TICK),
    INIT_DESC("staged.utxo_apply",
              "[supervisor] WARN staged.utxo_apply init failed — " "UTXO apply not running this boot",
              utxo_apply, UTXO_APPLY_BATCH_PER_TICK),
    INIT_DESC("staged.tip_finalize",
              "[supervisor] WARN staged.tip_finalize init failed — " "tip finalize not running this boot",
              tip_finalize, TIP_FINALIZE_BATCH_PER_TICK),
};

#undef INIT_DESC

#define STAGED_STAGE_COUNT (sizeof(g_stages) / sizeof(g_stages[0]))

/* progress.kv durability gate. During IBD / the bodies-only refold the
 * stage cursor commits do not need to fsync (a crash just replays from the
 * last durable cursor, idempotently), so synchronous=OFF removes the
 * per-block fsync from the fold's critical path. Once at-tip it MUST revert
 * to NORMAL so the live tip is not left in a wider crash-durability window.
 *
 * This is a pure I/O control — it never changes WHAT a stage computes or
 * commits, only when those bytes reach disk. We gate on the existing
 * is_initial_block_download() signal and issue the PRAGMA only on a
 * transition (tracked in g_progress_sync_ibd). -1 = not yet applied. */
static _Atomic int g_progress_sync_ibd = -1;

static void staged_sync_apply_progress_durability(struct main_state *ms)
{
    if (!ms) return;
    int want = is_initial_block_download(ms) ? 1 : 0;
    int have = atomic_load(&g_progress_sync_ibd);
    if (have == want) return;  /* no transition — skip the PRAGMA */
    if (progress_store_set_sync_mode(want != 0))
        atomic_store(&g_progress_sync_ibd, want);
}

/* Generic per-stage tick: drain a bounded batch each tick — keeps
 * progress.kv churn low and avoids starving other supervisor children —
 * then publish the cursor and heartbeat. Recovers its stage from the
 * contract's ctx pointer. */
static void staged_stage_tick(struct liveness_contract *c)
{
    struct staged_stage_desc *d = (struct staged_stage_desc *)c->ctx;
    if (!d || !d->ms) return;

    /* Keep progress.kv's durability mode tracking the IBD/at-tip signal.
     * Cheap: only flips the PRAGMA on an actual IBD<->at-tip transition. */
    staged_sync_apply_progress_durability(d->ms);
    /* Yield to an in-flight SYNCHRONOUS reducer drive (reducer_ingest_block on
     * the mining/submitblock/rebuild thread). The stages share the active-chain
     * window, which is not under the per-stage progress.kv lock, so draining a
     * stage here concurrently with that drive races and can record a permanent
     * failure row for the block being ingested. No-op for live network sync,
     * where reducer_ingest_block is never on the path so the flag stays 0; we
     * still heartbeat so the liveness contract does not trip on the skip. */
    if (reducer_drive_active()) {
        supervisor_progress(d->id, (int64_t)d->cursor());
        supervisor_tick(d->id);
        return;
    }
    (void)d->drain(d->batch);
    supervisor_progress(d->id, (int64_t)d->cursor());
    supervisor_tick(d->id);
}

/* Generic per-stage stall: defer to the stage's own LOG_WARN body. */
static void staged_stage_stall(struct liveness_contract *c)
{
    struct staged_stage_desc *d = (struct staged_stage_desc *)c->ctx;
    if (!d) return;
    d->log_stall();
}

/* Generic registration for one stage. Byte-equivalent to the prior
 * per-stage staged_*_register: idempotent on its own id, init-then-skip
 * on failure, period=2s, the shared quiet window, in the chain domain. */
static void staged_stage_register(struct staged_stage_desc *d,
                                  struct main_state *ms)
{
    if (!ms) return;
    if (d->id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    /* Bind the stage to the live chainstate. If progress_store didn't
     * open at boot, init returns false — log and skip supervisor wire
     * so a misconfigured boot doesn't loop on a perma-IDLE child. */
    if (!d->init(ms)) {
        LOG_WARN("supervisor", "%s", d->init_fail_msg);
        return;
    }

    d->ms = ms;
    liveness_contract_init(&d->contract, d->name);
    atomic_store(&d->contract.period_secs, (int64_t)2);
    /* Generous progress-quiet window: the stage can legitimately be IDLE
     * for long stretches when the live chain is stuck. 30 min before
     * we emit a progress warning. */
    atomic_store(&d->contract.progress_max_quiet_us, STAGED_STAGE_QUIET_US);
    d->contract.on_tick  = staged_stage_tick;
    d->contract.on_stall = staged_stage_stall;
    d->contract.ctx      = d;
    d->id = supervisor_register_in_domain(g_chain_sup, &d->contract);
    if (d->id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN %s register failed", d->name);
    }
}

void staged_sync_supervisor_register(struct main_state *ms)
{
    if (!ms) return;
    supervisor_domains_init();
    /* Pipeline order follows the reducer dataflow (table is in order). */
    for (size_t i = 0; i < STAGED_STAGE_COUNT; i++) {
        staged_stage_register(&g_stages[i], ms);
    }
}
