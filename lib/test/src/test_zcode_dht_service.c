/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Two-node protocol tests for the bounded ZCODE DHT service. */

#include "test/test_core.h"

#include "base/hex.h"
#include "base/safe_alloc.h"
#include "chain/chain.h"
#include "config/boot_zcode_dht.h"
#include "crypto/ed25519.h"
#include "json/json.h"
#include "net/net.h"
#include "support/cleanse.h"
#include "vcs/blob_store.h"
#include "vcs/package_manifest.h"
#include "vcs/package_store.h"
#include "vcs/zcode_dht_identity.h"
#include "vcs/zcode_dht_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct vcs_zcode_dht_time test_time(uint64_t wall) {
  return (struct vcs_zcode_dht_time){.wall_unix = wall, .monotonic_s = wall};
}

static bool chain_ok(void *ctx, const struct vcs_zcode_dht_delegation *d) {
  (void)ctx;
  return d && d->beacon_height == 120;
}

static uint64_t policy_calls[VCS_ZCODE_SOVEREIGNTY_ACTION_COUNT];

static bool policy_allow(void *ctx, enum vcs_zcode_sovereignty_action action,
                         const struct vcs_zcode_sovereignty_subject *subject) {
  (void)ctx;
  if (action < VCS_ZCODE_SOVEREIGNTY_ACTION_COUNT)
    policy_calls[action]++;
  return action < VCS_ZCODE_SOVEREIGNTY_ACTION_COUNT && subject != NULL;
}

static bool fixture_identity(const char *dir, uint8_t byte,
                             const uint8_t genesis[32],
                             const uint8_t noise[32]) {
  uint8_t online_seed[32], online_pub[32], master[32], beacon[32];
  char err[160];
  memset(master, byte, 32);
  memset(beacon, 0x44, 32);
  if (!vcs_zcode_dht_online_key_load_or_create(dir, online_seed, online_pub,
                                               err, sizeof(err)))
    return false;
  struct vcs_zcode_dht_delegation d;
  bool ok = vcs_zcode_dht_delegation_sign(&d, genesis, online_pub, noise, 120,
                                          beacon, 1000, 90000, 1, master) ==
                VCS_ZCODE_DHT_DELEGATION_OK &&
            vcs_zcode_dht_delegation_save(dir, &d, err, sizeof(err));
  memset(online_seed, 0, sizeof(online_seed));
  memset(master, 0, sizeof(master));
  return ok;
}

static struct vcs_zcode_dht_service *fixture_service(const char *dir,
                                                     const uint8_t genesis[32],
                                                     const uint8_t noise[32]) {
  struct vcs_zcode_dht_service_params p = {
      .datadir = dir,
      .transport_enabled = true,
      .now = {.wall_unix = 1000, .monotonic_s = 1000},
      .chain_verify = chain_ok,
      .policy_decide = policy_allow,
  };
  memcpy(p.network_genesis, genesis, 32);
  memcpy(p.local_noise_static, noise, 32);
  return vcs_zcode_dht_service_create(&p);
}

static bool fixture_material(const char *dir,
                             struct vcs_zcode_dht_delegation *delegation,
                             uint8_t online_seed[32], uint8_t node_id[32]) {
  uint8_t online_pub[32];
  char err[160];
  return vcs_zcode_dht_delegation_load(dir, delegation, err, sizeof(err)) &&
         vcs_zcode_dht_online_key_load(dir, online_seed, online_pub, err,
                                       sizeof(err)) &&
         memcmp(online_pub, delegation->online_pubkey, 32) == 0 &&
         vcs_zcode_dht_delegation_node_id(node_id, delegation);
}

static bool signed_find(const char *dir, uint64_t generation,
                        const uint8_t transcript[32], uint8_t query_byte,
                        uint8_t target_byte, uint8_t *wire, size_t cap,
                        size_t *len) {
  uint8_t seed[32], node_id[32];
  struct vcs_zcode_dht_msg_find_node msg;
  memset(&msg, 0, sizeof(msg));
  if (!fixture_material(dir, &msg.delegation, seed, node_id))
    return false;
  msg.session_generation = generation;
  memcpy(msg.sender_node_id, node_id, 32);
  memset(msg.query_id, query_byte, sizeof(msg.query_id));
  memset(msg.target_node_id, target_byte, sizeof(msg.target_node_id));
  enum vcs_zcode_dht_error e = vcs_zcode_dht_msg_serialize_find_node(
      &msg, transcript, seed, wire, cap, len);
  memory_cleanse(seed, sizeof(seed));
  return e == VCS_ZCODE_DHT_OK;
}

static bool signed_nodes(const char *dir, uint64_t generation,
                         const uint8_t transcript[32], uint8_t query_byte,
                         uint8_t *wire, size_t cap, size_t *len) {
  uint8_t seed[32], node_id[32];
  struct vcs_zcode_dht_msg_nodes msg;
  memset(&msg, 0, sizeof(msg));
  if (!fixture_material(dir, &msg.delegation, seed, node_id))
    return false;
  msg.session_generation = generation;
  memcpy(msg.sender_node_id, node_id, 32);
  memset(msg.query_id, query_byte, sizeof(msg.query_id));
  msg.contact_count = 1;
  memcpy(msg.node_ids[0], node_id, 32);
  enum vcs_zcode_dht_error e =
      vcs_zcode_dht_msg_serialize_nodes(&msg, transcript, seed, wire, cap, len);
  memory_cleanse(seed, sizeof(seed));
  return e == VCS_ZCODE_DHT_OK;
}

static bool resign_wire(const char *dir, const uint8_t transcript[32],
                        uint8_t *wire, size_t len) {
  if (len < VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES)
    return false;
  uint8_t seed[32], pub[32], secret[32], node_id[32];
  struct vcs_zcode_dht_delegation delegation;
  if (!fixture_material(dir, &delegation, seed, node_id))
    return false;
  ed25519_keypair(pub, secret, seed);
  uint8_t preimage[sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN) + 32 +
                   VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
  size_t unsigned_len = len - VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES, off = 0;
  memcpy(preimage + off, VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN,
         sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN));
  off += sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN);
  memcpy(preimage + off, transcript, 32);
  off += 32;
  memcpy(preimage + off, wire, unsigned_len);
  off += unsigned_len;
  ed25519_sign(wire + unsigned_len, preimage, off, secret, pub);
  memory_cleanse(seed, sizeof(seed));
  memory_cleanse(secret, sizeof(secret));
  memory_cleanse(preimage, off);
  return true;
}

static size_t drain(struct vcs_zcode_dht_service *s) {
  uint8_t wire[VCS_ZCODE_DHT_MAX_FRAME_BYTES];
  uint64_t peer;
  size_t len, count = 0;
  while (vcs_zcode_dht_service_next_outbound(s, 0, &peer, wire, sizeof(wire),
                                             &len))
    count++;
  return count;
}

static bool pump(struct vcs_zcode_dht_service *from,
                 struct vcs_zcode_dht_service *to, uint64_t from_peer,
                 uint64_t to_peer, uint64_t now, uint8_t *last,
                 size_t *last_len) {
  uint8_t wire[VCS_ZCODE_DHT_MAX_FRAME_BYTES];
  uint64_t peer = 0;
  size_t len = 0;
  bool moved = false;
  while (vcs_zcode_dht_service_next_outbound(from, 0, &peer, wire, sizeof(wire),
                                             &len)) {
    if (peer != from_peer) {
      printf("pump peer mismatch got=%llu want=%llu\n",
             (unsigned long long)peer, (unsigned long long)from_peer);
      return false;
    }
    if (last && last_len) {
      memcpy(last, wire, len);
      *last_len = len;
    }
    enum vcs_zcode_dht_reject_reason reason;
    if (!vcs_zcode_dht_service_handle_frame(to, to_peer, wire, len,
                                            test_time(now),
                                            &reason)) {
      printf("pump rejected peer=%llu len=%zu reason=%s\n",
             (unsigned long long)to_peer, len,
             vcs_zcode_dht_reject_reason_string(reason));
      return false;
    }
    moved = true;
  }
  return moved;
}

static void cleanup_fixture(const char *dir) {
  char path[512];
  snprintf(path, sizeof(path), "%s/zcode/dht/records.v1", dir);
  (void)unlink(path);
  snprintf(path, sizeof(path), "%s/zcode/dht/publications.v1", dir);
  (void)unlink(path);
  snprintf(path, sizeof(path), "%s/zcode/dht/contacts.v2", dir);
  (void)unlink(path);
  snprintf(path, sizeof(path), "%s/zcode/dht/online_ed25519.key", dir);
  (void)unlink(path);
  snprintf(path, sizeof(path), "%s/zcode/dht/delegation.v1", dir);
  (void)unlink(path);
  snprintf(path, sizeof(path), "%s/zcode/dht", dir);
  (void)rmdir(path);
  snprintf(path, sizeof(path), "%s/zcode", dir);
  (void)rmdir(path);
  (void)rmdir(dir);
}

static bool fixture_pointer_record(
    const char *dir, const uint8_t genesis[32], uint8_t semantic_byte,
    uint8_t transport_byte, struct vcs_zcode_dht_record *record) {
  uint8_t seed[32], node_id[32];
  memset(record, 0, sizeof(*record));
  if (!fixture_material(dir, &record->delegation, seed, node_id))
    return false;
  record->kind = VCS_ZCODE_DHT_RECORD_POINTER;
  (void)snprintf(record->namespace_name, sizeof(record->namespace_name),
                 "science.study");
  memcpy(record->network_genesis, genesis, 32);
  memset(record->semantic_root, semantic_byte, 32);
  memset(record->transport_root, transport_byte, 32);
  memcpy(record->provider_node_id, node_id, 32);
  record->sequence = 1;
  record->not_before = 1000;
  record->expiry = 4000;
  enum vcs_zcode_dht_record_error result =
      vcs_zcode_dht_record_sign(record, seed);
  memory_cleanse(seed, sizeof(seed));
  return result == VCS_ZCODE_DHT_RECORD_OK;
}

static bool fixture_provider_record(
    const char *dir, const uint8_t genesis[32], uint8_t transport_byte,
    struct vcs_zcode_dht_record *record) {
  uint8_t seed[32], node_id[32];
  memset(record, 0, sizeof(*record));
  if (!fixture_material(dir, &record->delegation, seed, node_id))
    return false;
  record->kind = VCS_ZCODE_DHT_RECORD_PROVIDER;
  (void)snprintf(record->namespace_name, sizeof(record->namespace_name),
                 "science");
  memcpy(record->network_genesis, genesis, 32);
  memset(record->transport_root, transport_byte, 32);
  memcpy(record->provider_node_id, node_id, 32);
  record->sequence = 1;
  record->not_before = 1000;
  record->expiry = 4000;
  enum vcs_zcode_dht_record_error result =
      vcs_zcode_dht_record_sign(record, seed);
  memory_cleanse(seed, sizeof(seed));
  return result == VCS_ZCODE_DHT_RECORD_OK;
}

#define MULTI_NODES 12u

struct multi_network;
struct multi_reach_ctx {
  struct multi_network *network;
  size_t owner;
};

struct multi_network {
  struct vcs_zcode_dht_service *service[MULTI_NODES];
  char dir[MULTI_NODES][80];
  uint8_t noise[MULTI_NODES][32];
  uint8_t node_id[MULTI_NODES][32];
  struct multi_reach_ctx reach[MULTI_NODES];
  bool connected[MULTI_NODES][MULTI_NODES];
  bool pending[MULTI_NODES][MULTI_NODES];
  bool deny[MULTI_NODES][MULTI_NODES];
  bool stall[MULTI_NODES][MULTI_NODES];
  bool banned[MULTI_NODES];
  uint8_t banned_root[32];
  uint64_t generation, frames, denied_hints;
  struct vcs_zcode_dht_time now;
};

static bool multi_policy(void *ctx, enum vcs_zcode_sovereignty_action action,
                         const struct vcs_zcode_sovereignty_subject *subject) {
  struct multi_reach_ctx *reach = ctx;
  if (!reach || !reach->network || !subject)
    return false;
  if (reach->network->banned[reach->owner] &&
      (memcmp(subject->semantic_root, reach->network->banned_root, 32) == 0 ||
       memcmp(subject->transport_root, reach->network->banned_root, 32) == 0) &&
      (action == VCS_ZCODE_SOVEREIGNTY_STORE ||
       action == VCS_ZCODE_SOVEREIGNTY_SERVE ||
       action == VCS_ZCODE_SOVEREIGNTY_FORWARD))
    return false;
  return true;
}

static bool multi_request_reachability(void *ctx, const uint8_t id[32],
                                       uint64_t wall_now) {
  (void)wall_now;
  struct multi_reach_ctx *reach = ctx;
  if (!reach || !reach->network)
    return false;
  struct multi_network *net = reach->network;
  for (size_t i = 0; i < MULTI_NODES; i++) {
    if (memcmp(net->node_id[i], id, 32) != 0)
      continue;
    if (net->deny[reach->owner][i]) {
      net->denied_hints++;
      return false;
    }
    if (net->stall[reach->owner][i])
      return true;
    if (!net->connected[reach->owner][i])
      net->pending[reach->owner][i] = true;
    return true;
  }
  net->denied_hints++;
  return false;
}

static struct vcs_zcode_dht_service *multi_service(
    struct multi_network *net, size_t index, const uint8_t genesis[32]) {
  struct vcs_zcode_dht_service_params p = {
      .datadir = net->dir[index],
      .transport_enabled = true,
      .now = net->now,
      .chain_verify = chain_ok,
      .request_reachability = multi_request_reachability,
      .reachability_ctx = &net->reach[index],
      .policy_decide = multi_policy,
      .policy_ctx = &net->reach[index],
  };
  memcpy(p.network_genesis, genesis, 32);
  memcpy(p.local_noise_static, net->noise[index], 32);
  return vcs_zcode_dht_service_create(&p);
}

static bool multi_connect(struct multi_network *net, size_t a, size_t b) {
  if (!net || a >= MULTI_NODES || b >= MULTI_NODES || a == b)
    return false;
  if (net->connected[a][b])
    return true;
  uint64_t generation = ++net->generation;
  uint8_t transcript[32];
  memset(transcript, (int)(0x60u + (a < b ? a * MULTI_NODES + b
                                             : b * MULTI_NODES + a)),
         sizeof(transcript));
  struct vcs_zcode_dht_session as = {.established = true,
                                     .generation = generation,
                                     .connection_serial = generation * 2};
  struct vcs_zcode_dht_session bs = as;
  bs.connection_serial = generation * 2 + 1;
  memcpy(as.remote_static, net->noise[b], 32);
  memcpy(bs.remote_static, net->noise[a], 32);
  memcpy(as.transcript_hash, transcript, 32);
  memcpy(bs.transcript_hash, transcript, 32);
  if (!vcs_zcode_dht_service_session_open(net->service[a], b + 1, &as,
                                          net->now) ||
      !vcs_zcode_dht_service_session_open(net->service[b], a + 1, &bs,
                                          net->now))
    return false;
  net->connected[a][b] = net->connected[b][a] = true;
  return true;
}

static bool multi_drive(struct multi_network *net) {
  for (size_t turn = 0; turn < 512; turn++) {
    bool moved = false;
    for (size_t from = 0; from < MULTI_NODES; from++) {
      uint8_t wire[VCS_ZCODE_DHT_MAX_FRAME_BYTES];
      uint64_t peer = 0;
      size_t len = 0;
      while (vcs_zcode_dht_service_next_outbound(
          net->service[from], 0, &peer, wire, sizeof(wire), &len)) {
        if (peer == 0 || peer > MULTI_NODES)
          return false;
        size_t to = (size_t)peer - 1;
        if (!net->connected[from][to])
          return false;
        enum vcs_zcode_dht_reject_reason rejected;
        if (!vcs_zcode_dht_service_handle_frame(
                net->service[to], from + 1, wire, len, net->now, &rejected)) {
          printf("multi frame %zu->%zu rejected: %s\n", from, to,
                 vcs_zcode_dht_reject_reason_string(rejected));
          return false;
        }
        net->frames++;
        moved = true;
      }
    }
    for (size_t a = 0; a < MULTI_NODES; a++)
      for (size_t b = a + 1; b < MULTI_NODES; b++)
        if (net->pending[a][b] || net->pending[b][a]) {
          net->pending[a][b] = net->pending[b][a] = false;
          if (!multi_connect(net, a, b))
            return false;
          moved = true;
        }
    if (!moved)
      return true;
  }
  return false;
}

static bool farther_node(const uint8_t a[32], const uint8_t b[32],
                         const uint8_t target[32]) {
  uint8_t ad[32], bd[32];
  vcs_zcode_dht_xor_distance(a, target, ad);
  vcs_zcode_dht_xor_distance(b, target, bd);
  int cmp = memcmp(ad, bd, 32);
  return cmp > 0 || (cmp == 0 && memcmp(a, b, 32) > 0);
}

static int test_disabled_diagnostics(void) {
  int failures = 0;
  TEST("zcode dht service: disabled reasons and diagnostics expose no identity material") {
    char dir[] = "/tmp/zcl_dht_service_disabled_XXXXXX";
    ASSERT(mkdtemp(dir) != NULL);
    struct vcs_zcode_dht_service_params params = {
        .datadir = dir,
        .transport_enabled = false,
        .now = {.wall_unix = 1000, .monotonic_s = 1000},
    };
    memset(params.network_genesis, 0x11, 32);
    memset(params.local_noise_static, 0x22, 32);
    struct vcs_zcode_dht_service *disabled =
        vcs_zcode_dht_service_create(&params);
    struct vcs_zcode_dht_service_status status;
    ASSERT(disabled != NULL);
    vcs_zcode_dht_service_status(disabled, &status);
    ASSERT(!status.enabled);
    ASSERT_STR_EQ(status.disabled_reason, "V2_TRANSPORT_DISABLED");
    vcs_zcode_dht_service_free(disabled, test_time(1000));
    params.transport_enabled = true;
    disabled = vcs_zcode_dht_service_create(&params);
    ASSERT(disabled != NULL);
    vcs_zcode_dht_service_status(disabled, &status);
    ASSERT(!status.enabled);
    ASSERT_STR_EQ(status.disabled_reason, "IDENTITY_MATERIAL_UNAVAILABLE");
    vcs_zcode_dht_service_free(disabled, test_time(1000));

    struct json_value dump;
    json_init(&dump);
    ASSERT(boot_zcode_dht_dump_state_json(&dump, NULL));
    char rendered[8192];
    size_t rendered_len = json_write(&dump, rendered, sizeof(rendered));
    ASSERT(rendered_len < sizeof(rendered));
    ASSERT(strstr(rendered, "delegation_wire") == NULL);
    ASSERT(strstr(rendered, "master_pubkey") == NULL);
    ASSERT(strstr(rendered, "online_pubkey") == NULL);
    ASSERT(strstr(rendered, "noise_static") == NULL);
    ASSERT(strstr(rendered, "peer_address") == NULL);
    ASSERT_EQ(json_get_int(json_get(&dump, "max_authenticated_peers")), 64);
    ASSERT_EQ(json_get_int(json_get(&dump, "max_active_queries")), 3);
    ASSERT_EQ(json_get_int(json_get(&dump, "buckets_used")), 0);
    ASSERT(!boot_zcode_dht_dump_state_json(&dump, "private"));
    json_free(&dump);
    cleanup_fixture(dir);
    PASS();
  }
_test_next:;
  return failures;
}

static int test_deep_ancestry(void) {
  int failures = 0;
  TEST("zcode dht service: delayed beacon uses logarithmic deep ancestry") {
    struct block_index beacon, tip;
    struct uint256 hash;
    memset(&beacon, 0, sizeof(beacon));
    memset(&tip, 0, sizeof(tip));
    memset(&hash, 0x6d, sizeof(hash));
    beacon.nHeight = 0;
    beacon.phashBlock = &hash;
    tip.nHeight = 524288;
    tip.pskip = &beacon;
    uint64_t span = 0;
    ASSERT(boot_zcode_dht_beacon_matches(&tip, 0, hash.data, &span));
    ASSERT_EQ(span, 524288);
    uint8_t wrong[32];
    memcpy(wrong, hash.data, 32);
    wrong[0] ^= 1;
    ASSERT(!boot_zcode_dht_beacon_matches(&tip, 0, wrong, &span));
    PASS();
  }
_test_next:;
  return failures;
}

static int test_peer_admission_order(void) {
  int failures = 0;
  TEST("zcode dht service: Noise bootstrap waits for version and verack") {
    struct p2p_node node;
    memset(&node, 0, sizeof(node));
    node.transport = (struct v2_transport *)(uintptr_t)1;
    atomic_store(&node.state, PEER_VERSION_RECEIVED);
    ASSERT(!boot_zcode_dht_peer_ready(&node));
    atomic_store(&node.state, PEER_HANDSHAKE_COMPLETE);
    ASSERT(boot_zcode_dht_peer_ready(&node));
    atomic_store(&node.disconnect, true);
    ASSERT(!boot_zcode_dht_peer_ready(&node));
    PASS();
  }
_test_next:;
  return failures;
}

static int test_record_transport_and_restart(void) {
  int failures = 0;
  TEST("zcode dht service: signed records share Noise, bounds and restart") {
    memset(policy_calls, 0, sizeof(policy_calls));
    char adir[] = "/tmp/zcl_dht_records_a_XXXXXX";
    char bdir[] = "/tmp/zcl_dht_records_b_XXXXXX";
    ASSERT(mkdtemp(adir) != NULL && mkdtemp(bdir) != NULL);
    uint8_t genesis[32], anoise[32], bnoise[32], transcript[32];
    memset(genesis, 0x11, 32);
    memset(anoise, 0x22, 32);
    memset(bnoise, 0x33, 32);
    memset(transcript, 0x55, 32);
    ASSERT(fixture_identity(adir, 0x61, genesis, anoise));
    ASSERT(fixture_identity(bdir, 0x62, genesis, bnoise));
    struct vcs_zcode_dht_service *a = fixture_service(adir, genesis, anoise);
    struct vcs_zcode_dht_service *b = fixture_service(bdir, genesis, bnoise);
    ASSERT(a != NULL && b != NULL);
    struct vcs_zcode_dht_session as = {.established = true,
                                       .generation = 42,
                                       .connection_serial = 1};
    struct vcs_zcode_dht_session bs = as;
    bs.connection_serial = 2;
    memcpy(as.remote_static, bnoise, 32);
    memcpy(bs.remote_static, anoise, 32);
    memcpy(as.transcript_hash, transcript, 32);
    memcpy(bs.transcript_hash, transcript, 32);
    ASSERT(vcs_zcode_dht_service_session_open(a, 2, &as, test_time(1001)));
    ASSERT(vcs_zcode_dht_service_session_open(b, 1, &bs, test_time(1001)));
    ASSERT(pump(a, b, 2, 1, 1001, NULL, NULL));
    ASSERT(pump(b, a, 1, 2, 1001, NULL, NULL));
    ASSERT(pump(a, b, 2, 1, 1001, NULL, NULL));

    struct vcs_zcode_dht_publish_spec publish;
    memset(&publish, 0, sizeof(publish));
    publish.kind = VCS_ZCODE_DHT_RECORD_POINTER;
    snprintf(publish.namespace_name, sizeof(publish.namespace_name),
             "science");
    memset(publish.semantic_root, 0x41, 32);
    memset(publish.transport_root, 0x42, 32);
    publish.sequence = 1;
    publish.not_before = 1000;
    publish.expiry = 2000;
    uint8_t plan_token[32];
    struct vcs_zcode_dht_record published_record;
    ASSERT(vcs_zcode_dht_service_record_publish_plan(
        a, &publish, plan_token, &published_record));
    ASSERT_EQ(vcs_zcode_dht_service_record_publish_commit(
                  a, &publish, plan_token, test_time(1002), &published_record),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    ASSERT_EQ(vcs_zcode_dht_service_record_publish_commit(
                  a, &publish, plan_token, test_time(1002), &published_record),
              VCS_ZCODE_DHT_RECORD_STORE_STALE);
    publish.kind = VCS_ZCODE_DHT_RECORD_STORAGE_ACK;
    ASSERT(!vcs_zcode_dht_service_record_publish_plan(
        a, &publish, plan_token, &published_record));
    publish.kind = VCS_ZCODE_DHT_RECORD_POINTER;

    struct vcs_zcode_dht_record first;
    ASSERT(fixture_pointer_record(adir, genesis, 0x61, 0x71, &first));
    ASSERT_EQ(vcs_zcode_dht_service_record_admit(a, &first, test_time(1002)),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    struct vcs_zcode_dht_record_selector selector = {
        .kind = VCS_ZCODE_DHT_RECORD_POINTER};
    (void)snprintf(selector.namespace_name, sizeof(selector.namespace_name),
                   "science.study");
    memcpy(selector.root, first.semantic_root, 32);
    uint64_t operation = 0;
    ASSERT(vcs_zcode_dht_service_record_query_begin(
        b, 1, &selector, test_time(1002), &operation));
    ASSERT(pump(b, a, 1, 2, 1002, NULL, NULL));
    ASSERT(pump(a, b, 2, 1, 1002, NULL, NULL));
    struct vcs_zcode_dht_record_operation_result result;
    ASSERT(vcs_zcode_dht_service_record_operation_poll(
        b, operation, test_time(1002), &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE);
    ASSERT_EQ(result.record_count, 1);
    ASSERT(memcmp(result.records[0].transport_root, first.transport_root, 32) ==
           0);

    struct vcs_zcode_dht_record second;
    ASSERT(fixture_pointer_record(adir, genesis, 0x62, 0x72, &second));
    ASSERT(vcs_zcode_dht_service_record_store_begin(
        a, 2, &second, test_time(1003), &operation));
    uint8_t replay[VCS_ZCODE_DHT_MAX_FRAME_BYTES];
    size_t replay_len = 0;
    ASSERT(pump(a, b, 2, 1, 1003, replay, &replay_len));
    ASSERT(pump(b, a, 1, 2, 1003, NULL, NULL));
    ASSERT(vcs_zcode_dht_service_record_operation_poll(
        a, operation, test_time(1003), &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE);
    ASSERT_EQ(result.store_status, VCS_ZCODE_DHT_STORE_STORED);
    enum vcs_zcode_dht_reject_reason rejected;
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        b, 1, replay, replay_len, test_time(1003), &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_REPLAY);
    memset(selector.root, 0x62, 32);
    struct vcs_zcode_dht_record local[1];
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  b, 1003, &selector, local, 1),
              1);

    /* Record work cannot escape the three shared authenticated query slots,
     * and all three name their monotonic deadline when no reply arrives. */
    uint64_t pending[VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES];
    memset(selector.root, 0x63, 32);
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
      ASSERT(vcs_zcode_dht_service_record_query_begin(
          b, 1, &selector, test_time(1004), &pending[i]));
    ASSERT(!vcs_zcode_dht_service_record_query_begin(
        b, 1, &selector, test_time(1004), &operation));
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++) {
      ASSERT(vcs_zcode_dht_service_record_operation_poll(
          b, pending[i], test_time(1010), &result));
      ASSERT_EQ(result.state, VCS_ZCODE_DHT_RECORD_OPERATION_TIMEOUT);
    }

    vcs_zcode_dht_service_free(b, test_time(1004));
    b = fixture_service(bdir, genesis, bnoise);
    ASSERT(b != NULL);
    memset(selector.root, 0x62, 32);
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  b, 1004, &selector, local, 1),
              1);
    ASSERT(policy_calls[VCS_ZCODE_SOVEREIGNTY_DISCOVER] > 0);
    ASSERT(policy_calls[VCS_ZCODE_SOVEREIGNTY_STORE] > 0);
    ASSERT(policy_calls[VCS_ZCODE_SOVEREIGNTY_INDEX] > 0);
    ASSERT(policy_calls[VCS_ZCODE_SOVEREIGNTY_SERVE] > 0);
    ASSERT(policy_calls[VCS_ZCODE_SOVEREIGNTY_FORWARD] > 0);

    /* ACK authorship crosses the real package-store possession gate. The
     * intent survives restart without a private key, but losing the pin
     * makes its next renewal fail closed while the last signed ACK simply
     * ages toward expiry. */
    char ack_dir[] = "/tmp/zcl_dht_ack_store_XXXXXX";
    ASSERT(mkdtemp(ack_dir) != NULL);
    struct vcs_package_store *ack_store =
        vcs_package_store_open(ack_dir, UINT64_C(4) * 1024 * 1024);
    ASSERT(ack_store != NULL);
    static const uint8_t ack_bytes[] = "possession-backed-storage-ack";
    uint8_t ack_root[32];
    ASSERT_EQ(vcs_blob_put_to(ack_store, ack_bytes, sizeof(ack_bytes),
                              ack_root),
              VCS_BLOB_OK);
    ASSERT_EQ(vcs_package_store_pin(ack_store, ack_root, true),
              VCS_PACKAGE_STORE_OK);
    struct vcs_zcode_dht_publish_spec ack_spec;
    memset(&ack_spec, 0, sizeof(ack_spec));
    ack_spec.kind = VCS_ZCODE_DHT_RECORD_STORAGE_ACK;
    (void)snprintf(ack_spec.namespace_name,
                   sizeof(ack_spec.namespace_name), "science");
    memcpy(ack_spec.transport_root, ack_root, 32);
    memset(ack_spec.owner_group, 0xa7, 32);
    ack_spec.sequence = 1;
    ack_spec.not_before = 1000;
    ack_spec.expiry = 2000;
    uint8_t ack_token[32];
    struct vcs_zcode_dht_record ack_record;
    ASSERT(!vcs_zcode_dht_service_record_publish_plan(
        a, &ack_spec, ack_token, &ack_record));
    ASSERT(vcs_zcode_dht_service_storage_ack_plan(
        a, ack_store, &ack_spec, ack_token, &ack_record));
    ASSERT_EQ(vcs_zcode_dht_service_storage_ack_commit(
                  a, ack_store, &ack_spec, ack_token, test_time(1004),
                  &ack_record),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    uint8_t ack_chunk_hash[32];
    char ack_chunk_hex[65], ack_chunk_path[512];
    ASSERT(vcs_package_chunk_hash(ack_bytes, sizeof(ack_bytes),
                                  ack_chunk_hash));
    zcl_hex_encode(ack_chunk_hash, sizeof(ack_chunk_hash), ack_chunk_hex);
    int ack_path_len = snprintf(
        ack_chunk_path, sizeof(ack_chunk_path), "%s/zcode/cas/sha3/%02x/%s",
        ack_dir, ack_chunk_hash[0], ack_chunk_hex);
    ASSERT(ack_path_len > 0 && (size_t)ack_path_len < sizeof(ack_chunk_path));
    ASSERT(unlink(ack_chunk_path) == 0);
    ASSERT(!vcs_package_store_verify_possession(ack_store, ack_root, true));
    vcs_zcode_dht_service_storage_ack_validation(a, ack_root, false);

    vcs_zcode_dht_service_free(a, test_time(1004));
    a = fixture_service(adir, genesis, anoise);
    ASSERT(a != NULL);
    struct vcs_zcode_dht_service_status publication_status;
    vcs_zcode_dht_service_status(a, &publication_status);
    ASSERT_EQ(publication_status.publication_intents, 2);
    struct vcs_zcode_dht_record_selector published_selector = {
        .kind = VCS_ZCODE_DHT_RECORD_POINTER};
    snprintf(published_selector.namespace_name,
             sizeof(published_selector.namespace_name), "science");
    memset(published_selector.root, 0x41, 32);
    vcs_zcode_dht_service_tick(a, test_time(1800));
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  a, 1800, &published_selector, local, 1),
              1);
    ASSERT_EQ(local[0].sequence, 2);
    struct vcs_zcode_dht_record_selector ack_selector = {
        .kind = VCS_ZCODE_DHT_RECORD_STORAGE_ACK};
    (void)snprintf(ack_selector.namespace_name,
                   sizeof(ack_selector.namespace_name), "science");
    memcpy(ack_selector.root, ack_root, 32);
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  a, 1800, &ack_selector, local, 1),
              1);
    ASSERT_EQ(local[0].sequence, 1);
    vcs_package_store_close(ack_store);
    test_rm_rf_recursive(ack_dir);
    vcs_zcode_dht_service_free(a, test_time(1800));
    vcs_zcode_dht_service_free(b, test_time(1004));
    cleanup_fixture(adir);
    cleanup_fixture(bdir);
    PASS();
  }
_test_next:;
  return failures;
}

static int test_sparse_iterative_network(void) {
  int failures = 0;
  TEST("zcode dht service: sparse multi-hop records, local ban and restart") {
    struct multi_network net;
    memset(&net, 0, sizeof(net));
    net.now = test_time(1001);
    uint8_t genesis[32];
    memset(genesis, 0x11, sizeof(genesis));
    for (size_t i = 0; i < MULTI_NODES; i++) {
      snprintf(net.dir[i], sizeof(net.dir[i]),
               "/tmp/zcl_dht_multi_%zu_XXXXXX", i);
      ASSERT(mkdtemp(net.dir[i]) != NULL);
      memset(net.noise[i], (int)(0x30 + i), 32);
      ASSERT(fixture_identity(net.dir[i], (uint8_t)(0x70 + i), genesis,
                              net.noise[i]));
      ASSERT(fixture_material(net.dir[i],
                              &(struct vcs_zcode_dht_delegation){0},
                              (uint8_t[32]){0}, net.node_id[i]));
      net.reach[i].network = &net;
      net.reach[i].owner = i;
    }
    for (size_t i = 0; i < MULTI_NODES; i++) {
      net.service[i] = multi_service(&net, i, genesis);
      ASSERT(net.service[i] != NULL);
      ASSERT(vcs_zcode_dht_service_enabled(net.service[i]));
    }

    /* Put six independent identities in descending XOR distance from the
     * target, then connect only that chain plus one B->D escape edge. */
    const size_t target_node = MULTI_NODES - 1;
    size_t order[MULTI_NODES];
    for (size_t i = 0; i < target_node; i++)
      order[i] = i;
    for (size_t i = 0; i < target_node; i++)
      for (size_t j = i + 1; j < target_node; j++)
        if (!farther_node(net.node_id[order[i]], net.node_id[order[j]],
                          net.node_id[target_node])) {
          size_t swap = order[i];
          order[i] = order[j];
          order[j] = swap;
        }
    order[target_node] = target_node;
    for (size_t i = 1; i < MULTI_NODES; i++)
      ASSERT(farther_node(net.node_id[order[i - 1]], net.node_id[order[i]],
                          net.node_id[target_node]));
    for (size_t i = 0; i + 1 < MULTI_NODES; i++)
      ASSERT(multi_connect(&net, order[i], order[i + 1]));
    ASSERT(multi_connect(&net, order[1], order[3]));
    ASSERT(multi_drive(&net));

    const size_t origin = order[0];
    net.deny[origin][order[2]] = true; /* break the obvious next hop */
    struct vcs_zcode_dht_service_status before, after;
    vcs_zcode_dht_service_status(net.service[origin], &before);
    ASSERT_EQ(before.connected_authenticated, 1);
    uint64_t lookup = 0;
    ASSERT(vcs_zcode_dht_service_lookup_begin(
        net.service[origin], net.node_id[target_node], net.now, &lookup));
    uint64_t frames_before = net.frames;
    ASSERT(multi_drive(&net));
    struct vcs_zcode_dht_lookup_result result;
    ASSERT(vcs_zcode_dht_service_lookup_poll(net.service[origin], lookup,
                                             net.now, &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    ASSERT_EQ(result.termination,
              VCS_ZCODE_DHT_TERMINATION_TARGET_AUTHENTICATED);
    ASSERT(result.rounds >= 3);
    ASSERT(result.xor_progress >= 3);
    ASSERT(net.denied_hints >= 1);
    bool found_target = false, found_denied = false;
    for (uint32_t i = 0; i < result.count; i++) {
      found_target |= memcmp(result.node_ids[i], net.node_id[target_node], 32) == 0;
      found_denied |= memcmp(result.node_ids[i], net.node_id[order[2]], 32) == 0;
    }
    ASSERT(found_target);
    ASSERT(!found_denied);
    ASSERT(net.frames - frames_before <= 96);
    vcs_zcode_dht_service_status(net.service[origin], &after);
    ASSERT(after.lookup_rounds >= result.rounds);
    ASSERT(after.lookup_xor_progress >= result.xor_progress);

    /* Resolve the generic record key rather than naming the publisher peer.
     * The iterative record operation reuses the S6 walk, queries the closest
     * authenticated nodes, merges signed results, and treats records.v1 only
     * as its local rebuildable cache. */
    struct vcs_zcode_dht_record pointers[MULTI_NODES];
    for (size_t i = 0; i < MULTI_NODES; i++) {
      ASSERT(fixture_pointer_record(net.dir[i], genesis, 0xc1,
                                    (uint8_t)(0xd0 + i), &pointers[i]));
      ASSERT_EQ(vcs_zcode_dht_service_record_admit(
                    net.service[target_node], &pointers[i], net.now),
                VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    }
    struct vcs_zcode_dht_record pointer = pointers[target_node];
    for (uint8_t conflict = 1; conflict < 8; conflict++) {
      struct vcs_zcode_dht_record flooded;
      ASSERT(fixture_pointer_record(net.dir[target_node], genesis, 0xc1,
                                    (uint8_t)(0xe0 + conflict), &flooded));
      ASSERT_EQ(vcs_zcode_dht_service_record_admit(
                    net.service[target_node], &flooded, net.now),
                VCS_ZCODE_DHT_RECORD_STORE_CONFLICT);
    }
    struct vcs_zcode_dht_record_selector selector = {
        .kind = VCS_ZCODE_DHT_RECORD_POINTER};
    snprintf(selector.namespace_name, sizeof(selector.namespace_name),
             "science.study");
    memcpy(selector.root, pointer.semantic_root, 32);

    /* Recreate the origin as the actual late joiner in the S7.1 proof: its
     * records cache and peer database do not exist, and its sole bootstrap
     * session is not the publisher. The signed pointer remains only at the
     * far target, so discovery below must traverse the sparse DHT. */
    vcs_zcode_dht_service_free(net.service[origin], net.now);
    net.service[origin] = NULL;
    char late_path[512];
    (void)snprintf(late_path, sizeof(late_path),
                   "%s/zcode/dht/contacts.v2", net.dir[origin]);
    (void)unlink(late_path);
    ASSERT(access(late_path, F_OK) != 0);
    (void)snprintf(late_path, sizeof(late_path),
                   "%s/zcode/dht/records.v1", net.dir[origin]);
    (void)unlink(late_path);
    ASSERT(access(late_path, F_OK) != 0);
    for (size_t i = 0; i < MULTI_NODES; i++) {
      net.connected[origin][i] = false;
      net.connected[i][origin] = false;
      net.pending[origin][i] = false;
      net.pending[i][origin] = false;
    }
    net.service[origin] = multi_service(&net, origin, genesis);
    ASSERT(net.service[origin] != NULL);
    struct vcs_zcode_dht_service_status late_status;
    vcs_zcode_dht_service_status(net.service[origin], &late_status);
    ASSERT_EQ(late_status.cold_contacts, 0);
    struct vcs_zcode_dht_record late_cache[1];
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  net.service[origin], net.now.wall_unix, &selector,
                  late_cache, 1),
              0);
    ASSERT(order[1] != target_node);
    ASSERT(multi_connect(&net, origin, order[1]));
    ASSERT(multi_drive(&net));
    ASSERT(!net.connected[origin][target_node]);
    vcs_zcode_dht_service_status(net.service[origin], &late_status);
    ASSERT_EQ(late_status.connected_authenticated, 1);

    uint64_t record_operation = 0;
    ASSERT(vcs_zcode_dht_service_record_discovery_begin(
        net.service[origin], &selector, net.now, &record_operation));
    ASSERT(multi_drive(&net));
    struct vcs_zcode_dht_record_discovery_result discovery_result;
    ASSERT(vcs_zcode_dht_service_record_discovery_poll(
        net.service[origin], record_operation, net.now, &discovery_result));
    ASSERT_EQ(discovery_result.state,
              VCS_ZCODE_DHT_RECORD_OPERATION_PENDING);
    for (size_t drive = 0;
         drive < VCS_ZCODE_DHT_K &&
         discovery_result.state == VCS_ZCODE_DHT_RECORD_OPERATION_PENDING;
         drive++) {
      ASSERT(multi_drive(&net));
      ASSERT(vcs_zcode_dht_service_record_discovery_poll(
          net.service[origin], record_operation, net.now, &discovery_result));
    }
    ASSERT_EQ(discovery_result.state,
              VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE);
    ASSERT_EQ(discovery_result.record_count, MULTI_NODES + 7u);
    for (size_t i = 0; i < MULTI_NODES; i++)
      for (size_t j = i + 1; j < MULTI_NODES; j++)
        ASSERT(memcmp(discovery_result.records[i].provider_node_id,
                      discovery_result.records[j].provider_node_id, 32) != 0);
    size_t target_conflicts = 0;
    for (uint32_t i = 0; i < discovery_result.record_count; i++)
      target_conflicts +=
          memcmp(discovery_result.records[i].provider_node_id,
                 pointer.provider_node_id, 32) == 0;
    ASSERT_EQ(target_conflicts, 8);
    struct vcs_zcode_dht_record cached[1];
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  net.service[origin], net.now.wall_unix, &selector, cached, 1),
              MULTI_NODES + 7u);

    /* Provider routing binds a signed claim to the currently authenticated
     * Noise/delegation session for that exact node ID. Local policy is
     * re-evaluated for FETCH/STORE/INDEX before the peer handle is exposed. */
    struct vcs_zcode_dht_record provider;
    ASSERT(fixture_provider_record(net.dir[target_node], genesis,
                                   0xf1, &provider));
    ASSERT_EQ(vcs_zcode_dht_service_record_admit(
                  net.service[origin], &provider, net.now),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    struct vcs_zcode_dht_record_selector provider_selector = {
        .kind = VCS_ZCODE_DHT_RECORD_PROVIDER};
    (void)snprintf(provider_selector.namespace_name,
                   sizeof(provider_selector.namespace_name), "science");
    memcpy(provider_selector.root, provider.transport_root, 32);
    struct vcs_zcode_dht_provider_route route;
    ASSERT(vcs_zcode_dht_service_provider_route(
        net.service[origin], net.now.wall_unix, &provider_selector, &route));
    ASSERT_EQ(route.authenticated_count, 1);
    ASSERT_EQ(route.peer_ids[0], target_node + 1);
    memcpy(net.banned_root, provider.transport_root, 32);
    net.banned[origin] = true;
    ASSERT(vcs_zcode_dht_service_provider_route(
        net.service[origin], net.now.wall_unix, &provider_selector, &route));
    ASSERT_EQ(route.authenticated_count, 0);
    ASSERT_EQ(route.policy_denied, 1);
    net.banned[origin] = false;

    /* Publication is a lookup against the deterministic record key, not a
     * broadcast to the publisher's current sessions. Select a node that is
     * not directly connected to the publisher, then prove that the closest
     * set walk reaches it and stores the signed record. */
    size_t indirect = MULTI_NODES;
    for (size_t i = 0; i < MULTI_NODES; i++)
      if (i != target_node && !net.connected[target_node][i]) {
        indirect = i;
        break;
      }
    ASSERT(indirect < MULTI_NODES);
    struct vcs_zcode_dht_publish_spec routed_publish;
    memset(&routed_publish, 0, sizeof(routed_publish));
    routed_publish.kind = VCS_ZCODE_DHT_RECORD_POINTER;
    (void)snprintf(routed_publish.namespace_name,
                   sizeof(routed_publish.namespace_name), "science");
    memset(routed_publish.semantic_root, 0xb4, 32);
    memset(routed_publish.transport_root, 0xb5, 32);
    routed_publish.sequence = 1;
    routed_publish.not_before = net.now.wall_unix;
    routed_publish.expiry = net.now.wall_unix + 999;
    uint8_t routed_token[32];
    struct vcs_zcode_dht_record routed_record;
    ASSERT(vcs_zcode_dht_service_record_publish_plan(
        net.service[target_node], &routed_publish, routed_token,
        &routed_record));
    ASSERT_EQ(vcs_zcode_dht_service_record_publish_commit(
                  net.service[target_node], &routed_publish, routed_token,
                  net.now, &routed_record),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    uint64_t publish_frames = net.frames;
    struct vcs_zcode_dht_record_selector routed_selector = {
        .kind = VCS_ZCODE_DHT_RECORD_POINTER};
    (void)snprintf(routed_selector.namespace_name,
                   sizeof(routed_selector.namespace_name), "science");
    memcpy(routed_selector.root, routed_publish.semantic_root, 32);
    struct vcs_zcode_dht_record routed_found[1];
    size_t routed_count = 0;
    for (size_t turn = 0; turn < 32 && routed_count == 0; turn++) {
      ASSERT(multi_drive(&net));
      vcs_zcode_dht_service_tick(net.service[target_node], net.now);
      routed_count = vcs_zcode_dht_service_record_local_query(
          net.service[indirect], net.now.wall_unix, &routed_selector,
          routed_found, 1);
    }
    ASSERT_EQ(routed_count, 1);
    ASSERT_EQ(routed_found[0].sequence, 1);
    ASSERT(net.connected[target_node][indirect]);
    struct vcs_zcode_dht_service_status publish_status;
    for (size_t turn = 0; turn < 32; turn++) {
      vcs_zcode_dht_service_status(net.service[target_node], &publish_status);
      if (publish_status.active_publications == 0)
        break;
      ASSERT(multi_drive(&net));
      vcs_zcode_dht_service_tick(net.service[target_node], net.now);
    }
    vcs_zcode_dht_service_status(net.service[target_node], &publish_status);
    ASSERT_EQ(publish_status.active_publications, 0);
    ASSERT(net.frames - publish_frames <= 256);

    /* A local policy change freezes forwarding and renewal. Once the local
     * root ban is removed, the same persisted intent renews and advances its
     * sequence; no global state or shared ban is involved. */
    memcpy(net.banned_root, routed_publish.semantic_root, 32);
    net.banned[target_node] = true;
    net.now.wall_unix = routed_publish.expiry - 300;
    net.now.monotonic_s = net.now.wall_unix;
    uint64_t frames_before_policy = net.frames;
    vcs_zcode_dht_service_tick(net.service[target_node], net.now);
    ASSERT(multi_drive(&net));
    ASSERT_EQ(net.frames, frames_before_policy);
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  net.service[target_node], net.now.wall_unix,
                  &routed_selector, routed_found, 1),
              1);
    ASSERT_EQ(routed_found[0].sequence, 1);
    net.banned[target_node] = false;
    vcs_zcode_dht_service_tick(net.service[target_node], net.now);
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  net.service[target_node], net.now.wall_unix,
                  &routed_selector, routed_found, 1),
              1);
    ASSERT_EQ(routed_found[0].sequence, 2);

    /* A ban is strictly local. Origin stops serving this root while the
     * target still serves the identical signed record to another node. */
    struct vcs_zcode_dht_record_operation_result record_result;
    memcpy(net.banned_root, pointer.semantic_root, 32);
    net.banned[origin] = true;
    ASSERT(vcs_zcode_dht_service_record_query_begin(
        net.service[target_node], origin + 1, &selector, net.now,
        &record_operation));
    ASSERT(multi_drive(&net));
    ASSERT(vcs_zcode_dht_service_record_operation_poll(
        net.service[target_node], record_operation, net.now, &record_result));
    ASSERT_EQ(record_result.record_count, 0);
    size_t alternate = order[target_node - 1];
    ASSERT(vcs_zcode_dht_service_record_query_begin(
        net.service[alternate], target_node + 1, &selector, net.now,
        &record_operation));
    ASSERT(multi_drive(&net));
    ASSERT(vcs_zcode_dht_service_record_operation_poll(
        net.service[alternate], record_operation, net.now, &record_result));
    ASSERT_EQ(record_result.record_count, VCS_ZCODE_DHT_RECORDS_PER_FRAME);

    /* A reachability request can be accepted while the bounded dial itself
     * fails.  The unverified ID must age out monotonically so a nonexistent
     * target reaches shortlist stability instead of the 30-second ceiling. */
    net.deny[origin][order[2]] = false;
    net.stall[origin][order[2]] = true;
    uint8_t absent_target[32];
    memset(absent_target, 0xa5, sizeof(absent_target));
    ASSERT(vcs_zcode_dht_service_lookup_begin(
        net.service[origin], absent_target, net.now, &lookup));
    ASSERT(multi_drive(&net));
    ASSERT(vcs_zcode_dht_service_lookup_poll(net.service[origin], lookup,
                                             net.now, &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_PENDING);
    net.now.monotonic_s +=
        VCS_ZCODE_DHT_SERVICE_REACHABILITY_TIMEOUT_S + 1;
    ASSERT(vcs_zcode_dht_service_lookup_poll(net.service[origin], lookup,
                                             net.now, &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    ASSERT_EQ(result.termination,
              VCS_ZCODE_DHT_TERMINATION_SHORTLIST_STABLE);
    net.stall[origin][order[2]] = false;
    net.deny[origin][order[2]] = true;

    /* All eight requests are admitted while the three global slots are
     * occupied. Scheduler rotation eventually gives every lookup a query. */
    uint64_t ids[VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS];
    uint8_t concurrent_target[32];
    memset(concurrent_target, 0xa6, sizeof(concurrent_target));
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++)
      ASSERT(vcs_zcode_dht_service_lookup_begin(
          net.service[origin], concurrent_target, net.now, &ids[i]));
    vcs_zcode_dht_service_status(net.service[origin], &after);
    ASSERT_EQ(after.queued_lookups, VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS);
    ASSERT_EQ(after.active_queries, VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES);
    net.now.monotonic_s++;
    ASSERT(multi_drive(&net));
    size_t waited = 0;
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++) {
      ASSERT(vcs_zcode_dht_service_lookup_poll(net.service[origin], ids[i],
                                               net.now, &result));
      ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
      ASSERT(result.rounds > 0);
      waited += result.queue_wait_s > 0;
    }
    ASSERT(waited >= 5);

    /* Durable authenticated contacts load cold; only a fresh Noise session
     * promotes the neighbour back into connected/authenticated state. */
    vcs_zcode_dht_service_free(net.service[origin], net.now);
    net.service[origin] = multi_service(&net, origin, genesis);
    ASSERT(net.service[origin] != NULL);
    vcs_zcode_dht_service_status(net.service[origin], &after);
    ASSERT(after.persistence_loaded);
    ASSERT(after.cold_contacts >= 1);
    struct vcs_zcode_dht_record cold_record[1];
    ASSERT_EQ(vcs_zcode_dht_service_record_local_query(
                  net.service[origin], net.now.wall_unix, &selector,
                  cold_record, 1),
              MULTI_NODES + 7u);
    ASSERT(memcmp(cold_record[0].semantic_root, pointer.semantic_root, 32) ==
           0);
    ASSERT_EQ(after.connected_authenticated, 0);
    memset(net.connected[origin], 0, sizeof(net.connected[origin]));
    for (size_t i = 0; i < MULTI_NODES; i++)
      net.connected[i][origin] = false;
    /* A new lookup seeds the closest persisted IDs as COLD/UNVERIFIED.  The
     * reachability callback may arrange a connection, but the result remains
     * pending until that connection freshly authenticates its Noise-bound
     * delegation.  No explicit reconnect is injected here. */
    ASSERT(vcs_zcode_dht_service_lookup_begin(
        net.service[origin], net.node_id[target_node], net.now, &lookup));
    ASSERT(net.pending[origin][target_node]);
    ASSERT(multi_drive(&net));
    ASSERT(vcs_zcode_dht_service_lookup_poll(net.service[origin], lookup,
                                             net.now, &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    ASSERT_EQ(result.termination,
              VCS_ZCODE_DHT_TERMINATION_TARGET_AUTHENTICATED);
    vcs_zcode_dht_service_status(net.service[origin], &after);
    ASSERT(after.connected_authenticated >= 1);

    for (size_t i = 0; i < MULTI_NODES; i++) {
      vcs_zcode_dht_service_free(net.service[i], net.now);
      cleanup_fixture(net.dir[i]);
    }
    PASS();
  }
_test_next:;
  return failures;
}

int test_zcode_dht_service(void) {
  int failures = test_disabled_diagnostics();
  failures += test_deep_ancestry();
  failures += test_peer_admission_order();
  failures += test_record_transport_and_restart();
  failures += test_sparse_iterative_network();
  TEST("zcode dht service: Noise-authenticated two-node lookup and restart") {
    char adir[] = "/tmp/zcl_dht_service_a_XXXXXX";
    char bdir[] = "/tmp/zcl_dht_service_b_XXXXXX";
    ASSERT(mkdtemp(adir) != NULL);
    ASSERT(mkdtemp(bdir) != NULL);
    uint8_t genesis[32], anoise[32], bnoise[32], transcript[32];
    memset(genesis, 0x11, 32);
    memset(anoise, 0x22, 32);
    memset(bnoise, 0x33, 32);
    memset(transcript, 0x55, 32);
    ASSERT(fixture_identity(adir, 0x61, genesis, anoise));
    ASSERT(fixture_identity(bdir, 0x62, genesis, bnoise));
    struct vcs_zcode_dht_service *a = fixture_service(adir, genesis, anoise);
    struct vcs_zcode_dht_service *b = fixture_service(bdir, genesis, bnoise);
    ASSERT(a && b);
    ASSERT(vcs_zcode_dht_service_enabled(a));
    ASSERT(vcs_zcode_dht_service_enabled(b));

    struct vcs_zcode_dht_session as =
                                     {
                                         .established = true,
                                         .generation = 42,
                                         .connection_serial = 1,
                                     },
                                 bs = {.established = true,
                                       .generation = 42,
                                       .connection_serial = 2};
    memcpy(as.remote_static, bnoise, 32);
    memcpy(bs.remote_static, anoise, 32);
    memcpy(as.transcript_hash, transcript, 32);
    memcpy(bs.transcript_hash, transcript, 32);
    ASSERT(vcs_zcode_dht_service_session_open(a, 2, &as, test_time(1001)));
    ASSERT(vcs_zcode_dht_service_session_open(b, 1, &bs, test_time(1001)));

    uint8_t replay[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    size_t replay_len = 0;
    ASSERT(pump(a, b, 2, 1, 1001, replay, &replay_len));
    ASSERT(pump(b, a, 1, 2, 1001, NULL, NULL));
    ASSERT(pump(a, b, 2, 1, 1001, NULL, NULL));

    struct vcs_zcode_dht_service_status ast, bst;
    vcs_zcode_dht_service_status(a, &ast);
    vcs_zcode_dht_service_status(b, &bst);
    ASSERT_EQ(ast.contacts, 1);
    ASSERT_EQ(bst.contacts, 1);
    ASSERT_EQ(ast.buckets_used, 1);
    ASSERT_EQ(bst.buckets_used, 1);
    ASSERT_EQ(ast.connected_authenticated, 1);
    ASSERT_EQ(bst.connected_authenticated, 1);
    ASSERT(ast.find_node_sent > 0 && ast.nodes_received > 0);

    enum vcs_zcode_dht_reject_reason rejected;
    ASSERT(!vcs_zcode_dht_service_handle_frame(b, 1, replay, replay_len,
                                               test_time(1001),
                                               &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_REPLAY);

    uint8_t target[32];
    memset(target, 0x7a, 32);
    uint64_t lookup = 0;
    ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, test_time(1002),
                                              &lookup));
    ASSERT(pump(a, b, 2, 1, 1002, NULL, NULL));
    ASSERT(pump(b, a, 1, 2, 1002, NULL, NULL));
    struct vcs_zcode_dht_lookup_result result;
    ASSERT(vcs_zcode_dht_service_lookup_poll(a, lookup, test_time(1002),
                                             &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    ASSERT_EQ(result.count, 2);
    uint8_t d0[32], d1[32];
    vcs_zcode_dht_xor_distance(result.node_ids[0], target, d0);
    vcs_zcode_dht_xor_distance(result.node_ids[1], target, d1);
    ASSERT(memcmp(d0, d1, 32) <= 0);

    /* A hostile request may reuse an outstanding response query ID. Request
     * and response replay namespaces are independent, so it cannot poison the
     * legitimate NODES response. */
    ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, test_time(1002),
                                              &lookup));
    uint8_t collision_query[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    uint64_t collision_peer = 0;
    size_t collision_query_len = 0;
    ASSERT(vcs_zcode_dht_service_next_outbound(
        a, 2, &collision_peer, collision_query, sizeof(collision_query),
        &collision_query_len));
    ASSERT_EQ(collision_peer, 2);
    ASSERT_EQ(collision_query[10], VCS_ZCODE_DHT_MSG_FIND_NODE);
    const size_t query_id_off = VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 8u + 32u;
    uint8_t collision_find[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    size_t collision_find_len = 0;
    ASSERT(signed_find(bdir, 42, transcript, 0xdd, 0x7a, collision_find,
                       sizeof(collision_find), &collision_find_len));
    memcpy(collision_find + query_id_off, collision_query + query_id_off,
           VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES);
    ASSERT(resign_wire(bdir, transcript, collision_find, collision_find_len));
    ASSERT(vcs_zcode_dht_service_handle_frame(
        a, 2, collision_find, collision_find_len, test_time(1002), &rejected));
    (void)drain(a);
    ASSERT(vcs_zcode_dht_service_handle_frame(
        b, 1, collision_query, collision_query_len, test_time(1002),
        &rejected));
    uint8_t collision_response[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    uint64_t collision_response_peer = 0;
    size_t collision_response_len = 0;
    ASSERT(vcs_zcode_dht_service_next_outbound(
        b, 1, &collision_response_peer, collision_response,
        sizeof(collision_response), &collision_response_len));
    ASSERT(vcs_zcode_dht_service_handle_frame(
        a, 2, collision_response, collision_response_len, test_time(1002),
        &rejected));
    ASSERT(vcs_zcode_dht_service_lookup_poll(a, lookup, test_time(1002),
                                             &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        a, 2, collision_response, collision_response_len, test_time(1002),
        &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_REPLAY);

    /* A correctly signed peer may name any ID. The frame is valid, but an
     * unknown ID is only an unreachable hint and never appears in results. */
    ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, test_time(1002),
                                              &lookup));
    ASSERT(pump(a, b, 2, 1, 1002, NULL, NULL));
    uint64_t hinted_peer = 0;
    size_t hinted_len = 0;
    uint8_t hinted[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    ASSERT(vcs_zcode_dht_service_next_outbound(
        b, 1, &hinted_peer, hinted, sizeof(hinted), &hinted_len));
    size_t hinted_off =
        VCS_ZCODE_DHT_MSGS_HEADER_BYTES + VCS_ZCODE_DHT_MSGS_AUTH_BYTES;
    ASSERT_EQ(hinted[hinted_off], 2);
    memset(hinted + hinted_off + 1 + 32, 0xff, 32);
    ASSERT(resign_wire(bdir, transcript, hinted, hinted_len));
    ASSERT(vcs_zcode_dht_service_handle_frame(
        a, 2, hinted, hinted_len, test_time(1002), &rejected));
    ASSERT(vcs_zcode_dht_service_lookup_poll(a, lookup, test_time(1002),
                                             &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    for (uint32_t i = 0; i < result.count; i++)
      ASSERT(result.node_ids[i][0] != 0xff);

    /* Exact bounds and established Noise are checked before identity or
     * query state, and an arbitrary NODES response is never admitted. */
    ASSERT(!vcs_zcode_dht_service_handle_frame(b, 99, replay, replay_len,
                                               test_time(1002),
                                               &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_PLAINTEXT);
    uint8_t oversized[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES + 1];
    memcpy(oversized, replay, replay_len);
    oversized[replay_len] = 0;
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        b, 1, oversized, replay_len + 1, test_time(1002), &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_MALFORMED);
    size_t forged_len = 0;
    ASSERT(signed_nodes(bdir, 42, transcript, 0xa1, oversized,
                        sizeof(oversized), &forged_len));
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        a, 2, oversized, forged_len, test_time(1002), &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_UNSOLICITED);
    ASSERT(signed_find(adir, 43, transcript, 0xa2, 0x7b, oversized,
                       sizeof(oversized), &forged_len));
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        b, 1, oversized, forged_len, test_time(1002), &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_SESSION);

    /* A response with valid authentication but poisoned ordering is
     * rejected without consuming its query; the later valid response is
     * then rejected under the exact 30-second deadline. */
    ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, test_time(1003),
                                              &lookup));
    ASSERT(pump(a, b, 2, 1, 1003, NULL, NULL));
    uint64_t response_peer = 0;
    size_t response_len = 0;
    uint8_t response[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    ASSERT(vcs_zcode_dht_service_next_outbound(
        b, 1, &response_peer, response, sizeof(response), &response_len));
    ASSERT_EQ(response_peer, 1);
    uint8_t poisoned[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    memcpy(poisoned, response, response_len);
    size_t nodes_off =
        VCS_ZCODE_DHT_MSGS_HEADER_BYTES + VCS_ZCODE_DHT_MSGS_AUTH_BYTES;
    ASSERT_EQ(poisoned[nodes_off], 2);
    uint8_t swap[32];
    memcpy(swap, poisoned + nodes_off + 1, 32);
    memcpy(poisoned + nodes_off + 1, poisoned + nodes_off + 33, 32);
    memcpy(poisoned + nodes_off + 33, swap, 32);
    ASSERT(resign_wire(bdir, transcript, poisoned, response_len));
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        a, 2, poisoned, response_len, test_time(1003), &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_POISONED);
    /* The periodic sweep may retire the active slot before a late frame is
     * dispatched.  A bounded tombstone must preserve the exact EXPIRED
     * diagnosis instead of degrading it to UNSOLICITED. */
    vcs_zcode_dht_service_tick(a, test_time(1034));
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        a, 2, response, response_len, test_time(1034), &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_EXPIRED);
    ASSERT(vcs_zcode_dht_service_lookup_poll(a, lookup, test_time(1034),
                                             &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    ASSERT_EQ(result.termination,
              VCS_ZCODE_DHT_TERMINATION_SHORTLIST_STABLE);

    /* Four requests/s with burst eight is exact and independent of peer
     * lengths; a ninth same-second request is named rate-limit. */
    (void)drain(b);
    for (uint8_t i = 1; i <= 9; i++) {
      ASSERT(signed_find(adir, 42, transcript, (uint8_t)(0xb0 + i), 0x7c,
                         oversized, sizeof(oversized), &forged_len));
      bool accepted = vcs_zcode_dht_service_handle_frame(
          b, 1, oversized, forged_len, test_time(2000), &rejected);
      if (i <= 8)
        ASSERT(accepted);
      else {
        ASSERT(!accepted);
        ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_RATE);
      }
    }
    ASSERT_EQ(drain(b), 8);
    ASSERT(signed_find(adir, 42, transcript, 0xca, 0x7c, oversized,
                       sizeof(oversized), &forged_len));
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        b, 1, oversized, forged_len,
        (struct vcs_zcode_dht_time){.wall_unix = 80000,
                                    .monotonic_s = 2000},
        &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_RATE);
    ASSERT(signed_find(adir, 42, transcript, 0xcb, 0x7c, oversized,
                       sizeof(oversized), &forged_len));
    ASSERT(vcs_zcode_dht_service_handle_frame(
        b, 1, oversized, forged_len,
        (struct vcs_zcode_dht_time){.wall_unix = 3000,
                                    .monotonic_s = 2001},
        &rejected));
    ASSERT_EQ(drain(b), 1);

    /* The replay ledger is sized for the whole admitted 30-second frame
     * population, not a 16-entry sample. More than sixteen distinct signed
     * frames cannot evict an earlier still-live query ID. */
    uint8_t old_frame[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    size_t old_frame_len = 0;
    for (uint8_t i = 0; i < 24; i++) {
      uint64_t mono = 3000 + i / 3;
      ASSERT(signed_find(adir, 42, transcript, (uint8_t)(0x10 + i), 0x7d,
                         oversized, sizeof(oversized), &forged_len));
      if (i == 0) {
        memcpy(old_frame, oversized, forged_len);
        old_frame_len = forged_len;
      }
      struct vcs_zcode_dht_time when = {.wall_unix = 3000,
                                        .monotonic_s = mono};
      ASSERT(vcs_zcode_dht_service_handle_frame(
          b, 1, oversized, forged_len, when, &rejected));
      (void)drain(b);
    }
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        b, 1, old_frame, old_frame_len,
        (struct vcs_zcode_dht_time){.wall_unix = 3000, .monotonic_s = 3007},
        &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_REPLAY);

    /* A node ID owns one authenticated service session. Newer local serials
     * replace older sessions; equal serials retain the lower peer ID, and a
     * retired exact connection cannot be re-admitted while still live. */
    uint8_t transcript2[32], transcript3[32];
    memset(transcript2, 0x56, sizeof(transcript2));
    memset(transcript3, 0x57, sizeof(transcript3));
    struct vcs_zcode_dht_session newer = {.established = true,
                                          .generation = 43,
                                          .connection_serial = 10};
    memcpy(newer.remote_static, bnoise, 32);
    memcpy(newer.transcript_hash, transcript2, 32);
    ASSERT(vcs_zcode_dht_service_session_open(a, 3, &newer,
                                              test_time(1007)));
    ASSERT(signed_find(bdir, 43, transcript2, 0xe1, 0x7e, oversized,
                       sizeof(oversized), &forged_len));
    ASSERT(vcs_zcode_dht_service_handle_frame(
        a, 3, oversized, forged_len, test_time(1007), &rejected));
    vcs_zcode_dht_service_status(a, &ast);
    ASSERT_EQ(ast.connected_authenticated, 1);
    ASSERT_EQ(ast.duplicate_sessions_retired, 1);
    ASSERT(!vcs_zcode_dht_service_session_open(a, 2, &as,
                                               test_time(1007)));

    struct vcs_zcode_dht_session tied = {.established = true,
                                         .generation = 44,
                                         .connection_serial = 10};
    memcpy(tied.remote_static, bnoise, 32);
    memcpy(tied.transcript_hash, transcript3, 32);
    ASSERT(vcs_zcode_dht_service_session_open(a, 4, &tied,
                                              test_time(1007)));
    ASSERT(signed_find(bdir, 44, transcript3, 0xe2, 0x7e, oversized,
                       sizeof(oversized), &forged_len));
    ASSERT(!vcs_zcode_dht_service_handle_frame(
        a, 4, oversized, forged_len, test_time(1007), &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_SESSION);
    ASSERT(!vcs_zcode_dht_service_session_open(a, 4, &tied,
                                               test_time(1007)));
    vcs_zcode_dht_service_status(a, &ast);
    ASSERT_EQ(ast.connected_authenticated, 1);
    ASSERT_EQ(ast.duplicate_sessions_retired, 2);

    vcs_zcode_dht_service_tick(a, test_time(1008));
    vcs_zcode_dht_service_free(a, test_time(1008));
    a = NULL;
    a = fixture_service(adir, genesis, anoise);
    ASSERT(a != NULL);
    vcs_zcode_dht_service_status(a, &ast);
    ASSERT(ast.persistence_loaded);
    ASSERT_EQ(ast.persistence_load_count, 1);
    ASSERT_EQ(ast.cold_contacts, 1);
    ASSERT_EQ(ast.buckets_used, 1);
    ASSERT_EQ(ast.connected_authenticated, 0);

    /* The eight-slot lookup queue is a hard cap even when every lookup
     * completes locally and waits for its caller to collect the result. */
    uint64_t ids[VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS];
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++)
      ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, test_time(1010),
                                                &ids[i]));
    ASSERT(!vcs_zcode_dht_service_lookup_begin(a, target, test_time(1010),
                                               &lookup));
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++) {
      ASSERT(vcs_zcode_dht_service_lookup_poll(a, ids[i], test_time(1010),
                                               &result));
      ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    }

    /* Unauthenticated handshakes lose their service slot after a monotonic
     * deadline and the still-live exact connection cannot immediately claim
     * it again. Session freshness follows the local serial, never numeric
     * ordering of the transcript-derived generation token. */
    ASSERT(vcs_zcode_dht_service_session_open(a, 900, &as,
                                              test_time(1011)));
    vcs_zcode_dht_service_tick(a, test_time(1026));
    vcs_zcode_dht_service_status(a, &ast);
    ASSERT_EQ(ast.unauthenticated_expired, 1);
    ASSERT(!vcs_zcode_dht_service_session_open(a, 900, &as,
                                               test_time(1026)));
    struct vcs_zcode_dht_session high_token = as;
    high_token.generation = UINT64_MAX;
    high_token.connection_serial = 20;
    ASSERT(vcs_zcode_dht_service_session_open(a, 901, &high_token,
                                              test_time(1027)));
    vcs_zcode_dht_service_session_close(a, 901, high_token.generation,
                                        test_time(1027));
    struct vcs_zcode_dht_session low_token = as;
    low_token.generation = 1;
    low_token.connection_serial = 21;
    ASSERT(vcs_zcode_dht_service_session_open(a, 901, &low_token,
                                              test_time(1027)));
    vcs_zcode_dht_service_session_close(a, 901, low_token.generation,
                                        test_time(1027));

    /* Peer session slots are reusable after disconnect; churn cannot
     * permanently exhaust the 64-session authentication budget. */
    for (uint64_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
      ASSERT(vcs_zcode_dht_service_session_open(a, 100 + i, &as,
                                                test_time(1028)));
    ASSERT(!vcs_zcode_dht_service_session_open(a, 1000, &as,
                                               test_time(1028)));
    vcs_zcode_dht_service_session_close(a, 100, 42, test_time(1028));
    ASSERT(vcs_zcode_dht_service_session_open(a, 1000, &as,
                                              test_time(1028)));

    vcs_zcode_dht_service_free(a, test_time(1009));
    vcs_zcode_dht_service_free(b, test_time(1009));

    /* A trailing byte on cold start never partially publishes the valid
     * prefix. The service stays enabled but starts with an empty table and
     * a named persistence error. */
    char contacts_path[512];
    snprintf(contacts_path, sizeof(contacts_path), "%s/zcode/dht/contacts.v2",
             adir);
    FILE *contacts = fopen(contacts_path, "ab");
    ASSERT(contacts != NULL);
    ASSERT(fputc(0, contacts) != EOF);
    ASSERT(fclose(contacts) == 0);
    a = fixture_service(adir, genesis, anoise);
    ASSERT(a != NULL);
    vcs_zcode_dht_service_status(a, &ast);
    ASSERT(!ast.persistence_loaded);
    ASSERT_EQ(ast.contacts, 0);
    ASSERT(ast.last_error[0] != '\0');
    vcs_zcode_dht_service_free(a, test_time(1012));
    cleanup_fixture(adir);
    cleanup_fixture(bdir);
    PASS();
  }
_test_next:;
  return failures;
}
