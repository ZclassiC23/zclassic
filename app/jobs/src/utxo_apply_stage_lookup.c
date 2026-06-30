/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage_lookup - production live-prevout resolver for the
 * utxo_apply Job. Kept separate from the stage step so the stage TU owns only
 * cursor/write flow while this TU owns read-side UTXO lookup semantics. */

#include "jobs/utxo_apply_stage.h"
#include "utxo_apply_stage_internal.h"

#include "coins/coins.h"
#include "core/uint256.h"
#include "storage/coins_kv.h"
#include "storage/coins_ram.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

/* Production prevout resolver for utxo_apply, the init-time default for
 * g_lookup - the analogue of script_validate's created_index_prevout
 * self-default, but with the CORRECT semantics for utxo_apply: found must
 * mean "currently UNSPENT". coins_kv DELETEs a coin on spend, so a hit from
 * coins_kv_get_coins == the coin is live (double-spend-safe); a creation
 * index (which never deletes spent rows) would report found=true for an
 * already-spent coin and let utxo_apply accept a double-spend (monetary
 * inflation / hard fork) AND false-trip BIP30 collision - it MUST NOT be
 * used here. The full pre-image (value/height/is_coinbase/script) is
 * required for the inverse-delta restore-ADD. A genuine miss returns
 * found=false (compute_block_delta then records spend_unknown_utxo with the
 * exact outpoint - never a silent pass).
 *
 * FRESHNESS CONTRACT: reads the authoritative coins set on the progress.kv
 * handle inside the apply path's progress_store_tx_lock, and apply_coins_kv
 * authors coins_kv IN the stage txn - a coin created by an earlier block is
 * already committed before a later block's step_apply resolves it, so reads
 * are inherently fresh with no catch_up dependency. */
bool utxo_apply_stage_lookup_live(const struct uint256 *txid, uint32_t vout,
                                  struct utxo_apply_lookup *out, void *user)
{
    (void)user;
    if (!txid || !out)
        return false;
    memset(out, 0, sizeof(*out));

    sqlite3 *db = progress_store_db();
    if (!db)
        return true;   /* store not open yet -> treat as absent (found=0),
                        * matching the lookup==NULL "all external absent"
                        * contract; never a false-accept. */

    /* Fast path: when the in-RAM overlay is active, resolve the ONE prevout
     * directly via coins_ram_get_prevout (O(1) probe + point read-through) and
     * AVOID coins_kv_get_coins -> coins_ram_get_coins, which reconstructs the
     * ENTIRE struct coins (two O(cap=8M) linear scans) per call. The resolver
     * only reads one vout, so the reconstruction is pure waste on the per-input
     * hot path. The four fields returned here (value/script/height/is_coinbase)
     * are exactly what the inverse-delta restore-ADD needs, matching what
     * the reconstruction path would yield for the same live coin. */
    if (coins_ram_active()) {
        int64_t  value = 0;
        int32_t  height = 0;
        bool     is_coinbase = false;
        size_t   slen = 0;
        if (!coins_ram_get_prevout(txid->data, vout, &value, out->script,
                                    UTXO_APPLY_SCRIPT_MAX, &slen,
                                    &height, &is_coinbase))
            return true;  /* absent in the effective set -> found stays false */
        if (slen > UTXO_APPLY_SCRIPT_MAX) {
            /* Contract violation (script > MAX_SCRIPT_SIZE). Fail the resolver
             * rather than truncate or over-read a consensus script - same gate
             * as the reconstruction path below. */
            return false;
        }
        out->found       = true;
        out->value       = value;
        out->height      = (uint32_t)(height < 0 ? 0 : height);
        out->is_coinbase = is_coinbase;
        out->script_len  = (uint32_t)slen;
        /* script already copied into out->script by coins_ram_get_prevout. */
        return true;
    }

    struct coins c;
    coins_init(&c);
    if (!coins_kv_get_coins(db, txid->data, &c)) {
        coins_free(&c);
        return true;   /* no live output at this txid -> found stays false */
    }

    bool ok = true;
    if (vout < c.num_vout && !tx_out_is_null(&c.vout[vout])) {
        const struct tx_out *o = &c.vout[vout];
        size_t slen = o->script_pub_key.size;
        if (slen > UTXO_APPLY_SCRIPT_MAX) {
            /* Contract violation (a UTXO scriptPubKey is <= MAX_SCRIPT_SIZE ==
             * UTXO_APPLY_SCRIPT_MAX). Fail the resolver (compute_block_delta
             * turns this into an internal_error) rather than truncate or
             * over-read a consensus script. */
            ok = false;
        } else {
            out->found       = true;
            out->value       = o->value;
            out->height      = (uint32_t)(c.height < 0 ? 0 : c.height);
            out->is_coinbase = c.is_coinbase;
            out->script_len  = (uint32_t)slen;
            if (slen)
                memcpy(out->script, o->script_pub_key.data, slen);
        }
    }
    coins_free(&c);
    return ok;
}
