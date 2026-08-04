/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Composition-root adapter between Noise peers and ZCODE DHT. */

#include "config/boot_zcode_dht.h"

#include "base/hex.h"
#include "config/boot_internal.h"
#include "config/boot_zcode_dht_chain.h"
#include "config/boot_zcode_dht_reachability.h"
#include "base/safe_alloc.h"
#include "net/connman.h"
#include "net/net.h"
#include "net/peer_scoring.h"
#include "net/v2_transport.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/sync.h"
#include "validation/chainstate.h"
#include "vcs/zcode_dht_service.h"
#include "json/json.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define DHT_FRAME_PREFIX "ZCDHTM"
#define DHT_RETRY_SECONDS 5

static zcl_mutex_t g_dht_lock;
static _Atomic int g_dht_lock_state;
static struct vcs_zcode_dht_service *g_dht;
static struct boot_svc_ctx *g_dht_svc;
static uint64_t g_dht_generation;
static uint64_t g_last_create_attempt_mono;
static bool g_create_in_progress;
static uint8_t g_dht_genesis[32];
static size_t g_cold_contact_cursor;

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
  json_push_kv_int(out, "max_record_operations",
                   VCS_ZCODE_DHT_SERVICE_MAX_RECORD_OPERATIONS);
  json_push_kv_int(out, "max_records_per_peer",
                   VCS_ZCODE_DHT_SERVICE_MAX_RECORDS_PER_PEER);
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
  json_push_kv_int(out, "find_record_received",
                   (int64_t)status.find_record_received);
  json_push_kv_int(out, "records_received",
                   (int64_t)status.records_received);
  json_push_kv_int(out, "store_record_received",
                   (int64_t)status.store_record_received);
  json_push_kv_int(out, "store_result_received",
                   (int64_t)status.store_result_received);
  json_push_kv_int(out, "find_record_sent",
                   (int64_t)status.find_record_sent);
  json_push_kv_int(out, "records_sent", (int64_t)status.records_sent);
  json_push_kv_int(out, "store_record_sent",
                   (int64_t)status.store_record_sent);
  json_push_kv_int(out, "store_result_sent",
                   (int64_t)status.store_result_sent);
  json_push_kv_int(out, "signed_records", status.signed_records);
  json_push_kv_int(out, "active_record_operations",
                   status.active_record_operations);
  json_push_kv_int(out, "unauthenticated_expired",
                   (int64_t)status.unauthenticated_expired);
  json_push_kv_int(out, "duplicate_sessions_retired",
                   (int64_t)status.duplicate_sessions_retired);
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
  struct json_value chain;
  json_init(&chain);
  boot_zcode_dht_chain_dump_json(&chain);
  json_push_kv(out, "chain_authorization", &chain);
  json_free(&chain);
  struct json_value reachability;
  json_init(&reachability);
  boot_zcode_dht_reachability_dump_json(&reachability);
  json_push_kv(out, "reachability", &reachability);
  json_free(&reachability);
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

static bool dht_chain_verify_external(
    void *ctx, const struct vcs_zcode_dht_delegation *delegation) {
  return boot_zcode_dht_chain_authorize(ctx, delegation);
}

static struct vcs_zcode_dht_service *dht_create(
    struct msg_processor *mp, struct boot_svc_ctx *svc,
    struct vcs_zcode_dht_time now) {
  if (!mp || !mp->params || !mp->net_mgr || !svc || !svc->datadir)
    return NULL;
  struct vcs_zcode_dht_service_params params = {
      .datadir = svc->datadir,
      .transport_enabled = mp->net_mgr->v2_enabled,
      .now = now,
      .chain_verify = dht_chain_verify_external,
      .chain_ctx = svc,
      .request_reachability = boot_zcode_dht_reachability_request,
      .reachability_ctx = svc,
  };
  memcpy(params.network_genesis, mp->params->consensus.hashGenesisBlock.data,
         32);
  memcpy(params.local_noise_static, mp->net_mgr->identity_pub, 32);
  struct vcs_zcode_dht_service *created =
      vcs_zcode_dht_service_create(&params);
  if (created)
    vcs_zcode_dht_service_set_chain_verify(
        created, boot_zcode_dht_chain_cached, NULL);
  return created;
}

static void dht_ensure_periodic(struct msg_processor *mp,
                                struct boot_svc_ctx *svc,
                                struct vcs_zcode_dht_time now) {
  dht_lock();
  if ((g_dht && vcs_zcode_dht_service_enabled(g_dht)) ||
      g_create_in_progress ||
      (g_dht && now.monotonic_s <
                    g_last_create_attempt_mono + DHT_RETRY_SECONDS)) {
    zcl_mutex_unlock(&g_dht_lock);
    return;
  }
  g_create_in_progress = true;
  g_last_create_attempt_mono = now.monotonic_s;
  uint64_t expected_generation = g_dht_generation;
  zcl_mutex_unlock(&g_dht_lock);

  /* Identity files, contacts.v2, database checks, and ancestry validation all
   * execute here with the global DHT mutex released. */
  struct vcs_zcode_dht_service *created = dht_create(mp, svc, now);
  struct vcs_zcode_dht_service *retired = NULL;
  dht_lock();
  if (created && g_dht_generation == expected_generation) {
    retired = g_dht;
    g_dht = created;
    created = NULL;
    memcpy(g_dht_genesis, mp->params->consensus.hashGenesisBlock.data, 32);
    g_dht_generation++;
    if (!g_dht_generation)
      g_dht_generation++;
  }
  g_create_in_progress = false;
  zcl_mutex_unlock(&g_dht_lock);
  if (retired)
    vcs_zcode_dht_service_free(retired, now);
  if (created)
    vcs_zcode_dht_service_free(created, now);
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
  out->connection_serial = snapshot.connection_serial;
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

static size_t dht_flush_node(struct msg_processor *mp, struct p2p_node *node) {
  uint8_t wire[VCS_ZCODE_DHT_MAX_FRAME_BYTES];
  uint64_t peer = 0;
  size_t wire_len = 0, sent = 0;
  for (;;) {
    dht_lock();
    bool have = g_dht && vcs_zcode_dht_service_next_outbound(
                             g_dht, (uint64_t)node->id + 1, &peer, wire,
                             sizeof(wire), &wire_len);
    zcl_mutex_unlock(&g_dht_lock);
    if (!have || peer != (uint64_t)node->id + 1)
      break;
    /* Socket/message-builder locks are strictly outside g_dht_lock. */
    dht_send(mp, node, wire, wire_len);
    sent++;
  }
  return sent;
}

static void dht_authorize_frame_chain(
    struct boot_svc_ctx *svc, const uint8_t genesis[32],
    const struct vcs_zcode_dht_session *session, const uint8_t *wire,
    size_t wire_len, uint64_t wall_unix) {
  if (!svc || !genesis || !session || !wire)
    return;
  struct vcs_zcode_dht_msg_verify_context verify = {
      .noise_established = session->established,
      .session_generation = session->generation,
      .now_unix = wall_unix,
  };
  memcpy(verify.noise_transcript_hash, session->transcript_hash, 32);
  memcpy(verify.remote_noise_static, session->remote_static, 32);
  memcpy(verify.network_genesis, genesis, 32);
  struct vcs_zcode_dht_msg message;
  if (vcs_zcode_dht_msg_parse(wire, wire_len, &verify, &message) !=
      VCS_ZCODE_DHT_OK)
    return;
  const struct vcs_zcode_dht_delegation *delegation = NULL;
  switch (message.kind) {
  case VCS_ZCODE_DHT_MSG_FIND_NODE:
    delegation = &message.find_node.delegation;
    break;
  case VCS_ZCODE_DHT_MSG_NODES:
    delegation = &message.nodes.delegation;
    break;
  case VCS_ZCODE_DHT_MSG_FIND_RECORD:
    delegation = &message.find_record.delegation;
    break;
  case VCS_ZCODE_DHT_MSG_RECORDS:
    delegation = &message.records.delegation;
    break;
  case VCS_ZCODE_DHT_MSG_STORE_RECORD:
    delegation = &message.store_record.delegation;
    break;
  case VCS_ZCODE_DHT_MSG_STORE_RESULT:
    delegation = &message.store_result.delegation;
    break;
  }
  if (!delegation)
    return;
  (void)boot_zcode_dht_chain_authorize(svc, delegation);
  if (message.kind == VCS_ZCODE_DHT_MSG_RECORDS)
    for (uint32_t i = 0; i < message.records.record_count; i++)
      (void)boot_zcode_dht_chain_authorize(
          svc, &message.records.records[i].delegation);
  else if (message.kind == VCS_ZCODE_DHT_MSG_STORE_RECORD)
    (void)boot_zcode_dht_chain_authorize(
        svc, &message.store_record.record.delegation);
}

static bool dht_chain_prepare(struct boot_svc_ctx *svc) {
  if (boot_zcode_dht_chain_epoch_current())
    return true;
  struct vcs_zcode_dht_delegation *delegations = zcl_malloc(
      VCS_ZCODE_DHT_SERVICE_MAX_CHAIN_DELEGATIONS * sizeof(*delegations),
      "dht.chain.snapshot");
  if (!delegations)
    return false;
  dht_lock();
  uint64_t generation = g_dht_generation;
  size_t count = g_dht ? vcs_zcode_dht_service_delegations(
                             g_dht, delegations,
                             VCS_ZCODE_DHT_SERVICE_MAX_CHAIN_DELEGATIONS)
                       : 0;
  zcl_mutex_unlock(&g_dht_lock);
  for (size_t i = 0; i < count; i++)
    (void)boot_zcode_dht_chain_authorize(svc, &delegations[i]);
  free(delegations);
  dht_lock();
  bool current = generation == g_dht_generation;
  zcl_mutex_unlock(&g_dht_lock);
  return current && boot_zcode_dht_chain_epoch_current();
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
  struct vcs_zcode_dht_session session;
  bool have_session = dht_snapshot(node, &session);
  uint8_t genesis[32] = {0};
  dht_lock();
  struct vcs_zcode_dht_service *dht = g_dht;
  uint64_t generation = g_dht_generation;
  if (dht)
    memcpy(genesis, g_dht_genesis, 32);
  zcl_mutex_unlock(&g_dht_lock);
  if (dht && have_session)
    dht_authorize_frame_chain(svc, genesis, &session, payload, payload_len,
                              now.wall_unix);

  dht_lock();
  dht = generation == g_dht_generation ? g_dht : NULL;
  if (dht && have_session)
    (void)vcs_zcode_dht_service_session_open(dht, (uint64_t)node->id + 1,
                                             &session, now);
  enum vcs_zcode_dht_reject_reason rejected = VCS_ZCODE_DHT_REJECT_MALFORMED;
  bool ok = dht && vcs_zcode_dht_service_handle_frame(
                       dht, (uint64_t)node->id + 1, payload, payload_len, now,
                       &rejected);
  zcl_mutex_unlock(&g_dht_lock);
  if (ok)
    (void)dht_flush_node(mp, node);
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
  dht_lock();
  g_dht_svc = svc;
  zcl_mutex_unlock(&g_dht_lock);
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

  /* Creation parses contacts and validates delegations entirely outside the
   * global mutex. Copy the immutable network key, then build the ZENDP index
   * with the mutex released as well. */
  dht_ensure_periodic(mp, svc, now);
  uint8_t genesis[32] = {0};
  dht_lock();
  struct vcs_zcode_dht_service *dht = g_dht;
  if (dht)
    memcpy(genesis, g_dht_genesis, sizeof(genesis));
  zcl_mutex_unlock(&g_dht_lock);
  if (dht)
    (void)boot_zcode_dht_reachability_refresh(genesis, now);
  if (dht)
    (void)dht_chain_prepare(svc);

  struct vcs_zcode_dht_peer_view cold[VCS_ZCODE_DHT_SERVICE_MAX_PEERS];
  size_t cold_count = 0;
  struct vcs_zcode_dht_persistence_snapshot *save_snapshot = NULL;
  uint64_t save_generation = 0;
  struct vcs_zcode_dht_service *save_service = NULL;
  struct vcs_zcode_dht_session sessions[VCS_ZCODE_DHT_SERVICE_MAX_PEERS];
  bool session_ready[VCS_ZCODE_DHT_SERVICE_MAX_PEERS] = {0};
  for (size_t i = 0; i < count; i++)
    session_ready[i] = dht_snapshot(nodes[i], &sessions[i]);
  dht_lock();
  dht = g_dht;
  struct vcs_zcode_dht_live_session live[VCS_ZCODE_DHT_SERVICE_MAX_PEERS];
  size_t live_count = 0;
  for (size_t i = 0; dht && i < count; i++) {
    if (!session_ready[i])
      continue;
    uint64_t peer = (uint64_t)nodes[i]->id + 1;
    if (vcs_zcode_dht_service_session_open(dht, peer, &sessions[i], now)) {
      live[live_count].peer_id = peer;
      live[live_count].generation = sessions[i].generation;
      live[live_count++].connection_serial = sessions[i].connection_serial;
    }
  }
  if (dht) {
    vcs_zcode_dht_service_sessions_reconcile(dht, live, live_count, now);
    vcs_zcode_dht_service_tick(dht, now);
    save_snapshot = vcs_zcode_dht_service_persistence_snapshot(
        dht, now.monotonic_s, false);
    if (save_snapshot) {
      save_generation = g_dht_generation;
      save_service = dht;
    }
    cold_count = vcs_zcode_dht_service_peers(
        dht, now.wall_unix, cold, VCS_ZCODE_DHT_SERVICE_MAX_PEERS,
        g_cold_contact_cursor);
    if (cold_count == VCS_ZCODE_DHT_SERVICE_MAX_PEERS)
      g_cold_contact_cursor += cold_count;
    else
      g_cold_contact_cursor = 0;
  }
  zcl_mutex_unlock(&g_dht_lock);
  if (save_snapshot) {
    bool written =
        vcs_zcode_dht_persistence_snapshot_write(save_snapshot);
    dht_lock();
    if (g_dht == save_service && g_dht_generation == save_generation)
      vcs_zcode_dht_service_persistence_commit(
          g_dht, save_snapshot, written);
    zcl_mutex_unlock(&g_dht_lock);
    vcs_zcode_dht_persistence_snapshot_free(save_snapshot);
  }
  for (size_t i = 0; i < count; i++)
    (void)dht_flush_node(mp, nodes[i]);
  boot_zcode_dht_reachability_drive(svc, cold, cold_count, now);
  boot_zcode_dht_public_tick(now.monotonic_s);
  for (size_t i = 0; i < count; i++)
    p2p_node_release(nodes[i]);
}

bool boot_zcode_dht_revalidate(void) {
  struct vcs_zcode_dht_time now = dht_now();
  /* Revalidation's node.db and ancestry reads populate the fixed cache before
   * the service lock is acquired. The service callback below is cache-only. */
  dht_lock();
  struct boot_svc_ctx *svc = g_dht_svc;
  zcl_mutex_unlock(&g_dht_lock);
  if (svc)
    (void)dht_chain_prepare(svc);
  dht_lock();
  bool ok = !g_dht || vcs_zcode_dht_service_revalidate(g_dht, now);
  zcl_mutex_unlock(&g_dht_lock);
  return ok;
}

bool boot_zcode_dht_lookup_begin(
    const uint8_t target[32], struct vcs_zcode_dht_time now,
    uint64_t *lookup_id, uint64_t *generation) {
  if (!target || !lookup_id || !generation)
    return false;
  dht_lock();
  bool ok = g_dht && vcs_zcode_dht_service_lookup_begin(
                         g_dht, target, now, lookup_id);
  if (ok)
    *generation = g_dht_generation;
  zcl_mutex_unlock(&g_dht_lock);
  return ok;
}

bool boot_zcode_dht_lookup_poll(
    uint64_t lookup_id, uint64_t generation, struct vcs_zcode_dht_time now,
    struct vcs_zcode_dht_lookup_result *out) {
  if (!lookup_id || !generation || !out)
    return false;
  dht_lock();
  bool ok = g_dht && generation == g_dht_generation &&
            vcs_zcode_dht_service_lookup_poll(g_dht, lookup_id, now, out);
  zcl_mutex_unlock(&g_dht_lock);
  return ok;
}

bool boot_zcode_dht_lookup_cancel(uint64_t lookup_id, uint64_t generation) {
  if (!lookup_id || !generation)
    return false;
  dht_lock();
  bool ok = g_dht && generation == g_dht_generation &&
            vcs_zcode_dht_service_lookup_cancel(g_dht, lookup_id);
  zcl_mutex_unlock(&g_dht_lock);
  return ok;
}

bool boot_zcode_dht_peers(uint64_t wall_now,
                          struct vcs_zcode_dht_peer_view *out, size_t max,
                          size_t offset, size_t *count_out) {
  if (!out || !max || !count_out)
    return false;
  dht_lock();
  bool ok = g_dht && vcs_zcode_dht_service_enabled(g_dht);
  *count_out = ok ? vcs_zcode_dht_service_peers(
                        g_dht, wall_now, out, max, offset)
                  : 0;
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

void boot_zcode_dht_shutdown(void) {
  boot_zcode_dht_public_reset();
  dht_lock();
  g_dht_generation++;
  if (!g_dht_generation)
    g_dht_generation++;
  struct vcs_zcode_dht_service *retired = g_dht;
  g_dht = NULL;
  g_dht_svc = NULL;
  g_last_create_attempt_mono = 0;
  g_cold_contact_cursor = 0;
  zcl_mutex_unlock(&g_dht_lock);
  if (retired)
    vcs_zcode_dht_service_free(retired, dht_now());
  boot_zcode_dht_reachability_reset();
  boot_zcode_dht_chain_reset();
}
