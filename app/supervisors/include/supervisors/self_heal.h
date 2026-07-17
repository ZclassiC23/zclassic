/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SELF_HEAL_SUPERVISOR_H
#define ZCL_SELF_HEAL_SUPERVISOR_H

struct main_state;

/* Register the self-heal engine as a child of the operations supervisor
 * domain (`g_op_sup`), wiring it to a 5-second tick that runs the
 * condition engine against `ms`. No deadline / progress-quiet stall.
 * Idempotent: a no-op if `ms` is NULL or the engine is already
 * registered. On registration failure (registry full), logs a warning
 * AND sets a PERMANENT "self_heal.unsupervised" blocker + emits
 * EV_OPERATOR_NEEDED — this is the only periodic hook that could retry,
 * and it cannot run without this same registration, so the loud blocker
 * is the deliverable, not a silent bare log. */
void self_heal_register(struct main_state *ms);

#ifdef ZCL_TESTING
/* Reset module state (g_ms, g_id, g_registered) between tests. */
void self_heal_test_reset(void);
#endif

#endif /* ZCL_SELF_HEAL_SUPERVISOR_H */
