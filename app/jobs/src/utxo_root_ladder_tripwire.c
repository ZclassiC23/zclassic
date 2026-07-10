/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * See utxo_root_ladder_tripwire.h for the contract. */

#include "utxo_root_ladder_tripwire.h"

#include "chain/mmr.h"                        /* MMR_COMMITMENT_INTERVAL */
#include "event/event.h"                       /* EV_UTXO_DRIFT_DETECTED */
#include "models/utxo_root_ladder_verify.h"
#include "storage/progress_store.h"            /* progress_store_db() */
#include "util/blocker.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum utxo_root_ladder_tripwire_result
utxo_root_ladder_tripwire_report(const struct utxo_root_ladder_verify_result *results,
                                 size_t n)
{
    if (!results || n == 0)
        return UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY;

    int32_t first_divergent_height = -1;
    size_t divergent = 0;
    for (size_t i = 0; i < n; i++) {
        if (results[i].status == UTXO_ROOT_LADDER_VERIFY_DIVERGENT) {
            if (divergent == 0)
                first_divergent_height = results[i].height;
            divergent++;
        }
    }

    if (divergent == 0)
        return UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY;  /* MATCH / NOT_YET_REACHED only */

    /* OBSERVE-ONLY, same doctrine as the SHA3 golden-window tripwire: the
     * ladder is an immutable commitment over frozen history, so a
     * divergence here means THIS node's own coins_kv boundary-root store no
     * longer reproduces a value it (or a cross-checked sibling) already
     * locked in — a state-wrong-coin class defect, not a transient miss.
     * BLOCKER_PERMANENT: no escape action, no deadline, pure evidence. */
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "utxo root ladder mismatch: %zu of %zu locked rung(s) diverged, "
             "first at h=%d (this node's own coins_kv boundary root differs "
             "from the locked golden-height ladder — state-wrong-coin class)",
             divergent, n, first_divergent_height);

    struct blocker_record r;
    if (!blocker_init(&r, "utxo_ladder_mismatch", "utxo_root_ladder_tripwire",
                      BLOCKER_PERMANENT, reason))
        return UTXO_ROOT_LADDER_TRIPWIRE_MISMATCH;  /* blocker_init logged why */
    r.escape_deadline_secs = 0;
    r.escape_action[0] = '\0';
    if (blocker_set(&r) == 0)
        event_emitf(EV_UTXO_DRIFT_DETECTED, 0,
                    "verdict=utxo_ladder_mismatch h=%d divergent=%zu of=%zu "
                    "observe_only=1",
                    first_divergent_height, divergent, n);

    return UTXO_ROOT_LADDER_TRIPWIRE_MISMATCH;
}

void utxo_root_ladder_tripwire_at_boundary(int height)
{
    /* Operator kill-switch, mirrors ZCL_DISABLE_SHA3_WINDOW_TRIPWIRE.
     * Checked before the (cheap) boundary-cadence gate so it costs nothing
     * either way. */
    if (getenv("ZCL_DISABLE_UTXO_LADDER_TRIPWIRE"))
        return;

    if (height < 0 || (height % MMR_COMMITMENT_INTERVAL) != 0)
        return;   /* only re-check when a fresh boundary root could exist */

    sqlite3 *pdb = progress_store_db();
    if (!pdb)
        return;   /* store not open yet — benign skip */

    struct utxo_root_ladder_verify_result results[256];
    size_t n = 0;
    (void)utxo_root_ladder_verify_against_store(pdb, results,
                                                sizeof(results) / sizeof(results[0]),
                                                &n);
    (void)utxo_root_ladder_tripwire_report(results, n);
}
