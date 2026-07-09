/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sticky escalator — the top-level ALWAYS-TERMINATING remedy ladder.
 *
 * This is the keystone of the sticky-node invariant S2: "Every stall reaches a
 * TERMINATING remedy; EV_OPERATOR_NEEDED is the LAST resort for genuinely-
 * unrecoverable local corruption only, never the response to a recoverable
 * class." See docs/work/sticky-node-plan.md #1 + #10.
 *
 * It is a single supervisor child (registered in g_op_sup) that consumes the
 * wedge signal raised by chain_tip_watchdog (deterministic_stall — the class
 * EVERY wedge produces) plus condition_engine_get_unresolved_count(), and drives
 * an ORDERED, PLUGGABLE rung ladder. A rung is advanced ONLY when the current
 * rung fails to make H-star/tip progress within a witness window. The deepest rung
 * re-arms with a cooldown — there is NO terminal give-up state on a recoverable
 * class. EV_OPERATOR_NEEDED, if emitted, is a NON-latching periodic "still
 * working / attention welcome" notice, never a stop.
 *
 * Verify-never-trust: no rung lowers a consensus gate. Rungs are node-local
 * recovery actions (sweep, reindex, re-derive, resnapshot, refold, peer
 * discovery, re-bootstrap). The actual chain-ADOPT step stays in the stages
 * (PoW + parity + anchor checks); the escalator only nudges re-derivation. */

#ifndef SERVICES_STICKY_ESCALATOR_H
#define SERVICES_STICKY_ESCALATOR_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct json_value;

/* The ordered rung ladder. Deeper = more disruptive / slower re-derivation.
 * Each must terminate; the deepest, given any honest peer set, always succeeds. */
enum sticky_rung {
    STICKY_RUNG_RETRY = 0,          /* sweep blockers + nudge conditions */
    STICKY_RUNG_TARGETED_REDERIVE,  /* blocker sweep + re-derive request */
    STICKY_RUNG_RESNAPSHOT,         /* re-pull a snapshot (lane: snapshot) */
    STICKY_RUNG_REINDEX,            /* bounded crash-only reindex-chainstate */
    STICKY_RUNG_SELF_MINT_REFOLD,   /* self-mint anchor + refold (lane: Act 3) */
    STICKY_RUNG_WIDEN_PEERS,        /* peer-discovery of last resort (lane #9) */
    STICKY_RUNG_REBOOTSTRAP,        /* re-bootstrap from genesis (floor rung) */
    STICKY_RUNG_COUNT
};

/* A rung action returns one of these. NOT_IMPLEMENTED and FAILED both advance
 * the ladder; PROGRESSING holds the current rung for another witness window;
 * RESOLVED clears the episode. A rung MUST be non-blocking (it runs on the
 * self-heal / supervisor tick thread) — it kicks off durable work and returns. */
enum sticky_rung_result {
    STICKY_RUNG_RESOLVED = 0,       /* symptom cleared by this rung */
    STICKY_RUNG_PROGRESSING,        /* work underway; hold this rung */
    STICKY_RUNG_FAILED,             /* rung ran, no progress -> advance */
    STICKY_RUNG_NOT_IMPLEMENTED,    /* default stub -> advance */
};

typedef enum sticky_rung_result (*sticky_rung_fn)(void);

/* Register the escalator as a supervisor child in g_op_sup. Idempotent.
 * Call after main_state is initialized and after supervisor_start(). */
void sticky_escalator_register(struct main_state *ms);

/* Give the reindex rung a datadir (the bounded boot_auto_reindex primitive needs
 * it). Optional; if unset the reindex rung degrades to a stub (advance). */
void sticky_escalator_set_datadir(const char *datadir);

/* Plug a real action into a rung, replacing its default stub. Lets another lane
 * wire its rung (resnapshot/refold/widen-peers/re-bootstrap) WITHOUT editing the
 * escalator. NULL fn restores the stub. Idempotent. */
void sticky_escalator_register_rung(enum sticky_rung rung, sticky_rung_fn fn);

/* Arm (or keep armed) the ladder from a named chain-tip stall cause. The
 * chain_tip watchdog calls this on deterministic stalls or exhausted restart
 * budgets; worker-scoped stalls raise typed blockers instead. Cheap +
 * reentrant-safe; the actual rung work happens on the tick. */
void sticky_escalator_note_stall(const char *cause);

/* Run ONE ladder step: re-evaluate progress, advance/hold/reset the rung, and
 * dispatch the current rung action if it is time. Called BOTH from the self-heal
 * tick (5 s cadence) and the escalator's own supervisor tick (30 s) so the
 * ladder advances even if one driver stalls. No-op while disarmed. */
void sticky_escalator_drive(void);

/* `zcl_state subsystem=sticky_escalator` dumper. `out` is an already-initialized
 * json object; `key` is ignored. */
bool sticky_escalator_dump_state_json(struct json_value *out, const char *key);

const char *sticky_rung_name(enum sticky_rung r);

/* Tunables (seconds / blocks). */
#define STICKY_PROGRESS_MARGIN        2     /* H* climb that clears an episode */
#define STICKY_REARM_COOLDOWN_SECS    120   /* deepest-rung re-arm pause */
#define STICKY_PAGE_MIN_INTERVAL_SECS 300   /* non-latching page rate limit */

#ifdef ZCL_TESTING
void sticky_escalator_test_reset(void);
/* Drive the ladder with an injected provable-tip and monotonic clock, running
 * the REAL advance/hold/reset logic. Returns the rung the ladder is now on. */
enum sticky_rung sticky_escalator_test_drive(int64_t injected_tip,
                                             int64_t now_unix);
enum sticky_rung sticky_escalator_test_current_rung(void);
bool sticky_escalator_test_armed(void);
uint64_t sticky_escalator_test_fires_operator_needed(void);
#endif

#endif /* SERVICES_STICKY_ESCALATOR_H */
