/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Iterative Kademlia shortlist and fair lookup scheduling. */

#include "zcode_dht_service_internal.h"

#include <string.h>

static bool node_id_nonzero(const uint8_t id[32]) {
  uint8_t any = 0;
  if (!id)
    return false;
  for (size_t i = 0; i < 32; i++)
    any |= id[i];
  return any != 0;
}

static uint32_t active_query_count(const struct vcs_zcode_dht_service *s) {
  uint32_t count = 0;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
    count += s->queries[i].used;
  return count;
}

struct service_lookup *vcs_zcode_dht_lookup_find(
    struct vcs_zcode_dht_service *s, uint64_t id) {
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++)
    if (s->lookups[i].used && s->lookups[i].id == id)
      return &s->lookups[i];
  return NULL;
}

bool vcs_zcode_dht_lookup_closer_id(const uint8_t a[32], const uint8_t b[32],
                                    const uint8_t target[32]) {
  uint8_t ad[32], bd[32];
  vcs_zcode_dht_xor_distance(a, target, ad);
  vcs_zcode_dht_xor_distance(b, target, bd);
  int comparison = memcmp(ad, bd, 32);
  return comparison < 0 || (comparison == 0 && memcmp(a, b, 32) < 0);
}

int vcs_zcode_dht_lookup_candidate_index(const struct service_lookup *l,
                                         const uint8_t id[32]) {
  if (!l || !id)
    return -1;
  for (uint32_t i = 0; i < l->candidate_count; i++)
    if (memcmp(l->candidates[i].node_id, id, 32) == 0)
      return (int)i;
  return -1;
}

uint32_t
vcs_zcode_dht_lookup_frontier_count(const struct service_lookup *l) {
  if (!l)
    return 0;
  return l->candidate_count < VCS_ZCODE_DHT_K ? l->candidate_count
                                               : VCS_ZCODE_DHT_K;
}

bool vcs_zcode_dht_lookup_candidate_authenticated(
    enum vcs_zcode_dht_candidate_state state) {
  return state == VCS_ZCODE_DHT_CANDIDATE_AUTHENTICATED ||
         state == VCS_ZCODE_DHT_CANDIDATE_QUERIED ||
         state == VCS_ZCODE_DHT_CANDIDATE_IN_FLIGHT ||
         state == VCS_ZCODE_DHT_CANDIDATE_RESPONDED;
}

struct service_peer *vcs_zcode_dht_lookup_peer_for_node(
    struct vcs_zcode_dht_service *s, const uint8_t id[32]) {
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
    if (s->peers[i].used && s->peers[i].connected &&
        s->peers[i].authenticated &&
        memcmp(s->peers[i].node_id, id, 32) == 0)
      return &s->peers[i];
  return NULL;
}

bool vcs_zcode_dht_lookup_insert(
    struct service_lookup *l, const uint8_t id[32],
    enum vcs_zcode_dht_candidate_state state, uint64_t peer_id) {
  if (!l || !node_id_nonzero(id))
    return false;
  int existing = vcs_zcode_dht_lookup_candidate_index(l, id);
  if (existing >= 0) {
    struct lookup_candidate *candidate = &l->candidates[existing];
    if (state == VCS_ZCODE_DHT_CANDIDATE_AUTHENTICATED &&
        (candidate->state == VCS_ZCODE_DHT_CANDIDATE_UNVERIFIED ||
         candidate->state == VCS_ZCODE_DHT_CANDIDATE_UNREACHABLE ||
         candidate->state == VCS_ZCODE_DHT_CANDIDATE_FAILED)) {
      candidate->state = state;
      candidate->reachability_deadline_mono = 0;
    }
    if (peer_id)
      candidate->peer_id = peer_id;
    return false;
  }
  uint32_t at = 0;
  while (at < l->candidate_count &&
         !vcs_zcode_dht_lookup_closer_id(id, l->candidates[at].node_id,
                                         l->target))
    at++;
  uint32_t end = l->candidate_count;
  if (l->candidate_count == VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES) {
    if (at >= VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES)
      return false;
    /* An in-flight entry remains addressable even after closer discoveries
     * push it outside the active frontier.  Evict the farthest non-flight
     * entry at or after the insertion point instead.  At most alpha entries
     * can be in flight, so a valid service lookup always has such a slot. */
    bool found = false;
    for (uint32_t i = VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES; i-- > at;) {
      if (l->candidates[i].state != VCS_ZCODE_DHT_CANDIDATE_IN_FLIGHT) {
        end = i;
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  if (end > at)
    memmove(&l->candidates[at + 1], &l->candidates[at],
            (end - at) * sizeof(l->candidates[0]));
  memset(&l->candidates[at], 0, sizeof(l->candidates[at]));
  l->candidates[at].used = true;
  memcpy(l->candidates[at].node_id, id, 32);
  l->candidates[at].state = state;
  l->candidates[at].peer_id = peer_id;
  if (l->candidate_count < VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES)
    l->candidate_count++;
  return true;
}

void vcs_zcode_dht_lookup_terminate(
    struct vcs_zcode_dht_service *s, struct service_lookup *l,
    enum vcs_zcode_dht_lookup_termination termination) {
  if (!s || !l || l->completed)
    return;
  l->completed = true;
  l->termination = termination;
  if ((unsigned)termination < VCS_ZCODE_DHT_TERMINATION_COUNT)
    s->lookup_terminations[termination]++;
}

static bool lookup_has_state(const struct service_lookup *l,
                             enum vcs_zcode_dht_candidate_state state) {
  uint32_t frontier = vcs_zcode_dht_lookup_frontier_count(l);
  for (uint32_t i = 0; i < frontier; i++)
    if (l->candidates[i].state == state)
      return true;
  return false;
}

static bool lookup_has_authenticated_result(const struct service_lookup *l) {
  for (uint32_t i = 0; i < l->candidate_count; i++)
    if (vcs_zcode_dht_lookup_candidate_authenticated(l->candidates[i].state))
      return true;
  return false;
}

void vcs_zcode_dht_lookup_assess(struct vcs_zcode_dht_service *s,
                                 struct service_lookup *l) {
  if (!l || l->completed)
    return;
  int target = vcs_zcode_dht_lookup_candidate_index(l, l->target);
  if (target >= 0 &&
      vcs_zcode_dht_lookup_candidate_authenticated(l->candidates[target].state)) {
    vcs_zcode_dht_lookup_terminate(
        s, l, VCS_ZCODE_DHT_TERMINATION_TARGET_AUTHENTICATED);
    return;
  }
  if (l->queries_pending ||
      lookup_has_state(l, VCS_ZCODE_DHT_CANDIDATE_AUTHENTICATED) ||
      lookup_has_state(l, VCS_ZCODE_DHT_CANDIDATE_UNVERIFIED))
    return;
  vcs_zcode_dht_lookup_terminate(
      s, l, lookup_has_authenticated_result(l)
                ? VCS_ZCODE_DHT_TERMINATION_SHORTLIST_STABLE
                : VCS_ZCODE_DHT_TERMINATION_NO_AUTHENTICATED_RESULT);
}

void vcs_zcode_dht_lookup_schedule(struct vcs_zcode_dht_service *s,
                                   struct vcs_zcode_dht_time now) {
  if (!s || !s->enabled)
    return;
  while (active_query_count(s) < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES) {
    bool launched = false;
    for (size_t scan = 0; scan < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; scan++) {
      size_t lookup_index =
          (s->scheduler_cursor + scan) % VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS;
      struct service_lookup *lookup = &s->lookups[lookup_index];
      if (!lookup->used || lookup->completed ||
          lookup->queries_pending >= VCS_ZCODE_DHT_ALPHA)
        continue;
      uint32_t frontier = vcs_zcode_dht_lookup_frontier_count(lookup);
      for (uint32_t i = 0; i < frontier; i++) {
        struct lookup_candidate *candidate = &lookup->candidates[i];
        if (candidate->state != VCS_ZCODE_DHT_CANDIDATE_AUTHENTICATED)
          continue;
        if (memcmp(candidate->node_id, s->self_id, 32) == 0) {
          candidate->state = VCS_ZCODE_DHT_CANDIDATE_RESPONDED;
          continue;
        }
        struct service_peer *peer =
            vcs_zcode_dht_lookup_peer_for_node(s, candidate->node_id);
        if (!peer) {
          candidate->state = VCS_ZCODE_DHT_CANDIDATE_UNREACHABLE;
          continue;
        }
        bool new_round = lookup->queries_pending == 0;
        candidate->state = VCS_ZCODE_DHT_CANDIDATE_QUERIED;
        if (!vcs_zcode_dht_service_send_find(
                s, peer, QUERY_LOOKUP, lookup->id, lookup->target, NULL,
                now.monotonic_s)) {
          candidate->state = VCS_ZCODE_DHT_CANDIDATE_FAILED;
          continue;
        }
        candidate->state = VCS_ZCODE_DHT_CANDIDATE_IN_FLIGHT;
        candidate->peer_id = peer->peer_id;
        lookup->queries_sent++;
        lookup->queries_pending++;
        if (new_round) {
          lookup->rounds++;
          s->lookup_rounds++;
          if (!lookup->first_query_mono) {
            lookup->first_query_mono = now.monotonic_s;
            if (now.monotonic_s >= lookup->started_mono)
              s->lookup_queue_wait_s +=
                  now.monotonic_s - lookup->started_mono;
          }
        }
        s->scheduler_cursor =
            (uint32_t)((lookup_index + 1) % VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS);
        launched = true;
        break;
      }
      if (launched)
        break;
      vcs_zcode_dht_lookup_assess(s, lookup);
    }
    if (!launched)
      break;
  }
}

bool vcs_zcode_dht_service_lookup_begin(struct vcs_zcode_dht_service *s,
                                        const uint8_t target[32],
                                        struct vcs_zcode_dht_time now,
                                        uint64_t *id_out) {
  if (!s || !s->enabled || !node_id_nonzero(target) || !id_out)
    return false;
  struct service_lookup *lookup = NULL;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++)
    if (!s->lookups[i].used) {
      lookup = &s->lookups[i];
      break;
    }
  if (!lookup)
    return false;
  memset(lookup, 0, sizeof(*lookup));
  lookup->used = true;
  lookup->id = s->next_lookup_id++;
  if (!lookup->id)
    lookup->id = s->next_lookup_id++;
  lookup->started_mono = now.monotonic_s;
  lookup->deadline_mono = now.monotonic_s + VCS_ZCODE_DHT_LOOKUP_CEILING_S;
  memcpy(lookup->target, target, 32);
  (void)vcs_zcode_dht_lookup_insert(
      lookup, s->self_id, VCS_ZCODE_DHT_CANDIDATE_RESPONDED, 0);
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
    if (s->peers[i].used && s->peers[i].connected &&
        s->peers[i].authenticated)
      (void)vcs_zcode_dht_lookup_insert(
          lookup, s->peers[i].node_id, VCS_ZCODE_DHT_CANDIDATE_AUTHENTICATED,
          s->peers[i].peer_id);
  *id_out = lookup->id;
  vcs_zcode_dht_lookup_schedule(s, now);
  vcs_zcode_dht_lookup_assess(s, lookup);
  return true;
}

bool vcs_zcode_dht_service_lookup_poll(
    struct vcs_zcode_dht_service *s, uint64_t id,
    struct vcs_zcode_dht_time now, struct vcs_zcode_dht_lookup_result *out) {
  if (!s || !out)
    return false;
  vcs_zcode_dht_service_tick(s, now);
  struct service_lookup *lookup = vcs_zcode_dht_lookup_find(s, id);
  if (!lookup)
    return false;
  memset(out, 0, sizeof(*out));
  out->termination = lookup->termination;
  out->rounds = lookup->rounds;
  out->xor_progress = lookup->xor_progress;
  out->queue_wait_s = lookup->first_query_mono >= lookup->started_mono
                          ? lookup->first_query_mono - lookup->started_mono
                          : 0;
  out->state = !lookup->completed
                   ? VCS_ZCODE_DHT_LOOKUP_PENDING
                   : (lookup->termination == VCS_ZCODE_DHT_TERMINATION_TIMEOUT
                          ? VCS_ZCODE_DHT_LOOKUP_TIMEOUT
                          : (lookup->termination ==
                                     VCS_ZCODE_DHT_TERMINATION_NO_AUTHENTICATED_RESULT
                                 ? VCS_ZCODE_DHT_LOOKUP_NOT_FOUND
                                 : VCS_ZCODE_DHT_LOOKUP_COMPLETE));
  for (uint32_t i = 0;
       i < lookup->candidate_count && out->count < VCS_ZCODE_DHT_K; i++)
    if (vcs_zcode_dht_lookup_candidate_authenticated(
            lookup->candidates[i].state))
      memcpy(out->node_ids[out->count++], lookup->candidates[i].node_id, 32);
  if (lookup->completed)
    memset(lookup, 0, sizeof(*lookup));
  return true;
}
