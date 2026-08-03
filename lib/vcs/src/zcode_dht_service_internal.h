/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Private fixed-size state shared by the ZCODE DHT service units. */

#ifndef ZCL_VCS_ZCODE_DHT_SERVICE_INTERNAL_H
#define ZCL_VCS_ZCODE_DHT_SERVICE_INTERNAL_H

#include "vcs/zcode_dht_service.h"

enum query_kind { QUERY_BOOTSTRAP = 0, QUERY_LOOKUP, QUERY_PROBE };

struct replay_entry {
  bool used;
  uint8_t id[16];
  uint64_t seen;
};

struct service_peer {
  bool used, connected, authenticated;
  uint64_t peer_id;
  struct vcs_zcode_dht_session session;
  uint8_t node_id[32];
  struct vcs_zcode_dht_contact contact;
  uint8_t rate_tokens;
  uint64_t rate_refill;
  struct replay_entry replay[VCS_ZCODE_DHT_SERVICE_REPLAY_PER_PEER];
};

struct service_query {
  bool used;
  enum query_kind kind;
  uint8_t id[16], target[32], victim[32];
  uint64_t peer_id, generation, deadline, lookup_id;
};

struct expired_query {
  bool used;
  uint8_t id[16];
  uint64_t peer_id, generation, expired_at;
};

struct service_lookup {
  bool used, completed, timed_out, response_received, not_found;
  uint64_t id, deadline;
  uint8_t target[32];
  uint8_t hints[VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES][32];
  uint32_t hint_count, queries_sent, queries_pending;
};

struct service_outbound {
  bool used;
  uint64_t peer_id;
  size_t len;
  uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
};

struct vcs_zcode_dht_service {
  bool enabled;
  char disabled_reason[96], last_error[160], datadir[1024];
  uint8_t genesis[32], self_id[32], online_seed[32], local_noise_static[32];
  struct vcs_zcode_dht_delegation delegation;
  vcs_zcode_dht_chain_verify_fn chain_verify;
  void *chain_ctx;
  struct vcs_zcode_dht_table *table;
  struct service_peer peers[VCS_ZCODE_DHT_SERVICE_MAX_PEERS];
  struct service_query queries[VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES];
  struct expired_query expired[VCS_ZCODE_DHT_SERVICE_REPLAY_PER_PEER];
  struct service_lookup lookups[VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS];
  struct service_outbound outbound[VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND];
  uint32_t outbound_count;
  uint64_t serial, next_lookup_id;
  bool persistence_loaded, persistence_dirty;
  uint64_t dirty_since, persistence_load_count, persistence_save_count;
  uint64_t frames_accepted, rejected[VCS_ZCODE_DHT_REJECT_COUNT];
  uint64_t find_received, nodes_received, find_sent, nodes_sent;
};

void vcs_zcode_dht_service_set_error(struct vcs_zcode_dht_service *service,
                                     const char *message);
bool vcs_zcode_dht_service_persistence_load(
    struct vcs_zcode_dht_service *service, uint64_t now_unix);
bool vcs_zcode_dht_service_persistence_save(
    struct vcs_zcode_dht_service *service);

#endif /* ZCL_VCS_ZCODE_DHT_SERVICE_INTERNAL_H */
