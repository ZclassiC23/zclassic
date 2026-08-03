/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Bounded long-lived FIND_NODE/NODES service for the ZCODE DHT. */

#ifndef ZCL_VCS_ZCODE_DHT_SERVICE_H
#define ZCL_VCS_ZCODE_DHT_SERVICE_H

#include "vcs/zcode_dht_msgs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_DHT_SERVICE_MAX_PEERS 64u
#define VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS 8u
#define VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES 3u
#define VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES 64u
#define VCS_ZCODE_DHT_SERVICE_REPLAY_PER_PEER 16u
#define VCS_ZCODE_DHT_SERVICE_REPLAY_SECONDS 30u
#define VCS_ZCODE_DHT_SERVICE_RATE_PER_SECOND 4u
#define VCS_ZCODE_DHT_SERVICE_RATE_BURST 8u
#define VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND 128u
#define VCS_ZCODE_DHT_SERVICE_SAVE_DEBOUNCE_S 5u

enum vcs_zcode_dht_reject_reason {
  VCS_ZCODE_DHT_REJECT_MALFORMED = 0,
  VCS_ZCODE_DHT_REJECT_PLAINTEXT,
  VCS_ZCODE_DHT_REJECT_DELEGATION,
  VCS_ZCODE_DHT_REJECT_IDENTITY,
  VCS_ZCODE_DHT_REJECT_SIGNATURE,
  VCS_ZCODE_DHT_REJECT_SESSION,
  VCS_ZCODE_DHT_REJECT_REPLAY,
  VCS_ZCODE_DHT_REJECT_UNSOLICITED,
  VCS_ZCODE_DHT_REJECT_EXPIRED,
  VCS_ZCODE_DHT_REJECT_POISONED,
  VCS_ZCODE_DHT_REJECT_RATE,
  VCS_ZCODE_DHT_REJECT_CAP,
  VCS_ZCODE_DHT_REJECT_COUNT
};

const char *
vcs_zcode_dht_reject_reason_string(enum vcs_zcode_dht_reject_reason reason);

struct vcs_zcode_dht_session {
  bool established;
  uint8_t remote_static[32];
  uint8_t transcript_hash[32];
  uint64_t generation;
};

struct vcs_zcode_dht_live_session {
  uint64_t peer_id;
  uint64_t generation;
};

struct vcs_zcode_dht_service_params {
  const char *datadir;
  uint8_t network_genesis[32];
  uint8_t local_noise_static[32];
  bool transport_enabled;
  uint64_t now_unix;
  vcs_zcode_dht_chain_verify_fn chain_verify;
  void *chain_ctx;
};

struct vcs_zcode_dht_service;

struct vcs_zcode_dht_service_status {
  bool enabled;
  uint8_t local_node_id[32];
  uint32_t contacts;
  uint32_t connected_authenticated;
  uint32_t cold_contacts;
  uint32_t pending_probes;
  uint32_t active_queries;
  uint32_t queued_lookups;
  uint32_t outbound_queued;
  uint64_t frames_accepted;
  uint64_t frames_rejected[VCS_ZCODE_DHT_REJECT_COUNT];
  uint64_t find_node_received;
  uint64_t nodes_received;
  uint64_t find_node_sent;
  uint64_t nodes_sent;
  bool persistence_loaded;
  bool persistence_dirty;
  uint64_t persistence_load_count;
  uint64_t persistence_save_count;
  char disabled_reason[96];
  char last_error[160];
};

struct vcs_zcode_dht_peer_view {
  uint64_t peer_id;
  uint8_t node_id[32];
  int bucket;
  bool connected;
  bool authenticated;
  bool cold;
  bool probing;
  uint64_t last_seen_age_s;
  uint32_t failures;
  uint64_t delegation_expiry;
  uint32_t beacon_height;
};

enum vcs_zcode_dht_lookup_state {
  VCS_ZCODE_DHT_LOOKUP_PENDING = 0,
  VCS_ZCODE_DHT_LOOKUP_COMPLETE,
  VCS_ZCODE_DHT_LOOKUP_TIMEOUT,
  VCS_ZCODE_DHT_LOOKUP_NOT_FOUND,
};

struct vcs_zcode_dht_lookup_result {
  enum vcs_zcode_dht_lookup_state state;
  uint32_t count;
  uint8_t node_ids[VCS_ZCODE_DHT_K][32];
};

struct vcs_zcode_dht_service *
vcs_zcode_dht_service_create(const struct vcs_zcode_dht_service_params *params);
void vcs_zcode_dht_service_free(struct vcs_zcode_dht_service *service,
                                uint64_t now_unix);
bool vcs_zcode_dht_service_enabled(const struct vcs_zcode_dht_service *service);

bool vcs_zcode_dht_service_session_open(
    struct vcs_zcode_dht_service *service, uint64_t peer_id,
    const struct vcs_zcode_dht_session *session, uint64_t now_unix);
void vcs_zcode_dht_service_session_close(struct vcs_zcode_dht_service *service,
                                         uint64_t peer_id, uint64_t generation,
                                         uint64_t now_unix);
void vcs_zcode_dht_service_sessions_reconcile(
    struct vcs_zcode_dht_service *service,
    const struct vcs_zcode_dht_live_session *live, size_t live_count,
    uint64_t now_unix);

/* Handle one ZCDHTM frame already extracted from zpkgswm.  The service queues
 * any response; network I/O remains the composition root's responsibility. */
bool vcs_zcode_dht_service_handle_frame(
    struct vcs_zcode_dht_service *service, uint64_t peer_id,
    const uint8_t *wire, size_t wire_len, uint64_t now_unix,
    enum vcs_zcode_dht_reject_reason *rejected_out);
bool vcs_zcode_dht_service_next_outbound(struct vcs_zcode_dht_service *service,
                                         uint64_t peer_filter,
                                         uint64_t *peer_out, uint8_t *wire_out,
                                         size_t wire_capacity,
                                         size_t *wire_len_out);
void vcs_zcode_dht_service_tick(struct vcs_zcode_dht_service *service,
                                uint64_t now_unix);

bool vcs_zcode_dht_service_lookup_begin(struct vcs_zcode_dht_service *service,
                                        const uint8_t target[32],
                                        uint64_t now_unix,
                                        uint64_t *lookup_id_out);
bool vcs_zcode_dht_service_lookup_poll(struct vcs_zcode_dht_service *service,
                                       uint64_t lookup_id, uint64_t now_unix,
                                       struct vcs_zcode_dht_lookup_result *out);

void vcs_zcode_dht_service_status(const struct vcs_zcode_dht_service *service,
                                  struct vcs_zcode_dht_service_status *out);
size_t vcs_zcode_dht_service_peers(const struct vcs_zcode_dht_service *service,
                                   uint64_t now_unix,
                                   struct vcs_zcode_dht_peer_view *out,
                                   size_t max, size_t offset);

/* Called by the existing ZID status-generation revalidation worker. */
bool vcs_zcode_dht_service_revalidate(struct vcs_zcode_dht_service *service,
                                      uint64_t now_unix);

#endif /* ZCL_VCS_ZCODE_DHT_SERVICE_H */
