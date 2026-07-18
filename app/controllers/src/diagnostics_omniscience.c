/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * diagnostics_omniscience.c — the "omniscience" dumpstate subsystem: the
 * capstone one-call answer to "does this node know everything it should about
 * itself and the ZClassic network?". Pure READ-ONLY composition over already-
 * shipped accessors (no new state, no mutation):
 *
 *   - the catalog_completeness per-index lag table, compared against the
 *     reducer's provable served height H* (reducer_frontier_provable_tip_cached);
 *   - the live P2P posture: handshaked-peer count + time-to-first-peer
 *     (connman), against the healthy-peer floor (ZCL_PEER_FLOOR_HEALTHY);
 *   - whole-network census freshness (census_read: age of the last topology
 *     sweep);
 *
 * folded into one string `verdict`: "omniscient" / "blocked:<index>@<height>" /
 * "degraded:peers" / "degraded:census" (catalog_completeness_verdict).
 *
 * This TU holds NO mutable file-scope state (safe alongside the hot-swap
 * dumpers); everything is snapshotted per call.
 */

#include "controllers/diagnostics_internal.h"

#include "jobs/reducer_frontier.h"
#include "net/connman.h"
#include "net/net.h"                 /* ZCL_PEER_FLOOR_HEALTHY */
#include "services/sync_monitor.h"
#include "storage/catalog_completeness.h"
#include "storage/census_read.h"
#include "platform/time_compat.h"
#include "json/json.h"

#include <stdint.h>
#include <stdio.h>

/* Census staleness bound: the crawler sweeps once per round
 * (NCRAWL_ROUND_INTERVAL_SECS_DEFAULT = 60s). A sweep older than 15 minutes
 * (or none at all) means the whole-network picture is stale — degraded:census.
 * Deliberately generous so a momentarily slow round never flaps the verdict. */
#define OMNISCIENCE_CENSUS_MAX_AGE_S 900

/* Age in seconds of the most recent finished topology sweep, or -1 if the
 * census store is absent or no sweep has finished yet. Read-only; opens the
 * census reader for this one snapshot and closes it. */
static int64_t omniscience_census_age_s(void)
{
    census_reader *r = NULL;
    if (census_read_open(diag_datadir(), &r) != CENSUS_READ_OK || !r)
        return -1;  // raw-return-ok:sentinel-no-census-absent-not-an-error
    struct census_graph_stats g;
    int64_t age = -1;
    if (census_read_graph(r, &g) && g.last_sweep_finished_unix > 0) {
        int64_t now = platform_time_wall_unix();
        age = now > g.last_sweep_finished_unix
                  ? now - g.last_sweep_finished_unix
                  : 0;
    }
    census_read_close(r);
    return age;
}

bool omniscience_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    /* ── P2P posture ─────────────────────────────────────────────────── */
    struct connman *cm = sync_monitor_connman();
    int handshaked = cm ? (int)connman_handshaked_peer_count(cm) : 0;
    int peer_floor = ZCL_PEER_FLOOR_HEALTHY;
    int64_t ttfp_us = connman_time_to_first_peer_us();

    json_push_kv_int(out, "time_to_first_peer_us", ttfp_us);
    json_push_kv_int(out, "handshaked_peers", handshaked);
    json_push_kv_int(out, "peer_floor", peer_floor);

    /* ── census freshness ────────────────────────────────────────────── */
    int64_t census_age = omniscience_census_age_s();
    json_push_kv_int(out, "census_age_s", census_age);

    /* ── per-index completeness against H* ───────────────────────────── */
    int64_t hstar = (int64_t)reducer_frontier_provable_tip_cached();
    struct catalog_index_status rows[CATALOG_COMPLETENESS_MAX_INDEXES];
    size_t n = catalog_completeness_snapshot(rows, CATALOG_COMPLETENESS_MAX_INDEXES,
                                             hstar);
    json_push_kv_int(out, "target_hstar", hstar);

    struct json_value indexes;
    json_init(&indexes);
    json_set_array(&indexes);
    for (size_t i = 0; i < n; i++) {
        struct json_value row;
        json_init(&row);
        json_set_object(&row);
        json_push_kv_str(&row, "name", rows[i].name ? rows[i].name : "");
        json_push_kv_int(&row, "cursor", rows[i].cursor);
        json_push_kv_int(&row, "target", rows[i].target);
        json_push_kv_int(&row, "lag", rows[i].lag);
        json_push_kv_bool(&row, "enabled", rows[i].enabled);
        json_push_kv_bool(&row, "always_on", rows[i].always_on);
        /* the exact typed blocker this condition raises when an enabled index
         * stays too far behind (see conditions/catalog_lag_exceeded.c). */
        char blocker[96];
        snprintf(blocker, sizeof(blocker), "catalog.%s.lag_exceeded",
                 rows[i].name ? rows[i].name : "?");
        json_push_kv_str(&row, "blocker", blocker);
        (void)json_push_back(&indexes, &row);
        json_free(&row);
    }
    (void)json_push_kv(out, "indexes", &indexes);
    json_free(&indexes);

    int64_t worst_lag = catalog_completeness_worst_lag(rows, n);
    json_push_kv_int(out, "worst_lag", worst_lag);

    /* ── the single-string verdict ───────────────────────────────────── */
    char verdict[96];
    enum catalog_verdict v =
        catalog_completeness_verdict(rows, n, handshaked, peer_floor,
                                     census_age, OMNISCIENCE_CENSUS_MAX_AGE_S,
                                     verdict, sizeof(verdict));
    json_push_kv_str(out, "verdict", verdict);

    diag_push_health(out, v == CATALOG_VERDICT_OMNISCIENT, verdict);
    return true;
}
