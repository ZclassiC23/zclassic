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
#include "platform/time_compat.h"
#include "util/reducer_stage_profile.h"

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
    int64_t started = platform_time_monotonic_us();
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
        reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                  RPF_PREVOUT_CREATED_LOOKUPS, 1);
        uint64_t prepares_before =
            created_outputs_index_prepare_count_thread();
        bool created_found = created_outputs_index_get_bounded(
            db, prevout->hash.data, prevout->n, min_created,
            view->height, &value, script, sizeof(script), &slen,
            &created_h);
        reducer_stage_profile_add(
            REDUCER_PROFILE_SCRIPT_VALIDATE, RPF_PREVOUT_PREPARES,
            created_outputs_index_prepare_count_thread() - prepares_before);
        if (created_found) {
            if (slen > MAX_SCRIPT_SIZE)
                return false;
            out->value = value;
            script_set(&out->script_pub_key, script, slen);
            reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                      RPF_PREVOUT_HITS, 1);
            reducer_stage_profile_observe_us(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                             RPF_PREVOUT_US,
                                      (uint64_t)(platform_time_monotonic_us() -
                                                 started));
            return true;
        }

        reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                  RPF_PREVOUT_COINS_FALLBACKS, 1);
        prepares_before = coins_kv_prepare_count_thread();
        int32_t coin_h = 0;
        bool coin_found = coins_kv_get_prevout(
            db, prevout->hash.data, prevout->n, &value, script,
            sizeof(script), &slen, &coin_h, NULL);
        reducer_stage_profile_add(
            REDUCER_PROFILE_SCRIPT_VALIDATE, RPF_PREVOUT_PREPARES,
            coins_kv_prepare_count_thread() - prepares_before);
        if (coin_found) {
            bool usable = coin_h < view->frontier && coin_h <= view->height;
            if (usable) {
                if (slen > MAX_SCRIPT_SIZE)
                    return false;
                out->value = value;
                script_set(&out->script_pub_key, script, slen);
                reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                          RPF_PREVOUT_HITS, 1);
                reducer_stage_profile_observe_us(
                    REDUCER_PROFILE_SCRIPT_VALIDATE, RPF_PREVOUT_US,
                                          (uint64_t)(platform_time_monotonic_us() -
                                                     started));
                return true;
            }
        }
        reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                  RPF_PREVOUT_MISSES, 1);
        reducer_stage_profile_observe_us(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                         RPF_PREVOUT_US,
                                  (uint64_t)(platform_time_monotonic_us() -
                                             started));
        return false;
    }

    reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                              RPF_PREVOUT_CREATED_LOOKUPS, 1);
    uint64_t prepares_before = created_outputs_index_prepare_count_thread();
    bool created_found = created_outputs_index_get(
        db, prevout->hash.data, prevout->n, &value, script, sizeof(script),
        &slen);
    reducer_stage_profile_add(
        REDUCER_PROFILE_SCRIPT_VALIDATE, RPF_PREVOUT_PREPARES,
        created_outputs_index_prepare_count_thread() - prepares_before);
    if (created_found) {
        if (slen > MAX_SCRIPT_SIZE)
            return false;  /* never silently truncate a scriptPubKey */
        out->value = value;
        script_set(&out->script_pub_key, script, slen);
        reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                  RPF_PREVOUT_HITS, 1);
        reducer_stage_profile_observe_us(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                         RPF_PREVOUT_US,
                                  (uint64_t)(platform_time_monotonic_us() -
                                             started));
        return true;
    }

    reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                              RPF_PREVOUT_COINS_FALLBACKS, 1);
    prepares_before = coins_kv_prepare_count_thread();
    bool coin_found = db && coins_kv_get(
        db, prevout->hash.data, prevout->n, &value, script, sizeof(script),
        &slen);
    reducer_stage_profile_add(
        REDUCER_PROFILE_SCRIPT_VALIDATE, RPF_PREVOUT_PREPARES,
        coins_kv_prepare_count_thread() - prepares_before);
    if (coin_found) {
        if (slen > MAX_SCRIPT_SIZE)
            return false;
        out->value = value;
        script_set(&out->script_pub_key, script, slen);
        reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                  RPF_PREVOUT_HITS, 1);
        reducer_stage_profile_observe_us(REDUCER_PROFILE_SCRIPT_VALIDATE,
                                         RPF_PREVOUT_US,
                                  (uint64_t)(platform_time_monotonic_us() -
                                             started));
        return true;
    }

    reducer_stage_profile_add(REDUCER_PROFILE_SCRIPT_VALIDATE,
                              RPF_PREVOUT_MISSES, 1);
    reducer_stage_profile_observe_us(
        REDUCER_PROFILE_SCRIPT_VALIDATE, RPF_PREVOUT_US,
        (uint64_t)(platform_time_monotonic_us() - started));
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
