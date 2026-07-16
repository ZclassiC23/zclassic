/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * The operational-vs-sovereign trust split, made explicit and enforced.
 *
 * coins_kv_is_proven_authority() is true for BOTH a self-derived coin set
 * AND a borrowed zclassicd-chainstate copy — "the store is populated and
 * self-consistent, can serve tip" (the OPERATIONAL bit).
 * coins_kv_contains_refold_marker() (COINS_KV_SELF_FOLDED_KEY) is the
 * separate SOVEREIGN bit: the set was produced by a self-derived /
 * checkpoint-verified path, never the borrow. coins_kv_tip_is_self_derived()
 * is the composite G-SOV predicate (parts 2+3): continuous
 * coins_applied_height coverage AND (not-borrowed OR self-folded).
 *
 * Before this module, only snapshot/bundle export was gated on the split
 * (config/src/bundle_exporter.c bx_qualified,
 * config/src/consensus_state_snapshot_export_proof.c,
 * config/src/boot_snapshot_offer.c) — mining and wallet spending were NOT,
 * so an operator could mint or spend on a borrowed shielded history. This
 * file closes that gap with one guard, sovereignty_guard_allow(), called
 * from the mint entry (app/controllers/src/mining_controller.c) and the
 * wallet-spend entry (app/controllers/src/wallet_shielded_send.c
 * rpc_z_sendmany — covers t->t / t->z / z->t / z->z). Tip-FOLLOWING (the
 * reducer forward fold, P2P relay, explorer, wallet viewing) is never
 * gated. See docs/work/fast-sync-to-tip-plan-2026-07-16.md §5. */

#include "controllers/sovereignty_controller.h"

#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char *sovereignty_trust_mode(bool proven_authority, bool self_folded)
{
    if (!proven_authority)
        return "bare";
    return self_folded ? "sovereign" : "release_assisted";
}

bool sovereignty_guard_allow(const char *action, char *reason,
                             size_t reason_cap)
{
    if (reason && reason_cap)
        reason[0] = '\0';

    sqlite3 *pdb = progress_store_db();
    if (!pdb) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "progress_store_unavailable");
        LOG_WARN("sovereignty", "%s refused: progress store not open",
                 action ? action : "(action)");
        return false;
    }

    int32_t hstar = reducer_frontier_provable_tip_cached();
    char why[96] = {0};
    /* coins_kv_tip_is_self_derived acquires progress_store_tx_lock itself
     * (recursive) — no outer lock needed for this single read. */
    bool self_derived = coins_kv_tip_is_self_derived(pdb, hstar, why,
                                                     sizeof(why));
    if (!self_derived) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "release_assisted: %s",
                     why[0] ? why : "not_self_derived");
        LOG_WARN("sovereignty",
                 "%s refused — tip is release_assisted (borrowed shielded "
                 "history, not self-folded): %s",
                 action ? action : "(action)", why[0] ? why : "unknown");
        return false;
    }
    return true;
}

bool sovereignty_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    sqlite3 *pdb = progress_store_db();
    bool progress_open = pdb != NULL;
    bool applied_found = false;
    bool applied_ok = false;
    bool proven_authority = false;
    bool self_folded = false;
    bool hstar_ok = false;
    bool coins_cover_hstar = false;
    bool self_derived = false;
    int32_t applied = -1;
    int32_t hstar = -1;
    int32_t served_floor = -1;
    char self_reason[128] = "progress_store_not_open";

    if (progress_open) {
        progress_store_tx_lock();
        applied_ok = coins_kv_get_applied_height(pdb, &applied,
                                                 &applied_found);
        proven_authority = coins_kv_is_proven_authority(pdb, NULL);
        self_folded = coins_kv_contains_refold_marker(pdb);
        hstar_ok = reducer_frontier_compute_hstar(pdb, &hstar, &served_floor);
        if (hstar_ok) {
            coins_cover_hstar = applied_ok && applied_found &&
                applied >= hstar + 1;
            self_derived = coins_kv_tip_is_self_derived(
                pdb, hstar, self_reason, sizeof(self_reason));
            if (self_derived)
                snprintf(self_reason, sizeof(self_reason), "ok");
        } else {
            snprintf(self_reason, sizeof(self_reason), "hstar_unavailable");
        }
        progress_store_tx_unlock();
    }

    const char *authority_posture =
        !progress_open ? "unknown_no_progress_store" :
        !proven_authority ? "not_proven" :
        self_folded ? "self_folded_marker_present" :
        "proven_but_not_self_folded";
    const char *trust_mode = sovereignty_trust_mode(proven_authority,
                                                     self_folded);

    json_push_kv_str(out, "schema", "zcl.sovereignty.v1");
    json_push_kv_int(out, "schema_version", 1);
    json_push_kv_bool(out, "progress_store_open", progress_open);
    json_push_kv_bool(out, "coins_kv_proven_authority", proven_authority);
    json_push_kv_bool(out, "self_folded_marker", self_folded);
    json_push_kv_bool(out, "hstar_available", hstar_ok);
    json_push_kv_int(out, "hstar", hstar_ok ? hstar : -1);
    json_push_kv_bool(out, "coins_applied_height_present", applied_found);
    json_push_kv_int(out, "coins_applied_height",
                     applied_found ? applied : -1);
    json_push_kv_bool(out, "coins_cover_hstar", coins_cover_hstar);
    json_push_kv_bool(out, "self_derived_tip_static_checks", self_derived);
    json_push_kv_str(out, "self_derived_reason", self_reason);
    json_push_kv_str(out, "authority_posture", authority_posture);
    json_push_kv_str(out, "trust_mode", trust_mode);

    /* What is gated at the CURRENT posture — probe the actual guard so this
     * report can never drift from the enforcement it describes (one source
     * of truth: sovereignty_guard_allow). */
    char mint_reason[96] = {0};
    char spend_reason[96] = {0};
    bool mint_allowed = progress_open &&
        sovereignty_guard_allow("mint", mint_reason, sizeof(mint_reason));
    bool spend_allowed = progress_open &&
        sovereignty_guard_allow("wallet_spend", spend_reason,
                                sizeof(spend_reason));

    /* Snapshot/bundle export requires BOTH proven_authority AND the
     * self_folded marker (config/src/bundle_exporter.c bx_qualified: "coins
     * not proven authority" / "coins lacks self-folded refold marker") — a
     * stricter, non-disjunctive gate than G-SOV's `self_derived` above (which
     * lets an un-migrated/bare store through on part 3's first disjunct, and
     * needs hstar for part 2). Mirror bx_qualified's exact predicate here so
     * this field never disagrees with the export gate it describes, and
     * stays reportable even when hstar isn't (yet) computable. */
    bool export_allowed = proven_authority && self_folded;

    struct json_value gated = {0};
    json_set_object(&gated);
    json_push_kv_bool(&gated, "mint_mining_allowed", mint_allowed);
    json_push_kv_bool(&gated, "wallet_spend_allowed", spend_allowed);
    json_push_kv_bool(&gated, "snapshot_bundle_export_allowed",
                      export_allowed);
    json_push_kv_bool(&gated, "tip_following_allowed", true);
    json_push_kv_str(&gated, "note",
                     "tip-following (reducer fold / P2P relay / explorer / "
                     "wallet viewing) is never gated; mint, wallet spend, "
                     "and snapshot/bundle export require self_folded");
    json_push_kv(out, "gated", &gated);
    json_free(&gated);

    return true;
}
