/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Bounded long-lived FIND_NODE/NODES service for the ZCODE DHT. */

#include "vcs/zcode_dht_service.h"
#include "zcode_dht_service_internal.h"

#include "base/safe_alloc.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "support/cleanse.h"
#include "vcs/zcode_dht_identity.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nonzero(const uint8_t *p, size_t n) {
  uint8_t any = 0;
  if (!p)
    return false;
  for (size_t i = 0; i < n; i++)
    any |= p[i];
  return any != 0;
}

const char *
vcs_zcode_dht_reject_reason_string(enum vcs_zcode_dht_reject_reason r) {
  static const char *const names[] = {
      "malformed", "plaintext",         "delegation", "identity",
      "signature", "wrong-session",     "replay",     "unsolicited",
      "expired",   "poisoned-contacts", "rate-limit", "capacity"};
  return (unsigned)r < VCS_ZCODE_DHT_REJECT_COUNT ? names[r] : "unknown";
}

void vcs_zcode_dht_service_set_error(struct vcs_zcode_dht_service *s,
                                     const char *e) {
  if (s)
    (void)snprintf(s->last_error, sizeof(s->last_error), "%s", e ? e : "");
}

static void reject(struct vcs_zcode_dht_service *s,
                   enum vcs_zcode_dht_reject_reason r,
                   enum vcs_zcode_dht_reject_reason *out) {
  if ((unsigned)r < VCS_ZCODE_DHT_REJECT_COUNT)
    s->rejected[r]++;
  if (out)
    *out = r;
}

static struct service_peer *peer_find(struct vcs_zcode_dht_service *s,
                                      uint64_t peer_id) {
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
    if (s->peers[i].used && s->peers[i].peer_id == peer_id)
      return &s->peers[i];
  return NULL;
}

static struct service_query *query_find(struct vcs_zcode_dht_service *s,
                                        uint64_t peer, const uint8_t id[16]) {
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
    if (s->queries[i].used && s->queries[i].peer_id == peer &&
        memcmp(s->queries[i].id, id, 16) == 0)
      return &s->queries[i];
  return NULL;
}

static struct service_lookup *lookup_find(struct vcs_zcode_dht_service *s,
                                          uint64_t id) {
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++)
    if (s->lookups[i].used && s->lookups[i].id == id)
      return &s->lookups[i];
  return NULL;
}

static uint32_t query_count(const struct vcs_zcode_dht_service *s) {
  uint32_t n = 0;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
    n += s->queries[i].used;
  return n;
}

static void mark_dirty(struct vcs_zcode_dht_service *s, uint64_t now) {
  if (!s->persistence_dirty)
    s->dirty_since = now;
  s->persistence_dirty = true;
}

static bool query_id(struct vcs_zcode_dht_service *s, uint64_t peer,
                     uint64_t generation, uint8_t out[16]) {
  uint8_t digest[32];
  struct sha3_256_ctx h;
  s->serial++;
  sha3_256_init(&h);
  sha3_256_write(&h, (const uint8_t *)"zcl.dht.query.v1", 17);
  sha3_256_write(&h, s->self_id, 32);
  sha3_256_write(&h, (uint8_t *)&peer, 8);
  sha3_256_write(&h, (uint8_t *)&generation, 8);
  sha3_256_write(&h, (uint8_t *)&s->serial, 8);
  sha3_256_finalize(&h, digest);
  memcpy(out, digest, 16);
  return nonzero(out, 16);
}

static bool outbound_push(struct vcs_zcode_dht_service *s, uint64_t peer,
                          const uint8_t *wire, size_t len) {
  if (!wire || !len || len > VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES ||
      s->outbound_count >= VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND)
    return false;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND; i++)
    if (!s->outbound[i].used) {
      s->outbound[i].used = true;
      s->outbound[i].peer_id = peer;
      s->outbound[i].len = len;
      memcpy(s->outbound[i].wire, wire, len);
      s->outbound_count++;
      return true;
    }
  return false;
}

static bool send_find(struct vcs_zcode_dht_service *s, struct service_peer *p,
                      enum query_kind kind, uint64_t lookup_id,
                      const uint8_t target[32], const uint8_t victim[32],
                      uint64_t now) {
  struct service_query *q = NULL;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
    if (!s->queries[i].used) {
      q = &s->queries[i];
      break;
    }
  if (!q || !p || !p->connected || !p->session.established)
    return false;
  memset(q, 0, sizeof(*q));
  q->used = true;
  q->kind = kind;
  q->peer_id = p->peer_id;
  q->generation = p->session.generation;
  q->deadline = now + (kind == QUERY_PROBE ? VCS_ZCODE_DHT_PROBE_TIMEOUT_S
                                           : VCS_ZCODE_DHT_LOOKUP_CEILING_S);
  q->lookup_id = lookup_id;
  memcpy(q->target, target, 32);
  if (victim)
    memcpy(q->victim, victim, 32);
  if (!query_id(s, p->peer_id, p->session.generation, q->id)) {
    q->used = false;
    return false;
  }
  struct vcs_zcode_dht_msg_find_node m = {.session_generation =
                                              p->session.generation};
  memcpy(m.sender_node_id, s->self_id, 32);
  memcpy(m.query_id, q->id, 16);
  memcpy(m.target_node_id, target, 32);
  m.delegation = s->delegation;
  uint8_t wire[VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES];
  size_t len = 0;
  if (vcs_zcode_dht_msg_serialize_find_node(&m, p->session.transcript_hash,
                                            s->online_seed, wire, sizeof(wire),
                                            &len) != VCS_ZCODE_DHT_OK ||
      !outbound_push(s, p->peer_id, wire, len)) {
    q->used = false;
    return false;
  }
  s->find_sent++;
  return true;
}

static void lookup_hint(struct service_lookup *l, const uint8_t id[32]) {
  if (!l || !nonzero(id, 32))
    return;
  for (uint32_t i = 0; i < l->hint_count; i++)
    if (memcmp(l->hints[i], id, 32) == 0)
      return;
  if (l->hint_count < VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES)
    memcpy(l->hints[l->hint_count++], id, 32);
}

static void query_finish(struct vcs_zcode_dht_service *s,
                         struct service_query *q, bool response, uint64_t now) {
  if (q->kind == QUERY_LOOKUP) {
    struct service_lookup *l = lookup_find(s, q->lookup_id);
    if (l && response)
      l->response_received = true;
    if (l && l->queries_pending)
      l->queries_pending--;
    if (l && l->queries_pending == 0) {
      l->completed = true;
      l->not_found = !l->response_received;
    }
  } else if (q->kind == QUERY_PROBE && nonzero(q->victim, 32)) {
    (void)vcs_zcode_dht_table_probe_result(s->table, q->victim, response,
                                           (int64_t)now);
    mark_dirty(s, now);
  }
  memset(q, 0, sizeof(*q));
}

static void query_expire(struct vcs_zcode_dht_service *s,
                         struct service_query *q, uint64_t now) {
  if (q->kind == QUERY_LOOKUP) {
    struct service_lookup *l = lookup_find(s, q->lookup_id);
    if (l)
      l->timed_out = true;
  }
  query_finish(s, q, false, now);
}

static enum vcs_zcode_dht_reject_reason
map_parse_error(enum vcs_zcode_dht_error e) {
  if (e == VCS_ZCODE_DHT_ERR_SESSION)
    return VCS_ZCODE_DHT_REJECT_SESSION;
  if (e == VCS_ZCODE_DHT_ERR_SIGNATURE)
    return VCS_ZCODE_DHT_REJECT_SIGNATURE;
  if (e == VCS_ZCODE_DHT_ERR_IDENTITY)
    return VCS_ZCODE_DHT_REJECT_IDENTITY;
  if (e == VCS_ZCODE_DHT_ERR_DELEGATION || e == VCS_ZCODE_DHT_ERR_NETWORK)
    return VCS_ZCODE_DHT_REJECT_DELEGATION;
  if (e == VCS_ZCODE_DHT_ERR_WIRE_ORDER || e == VCS_ZCODE_DHT_ERR_ID_ZERO)
    return VCS_ZCODE_DHT_REJECT_POISONED;
  return VCS_ZCODE_DHT_REJECT_MALFORMED;
}

static bool replay_accept(struct service_peer *p, const uint8_t id[16],
                          uint64_t now) {
  size_t oldest = 0;
  uint64_t oldest_seen = UINT64_MAX;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_REPLAY_PER_PEER; i++) {
    if (p->replay[i].used &&
        now - p->replay[i].seen <= VCS_ZCODE_DHT_SERVICE_REPLAY_SECONDS &&
        memcmp(p->replay[i].id, id, 16) == 0)
      return false;
    if (!p->replay[i].used ||
        now - p->replay[i].seen > VCS_ZCODE_DHT_SERVICE_REPLAY_SECONDS) {
      oldest = i;
      oldest_seen = 0;
      break;
    }
    if (p->replay[i].seen < oldest_seen) {
      oldest = i;
      oldest_seen = p->replay[i].seen;
    }
  }
  p->replay[oldest].used = true;
  memcpy(p->replay[oldest].id, id, 16);
  p->replay[oldest].seen = now;
  return true;
}

static bool rate_accept(struct service_peer *p, uint64_t now) {
  if (p->rate_refill == 0) {
    p->rate_refill = now;
    p->rate_tokens = VCS_ZCODE_DHT_SERVICE_RATE_BURST;
  }
  if (now > p->rate_refill) {
    uint64_t add =
        (now - p->rate_refill) * VCS_ZCODE_DHT_SERVICE_RATE_PER_SECOND;
    uint64_t total = p->rate_tokens + add;
    p->rate_tokens = (uint8_t)(total > VCS_ZCODE_DHT_SERVICE_RATE_BURST
                                   ? VCS_ZCODE_DHT_SERVICE_RATE_BURST
                                   : total);
    p->rate_refill = now;
  }
  if (!p->rate_tokens)
    return false;
  p->rate_tokens--;
  return true;
}

static int node_cmp(const void *a, const void *b) { return memcmp(a, b, 32); }

static bool reply_nodes(struct vcs_zcode_dht_service *s, struct service_peer *p,
                        const uint8_t query[16], const uint8_t target[32],
                        uint64_t now) {
  struct vcs_zcode_dht_contact closest[VCS_ZCODE_DHT_K];
  size_t n = vcs_zcode_dht_table_closest(s->table, target, closest,
                                         VCS_ZCODE_DHT_K - 1);
  struct vcs_zcode_dht_msg_nodes m = {
      .session_generation = p->session.generation, .delegation = s->delegation};
  memcpy(m.sender_node_id, s->self_id, 32);
  memcpy(m.query_id, query, 16);
  memcpy(m.node_ids[m.contact_count++], s->self_id, 32);
  for (size_t i = 0; i < n && m.contact_count < VCS_ZCODE_DHT_K; i++)
    if (closest[i].delegation_expiry > now &&
        memcmp(closest[i].node_id, s->self_id, 32) != 0)
      memcpy(m.node_ids[m.contact_count++], closest[i].node_id, 32);
  qsort(m.node_ids, m.contact_count, 32, node_cmp);
  uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
  size_t len = 0;
  if (vcs_zcode_dht_msg_serialize_nodes(&m, p->session.transcript_hash,
                                        s->online_seed, wire, sizeof(wire),
                                        &len) != VCS_ZCODE_DHT_OK ||
      !outbound_push(s, p->peer_id, wire, len))
    return false;
  s->nodes_sent++;
  return true;
}

struct vcs_zcode_dht_service *
vcs_zcode_dht_service_create(const struct vcs_zcode_dht_service_params *p) {
  if (!p || !p->datadir)
    return NULL;
  struct vcs_zcode_dht_service *s =
      zcl_calloc(1, sizeof(*s), "zcode_dht_service");
  if (!s)
    return NULL;
  s->table = zcl_malloc(sizeof(*s->table), "zcode_dht_table");
  if (!s->table) {
    free(s);
    return NULL;
  }
  snprintf(s->datadir, sizeof(s->datadir), "%s", p->datadir);
  memcpy(s->genesis, p->network_genesis, 32);
  memcpy(s->local_noise_static, p->local_noise_static, 32);
  s->chain_verify = p->chain_verify;
  s->chain_ctx = p->chain_ctx;
  s->next_lookup_id = 1;
  char err[160];
  uint8_t online_pub[32], secret_copy[32];
  if (!p->transport_enabled) {
    snprintf(s->disabled_reason, sizeof(s->disabled_reason),
             "V2_TRANSPORT_DISABLED");
    return s;
  }
  if (!vcs_zcode_dht_delegation_load(p->datadir, &s->delegation, err,
                                     sizeof(err)) ||
      !vcs_zcode_dht_online_key_load(p->datadir, s->online_seed, online_pub,
                                     err, sizeof(err))) {
    snprintf(s->disabled_reason, sizeof(s->disabled_reason),
             "IDENTITY_MATERIAL_UNAVAILABLE");
    vcs_zcode_dht_service_set_error(s, err);
    return s;
  }
  zcl_ed25519_keypair(online_pub, secret_copy, s->online_seed);
  memory_cleanse(secret_copy, 32);
  if (memcmp(online_pub, s->delegation.online_pubkey, 32) != 0 ||
      vcs_zcode_dht_delegation_verify(
          &s->delegation, s->genesis, p->local_noise_static, 0, NULL,
          p->now_unix) != VCS_ZCODE_DHT_DELEGATION_OK ||
      (s->chain_verify && !s->chain_verify(s->chain_ctx, &s->delegation)) ||
      !vcs_zcode_dht_delegation_node_id(s->self_id, &s->delegation) ||
      !vcs_zcode_dht_table_init(s->table, s->self_id)) {
    snprintf(s->disabled_reason, sizeof(s->disabled_reason),
             "IDENTITY_MISMATCH_OR_STALE");
    vcs_zcode_dht_service_set_error(s, "local delegation verification failed");
    return s;
  }
  s->enabled = true;
  (void)vcs_zcode_dht_service_persistence_load(s, p->now_unix);
  return s;
}

void vcs_zcode_dht_service_free(struct vcs_zcode_dht_service *s, uint64_t now) {
  (void)now;
  if (!s)
    return;
  if (s->enabled && s->persistence_dirty)
    (void)vcs_zcode_dht_service_persistence_save(s);
  memory_cleanse(s->online_seed, 32);
  free(s->table);
  free(s);
}

bool vcs_zcode_dht_service_enabled(const struct vcs_zcode_dht_service *s) {
  return s && s->enabled;
}

bool vcs_zcode_dht_service_session_open(
    struct vcs_zcode_dht_service *s, uint64_t peer_id,
    const struct vcs_zcode_dht_session *session, uint64_t now) {
  if (!s || !s->enabled || !peer_id || !session || !session->established ||
      !session->generation)
    return false;
  struct service_peer *p = peer_find(s, peer_id);
  if (p && p->connected && p->session.generation == session->generation &&
      memcmp(p->session.transcript_hash, session->transcript_hash, 32) == 0 &&
      memcmp(p->session.remote_static, session->remote_static, 32) == 0)
    return true;
  if (!p)
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
      if (!s->peers[i].used) {
        p = &s->peers[i];
        break;
      }
  if (!p)
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
      if (!s->peers[i].connected) {
        p = &s->peers[i];
        break;
      }
  if (!p) {
    s->rejected[VCS_ZCODE_DHT_REJECT_CAP]++;
    return false;
  }
  bool changed = !p->used || p->session.generation != session->generation;
  memset(p, 0, sizeof(*p));
  p->used = true;
  p->connected = true;
  p->peer_id = peer_id;
  p->session = *session;
  p->rate_tokens = VCS_ZCODE_DHT_SERVICE_RATE_BURST;
  p->rate_refill = now;
  if (changed && query_count(s) < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES)
    (void)send_find(s, p, QUERY_BOOTSTRAP, 0, s->self_id, NULL, now);
  return true;
}

void vcs_zcode_dht_service_session_close(struct vcs_zcode_dht_service *s,
                                         uint64_t peer_id, uint64_t generation,
                                         uint64_t now) {
  if (!s)
    return;
  struct service_peer *p = peer_find(s, peer_id);
  if (!p || p->session.generation != generation)
    return;
  p->connected = false;
  if (p->authenticated &&
      vcs_zcode_dht_table_note_failure(s->table, p->node_id))
    mark_dirty(s, now);
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
    if (s->queries[i].used && s->queries[i].peer_id == peer_id) {
      query_finish(s, &s->queries[i], false, now);
    }
}

void vcs_zcode_dht_service_sessions_reconcile(
    struct vcs_zcode_dht_service *s,
    const struct vcs_zcode_dht_live_session *live, size_t live_count,
    uint64_t now) {
  if (!s || (live_count && !live))
    return;
  if (live_count > VCS_ZCODE_DHT_SERVICE_MAX_PEERS)
    live_count = VCS_ZCODE_DHT_SERVICE_MAX_PEERS;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++) {
    struct service_peer *p = &s->peers[i];
    if (!p->used || !p->connected)
      continue;
    bool found = false;
    for (size_t j = 0; j < live_count; j++)
      if (live[j].peer_id == p->peer_id &&
          live[j].generation == p->session.generation) {
        found = true;
        break;
      }
    if (!found)
      vcs_zcode_dht_service_session_close(s, p->peer_id, p->session.generation,
                                          now);
  }
}

bool vcs_zcode_dht_service_handle_frame(
    struct vcs_zcode_dht_service *s, uint64_t peer_id, const uint8_t *wire,
    size_t len, uint64_t now, enum vcs_zcode_dht_reject_reason *rejected_out) {
  if (!s || !s->enabled || !wire)
    return false;
  struct service_peer *p = peer_find(s, peer_id);
  if (!p || !p->connected || !p->session.established) {
    reject(s, VCS_ZCODE_DHT_REJECT_PLAINTEXT, rejected_out);
    return false;
  }
  struct vcs_zcode_dht_msg_verify_context v = {.noise_established = true,
                                               .session_generation =
                                                   p->session.generation,
                                               .now_unix = now,
                                               .chain_verify = s->chain_verify,
                                               .chain_ctx = s->chain_ctx};
  memcpy(v.noise_transcript_hash, p->session.transcript_hash, 32);
  memcpy(v.remote_noise_static, p->session.remote_static, 32);
  memcpy(v.network_genesis, s->genesis, 32);
  struct vcs_zcode_dht_msg m;
  enum vcs_zcode_dht_error e = vcs_zcode_dht_msg_parse(wire, len, &v, &m);
  if (e != VCS_ZCODE_DHT_OK) {
    reject(s, map_parse_error(e), rejected_out);
    return false;
  }
  const uint8_t *qid = m.kind == VCS_ZCODE_DHT_MSG_FIND_NODE
                           ? m.find_node.query_id
                           : m.nodes.query_id;
  const struct vcs_zcode_dht_delegation *d =
      m.kind == VCS_ZCODE_DHT_MSG_FIND_NODE ? &m.find_node.delegation
                                            : &m.nodes.delegation;
  struct service_query *q = NULL;
  if (m.kind == VCS_ZCODE_DHT_MSG_NODES) {
    q = query_find(s, peer_id, qid);
    if (!q) {
      reject(s, VCS_ZCODE_DHT_REJECT_UNSOLICITED, rejected_out);
      return false;
    }
    if (q->deadline <= now) {
      query_expire(s, q, now);
      reject(s, VCS_ZCODE_DHT_REJECT_EXPIRED, rejected_out);
      return false;
    }
  }
  if (!replay_accept(p, qid, now)) {
    reject(s, VCS_ZCODE_DHT_REJECT_REPLAY, rejected_out);
    return false;
  }
  if (m.kind == VCS_ZCODE_DHT_MSG_FIND_NODE && !rate_accept(p, now)) {
    reject(s, VCS_ZCODE_DHT_REJECT_RATE, rejected_out);
    return false;
  }
  struct vcs_zcode_dht_contact c;
  if (!vcs_zcode_dht_contact_from_delegation(&c, d, (int64_t)now, 0)) {
    reject(s, VCS_ZCODE_DHT_REJECT_DELEGATION, rejected_out);
    return false;
  }
  enum vcs_zcode_dht_add_result ar =
      vcs_zcode_dht_table_add_contact(s->table, &c, (int64_t)now);
  if (ar >= VCS_ZCODE_DHT_ADD_REJECTED_SELF &&
      ar != VCS_ZCODE_DHT_ADD_REJECTED_PENDING) {
    reject(s, VCS_ZCODE_DHT_REJECT_IDENTITY, rejected_out);
    return false;
  }
  p->authenticated = true;
  memcpy(p->node_id, c.node_id, 32);
  p->contact = c;
  mark_dirty(s, now);
  if (m.kind == VCS_ZCODE_DHT_MSG_FIND_NODE) {
    s->find_received++;
    if (!reply_nodes(s, p, qid, m.find_node.target_node_id, now)) {
      reject(s, VCS_ZCODE_DHT_REJECT_CAP, rejected_out);
      return false;
    }
  } else {
    s->nodes_received++;
    if (q->kind == QUERY_LOOKUP) {
      struct service_lookup *l = lookup_find(s, q->lookup_id);
      if (l)
        for (uint32_t i = 0; i < m.nodes.contact_count; i++)
          lookup_hint(l, m.nodes.node_ids[i]);
    }
    query_finish(s, q, true, now);
  }
  s->frames_accepted++;
  return true;
}

bool vcs_zcode_dht_service_next_outbound(struct vcs_zcode_dht_service *s,
                                         uint64_t filter, uint64_t *peer_out,
                                         uint8_t *wire, size_t cap,
                                         size_t *len_out) {
  if (!s || !peer_out || !wire || !len_out)
    return false;
  if (!s->enabled)
    return false;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND; i++) {
    struct service_outbound *o = &s->outbound[i];
    if (!o->used || (filter && o->peer_id != filter))
      continue;
    if (cap < o->len)
      return false;
    *peer_out = o->peer_id;
    *len_out = o->len;
    memcpy(wire, o->wire, o->len);
    memset(o, 0, sizeof(*o));
    s->outbound_count--;
    return true;
  }
  return false;
}

void vcs_zcode_dht_service_tick(struct vcs_zcode_dht_service *s, uint64_t now) {
  if (!s || !s->enabled)
    return;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
    if (s->queries[i].used && s->queries[i].deadline <= now)
      query_expire(s, &s->queries[i], now);
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++)
    if (s->lookups[i].used && !s->lookups[i].completed &&
        s->lookups[i].deadline <= now) {
      s->lookups[i].timed_out = true;
      s->lookups[i].completed = true;
    }
  size_t expired = vcs_zcode_dht_table_expire_probes(s->table, (int64_t)now);
  if (expired)
    mark_dirty(s, now);
  for (size_t i = 0; i < VCS_ZCODE_DHT_MAX_PENDING &&
                     query_count(s) < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES;
       i++)
    if (s->table->pending[i].active) {
      bool already = false;
      for (size_t q = 0; q < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; q++)
        if (s->queries[q].used &&
            memcmp(s->queries[q].victim, s->table->pending[i].victim_node_id,
                   32) == 0)
          already = true;
      if (already)
        continue;
      for (size_t p = 0; p < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; p++)
        if (s->peers[p].used && s->peers[p].connected &&
            s->peers[p].authenticated &&
            memcmp(s->peers[p].node_id, s->table->pending[i].victim_node_id,
                   32) == 0) {
          (void)send_find(s, &s->peers[p], QUERY_PROBE, 0, s->self_id,
                          s->table->pending[i].victim_node_id, now);
          break;
        }
    }
  if (s->persistence_dirty &&
      now >= s->dirty_since + VCS_ZCODE_DHT_SERVICE_SAVE_DEBOUNCE_S)
    (void)vcs_zcode_dht_service_persistence_save(s);
}

bool vcs_zcode_dht_service_lookup_begin(struct vcs_zcode_dht_service *s,
                                        const uint8_t target[32], uint64_t now,
                                        uint64_t *id_out) {
  if (!s || !s->enabled || !target || !nonzero(target, 32) || !id_out)
    return false;
  struct service_lookup *l = NULL;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++)
    if (!s->lookups[i].used) {
      l = &s->lookups[i];
      break;
    }
  if (!l)
    return false;
  memset(l, 0, sizeof(*l));
  l->used = true;
  l->id = s->next_lookup_id++;
  if (!l->id)
    l->id = s->next_lookup_id++;
  l->deadline = now + VCS_ZCODE_DHT_LOOKUP_CEILING_S;
  memcpy(l->target, target, 32);
  lookup_hint(l, s->self_id);
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS &&
                     l->queries_sent < VCS_ZCODE_DHT_ALPHA &&
                     query_count(s) < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES;
       i++)
    if (s->peers[i].used && s->peers[i].connected &&
        s->peers[i].authenticated &&
        send_find(s, &s->peers[i], QUERY_LOOKUP, l->id, target, NULL, now)) {
      l->queries_sent++;
      l->queries_pending++;
    }
  if (!l->queries_sent)
    l->completed = true;
  *id_out = l->id;
  return true;
}

static bool closer_id(const uint8_t a[32], const uint8_t b[32],
                      const uint8_t target[32]) {
  uint8_t ad[32], bd[32];
  vcs_zcode_dht_xor_distance(a, target, ad);
  vcs_zcode_dht_xor_distance(b, target, bd);
  int c = memcmp(ad, bd, 32);
  return c < 0 || (c == 0 && memcmp(a, b, 32) < 0);
}

bool vcs_zcode_dht_service_lookup_poll(
    struct vcs_zcode_dht_service *s, uint64_t id, uint64_t now,
    struct vcs_zcode_dht_lookup_result *out) {
  if (!s || !out)
    return false;
  vcs_zcode_dht_service_tick(s, now);
  struct service_lookup *l = lookup_find(s, id);
  if (!l)
    return false;
  memset(out, 0, sizeof(*out));
  out->state =
      !l->completed
          ? VCS_ZCODE_DHT_LOOKUP_PENDING
          : (l->timed_out ? VCS_ZCODE_DHT_LOOKUP_TIMEOUT
                          : (l->not_found ? VCS_ZCODE_DHT_LOOKUP_NOT_FOUND
                                          : VCS_ZCODE_DHT_LOOKUP_COMPLETE));
  struct vcs_zcode_dht_contact contacts[VCS_ZCODE_DHT_K];
  size_t n = vcs_zcode_dht_table_closest(s->table, l->target, contacts,
                                         VCS_ZCODE_DHT_K - 1);
  memcpy(out->node_ids[out->count++], s->self_id, 32);
  for (size_t i = 0; i < n && out->count < VCS_ZCODE_DHT_K; i++) {
    size_t at = out->count;
    while (at &&
           closer_id(contacts[i].node_id, out->node_ids[at - 1], l->target)) {
      memcpy(out->node_ids[at], out->node_ids[at - 1], 32);
      at--;
    }
    memcpy(out->node_ids[at], contacts[i].node_id, 32);
    out->count++;
  }
  if (l->completed)
    memset(l, 0, sizeof(*l));
  return true;
}
