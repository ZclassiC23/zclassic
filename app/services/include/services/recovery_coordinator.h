/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * recovery_coordinator — one supervised organ that UNIFIES the scattered
 * recovery rungs behind a single cheapest-sufficient selector.
 *
 * The node already owns several healers, each proven on its own:
 *   - the reducer cursor warm-restart clamp
 *     (jobs/stage_repair.h: stage_reconcile_clamp_tip_finalize_to_floor),
 *   - the bounded range re-derive
 *     (jobs/stage_repair.h: stage_reducer_frontier_reconcile_light),
 *   - the sealed-segment refetch-by-hash healer
 *     (conditions/segment_corruption.h: segment_corruption_repair).
 *
 * This coordinator does NOT re-implement any of them. On a detected
 * inconsistency it selects the CHEAPEST sufficient rung, in cost order —
 * (1) cursor warm restart, (2) bounded range re-derive, (3) segment
 * refetch-by-hash, (4) name a typed blocker if none applies — calls that
 * healer's existing entry point, and records which rung fired + its outcome
 * for `dumpstate recovery`.
 *
 * LOCK-ORDER LAW: the coordinator runs on a supervisor tick (chain domain),
 * never on a reducer drive path, and never takes a coins-store csr->lock. The
 * healers it calls acquire only their own store/main locks. */

#ifndef ZCL_SERVICES_RECOVERY_COORDINATOR_H
#define ZCL_SERVICES_RECOVERY_COORDINATOR_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct json_value;
struct sqlite3;

/* Recovery rungs, cheapest first. The coordinator picks the lowest-numbered
 * rung whose class is present and acts through that rung's existing healer. */
enum recovery_rung {
    RECOVERY_RUNG_NONE                = 0,
    RECOVERY_RUNG_CURSOR_WARM_RESTART = 1, /* journal replay / cursor clamp */
    RECOVERY_RUNG_REDERIVE_RANGE      = 2, /* bounded stage range re-derive  */
    RECOVERY_RUNG_SEGMENT_REFETCH     = 3, /* segment refetch-by-hash        */
    RECOVERY_RUNG_BLOCKER             = 4, /* name a typed blocker           */
    RECOVERY_RUNG_COUNT
};

/* Outcome of the rung that fired. */
enum recovery_outcome {
    RECOVERY_OUTCOME_NONE        = 0,
    RECOVERY_OUTCOME_RECOVERED   = 1, /* class cleared outright             */
    RECOVERY_OUTCOME_PROGRESSING = 2, /* durable repair staged; stages re-derive */
    RECOVERY_OUTCOME_NOOP        = 3, /* nothing actionable                 */
    RECOVERY_OUTCOME_BLOCKED     = 4, /* named a typed blocker              */
};

const char *recovery_rung_name(enum recovery_rung r);
const char *recovery_outcome_name(enum recovery_outcome o);

/* One recovery pass. The caller has DETECTED an inconsistency (a stall / an
 * armed critical condition); this selects the cheapest sufficient rung, runs
 * its healer, records the result, and returns the rung that fired. When no
 * rung 1-3 class is present it names a typed blocker (rung 4) — a silent halt
 * is unrepresentable.
 *
 * `db`/`ms` are the progress store + main_state (pass NULL to skip the
 * progress-store rungs); `segments_dir` is the sealed-segment directory to
 * spot-verify (NULL skips rung 3); `coins_best` is the durably-applied coins
 * frontier used for the cursor-clamp floor (pass -1 when unknown). */
struct recovery_ctx {
    struct sqlite3    *db;
    struct main_state *ms;
    const char        *segments_dir;
    int                coins_best;
    uint32_t          *segment_scan_cursor; /* round-robin, caller-owned; may be NULL */
    /* Scratch filled by the segment rung's detect, consumed by its remedy. */
    uint32_t           seg_first;
    uint32_t           seg_count;
};

enum recovery_rung recovery_coordinator_run(struct recovery_ctx *ctx,
                                            enum recovery_outcome *out_outcome);

/* Supervised organ: registers a chain-domain liveness contract and, when a
 * critical condition is unresolved, drives one recovery pass per tick. */
void recovery_coordinator_register(struct main_state *ms);
void recovery_coordinator_set_datadir(const char *datadir);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
bool recovery_coordinator_dump_state_json(struct json_value *out, const char *key);

/* A rung attempt seam. Returns true iff this rung's class was present and it
 * acted (setting *outcome); false falls through to the next rung. Defaults
 * call the real healers; tests may override to exercise the selector in
 * isolation. */
typedef bool (*recovery_rung_fn)(struct recovery_ctx *ctx,
                                 enum recovery_outcome *outcome);

#ifdef ZCL_TESTING
void recovery_coordinator_test_reset(void);
/* Override rung 1..3's attempt fn (RECOVERY_RUNG_CURSOR_WARM_RESTART ..
 * RECOVERY_RUNG_SEGMENT_REFETCH). NULL restores the default. */
void recovery_coordinator_test_set_rung_fn(enum recovery_rung r,
                                           recovery_rung_fn fn);
enum recovery_rung recovery_coordinator_test_last_rung(void);
enum recovery_outcome recovery_coordinator_test_last_outcome(void);
#endif

#endif /* ZCL_SERVICES_RECOVERY_COORDINATOR_H */
