/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_phase — per-step stall logger for the boot sequence.
 *
 * A scoped wrapper around a boot step that:
 *   * logs `[boot-phase] BEGIN <name>` at start
 *   * registers a 30s stall entry in lib/health so if the phase
 *     hangs, `[boot-phase] STALL <name> <elapsed_ms>ms` appears in
 *     node.log on the heartbeat sweeper's tick (~1s after the
 *     deadline)
 *   * logs `[boot-phase] END <name> <elapsed_ms>ms` on completion,
 *     unregistering the health entry
 *
 * Usage:
 *   struct boot_phase p;
 *   boot_phase_begin(&p, "block_index_load");
 *   ... slow work ...
 *   boot_phase_end(&p);
 *
 * Before this module used the unified watchdog, every boot phase
 * spawned its own pthread that polled an atomic flag. With ~10 boot
 * phases that was 10 transient threads. After: zero. The heartbeat
 * sweeper (lib/health) does the work for all of them, and operator
 * visibility (STALL fires once per phase that exceeds 30s) is
 * preserved. */

#ifndef ZCL_BOOT_PHASE_H
#define ZCL_BOOT_PHASE_H

#include "health/heartbeat.h"

#include <stdbool.h>
#include <stdint.h>

#define BOOT_PHASE_NAME_MAX 64

struct boot_phase {
    char                name[BOOT_PHASE_NAME_MAX];
    int64_t             start_ms;
    health_subsystem_id health_id;
};

void boot_phase_begin(struct boot_phase *p, const char *name);
void boot_phase_end(struct boot_phase *p);

/* ──────────────────────────────────────────────────────────────────
 * Coarse-grained boot ordering invariants (Campaign C1).
 *
 * `app_init` historically grew to ~2,100 LOC of sequential side
 * effects across 30+ named `boot_step_*` functions plus inline
 * wallet/chain logic. The load-bearing ordering invariants
 * (e.g., "coins.db must commit before block_index fsync") lived in
 * commit messages, not the type system, and a misorder produced
 * silent corruption rather than a compile-time error.
 *
 * `boot_stage_advance_to()` codifies the major boundaries:
 *
 *   STAGE_INIT  → process barely up — signal handlers + log only.
 *   STAGE_DATADIR_LOCKED  → datadir picked, lock file held, chain
 *                           params selected, unclean shutdown detected.
 *   STAGE_CRYPTO_READY  → ECC + SHA self-tests passed, main_state
 *                         initialized.
 *   STAGE_DB_OPEN  → node.db + coins.db opened, migrations applied.
 *   STAGE_WALLET_LOADED  → wallet keys read, canary self-test OK,
 *                          STATE D/E/F invariants satisfied.
 *   STAGE_BLOCK_INDEX_LOADED  → block_index loaded from LevelDB.
 *   STAGE_CHAIN_TIP_RESOLVED  → tip established, CSR consistent.
 *   STAGE_NETWORK_READY  → connman + peer manager initialized.
 *   STAGE_SERVICES_RUNNING  → background services (disk monitor,
 *                             ibd throttle, db maintenance) started.
 *   STAGE_READY  → HTTPS + RPC listening, accepting requests.
 *   STAGE_SHUTDOWN_REQUESTED  → app_shutdown entered.
 *   STAGE_SHUTDOWN_COMPLETE  → resources released.
 *
 * Each `boot_stage_advance_to(next)` call asserts the current stage
 * is the immediate predecessor (or equal — idempotent re-advance is
 * a no-op). A misorder calls `abort()` with a precise diagnostic so
 * the bug surfaces at the failing call site, not as later silent
 * corruption.
 *
 * Cross-reference: BOOT_INVARIANTS.md documents what each stage
 * guarantees about global state.
 */
enum boot_stage {
    BOOT_STAGE_INIT = 0,
    BOOT_STAGE_DATADIR_LOCKED,
    BOOT_STAGE_CRYPTO_READY,
    BOOT_STAGE_DB_OPEN,
    BOOT_STAGE_WALLET_LOADED,
    BOOT_STAGE_BLOCK_INDEX_LOADED,
    BOOT_STAGE_CHAIN_TIP_RESOLVED,
    BOOT_STAGE_NETWORK_READY,
    BOOT_STAGE_SERVICES_RUNNING,
    BOOT_STAGE_READY,
    BOOT_STAGE_SHUTDOWN_REQUESTED,
    BOOT_STAGE_SHUTDOWN_COMPLETE,
    BOOT_STAGE__MAX
};

const char *boot_stage_name(enum boot_stage s);
enum boot_stage boot_stage_current(void);

/* Advance to `next`. Aborts unless the current stage is `next` (no-op)
 * or `next - 1` (normal forward step). Shutdown stages may also be
 * entered from any non-shutdown stage (operator may halt mid-boot). */
void boot_stage_advance_to(enum boot_stage next);

/* Soft check used by lint / diagnostics. Returns false (without abort)
 * if `s` is not the current stage. Use for read-only assertions. */
bool boot_stage_is(enum boot_stage s);

#ifdef ZCL_TESTING
/* Test-only escape hatch: reset the global stage back to INIT so unit
 * tests can exercise the advance state machine without polluting the
 * stage observed by later tests in the same process. Production code
 * MUST NOT call this — it would defeat the misorder-detection invariant.
 * Only compiled in -DZCL_TESTING builds (test_zcl, test_parallel). */
void boot_stage_reset_for_testing(void);
#endif

#endif
