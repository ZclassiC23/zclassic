/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_FRAMEWORK_CONDITION_H
#define ZCL_FRAMEWORK_CONDITION_H

#include "json/json.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define CONDITION_MAX_NAME 64
#define CONDITION_MAX_REGISTRY 64

enum condition_severity {
    COND_INFO = 0,
    COND_WARN,
    COND_CRITICAL,
};

enum condition_remedy_result {
    COND_REMEDY_OK = 0,
    COND_REMEDY_FAILED,
    COND_REMEDY_SKIP,
    /* Engine-only synthetic outcome: a remedy returned COND_REMEDY_OK but
     * the condition's witness did NOT confirm the symptom cleared within
     * witness_window_secs. Doctrine: "A remedy that returns ok must resolve
     * the symptom." The engine downgrades an un-witnessed remedy to this so
     * node.log never shows result=ok for a symptom that persists. A remedy
     * function MUST NOT return this value itself. */
    COND_REMEDY_UNWITNESSED,
};

typedef bool (*condition_detect_fn)(void);
typedef enum condition_remedy_result (*condition_remedy_fn)(void);
typedef bool (*condition_witness_fn)(int64_t target_at_detect);
typedef bool (*condition_detail_fn)(struct json_value *out);

struct condition_state {
    _Atomic int64_t first_detect_unix;
    _Atomic int64_t last_poll_unix;
    _Atomic int64_t last_remedy_unix;
    _Atomic int64_t last_operator_needed_unix;
    _Atomic int64_t target_at_detect;
    _Atomic int attempts;
    _Atomic int last_outcome;
    _Atomic bool currently_active;
    _Atomic bool operator_needed_emitted;
    _Atomic int cleared_count;
    /* Continue-with-cooldown tier (sticky-node plan #7). For an
     * external-resource-dependent condition (peers/oracle absent), giving
     * up forever at max_attempts is a human dead-end on a RECOVERABLE class.
     * When cooldown_secs > 0 the engine re-arms the remedy after that long
     * backoff instead of latching: last_cooldown_unix gates the re-arm gap,
     * cooldown_rearms counts re-arms in the current fault episode, and
     * cooldown_episode_key is the fault identity at the last re-arm. A change
     * in fault identity (the condition's target_at_detect moved) starts a
     * fresh budget, exactly like chain_tip_watchdog.c's episode keying. */
    _Atomic int64_t last_cooldown_unix;
    _Atomic int cooldown_rearms;
    _Atomic int64_t cooldown_episode_key;
};

struct condition {
    const char *name;
    enum condition_severity severity;
    int poll_secs;
    int backoff_secs;
    int max_attempts;
    /* Continue-with-cooldown (sticky-node plan #7). 0 = legacy behavior:
     * latch permanently at max_attempts (correct for a genuinely-local,
     * deterministic-unrecoverable condition). > 0 = after max_attempts the
     * engine re-arms the remedy every cooldown_secs (a long 5-15 min backoff)
     * so an external-resource stall (peers/oracle) never permanently gives
     * up. cooldown_max_rearms bounds re-arms within ONE fault episode (a
     * continuous active span with an unchanged fault identity); 0 = unbounded
     * (the right default for a purely-transient external dependency). The
     * operator page still fires once per episode at max_attempts so a human
     * is informed, but the node keeps trying to self-heal. */
    int cooldown_secs;
    int cooldown_max_rearms;
    condition_detect_fn detect;
    condition_remedy_fn remedy;
    condition_witness_fn witness;
    condition_detail_fn detail;
    int witness_window_secs;
    struct condition_state state;
};

struct condition_runtime_snapshot {
    bool registered;
    enum condition_severity severity;
    int poll_secs;
    int backoff_secs;
    int max_attempts;
    int witness_window_secs;
    bool currently_active;
    bool operator_needed_emitted;
    int attempts;
    int last_outcome;
    int cleared_count;
};

struct main_state;

bool condition_register(const struct condition *cond);
bool condition_engine_has_registered(const char *name);
bool condition_engine_get_registered_snapshot(
    const char *name, struct condition_runtime_snapshot *out);
void condition_engine_tick(void);
bool condition_engine_dump_state_json(struct json_value *out, const char *key);
int condition_engine_get_active_count(void);
int condition_engine_get_unresolved_count(void);
void condition_engine_set_main_state(struct main_state *ms);
struct main_state *condition_engine_main_state(void);

const char *condition_severity_name(enum condition_severity s);
const char *condition_remedy_result_name(enum condition_remedy_result r);

#ifdef ZCL_TESTING
void condition_engine_reset_for_testing(void);

/* Zero the shared per-condition state atoms that every *_test_reset() must
 * clear between tests: attempts, last_outcome (-> COND_REMEDY_SKIP),
 * currently_active, operator_needed_emitted. Module-static detect/remedy
 * bookkeeping is each condition's own responsibility. */
void condition_reset_state(struct condition *c);
#endif

#endif /* ZCL_FRAMEWORK_CONDITION_H */
