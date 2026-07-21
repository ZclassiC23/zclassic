/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * checkpoint_bundle_install_ready — see conditions/checkpoint_bundle_install_ready.h.
 *
 * This condition NEVER installs in-process: the atomic consensus-state installer
 * (consensus_state_snapshot_install_activate) resets the reducer's stage cursors
 * and tip_finalize log and must run single-threaded at boot, not concurrently
 * with the live reducer. So the remedy ARMS the durable install-on-next-boot
 * request and pulls the supervised-respawn trigger — byte-for-byte the shielded
 * ladder's Rung B pattern (shielded_selfheal_ladder.c) — and the install runs at
 * the next boot's app_init, where the header chain is durably at the checkpoint.
 * CONSENSUS PARITY: nothing here relaxes a check; it only re-times the EXISTING
 * ROM/PoW-gated installer to fire once the header chain genuinely owns the
 * compiled checkpoint block. */

#include "framework/condition.h"

#include "conditions/checkpoint_bundle_install_ready.h"

#include "chain/checkpoints.h"                        /* get_sha3_utxo_checkpoint */
#include "config/consensus_state_install_runtime.h"   /* autodetect + arm + ready predicate */
#include "controllers/agent_controller.h"             /* agent_runtime_context_datadir */
#include "event/event.h"
#include "jobs/reducer_frontier.h"                     /* reducer_frontier_provable_tip_cached */
#include "services/chain_tip_watchdog.h"               /* chain_tip_watchdog_request_respawn */
#include "services/sync_monitor.h"                     /* sync_monitor_main_state */
#include "util/log_macros.h"
#include "util/thread_registry.h"                      /* thread_registry_request_shutdown */
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CBIR_SUBSYS "condition"
#define CBIR_NAME   "checkpoint_bundle_install_ready"

#ifdef ZCL_TESTING
static _Atomic(struct main_state *) g_test_ms;
static const char *g_test_datadir;
static _Atomic bool g_test_suppress_restart;
static _Atomic int g_test_remedy_calls;
#endif

static struct main_state *cbir_main_state(void)
{
#ifdef ZCL_TESTING
    struct main_state *t = atomic_load(&g_test_ms);
    if (t)
        return t;
#endif
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        ms = condition_engine_main_state();
    return ms;
}

static const char *cbir_datadir(void)
{
#ifdef ZCL_TESTING
    if (g_test_datadir && g_test_datadir[0])
        return g_test_datadir;
#endif
    return agent_runtime_context_datadir();
}

/* The genuine instant-on window: the node has NOT yet folded/served past the
 * checkpoint (H* below it). A node already at/above the checkpoint owns its own
 * self-derived state and must never be pulled back onto a bundle. */
static bool cbir_below_checkpoint(const struct sha3_utxo_checkpoint *cp)
{
    return cp && reducer_frontier_provable_tip_cached() < cp->height;
}

static bool detect_checkpoint_bundle_install_ready(void)
{
    struct main_state *ms = cbir_main_state();
    const char *datadir = cbir_datadir();
    if (!ms || !datadir || !datadir[0])
        return false;

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp || !cbir_below_checkpoint(cp))
        return false; /* already at/above the checkpoint: not the instant-on case */

    /* The header chain must genuinely own the compiled checkpoint block before
     * the install can bind — the exact precondition the boot-time deferral waits
     * on. */
    if (!consensus_state_checkpoint_header_ready(ms))
        return false; // raw-return-ok:detect-poll-headers-not-yet-at-checkpoint-is-normal

    /* A staged, installable bundle must still be present. autodetect returns
     * NULL once the sovereign marker exists (installed), when no installable
     * bundle is present under bundles/, or when every candidate carries a
     * .failed sibling. Side-effect free (pure discovery). */
    char *bundle = boot_autodetect_consensus_bundle(datadir);
    if (!bundle)
        return false;
    free(bundle);
    return true;
}

static enum condition_remedy_result remedy_checkpoint_bundle_install_ready(void)
{
#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    struct main_state *ms = cbir_main_state();
    const char *datadir = cbir_datadir();
    if (!ms || !datadir || !datadir[0])
        return COND_REMEDY_SKIP; // raw-return-ok:runtime-not-wired-this-tick

    /* Re-confirm at remedy time: never respawn to install a bundle whose
     * checkpoint header is not (still) genuinely on-chain. */
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp || !cbir_below_checkpoint(cp) ||
        !consensus_state_checkpoint_header_ready(ms))
        return COND_REMEDY_SKIP; // raw-return-ok:precondition-regressed-this-tick

    char *bundle = boot_autodetect_consensus_bundle(datadir);
    if (!bundle)
        return COND_REMEDY_SKIP; // raw-return-ok:installed/failed-between-detect-and-remedy

    /* Arm the durable, bounded install-on-next-boot request. Idempotent while
     * pending (the attempt count only bumps at CONSUME/boot), so repeated ticks
     * never inflate the budget; a spent budget returns TERMINAL. */
    int armed = boot_install_bundle_request(datadir, bundle);
    if (armed == BOOT_INSTALL_BUNDLE_TERMINAL) {
        LOG_WARN(CBIR_SUBSYS,
                 "[condition:%s] install-on-next-boot budget exhausted for %s — "
                 "NOT respawning (operator paged); a genuinely uninstallable "
                 "bundle degrades to normal sync", CBIR_NAME, bundle);
        free(bundle);
        return COND_REMEDY_FAILED;
    }
    if (armed <= 0) {
        LOG_WARN(CBIR_SUBSYS,
                 "[condition:%s] could not arm install-on-next-boot for %s",
                 CBIR_NAME, bundle);
        free(bundle);
        return COND_REMEDY_FAILED;
    }

    LOG_WARN(CBIR_SUBSYS,
             "[condition:%s] header chain reached the checkpoint (h=%d) — ARMED "
             "checkpoint-bundle install-on-next-boot of %s (attempt %d); "
             "requesting a supervised respawn so the atomic ROM/PoW-gated "
             "installer runs at the next single-threaded boot",
             CBIR_NAME, cp->height, bundle, armed);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=checkpoint_bundle_install_armed height=%d attempt=%d",
                cp->height, armed);
    free(bundle);

#ifdef ZCL_TESTING
    if (!atomic_load(&g_test_suppress_restart))
#endif
    {
        chain_tip_watchdog_request_respawn();
        thread_registry_request_shutdown();
    }
    return COND_REMEDY_OK;
}

static bool witness_checkpoint_bundle_install_ready(int64_t target_at_detect)
{
    (void)target_at_detect;
    /* The install runs on the NEXT boot; the honest OBSERVABLE clear is that the
     * node now serves state at or above the checkpoint — H* (the provable tip)
     * climbed onto the installed bundle. Read the live provable tip, not a flag:
     * in THIS process (pre-respawn) it is still below the checkpoint (correctly
     * unwitnessed — the respawn is the forward step), and the reboot's install
     * makes it true. */
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    return cp && reducer_frontier_provable_tip_cached() >= cp->height;
}

static struct condition c_checkpoint_bundle_install_ready = {
    .name = CBIR_NAME,
    .severity = COND_WARN,
    .poll_secs = 15,
    .backoff_secs = 30,
    .max_attempts = 3,
    .detect = detect_checkpoint_bundle_install_ready,
    .remedy = remedy_checkpoint_bundle_install_ready,
    .witness = witness_checkpoint_bundle_install_ready,
    .witness_window_secs = 30,
};

void register_checkpoint_bundle_install_ready(void)
{
    (void)condition_register(&c_checkpoint_bundle_install_ready);
}

#ifdef ZCL_TESTING
void checkpoint_bundle_install_ready_test_reset(void)
{
    atomic_store(&g_test_ms, NULL);
    g_test_datadir = NULL;
    atomic_store(&g_test_suppress_restart, false);
    atomic_store(&g_test_remedy_calls, 0);
    condition_reset_state(&c_checkpoint_bundle_install_ready);
}

void checkpoint_bundle_install_ready_test_set_main_state(struct main_state *ms)
{
    atomic_store(&g_test_ms, ms);
}

void checkpoint_bundle_install_ready_test_set_datadir(const char *datadir)
{
    g_test_datadir = datadir;
}

void checkpoint_bundle_install_ready_test_suppress_restart(bool suppress)
{
    atomic_store(&g_test_suppress_restart, suppress);
}

int checkpoint_bundle_install_ready_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}

bool checkpoint_bundle_install_ready_test_detect(void)
{
    return detect_checkpoint_bundle_install_ready();
}

int checkpoint_bundle_install_ready_test_remedy(void)
{
    return (int)remedy_checkpoint_bundle_install_ready();
}
#endif
