/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Typed blocker primitive — every "blocked" state in the codebase is
 * a typed record with a class, deadline, and escape action.
 *
 * Why this exists
 * ----------------
 * Before Round 6, "blocked" was a string. legacy_mirror_sync_service
 * stored a `char activation_blocker[128]` text field; chain_advance_
 * coordinator's `mir->blocked` was a bare bool computed from whether
 * that string was non-empty. There was no way to ask:
 *
 *   - Is this blocker recoverable, or permanent?
 *   - What's the deadline before something must escalate?
 *   - What's the escape action when the deadline lapses?
 *   - How many times has it re-fired? Is it spam?
 *
 * Concretely: on 2026-05-21 the live node ran 4.3 days with
 * `activation_blocker = "activation-no-progress"` re-firing every ~3 s
 * (~5/sec at peak), 193+ counter increments in 10 s of telemetry —
 * because the underlying recorder had no de-duplication and the
 * coordinator had no durable way to distinguish a transient blocker
 * from a permanent one. The Round 5 supervisor was firing recoveries,
 * but the recovery target itself was wedged behind a typeless blocker
 * that had no escape.
 *
 * The primitive
 * --------------
 * A blocker is a typed record:
 *   - `class` ∈ {PERMANENT, TRANSIENT, DEPENDENCY, RESOURCE}
 *   - identity (`id`, `owner_subsystem`)
 *   - timing (`since_us`, `escape_deadline_us`, derived `age_us`)
 *   - escape (`escape_action` name; supervisor dispatches via name→fn)
 *   - retry accounting (`retry_count`, `retry_budget`)
 *   - reason (human + structured tail)
 *
 * Sites that previously set a string blocker now call `blocker_set`
 * with a typed record. The primitive de-duplicates within a token-
 * bucket window (5/min default). A dedicated supervisor child
 * (`chain.blocker_escape`) sweeps the registry, calls the registered
 * `escape_action` on deadline edge.
 *
 * Modern-architecture analogues this implements:
 *   - Hystrix circuit breaker (closed/open/half-open), per-blocker
 *   - Saga compensations: every blocker NAMES its escape, no global handlers
 *   - Observability-driven (Charity Majors): the blocker IS the metric
 *   - Erlang/OTP supervisor signal-only restart: escape is a signal,
 *     child observes and acts on its own loop
 *
 * Ownership / threading rules
 * ----------------------------
 * The primitive owns a static fixed-cap registry (BLOCKER_CAP=128).
 * Records are COPIED into the registry on `blocker_set` — caller can
 * stack-allocate. Reads always go through `blocker_snapshot_all`
 * (copies under lock, releases before return). The supervisor child
 * snapshot-and-dispatches outside the lock; escape callbacks may
 * freely call any blocker_* function. */

#ifndef ZCL_UTIL_BLOCKER_H
#define ZCL_UTIL_BLOCKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCKER_ID_MAX        64
#define BLOCKER_OWNER_MAX     40
#define BLOCKER_REASON_MAX    256
#define BLOCKER_ACTION_MAX    64
#define BLOCKER_CAP           128
#define BLOCKER_ESCAPE_CAP    32

/* Default rate limit: a blocker_set with the same `id` more frequently
 * than this is suppressed (fire_count still increments). Tunable via
 * env `ZCL_BLOCKER_RATE_LIMIT_MS` for testing. */
#define BLOCKER_DEFAULT_RATE_LIMIT_MS  12000  /* 5/min */

enum blocker_class {
    BLOCKER_PERMANENT  = 0, /* bad PoW, malformed block, consensus reject
                             * — never auto-retry. Only operator clears. */
    BLOCKER_TRANSIENT  = 1, /* UTXO miss at the time, peer down, queue full
                             * — retry with bounded budget. Default class. */
    BLOCKER_DEPENDENCY = 2, /* waiting on subsystem X — escalate to X's
                             * supervisor child. Escape fires X's escape. */
    BLOCKER_RESOURCE   = 3, /* OOM, disk full, fd exhaustion — operator
                             * action required; escape opens circuit. */
};

const char *blocker_class_name(enum blocker_class c);

/* Caller-owned template. Pass to blocker_set; the registry COPIES it. */
struct blocker_record {
    char     id[BLOCKER_ID_MAX];
    char     owner_subsystem[BLOCKER_OWNER_MAX];
    enum blocker_class class;
    int64_t  escape_deadline_secs;    /* relative from since_us; 0 = none */
    char     escape_action[BLOCKER_ACTION_MAX];
    int32_t  retry_budget;            /* -1 = unbounded (PERMANENT only) */
    char     reason[BLOCKER_REASON_MAX];
};

/* Convenience initializer; zeroes the struct and fills required fields,
 * then defaults retry_budget (-1 for PERMANENT, else 0).
 * Return contract:
 *   false — `out`, `id`, or `owner` is NULL, or `id`/`owner` would not
 *           fit (>= BLOCKER_ID_MAX / BLOCKER_OWNER_MAX). The offending
 *           case is logged via LOG_FAIL; `out` may be partially written.
 *   true  — fields stored. `reason` is optional (NULL leaves it empty)
 *           and is truncated to fit BLOCKER_REASON_MAX, never rejected. */
bool blocker_init(struct blocker_record *out,
                  const char *id,
                  const char *owner,
                  enum blocker_class class,
                  const char *reason);

/* Set or refresh a blocker. If `r->id` already exists in the registry:
 *   - rate-limited: if last set < rate-limit, only fire_count++; returns 1
 *   - otherwise: replaces fields, fire_count++, since_us untouched; returns 0
 * If new: inserts a record, since_us = now, retry_count = 0; returns 0.
 * Capacity exhaustion → logs LOG_FAIL and returns -1.
 *
 * Return code contract:
 *   0  — fresh write stored (caller may emit event)
 *   1  — rate-limited dup (caller SHOULD suppress event emission)
 *  -1  — error (bad input, cap exhausted) */
int blocker_set(const struct blocker_record *r);

/* Mark a retry attempt against a blocker. Increments retry_count.
 * If retry_count >= retry_budget (and budget > 0), emits EV_
 * BLOCKER_BUDGET_EXHAUSTED via supervisor sweep next tick. */
void blocker_record_retry(const char *id);

/* Remove a blocker. No-op if not present. */
void blocker_clear(const char *id);

/* Test existence + class. Returns -1 if absent. */
int  blocker_class_for(const char *id);

bool blocker_exists(const char *id);

/* Snapshot view for read-side consumers. */
struct blocker_snapshot {
    char     id[BLOCKER_ID_MAX];
    char     owner_subsystem[BLOCKER_OWNER_MAX];
    int      class;                            /* enum blocker_class */
    int64_t  since_us;
    int64_t  age_us;                           /* now - since_us */
    int64_t  escape_deadline_us;               /* absolute deadline, or 0 */
    int64_t  deadline_remaining_us;            /* signed; negative if elapsed */
    char     escape_action[BLOCKER_ACTION_MAX];
    int32_t  retry_count;
    int32_t  retry_budget;
    uint32_t fire_count;
    char     reason[BLOCKER_REASON_MAX];
};

/* Copy up to `max` snapshots into `out`. Returns count actually written. */
int blocker_snapshot_all(struct blocker_snapshot *out, int max);

/* Same locked registry copy with the metadata that belongs to that exact
 * snapshot.  `generation_out` advances on every observable registry change;
 * consumers can bind counts/rendering to one captured generation instead of
 * combining independent read calls.  Optional metadata pointers may be NULL. */
int blocker_snapshot_all_with_meta(struct blocker_snapshot *out, int max,
                                   uint64_t *generation_out,
                                   int *escape_dispatched_out,
                                   int *rate_limit_ms_out);

/* Causal operator-status ordering. Resource exhaustion remains the highest
 * generic class; the two shielded-history gaps outrank other permanent
 * symptoms (for example script_validate.prevout_unresolved) because those
 * downstream symptoms cannot be cured until the missing consensus history is
 * restored. All status surfaces use this one selector so they cannot disagree
 * about the primary blocker. The returned pointer aliases caller-owned
 * `snapshots` and remains valid only as long as that array does. */
int blocker_causal_priority(enum blocker_class c, const char *id);
const struct blocker_snapshot *blocker_select_dominant(
    const struct blocker_snapshot *snapshots, int count);

/* Count of active blockers in a class. */
int blocker_count_by_class(enum blocker_class c);

/* Total active blockers. */
int blocker_count_active(void);

/* JSON dumper for diagnostics_controller g_dumpers + zcl_state subsystem=blocker.
 * Caller initializes `out` with json_set_object before calling. */
struct json_value;
bool blocker_dump_state_json(struct json_value *out, const char *key);

/* Escape dispatcher.
 * Subsystems register an action name → function. On deadline edge,
 * supervisor sweep dispatches; missing actions log a warning but do
 * not crash. Callback receives a SNAPSHOT (not a live record). */
typedef void (*blocker_escape_fn)(const struct blocker_snapshot *snap);

/* Register `action_name` → `fn`. Caller-owned name pointer must be
 * static-lifetime (interned). Up to BLOCKER_ESCAPE_CAP entries; returns
 * false on overflow or duplicate. */
bool blocker_register_escape(const char *action_name, blocker_escape_fn fn);

/* Look up the function registered for an action name. NULL if unknown. */
blocker_escape_fn blocker_lookup_escape(const char *action_name);

/* Sweep registry once: for each blocker whose escape_deadline_us has
 * elapsed and whose escape has not yet fired in the current deadline
 * crossing, fire the escape. Edge-triggered. Returns the number of
 * escapes fired. Intended for a supervisor child running ~1 Hz. */
int blocker_supervisor_sweep(void);

/* Monotonic count of escape dispatches since module init. Exposed for
 * Prometheus + zcl_blockers MCP tool. */
int blocker_escape_dispatched_count(void);

/* Lifecycle: init at boot, before any blocker_set call. shutdown is
 * idempotent. */
bool blocker_module_init(void);
void blocker_module_shutdown(void);

/* Test hooks — wipe state, override clock, query private counters. */
void blocker_reset_for_testing(void);
void blocker_set_clock_for_testing(int64_t now_us);
void blocker_advance_clock_for_testing(int64_t delta_us);
uint32_t blocker_fire_count_for_testing(const char *id);
void blocker_set_rate_limit_ms_for_testing(int ms);

#endif /* ZCL_UTIL_BLOCKER_H */
