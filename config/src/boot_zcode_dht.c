/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Composition-root adapter between Noise peers and ZCODE DHT. */

#include "config/boot_zcode_dht.h"

#include "base/hex.h"
#include "config/boot_internal.h"
#include "models/zid_identity.h"
#include "net/addrman.h"
#include "net/connman.h"
#include "net/net.h"
#include "net/netaddr.h"
#include "net/peer_scoring.h"
#include "net/v2_transport.h"
#include "platform/time_compat.h"
#include "rpc/server.h"
#include "services/chain_state_service.h"
#include "util/log_macros.h"
#include "util/sync.h"
#include "validation/chainstate.h"
#include "validation/main_constants.h"
#include "vcs/zcode_dht_service.h"
#include "vcs/zendp_swarm.h"
#include "json/json.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define DHT_FRAME_PREFIX "ZCDHTM"
#define DHT_RETRY_SECONDS 5

static zcl_mutex_t g_dht_lock;
static _Atomic int g_dht_lock_state;
static struct vcs_zcode_dht_service *g_dht;
static uint64_t g_last_create_attempt_mono;
static uint8_t g_dht_genesis[32];
static _Atomic uint64_t g_ancestry_lookups;
static _Atomic uint64_t g_ancestry_height_span;

static struct vcs_zcode_dht_time dht_now(void) {
  struct vcs_zcode_dht_time now = {
      .wall_unix = (uint64_t)platform_time_wall_time_t(),
      .monotonic_s = (uint64_t)(platform_time_monotonic_ms() / 1000),
  };
  return now;
}

static void dht_status_json_locked(struct json_value *out) {
  struct vcs_zcode_dht_service_status status;
  vcs_zcode_dht_service_status(g_dht, &status);
  char node_id[65] = {0};
  if (status.enabled)
    zcl_hex_encode(status.local_node_id, 32, node_id);
  json_set_object(out);
  json_push_kv_bool(out, "enabled", status.enabled);
  json_push_kv_str(out, "disabled_reason", status.disabled_reason);
  json_push_kv_str(out, "local_node_id", node_id);
  json_push_kv_int(out, "k", VCS_ZCODE_DHT_K);
  json_push_kv_int(out, "alpha", VCS_ZCODE_DHT_ALPHA);
  json_push_kv_int(out, "max_contacts", VCS_ZCODE_DHT_MAX_CONTACTS);
  json_push_kv_int(out, "max_authenticated_peers",
                   VCS_ZCODE_DHT_SERVICE_MAX_PEERS);
  json_push_kv_int(out, "max_queued_lookups",
                   VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS);
  json_push_kv_int(out, "max_active_queries",
                   VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES);
  json_push_kv_int(out, "max_lookup_candidates",
                   VCS_ZCODE_DHT_SERVICE_MAX_CANDIDATES);
  json_push_kv_int(out, "lookup_ceiling_seconds",
                   VCS_ZCODE_DHT_LOOKUP_CEILING_S);
  json_push_kv_int(out, "replay_entries_per_peer",
                   VCS_ZCODE_DHT_SERVICE_REPLAY_PER_PEER);
  json_push_kv_int(out, "replay_retention_seconds",
                   VCS_ZCODE_DHT_SERVICE_REPLAY_SECONDS);
  json_push_kv_int(out, "inbound_rate_per_second",
                   VCS_ZCODE_DHT_SERVICE_RATE_PER_SECOND);
  json_push_kv_int(out, "inbound_rate_burst", VCS_ZCODE_DHT_SERVICE_RATE_BURST);
  json_push_kv_int(out, "max_outbound_frames",
                   VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND);
  json_push_kv_int(out, "contacts", status.contacts);
  json_push_kv_int(out, "buckets_used", status.buckets_used);
  json_push_kv_int(out, "connected_authenticated",
                   status.connected_authenticated);
  json_push_kv_int(out, "cold_contacts", status.cold_contacts);
  json_push_kv_int(out, "pending_probes", status.pending_probes);
  static const char *const probe_names[] = {
      "waiting", "in_flight", "responded", "failed", "expired"};
  struct json_value probes;
  json_init(&probes);
  json_set_object(&probes);
  for (int i = 0; i < VCS_ZCODE_DHT_PROBE_STATE_COUNT; i++)
    json_push_kv_int(&probes, probe_names[i],
                     (int64_t)status.probe_transitions[i]);
  json_push_kv(out, "probe_transitions", &probes);
  json_free(&probes);
  json_push_kv_int(out, "active_queries", status.active_queries);
  json_push_kv_int(out, "queued_lookups", status.queued_lookups);
  json_push_kv_int(out, "outbound_queued", status.outbound_queued);
  json_push_kv_int(out, "frames_accepted", (int64_t)status.frames_accepted);
  struct json_value rejected;
  json_init(&rejected);
  json_set_object(&rejected);
  for (int i = 0; i < VCS_ZCODE_DHT_REJECT_COUNT; i++)
    json_push_kv_int(&rejected, vcs_zcode_dht_reject_reason_string(i),
                     (int64_t)status.frames_rejected[i]);
  json_push_kv(out, "frames_rejected", &rejected);
  json_free(&rejected);
  json_push_kv_int(out, "find_node_received",
                   (int64_t)status.find_node_received);
  json_push_kv_int(out, "nodes_received", (int64_t)status.nodes_received);
  json_push_kv_int(out, "find_node_sent", (int64_t)status.find_node_sent);
  json_push_kv_int(out, "nodes_sent", (int64_t)status.nodes_sent);
  json_push_kv_int(out, "lookup_rounds", (int64_t)status.lookup_rounds);
  json_push_kv_int(out, "lookup_xor_progress",
                   (int64_t)status.lookup_xor_progress);
  json_push_kv_int(out, "lookup_queue_wait_seconds",
                   (int64_t)status.lookup_queue_wait_s);
  static const char *const candidate_names[] = {
      "unverified", "unreachable", "authenticated", "queried",
      "in_flight", "responded", "failed"};
  struct json_value shortlist;
  json_init(&shortlist);
  json_set_object(&shortlist);
  for (int i = 0; i < VCS_ZCODE_DHT_CANDIDATE_STATE_COUNT; i++)
    json_push_kv_int(&shortlist, candidate_names[i],
                     (int64_t)status.lookup_shortlist_states[i]);
  json_push_kv(out, "lookup_shortlist", &shortlist);
  json_free(&shortlist);
  static const char *const termination_names[] = {
      "none", "target_authenticated", "shortlist_stable", "timeout",
      "no_authenticated_result"};
  struct json_value terminations;
  json_init(&terminations);
  json_set_object(&terminations);
  for (int i = 0; i < VCS_ZCODE_DHT_TERMINATION_COUNT; i++)
    json_push_kv_int(&terminations, termination_names[i],
                     (int64_t)status.lookup_terminations[i]);
  json_push_kv(out, "lookup_terminations", &terminations);
  json_free(&terminations);
  json_push_kv_int(out, "ancestry_lookups",
                   (int64_t)atomic_load(&g_ancestry_lookups));
  json_push_kv_int(out, "ancestry_max_height_span",
                   (int64_t)atomic_load(&g_ancestry_height_span));
  json_push_kv_bool(out, "persistence_loaded", status.persistence_loaded);
  json_push_kv_bool(out, "persistence_dirty", status.persistence_dirty);
  json_push_kv_int(out, "persistence_load_count",
                   (int64_t)status.persistence_load_count);
  json_push_kv_int(out, "persistence_save_count",
                   (int64_t)status.persistence_save_count);
  json_push_kv_str(out, "last_error", status.last_error);
}

static void dht_lock(void) {
  if (atomic_load_explicit(&g_dht_lock_state, memory_order_acquire) != 2) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_dht_lock_state, &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
      zcl_mutex_init(&g_dht_lock);
      atomic_store_explicit(&g_dht_lock_state, 2, memory_order_release);
    } else {
      while (atomic_load_explicit(&g_dht_lock_state, memory_order_acquire) != 2)
        ;
    }
  }
  zcl_mutex_lock(&g_dht_lock);
}

bool boot_zcode_dht_beacon_matches(const struct block_index *header_tip,
                                   uint32_t beacon_height,
                                   const uint8_t beacon_hash[32],
                                   uint64_t *height_span_out) {
  if (height_span_out)
    *height_span_out = 0;
  if (!header_tip || !beacon_hash || header_tip->nHeight < 0 ||
      beacon_height > (uint32_t)header_tip->nHeight)
    return false;
  uint64_t span = (uint64_t)header_tip->nHeight - beacon_height;
  if (height_span_out)
    *height_span_out = span;
  struct block_index *beacon = block_index_get_ancestor(
      (struct block_index *)header_tip, (int)beacon_height);
  return beacon && beacon->phashBlock &&
         beacon->nHeight == (int)beacon_height &&
         memcmp(beacon->phashBlock->data, beacon_hash, 32) == 0;
}

static bool dht_chain_verify(
    void *ctx, const struct vcs_zcode_dht_delegation *delegation) {
  struct boot_svc_ctx *svc = ctx;
  if (!svc || !svc->state || !svc->node_db || !delegation)
    return false;
  struct zid_identity identity;
  if (!db_zid_identity_find(svc->node_db, delegation->doc.master_pubkey,
                            &identity) ||
      strcmp(identity.status, ZID_IDENTITY_STATUS_ACTIVE) != 0 ||
      identity.anchor_height < 0 ||
      identity.anchor_height > INT32_MAX - ZCL_FINALITY_DEPTH ||
      delegation->beacon_height !=
          (uint32_t)(identity.anchor_height + ZCL_FINALITY_DEPTH))
    return false;
  /* The ZID row proves the anchor body was folded.  Finality is a property
   * of the validated best-header ancestry, not of whether the block-body
   * cache happened to reconstruct its active window on this boot.  Regtest
   * exposed the distinction: after a clean restart the durable header tip
   * and ZID projection were at 122 while the body window was temporarily at
   * genesis, disabling an otherwise valid delegation forever.  Capture the
   * CSR-bound header pointer, then walk its immutable ancestry to ensure the
   * delegated beacon is actually on that chain and ten blocks deep. */
  const struct block_index *header_tip =
      csr_header_tip_snapshot(csr_instance());
  if (!header_tip ||
      header_tip->nHeight <
          (int)delegation->beacon_height + ZCL_FINALITY_DEPTH)
    return false;
  uint64_t span = 0;
  bool ok = boot_zcode_dht_beacon_matches(
      header_tip, delegation->beacon_height, delegation->beacon_hash, &span);
  atomic_fetch_add(&g_ancestry_lookups, 1);
  uint64_t old = atomic_load(&g_ancestry_height_span);
  while (span > old && !atomic_compare_exchange_weak(
                           &g_ancestry_height_span, &old, span))
    ;
  return ok;
}

/* Address-free NODES remains deliberately address-free. A named ID can only
 * request a dial when this node already holds a fresh, chain-verified ZENDP
 * record for its master and can derive the same delayed-beacon node ID. The
 * address enters addrman as a hint; Noise plus the delegation is still the
 * sole promotion authority. */
static bool dht_request_reachability(void *ctx, const uint8_t node_id[32],
                                     uint64_t wall_now) {
  struct boot_svc_ctx *svc = ctx;
  if (!svc || !svc->connman || !node_id)
    return false;
  struct zendp_record_view views[ZENDP_DIR_MAX];
  size_t n = zendp_global_records(wall_now, views, ZENDP_DIR_MAX);
  const struct block_index *tip = csr_header_tip_snapshot(csr_instance());
  if (!tip)
    return false;
  for (size_t i = 0; i < n; i++) {
    const struct zendp_record_view *view = &views[i];
    if (view->anchor_height < 0 ||
        view->anchor_height > INT32_MAX - ZCL_FINALITY_DEPTH)
      continue;
    uint32_t beacon_height =
        (uint32_t)(view->anchor_height + ZCL_FINALITY_DEPTH);
    struct block_index *beacon = block_index_get_ancestor(
        (struct block_index *)tip, (int)beacon_height);
    uint8_t derived[32];
    if (!beacon || !beacon->phashBlock ||
        !vcs_zcode_dht_node_id(derived, g_dht_genesis,
                               view->master_pubkey,
                               beacon->phashBlock->data) ||
        memcmp(derived, node_id, 32) != 0)
      continue;
    struct net_address addr;
    net_address_init(&addr);
    addr.nServices = view->ep.services;
    addr.nTime = (uint32_t)wall_now;
    if (view->ep.flags & ZENDP_HAS_IPV4) {
      net_addr_set_ipv4(&addr.svc.addr, view->ep.ipv4);
      addr.svc.port = view->ep.ipv4_port;
    } else if (view->ep.flags & ZENDP_HAS_IPV6) {
      memcpy(addr.svc.addr.ip, view->ep.ipv6, 16);
      addr.svc.port = view->ep.ipv6_port;
    } else {
      /* Onion-only records remain owned by the existing Tor/ZENDP discovery
       * path. This adapter intentionally does not add a second socket stack. */
      return false;
    }
    return connman_queue_dht_hint(svc->connman, &addr);
  }
  return false;
}

static struct vcs_zcode_dht_service *
dht_ensure(struct msg_processor *mp, struct boot_svc_ctx *svc,
           struct vcs_zcode_dht_time now) {
  if (g_dht && vcs_zcode_dht_service_enabled(g_dht))
    return g_dht;
  if (g_dht && now.monotonic_s <
                   g_last_create_attempt_mono + DHT_RETRY_SECONDS)
    return g_dht;
  g_last_create_attempt_mono = now.monotonic_s;
  if (g_dht)
    vcs_zcode_dht_service_free(g_dht, now);
  g_dht = NULL;
  if (!mp || !mp->params || !mp->net_mgr || !svc || !svc->datadir)
    return NULL;
  struct vcs_zcode_dht_service_params params = {
      .datadir = svc->datadir,
      .transport_enabled = mp->net_mgr->v2_enabled,
      .now = now,
      .chain_verify = dht_chain_verify,
      .chain_ctx = svc,
      .request_reachability = dht_request_reachability,
      .reachability_ctx = svc,
  };
  memcpy(params.network_genesis, mp->params->consensus.hashGenesisBlock.data,
         32);
  memcpy(g_dht_genesis, params.network_genesis, sizeof(g_dht_genesis));
  memcpy(params.local_noise_static, mp->net_mgr->identity_pub, 32);
  g_dht = vcs_zcode_dht_service_create(&params);
  return g_dht;
}

static bool dht_snapshot(struct p2p_node *node,
                         struct vcs_zcode_dht_session *out) {
  memset(out, 0, sizeof(*out));
  struct v2_transport_snapshot snapshot;
  if (!node || !node->transport ||
      !v2_transport_snapshot(node->transport, &snapshot))
    return false;
  out->established = snapshot.established;
  out->generation = snapshot.connection_generation;
  memcpy(out->remote_static, snapshot.remote_static, 32);
  memcpy(out->transcript_hash, snapshot.transcript_hash, 32);
  return out->established;
}

static enum peer_offence dht_offence(enum vcs_zcode_dht_reject_reason reason) {
  if (reason == VCS_ZCODE_DHT_REJECT_RATE || reason == VCS_ZCODE_DHT_REJECT_CAP)
    return PEER_OFFENCE_FLOOD;
  if (reason == VCS_ZCODE_DHT_REJECT_UNSOLICITED)
    return PEER_OFFENCE_UNREQUESTED;
  return PEER_OFFENCE_INVALID_PAYLOAD;
}

static void dht_send(struct msg_processor *mp, struct p2p_node *node,
                     const uint8_t *wire, size_t wire_len) {
  if (!p2p_node_begin_message(node, "zpkgswm", mp->params->pchMessageStart)) {
    LOG_ERROR("net.zcode_dht", "begin_message failed for peer %lld",
              (long long)node->id);
    return;
  }
  p2p_node_write_message_data(node, wire, wire_len);
  if (!p2p_node_end_message(node))
    LOG_ERROR("net.zcode_dht", "end_message failed for peer %lld",
              (long long)node->id);
}

static size_t dht_drain(struct msg_processor *mp, struct p2p_node *node) {
  uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
  uint64_t peer = 0;
  size_t wire_len = 0, sent = 0;
  while (vcs_zcode_dht_service_next_outbound(
      g_dht, (uint64_t)node->id + 1, &peer, wire, sizeof(wire), &wire_len)) {
    if (peer != (uint64_t)node->id + 1)
      break;
    dht_send(mp, node, wire, wire_len);
    sent++;
  }
  return sent;
}

bool boot_zcode_dht_peer_ready(const struct p2p_node *node) {
  return node && !atomic_load(&node->disconnect) && node->transport &&
         atomic_load(&node->state) >= PEER_HANDSHAKE_COMPLETE;
}

bool boot_zcode_dht_frame(struct msg_processor *mp, struct p2p_node *node,
                          const uint8_t *payload, size_t payload_len,
                          struct boot_svc_ctx *svc) {
  if (!payload || payload_len < 6 || memcmp(payload, DHT_FRAME_PREFIX, 6) != 0)
    return false;
  struct vcs_zcode_dht_time now = dht_now();
  dht_lock();
  struct vcs_zcode_dht_service *dht = dht_ensure(mp, svc, now);
  struct vcs_zcode_dht_session session;
  if (dht && dht_snapshot(node, &session))
    (void)vcs_zcode_dht_service_session_open(dht, (uint64_t)node->id + 1,
                                             &session, now);
  enum vcs_zcode_dht_reject_reason rejected = VCS_ZCODE_DHT_REJECT_MALFORMED;
  bool ok = dht && vcs_zcode_dht_service_handle_frame(
                       dht, (uint64_t)node->id + 1, payload, payload_len, now,
                       &rejected);
  if (ok)
    (void)dht_drain(mp, node);
  zcl_mutex_unlock(&g_dht_lock);
  if (!ok && mp && mp->net_mgr) {
    char context[96];
    snprintf(context, sizeof(context), "zcode dht: %s",
             vcs_zcode_dht_reject_reason_string(rejected));
    peer_scoring_record(mp->net_mgr, node, dht_offence(rejected), context);
  }
  return true;
}

void boot_zcode_dht_periodic(struct msg_processor *mp,
                             struct boot_svc_ctx *svc) {
  if (!mp || !mp->net_mgr || !svc)
    return;
  struct vcs_zcode_dht_time now = dht_now();
  struct p2p_node *nodes[VCS_ZCODE_DHT_SERVICE_MAX_PEERS];
  size_t count = 0;
  zcl_mutex_lock(&mp->net_mgr->cs_nodes);
  for (size_t i = 0;
       i < mp->net_mgr->num_nodes && count < VCS_ZCODE_DHT_SERVICE_MAX_PEERS;
       i++) {
    struct p2p_node *node = mp->net_mgr->nodes[i];
    /* Noise establishment alone is below the P2P message-layer boundary.
     * Opening the DHT session before version+verack lets its one-shot
     * bootstrap FIND_NODE arrive as "zpkgswm before version" and be dropped
     * forever. Admit and reconcile only fully handshaked peers. */
    if (!boot_zcode_dht_peer_ready(node))
      continue;
    nodes[count++] = node;
    p2p_node_add_ref(node);
  }
  zcl_mutex_unlock(&mp->net_mgr->cs_nodes);

  dht_lock();
  struct vcs_zcode_dht_service *dht = dht_ensure(mp, svc, now);
  struct vcs_zcode_dht_live_session live[VCS_ZCODE_DHT_SERVICE_MAX_PEERS];
  size_t live_count = 0;
  for (size_t i = 0; dht && i < count; i++) {
    struct vcs_zcode_dht_session session;
    if (!dht_snapshot(nodes[i], &session))
      continue;
    uint64_t peer = (uint64_t)nodes[i]->id + 1;
    if (vcs_zcode_dht_service_session_open(dht, peer, &session, now)) {
      live[live_count].peer_id = peer;
      live[live_count++].generation = session.generation;
    }
  }
  if (dht) {
    vcs_zcode_dht_service_sessions_reconcile(dht, live, live_count, now);
    vcs_zcode_dht_service_tick(dht, now);
    for (size_t i = 0; i < count; i++)
      (void)dht_drain(mp, nodes[i]);
  }
  zcl_mutex_unlock(&g_dht_lock);
  for (size_t i = 0; i < count; i++)
    p2p_node_release(nodes[i]);
}

bool boot_zcode_dht_revalidate(void) {
  struct vcs_zcode_dht_time now = dht_now();
  dht_lock();
  bool ok = !g_dht || vcs_zcode_dht_service_revalidate(g_dht, now);
  zcl_mutex_unlock(&g_dht_lock);
  return ok;
}

bool boot_zcode_dht_dump_state_json(struct json_value *out, const char *key) {
  if (!out)
    return false;
  json_set_object(out);
  if (key && key[0] && strcmp(key, "status") != 0) {
    json_push_kv_str(out, "error", "accepted key is empty or status");
    return false;
  }
  dht_lock();
  dht_status_json_locked(out);
  zcl_mutex_unlock(&g_dht_lock);
  return true;
}

static const struct json_value *dht_rpc_input(const struct json_value *params) {
  const struct json_value *first =
      params && json_size(params) ? json_at(params, 0) : NULL;
  return first && first->type == JSON_OBJ ? first : NULL;
}

static int64_t dht_input_int(const struct json_value *in, const char *key,
                             int64_t fallback) {
  const struct json_value *v = in ? json_get(in, key) : NULL;
  return v && v->type == JSON_INT ? json_get_int(v) : fallback;
}

static void dht_rpc_error(struct json_value *result, const char *code,
                          const char *message) {
  json_set_object(result);
  json_push_kv_bool(result, "ok", false);
  json_push_kv_str(result, "code", code);
  json_push_kv_str(result, "message", message);
}

static bool rpc_zcode_dht_status(const struct json_value *params, bool help,
                                 struct json_value *result) {
  (void)params;
  if (help) {
    json_set_str(result, "zcode_dht_status\nBounded authenticated DHT state");
    return true;
  }
  dht_lock();
  dht_status_json_locked(result);
  json_push_kv_bool(result, "ok", true);
  zcl_mutex_unlock(&g_dht_lock);
  return true;
}

static bool rpc_zcode_dht_peers(const struct json_value *params, bool help,
                                struct json_value *result) {
  if (help) {
    json_set_str(result,
                 "zcode_dht_peers {\"limit\":64,\"offset\":0}\n"
                 "Authenticated contacts only; no addresses or key material");
    return true;
  }
  const struct json_value *in = dht_rpc_input(params);
  int64_t limit = dht_input_int(in, "limit", 64);
  int64_t offset = dht_input_int(in, "offset", 0);
  if (limit < 1 || limit > VCS_ZCODE_DHT_SERVICE_MAX_PEERS || offset < 0 ||
      offset > VCS_ZCODE_DHT_MAX_CONTACTS) {
    dht_rpc_error(result, "INVALID_PAGE",
                  "limit must be 1..64 and offset must be 0..1024");
    return true;
  }
  struct vcs_zcode_dht_peer_view peers[VCS_ZCODE_DHT_SERVICE_MAX_PEERS];
  uint64_t now = (uint64_t)platform_time_wall_time_t();
  dht_lock();
  if (!g_dht || !vcs_zcode_dht_service_enabled(g_dht)) {
    dht_rpc_error(result, "DHT_DISABLED",
                  "the authenticated DHT service is disabled");
    zcl_mutex_unlock(&g_dht_lock);
    return true;
  }
  size_t count = vcs_zcode_dht_service_peers(g_dht, now, peers, (size_t)limit,
                                             (size_t)offset);
  zcl_mutex_unlock(&g_dht_lock);
  json_set_object(result);
  json_push_kv_bool(result, "ok", true);
  json_push_kv_int(result, "limit", limit);
  json_push_kv_int(result, "offset", offset);
  json_push_kv_int(result, "count", (int64_t)count);
  struct json_value rows;
  json_init(&rows);
  json_set_array(&rows);
  for (size_t i = 0; i < count; i++) {
    char node_id[65];
    zcl_hex_encode(peers[i].node_id, 32, node_id);
    struct json_value row;
    json_init(&row);
    json_set_object(&row);
    json_push_kv_str(&row, "node_id", node_id);
    json_push_kv_int(&row, "bucket", peers[i].bucket);
    json_push_kv_bool(&row, "connected", peers[i].connected);
    json_push_kv_bool(&row, "cold", peers[i].cold);
    json_push_kv_bool(&row, "probing", peers[i].probing);
    json_push_kv_int(&row, "last_seen_age_seconds",
                     (int64_t)peers[i].last_seen_age_s);
    json_push_kv_int(&row, "failure_count", peers[i].failures);
    json_push_kv_int(&row, "delegation_expiry",
                     (int64_t)peers[i].delegation_expiry);
    json_push_kv_int(&row, "beacon_height", peers[i].beacon_height);
    json_push_back(&rows, &row);
    json_free(&row);
  }
  json_push_kv(result, "peers", &rows);
  json_free(&rows);
  return true;
}

static const char *
dht_lookup_state_name(enum vcs_zcode_dht_lookup_state state) {
  if (state == VCS_ZCODE_DHT_LOOKUP_COMPLETE)
    return "complete";
  if (state == VCS_ZCODE_DHT_LOOKUP_TIMEOUT)
    return "timeout";
  if (state == VCS_ZCODE_DHT_LOOKUP_NOT_FOUND)
    return "not_found";
  return "pending";
}

static const char *dht_lookup_termination_name(
    enum vcs_zcode_dht_lookup_termination termination) {
  static const char *const names[] = {
      "none", "target_authenticated", "shortlist_stable", "timeout",
      "no_authenticated_result"};
  return (unsigned)termination < VCS_ZCODE_DHT_TERMINATION_COUNT
             ? names[termination]
             : "unknown";
}

static void dht_lookup_json(struct json_value *result,
                            const struct vcs_zcode_dht_lookup_result *lookup) {
  json_set_object(result);
  bool complete = lookup->state == VCS_ZCODE_DHT_LOOKUP_COMPLETE;
  json_push_kv_bool(result, "ok", complete);
  json_push_kv_str(result, "state", dht_lookup_state_name(lookup->state));
  json_push_kv_str(result, "termination",
                   dht_lookup_termination_name(lookup->termination));
  json_push_kv_int(result, "rounds", lookup->rounds);
  json_push_kv_int(result, "xor_progress", lookup->xor_progress);
  json_push_kv_int(result, "queue_wait_seconds",
                   (int64_t)lookup->queue_wait_s);
  if (!complete) {
    bool timeout = lookup->state == VCS_ZCODE_DHT_LOOKUP_TIMEOUT;
    json_push_kv_str(result, "code",
                     timeout ? "LOOKUP_TIMEOUT" : "LOOKUP_NOT_FOUND");
    json_push_kv_str(
        result, "message",
        timeout ? "lookup reached its 30-second bounded deadline"
                : "all authenticated queries failed without a response");
  }
  json_push_kv_int(result, "count", lookup->count);
  struct json_value rows;
  json_init(&rows);
  json_set_array(&rows);
  for (uint32_t i = 0; i < lookup->count; i++) {
    char node_id[65];
    zcl_hex_encode(lookup->node_ids[i], 32, node_id);
    struct json_value row;
    json_init(&row);
    json_set_str(&row, node_id);
    json_push_back(&rows, &row);
    json_free(&row);
  }
  json_push_kv(result, "node_ids", &rows);
  json_free(&rows);
}

static bool rpc_zcode_dht_find(const struct json_value *params, bool help,
                               struct json_value *result) {
  if (help) {
    json_set_str(result, "zcode_dht_find {\"node_id\":\"<64 lowercase hex>\"}");
    return true;
  }
  const struct json_value *in = dht_rpc_input(params);
  const struct json_value *node = in ? json_get(in, "node_id") : NULL;
  const char *hex = node && node->type == JSON_STR ? json_get_str(node) : NULL;
  uint8_t target[32];
  if (!hex || strlen(hex) != 64 || !zcl_hex_decode_lower(hex, target, 32)) {
    dht_rpc_error(result, "INVALID_NODE_ID",
                  "node_id must be 64 canonical lowercase hex chars");
    return true;
  }
  struct vcs_zcode_dht_time started = dht_now();
  uint64_t lookup_id = 0;
  int64_t monotonic_deadline = platform_time_monotonic_ms() +
                               (int64_t)VCS_ZCODE_DHT_LOOKUP_CEILING_S * 1000;
  dht_lock();
  bool began = g_dht && vcs_zcode_dht_service_lookup_begin(g_dht, target,
                                                           started, &lookup_id);
  zcl_mutex_unlock(&g_dht_lock);
  if (!began) {
    dht_rpc_error(result, "LOOKUP_UNAVAILABLE",
                  "DHT is disabled or its bounded lookup queue is full");
    return true;
  }
  struct vcs_zcode_dht_lookup_result lookup;
  for (;;) {
    struct vcs_zcode_dht_time now = dht_now();
    if (platform_time_monotonic_ms() >= monotonic_deadline)
      now.monotonic_s =
          started.monotonic_s + VCS_ZCODE_DHT_LOOKUP_CEILING_S;
    dht_lock();
    bool found = g_dht && vcs_zcode_dht_service_lookup_poll(g_dht, lookup_id,
                                                            now, &lookup);
    zcl_mutex_unlock(&g_dht_lock);
    if (!found) {
      dht_rpc_error(result, "LOOKUP_INTERRUPTED",
                    "DHT service restarted during the lookup");
      return true;
    }
    if (lookup.state != VCS_ZCODE_DHT_LOOKUP_PENDING) {
      dht_lookup_json(result, &lookup);
      return true;
    }
    platform_sleep_ms(50);
  }
}

void boot_zcode_dht_register_rpc(struct rpc_table *table) {
  const struct rpc_command commands[] = {
      {"zcode", "zcode_dht_status", rpc_zcode_dht_status, true},
      {"zcode", "zcode_dht_peers", rpc_zcode_dht_peers, true},
      {"zcode", "zcode_dht_find", rpc_zcode_dht_find, true},
  };
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    rpc_table_must_append(table, &commands[i]);
}

void boot_zcode_dht_shutdown(void) {
  dht_lock();
  if (g_dht)
    vcs_zcode_dht_service_free(g_dht, dht_now());
  g_dht = NULL;
  g_last_create_attempt_mono = 0;
  zcl_mutex_unlock(&g_dht_lock);
}
