/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SELF_HEAL_SUPERVISOR_H
#define ZCL_SELF_HEAL_SUPERVISOR_H

struct main_state;

/* Register the self-heal engine as a child of the operations supervisor
 * domain (`g_op_sup`), wiring it to a 5-second tick that runs the
 * condition engine against `ms`. No deadline / progress-quiet stall.
 * Idempotent: a no-op if `ms` is NULL or the engine is already
 * registered. Logs a warning (does not abort) if registration fails. */
void self_heal_register(struct main_state *ms);

#endif /* ZCL_SELF_HEAL_SUPERVISOR_H */
