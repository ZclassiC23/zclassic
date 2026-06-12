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
    /* Verb check FIRST (defect #6, live 2026-06-11): on a cold-import
     * datadir there is no block data below the import window, so
     * replay-from-blocks/ can never succeed — requesting it burns a
     * full boot cycle per attempt and (pre-fix) wiped the mirror before
     * discovering the dead end. Name the right verb loudly and tell the
     * caller to keep serving instead of exiting into an impossible
     * rebuild. */
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
         * datadir; clearing it stops the per-boot consume→refuse cycle
         * (task #29 follow-up). */
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
        boot_auto_reindex_clear(datadir);
        fprintf(stderr,
            "[boot] crash-only recovery EXHAUSTED after %d reindex attempts at "
            "tip_h=%d — pausing for the operator (block data may be genuinely "
            "corrupt).\n", BOOT_AUTO_REINDEX_MAX, tip_h);
        event_emitf(EV_OPERATOR_NEEDED, 0,
            "condition=crashonly_auto_reindex_exhausted tip=%d attempts=%d",
            tip_h, BOOT_AUTO_REINDEX_MAX);
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
