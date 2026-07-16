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
#include "services/shielded_history_import_service.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Shielded-import provenance introspection ──────────────────────────
 *
 * Per-pool row counts (sprout_anchors/sapling_anchors/nullifiers, the same
 * tables shielded_history_import_from_chainstate streams into — see
 * app/services/src/shielded_history_import_service.c) plus the
 * shielded_import.provenance metadata row that importer stamps in the same
 * atomic transaction on a successful commit. SELECT-only: a missing table
 * (no import has ever run on this datadir) or a missing provenance row is a
 * normal, quiet zero/absent result, never a warning. */

static bool sov_table_exists(sqlite3 *db, const char *table)
{
    if (!db || !table)
        return false;
    sqlite3_stmt *stmt = NULL;
    bool exists = false;
    int rc = sqlite3_prepare_v2(db,  // raw-controller-sql-ok:sovereignty-readonly-table-exists
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
        exists = sqlite3_step(stmt) == SQLITE_ROW;  // raw-sql-ok:read-only-introspection
    }
    if (stmt)
        sqlite3_finalize(stmt);
    return exists;
}

static int64_t sov_count_rows(sqlite3 *db, const char *table,
                              const char *where_clause)
{
    if (!db || !sov_table_exists(db, table))
        return 0;
    char sql[192];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s%s", table,
             where_clause ? where_clause : "");
    sqlite3_stmt *stmt = NULL;
    int64_t val = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);  // raw-controller-sql-ok:sovereignty-readonly-pool-count
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)  // raw-sql-ok:read-only-introspection
            val = sqlite3_column_int64(stmt, 0);
    } else {
        LOG_WARN("sovereignty", "pool count query prepare failed: %s [%s]",
                 sqlite3_errmsg(db), sql);
    }
    if (stmt)
        sqlite3_finalize(stmt);
    return val;
}

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
    int64_t sprout_anchor_rows = 0;
    int64_t sapling_anchor_rows = 0;
    int64_t sprout_nullifier_rows = 0;
    int64_t sapling_nullifier_rows = 0;
    char provenance[320] = {0};
    bool provenance_present = false;

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

        /* Per-pool shielded-history row counts + the importer's provenance
         * stamp (see the sov_* helpers above) — same lock scope as the reads
         * above; progress_meta_get recursively acquires progress_store_tx_lock
         * itself, matching the coins_kv_tip_is_self_derived precedent. */
        sprout_anchor_rows = sov_count_rows(pdb, "sprout_anchors", NULL);
        sapling_anchor_rows = sov_count_rows(pdb, "sapling_anchors", NULL);
        sprout_nullifier_rows = sov_count_rows(pdb, "nullifiers",
                                               " WHERE pool=0");
        sapling_nullifier_rows = sov_count_rows(pdb, "nullifiers",
                                                " WHERE pool=1");
        size_t prov_len = 0;
        bool prov_found = false;
        bool prov_ok = progress_meta_get(pdb, SHIELDED_IMPORT_PROVENANCE_KEY,
                                         provenance, sizeof(provenance) - 1,
                                         &prov_len, &prov_found);
        if (prov_ok && prov_found) {
            size_t copy = prov_len < sizeof(provenance) - 1
                              ? prov_len : sizeof(provenance) - 1;
            provenance[copy] = '\0';
            provenance_present = true;
        } else {
            provenance[0] = '\0';
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

    /* Per-pool shielded-history row counts (anchor_kv/nullifier_kv tables) —
     * distinguishable from `self_folded_marker` above: a `release_assisted`
     * node can carry a full complete history (post-import) with these counts
     * populated, while `self_folded` stays false until sovereign promotion. */
    struct json_value pools = {0};
    json_set_object(&pools);
    json_push_kv_int(&pools, "sprout_anchors", sprout_anchor_rows);
    json_push_kv_int(&pools, "sapling_anchors", sapling_anchor_rows);
    json_push_kv_int(&pools, "sprout_nullifiers", sprout_nullifier_rows);
    json_push_kv_int(&pools, "sapling_nullifiers", sapling_nullifier_rows);
    json_push_kv(out, "shielded_pool_counts", &pools);
    json_free(&pools);

    /* The importer's provenance stamp (shielded_import.provenance in
     * progress_meta) — see shi_write_provenance in
     * app/services/src/shielded_history_import_service.c. Absent on a node
     * that never ran -import-complete-shielded (a fresh/from-genesis-only
     * datadir); present and "self_folded=false" right after a successful
     * import, superseded wholesale by a later sovereign bundle install. */
    json_push_kv_bool(out, "shielded_import_provenance_present",
                      provenance_present);
    json_push_kv_str(out, "shielded_import_provenance", provenance);

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
