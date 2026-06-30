/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * script_validate_stage_prevout — production prevout resolver for the
 * script_validate Job. Kept beside the stage so the cursor/verification state
 * machine stays focused on stage advancement.
 */

#include "script_validate_stage_internal.h"

#include "coins/coins.h"
#include "jobs/created_outputs_index.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"

#include <stdbool.h>
#include <string.h>

/* Production prevout resolver (P0 §2.1) — resolves an outpoint to its spent
 * output WITHOUT requiring -txindex, correct at the script_validate frontier
 * (which runs ahead of utxo_apply). Two layers, in order:
 *   (1) the forward creation index body_persist maintains — covers every coin
 *       created in a block this node has persisted (body_persist.cursor is
 *       strictly > script_validate.cursor, so the row is present before needed);
 *   (2) coins_kv (the canonical UTXO set in progress.kv) — covers pre-anchor
 *       coins on a fast-synced node (seeded from the snapshot at boot) and the
 *       reducer-applied live set; a spent output is DELETEd, so a hit here is a
 *       currently-live coin.
 * A genuine miss returns false; the caller then FAILS LOUD with the exact
 * outpoint (never silently passes). The verifier (verify_script) is unchanged. */
bool script_validate_created_index_prevout(const struct outpoint *prevout,
                                           struct tx_out *out,
                                           void *user)
{
    if (!prevout || !out)
        return false;

    int64_t value = 0;
    unsigned char script[MAX_SCRIPT_SIZE];
    size_t slen = 0;

    const struct script_validate_created_prevout_view *view = user;
    sqlite3 *db = view && view->db ? view->db : progress_store_db();
    if (!db)
        return false;

    if (view && view->have_frontier) {
        int min_created = view->frontier <= view->height ? view->frontier : 0;
        int created_h = -1;
        if (created_outputs_index_get_bounded(
                db, prevout->hash.data, prevout->n, min_created,
                view->height, &value, script, sizeof(script), &slen,
                &created_h)) {
            if (slen > MAX_SCRIPT_SIZE)
                return false;
            out->value = value;
            script_set(&out->script_pub_key, script, slen);
            return true;
        }

        struct coins c;
        coins_init(&c);
        if (coins_kv_get_coins(db, prevout->hash.data, &c)) {
            bool usable = c.height < view->frontier &&
                          c.height <= view->height &&
                          prevout->n < c.num_vout &&
                          !tx_out_is_null(&c.vout[prevout->n]);
            if (usable) {
                const struct tx_out *src = &c.vout[prevout->n];
                size_t src_len = src->script_pub_key.size;
                if (src_len <= MAX_SCRIPT_SIZE) {
                    out->value = src->value;
                    script_set(&out->script_pub_key,
                               src->script_pub_key.data, src_len);
                    coins_free(&c);
                    return true;
                }
            }
            coins_free(&c);
        }
        return false;
    }

    if (created_outputs_index_get(db, prevout->hash.data, prevout->n,
                                  &value, script, sizeof(script), &slen)) {
        if (slen > MAX_SCRIPT_SIZE)
            return false;  /* never silently truncate a scriptPubKey */
        out->value = value;
        script_set(&out->script_pub_key, script, slen);
        return true;
    }

    if (db && coins_kv_get(db, prevout->hash.data, prevout->n,
                           &value, script, sizeof(script), &slen)) {
        if (slen > MAX_SCRIPT_SIZE)
            return false;
        out->value = value;
        script_set(&out->script_pub_key, script, slen);
        return true;
    }

    return false;
}

void script_validate_created_prevout_view_init(
    struct script_validate_created_prevout_view *view,
    int height)
{
    memset(view, 0, sizeof(*view));
    view->db = progress_store_db();
    view->height = height;
    view->frontier = 0;
    view->have_frontier = false;
    if (!view->db)
        return;

    int32_t frontier = 0;
    bool found = false;
    if (coins_kv_get_applied_height(view->db, &frontier, &found) && found) {
        view->frontier = frontier;
        view->have_frontier = true;
    }
}
