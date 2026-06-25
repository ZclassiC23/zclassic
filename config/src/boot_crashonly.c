/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_crashonly — implementation. See header. Crash-only recovery: re-derive
 * inconsistent derived state via -reindex-chainstate (rewind to the consistent
 * scan_reindex_best target + replay from blocks/) instead of FATAL or surgical
 * repair, bounded so a genuinely corrupt blocks/ pages the operator rather than
 * looping. The reindex never deletes blocks/ or wallet — only the derived UTXO set.
 */

#include "config/boot_crashonly.h"

#include "event/event.h"
#include "storage/boot_auto_reindex.h"

#include <stdio.h>

bool boot_crashonly_consume_reindex_request(const char *datadir)
{
    if (!boot_auto_reindex_pending(datadir))
        return false;
    fprintf(stderr,
            "[boot] crash-only recovery: consuming auto-reindex request — "
            "rebuilding the UTXO set from block data (-reindex-chainstate)\n");
    return true;
}

void boot_crashonly_clear(const char *datadir)
{
    boot_auto_reindex_clear(datadir);
}

bool boot_crashonly_handle_unrecoverable(const char *datadir, int tip_h,
                                         int zero_nbits, int mismatches,
                                         int first_mismatch_h,
                                         bool reindex_executable)
{
    /* Verb check FIRST: on a cold-import datadir there is no block data
     * below the import window, so replay-from-blocks/ can never succeed —
     * requesting it burns a full boot cycle per attempt and can wipe the
     * mirror before discovering the dead end. Name the right verb
     * (cold-import re-seed) loudly and tell the caller to keep serving
     * degraded instead of exiting into an impossible rebuild. */
    if (zero_nbits == 0 && !reindex_executable) {
        fprintf(stderr,
            "[boot] crash-only recovery: tip-above-extent at tip_h=%d is the "
            "reindex-recoverable shape, but blocks/ cannot serve the replay "
            "(cold-import window, no genesis-side block data) — NOT requesting "
            "-reindex-chainstate. Remedy: cold-import re-seed. Serving "
            "degraded; the reducer reconciles forward.\n", tip_h);
        event_emitf(EV_OPERATOR_NEEDED, 0,
            "condition=cold_import_reseed_required tip=%d "
            "reason=reindex_unexecutable", tip_h);
        /* A sentinel left by an earlier boot can never be served on this
         * datadir; clearing it stops the per-boot consume→refuse cycle. */
        boot_auto_reindex_clear(datadir);
        return false;
    }

    /* zero_nbits==0 => the "corruption" is only holes ABOVE the validated
     * on-disk index extent (a derived tip installed too high), NOT structural
     * nBits damage — exactly what -reindex-chainstate rebuilds. Request a
     * bounded self-rebuild; the caller exits and the restart re-enters with the
     * reindex. */
    if (zero_nbits == 0) {
        int n = boot_auto_reindex_request(datadir, tip_h);
        if (n >= 1 && n <= BOOT_AUTO_REINDEX_MAX) {
            fprintf(stderr,
                "[boot] crash-only recovery: post-restore tip-above-extent at "
                "tip_h=%d (zero_nbits=0, attempt %d/%d) — requesting "
                "-reindex-chainstate; restarting to rebuild from blocks/ "
                "(re-derive, no surgical repair, no data loss).\n",
                tip_h, n, BOOT_AUTO_REINDEX_MAX);
            event_emitf(EV_BOOT_ACTIVATE, 0,
                "crashonly_auto_reindex_requested tip=%d attempt=%d", tip_h, n);
            return true;
        }
        /* Budget exhausted (n == BOOT_AUTO_REINDEX_MAX+1, the terminal marker
         * already on disk, or a write error). PERSIST the exhausted state by
         * REWRITING the sentinel as a TERMINAL marker — do NOT clear it.
         * Clearing would let the next boot find no sentinel, re-detect the same
         * damage, and write a fresh count=1, re-arming the 3-attempt budget from
         * scratch → an unbounded reindex loop throttled only by systemd backoff.
         * The terminal marker makes boot_auto_reindex_pending()/consume return
         * false forever after, so the next boot does NOT re-request a reindex.
         * This matches chain_tip_watchdog: exhaustion is persisted, the operator
         * is paged ONCE, and the node stays up degraded. Return false so the
         * caller serves DEGRADED instead of exiting into a crash-loop. */
        (void)boot_auto_reindex_mark_terminal(datadir, tip_h);
        fprintf(stderr,
            "[boot] crash-only recovery EXHAUSTED after %d reindex attempts at "
            "tip_h=%d — NOT restarting, staying up DEGRADED for the operator "
            "(block data may be genuinely corrupt; terminal marker persisted so "
            "this does not re-arm the reindex budget).\n",
            BOOT_AUTO_REINDEX_MAX, tip_h);
        event_emitf(EV_OPERATOR_NEEDED, 0,
            "condition=crashonly_auto_reindex_exhausted tip=%d attempts=%d",
            tip_h, BOOT_AUTO_REINDEX_MAX);
        /* Stay-up-degraded: the caller takes the DEGRADED_SERVING branch, the
         * reducer reconciles forward, and operator_needed is latched (above). */
        return false;
    }
    fprintf(stderr,
        "[boot] FATAL: post-restore integrity found structural corruption at "
        "tip_h=%d (zero_nbits=%d mismatches=%d first_mismatch_h=%d). Re-run "
        "with -allow-degraded to serve anyway, or -reindex-chainstate to "
        "rebuild.\n", tip_h, zero_nbits, mismatches, first_mismatch_h);
    event_emitf(EV_BOOT_ACTIVATE, 0,
        "FATAL post_restore_integrity_corrupt tip=%d zero_nbits=%d mismatches=%d",
        tip_h, zero_nbits, mismatches);
    return true;
}

/* Boot-storage episode anchor: these gates fire BEFORE a tip height is known,
 * so they all share ONE bounded episode keyed on a fixed sentinel (0). The
 * budget keying folds to the episode minimum, so repeated boots at any of these
 * gates increment the SAME count toward BOOT_AUTO_REINDEX_MAX rather than each
 * gate re-arming a fresh budget. */
#define BOOT_STORAGE_EPISODE_ANCHOR 0

enum boot_gate_action boot_crashonly_storage_gate(const char *datadir,
                                                  const char *gate_name)
{
    if (!gate_name) gate_name = "boot_storage_gate";

    /* No datadir (degenerate / unit-test path): there is nowhere to persist a
     * bounded budget, so we cannot promise the restart will re-derive. Park
     * rather than crash-loop — the safest terminating end-state. */
    if (!datadir || !datadir[0]) {
        event_emitf(EV_OPERATOR_NEEDED, 0,
            "condition=boot_storage_gate gate=%s reason=no_datadir", gate_name);
        return BOOT_GATE_PARK_DEGRADED;
    }

    int n = boot_auto_reindex_request(datadir, BOOT_STORAGE_EPISODE_ANCHOR);
    if (n >= 1 && n <= BOOT_AUTO_REINDEX_MAX) {
        fprintf(stderr,
            "[boot] crash-only recovery: boot-storage gate '%s' failed "
            "(attempt %d/%d) — requesting -reindex-chainstate; restarting to "
            "re-derive the UTXO set from blocks/ instead of re-hitting the "
            "identical corrupt derived state (no FATAL, no data loss).\n",
            gate_name, n, BOOT_AUTO_REINDEX_MAX);
        event_emitf(EV_BOOT_ACTIVATE, 0,
            "crashonly_boot_storage_reindex_requested gate=%s attempt=%d",
            gate_name, n);
        return BOOT_GATE_EXIT_FOR_REINDEX;
    }

    /* Budget exhausted (n == terminal, the marker already on disk, or a write
     * error). PERSIST the exhausted state as a TERMINAL marker so the next boot
     * does NOT re-arm a fresh count and crash-loop, page the operator ONCE, and
     * tell the caller to PARK alive-degraded — matching chain_tip_watchdog's
     * "stay up, paged, never power-cycle" terminal end-state. */
    (void)boot_auto_reindex_mark_terminal(datadir, BOOT_STORAGE_EPISODE_ANCHOR);
    fprintf(stderr,
        "[boot] crash-only recovery EXHAUSTED at boot-storage gate '%s' after "
        "%d reindex attempts — block data may be genuinely corrupt. NOT "
        "crash-looping: parking alive-degraded for the operator (terminal "
        "marker persisted so this does not re-arm the reindex budget).\n",
        gate_name, BOOT_AUTO_REINDEX_MAX);
    event_emitf(EV_OPERATOR_NEEDED, 0,
        "condition=boot_storage_gate_exhausted gate=%s attempts=%d",
        gate_name, BOOT_AUTO_REINDEX_MAX);
    return BOOT_GATE_PARK_DEGRADED;
}
