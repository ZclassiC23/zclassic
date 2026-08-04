/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Adversarial tests for the bounded iterative DHT lookup pool. */

#include "test/test_core.h"

#include "../../vcs/src/zcode_dht_service_internal.h"

#include <stdlib.h>
#include <string.h>

static void hostile_id(uint8_t out[32], uint32_t value) {
  memset(out, 0, 32);
  out[0] = 0x40;
  out[28] = (uint8_t)(value >> 24);
  out[29] = (uint8_t)(value >> 16);
  out[30] = (uint8_t)(value >> 8);
  out[31] = (uint8_t)value;
}

int test_zcode_dht_lookup(void) {
  int failures = 0;
  TEST("zcode dht lookup: full pool retains flights and closest frontier") {
    struct vcs_zcode_dht_service *service = calloc(1, sizeof(*service));
    struct vcs_zcode_dht_table *table = calloc(1, sizeof(*table));
    ASSERT(service != NULL);
    ASSERT(table != NULL);
    uint8_t self[32] = {0x7f};
    ASSERT(vcs_zcode_dht_table_init(table, self));
    service->enabled = true;
    service->table = table;

    struct service_lookup *lookup = &service->lookups[0];
    lookup->used = true;
    lookup->id = 17;
    lookup->started_mono = 10;
    lookup->deadline_mono = 100;
    memset(lookup->target, 0, sizeof(lookup->target));
    lookup->target[0] = 0x20;
    lookup->target[31] = 0x01;

    /* Seed k entries in forward order, then deliver 48 more in hostile
     * reverse order.  Duplicate every ID: dedupe is against the whole pool,
     * not merely the active frontier. */
    uint8_t id[32];
    for (uint32_t value = 1; value <= VCS_ZCODE_DHT_K; value++) {
      hostile_id(id, value);
      ASSERT(vcs_zcode_dht_lookup_insert(
          lookup, id, VCS_ZCODE_DHT_CANDIDATE_UNVERIFIED, 0));
      ASSERT(!vcs_zcode_dht_lookup_insert(
          lookup, id, VCS_ZCODE_DHT_CANDIDATE_UNVERIFIED, 0));
    }
    for (uint32_t value = VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES;
         value > VCS_ZCODE_DHT_K; value--) {
      hostile_id(id, value);
      ASSERT(vcs_zcode_dht_lookup_insert(
          lookup, id, VCS_ZCODE_DHT_CANDIDATE_UNVERIFIED, 0));
      ASSERT(!vcs_zcode_dht_lookup_insert(
          lookup, id, VCS_ZCODE_DHT_CANDIDATE_UNVERIFIED, 0));
    }
    ASSERT_EQ(lookup->candidate_count,
              VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES);
    ASSERT_EQ(vcs_zcode_dht_lookup_frontier_count(lookup), VCS_ZCODE_DHT_K);

    uint8_t flights[3][32];
    uint64_t flight_peer[3] = {301, 302, 303};
    uint64_t flight_deadline[3] = {901, 902, 903};
    for (size_t i = 0; i < 3; i++) {
      uint32_t at = lookup->candidate_count - 1u - (uint32_t)i;
      memcpy(flights[i], lookup->candidates[at].node_id, 32);
      lookup->candidates[at].state = VCS_ZCODE_DHT_CANDIDATE_IN_FLIGHT;
      lookup->candidates[at].peer_id = flight_peer[i];
      lookup->candidates[at].reachability_deadline_mono = flight_deadline[i];
    }

    /* The exact target arrives last into a full pool.  It must become the
     * head without discarding any in-flight bookkeeping. */
    ASSERT(vcs_zcode_dht_lookup_insert(
        lookup, lookup->target, VCS_ZCODE_DHT_CANDIDATE_UNVERIFIED, 0));
    ASSERT_EQ(lookup->candidate_count,
              VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES);
    ASSERT_EQ(vcs_zcode_dht_lookup_candidate_index(lookup, lookup->target), 0);
    for (uint32_t i = 1; i < lookup->candidate_count; i++)
      ASSERT(vcs_zcode_dht_lookup_closer_id(
          lookup->candidates[i - 1].node_id, lookup->candidates[i].node_id,
          lookup->target));
    for (size_t i = 0; i < 3; i++) {
      int at = vcs_zcode_dht_lookup_candidate_index(lookup, flights[i]);
      ASSERT(at >= 0);
      ASSERT_EQ(lookup->candidates[at].state,
                VCS_ZCODE_DHT_CANDIDATE_IN_FLIGHT);
      ASSERT_EQ(lookup->candidates[at].peer_id, flight_peer[i]);
      ASSERT_EQ(lookup->candidates[at].reachability_deadline_mono,
                flight_deadline[i]);
    }

    /* Authenticate a mixture extending beyond the active frontier.  Result
     * selection must return exactly the closest authenticated k, in order,
     * and must never leak any unverified entry. */
    for (uint32_t i = 0; i < 40; i += 2)
      (void)vcs_zcode_dht_lookup_insert(
          lookup, lookup->candidates[i].node_id,
          VCS_ZCODE_DHT_CANDIDATE_AUTHENTICATED, 1000 + i);
    uint8_t expected[VCS_ZCODE_DHT_K][32];
    uint32_t expected_count = 0;
    for (uint32_t i = 0; i < lookup->candidate_count &&
                         expected_count < VCS_ZCODE_DHT_K;
         i++)
      if (vcs_zcode_dht_lookup_candidate_authenticated(
              lookup->candidates[i].state))
        memcpy(expected[expected_count++], lookup->candidates[i].node_id, 32);
    ASSERT_EQ(expected_count, VCS_ZCODE_DHT_K);
    lookup->completed = true;
    lookup->termination = VCS_ZCODE_DHT_TERMINATION_SHORTLIST_STABLE;
    struct vcs_zcode_dht_lookup_result result;
    ASSERT(vcs_zcode_dht_service_lookup_poll(
        service, lookup->id,
        (struct vcs_zcode_dht_time){.wall_unix = 20, .monotonic_s = 20},
        &result));
    ASSERT_EQ(result.count, VCS_ZCODE_DHT_K);
    for (uint32_t i = 0; i < result.count; i++)
      ASSERT(memcmp(result.node_ids[i], expected[i], 32) == 0);

    free(table);
    free(service);
    PASS();
  }
_test_next:;
  return failures;
}
