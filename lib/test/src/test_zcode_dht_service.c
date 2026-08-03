/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Two-node protocol tests for the bounded ZCODE DHT service. */

#include "test/test_core.h"

#include "base/safe_alloc.h"
#include "config/boot_zcode_dht.h"
#include "crypto/ed25519.h"
#include "json/json.h"
#include "support/cleanse.h"
#include "vcs/zcode_dht_identity.h"
#include "vcs/zcode_dht_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool chain_ok(void *ctx, const struct vcs_zcode_dht_delegation *d) {
  (void)ctx;
  return d && d->beacon_height == 120;
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
      .now_unix = 1000,
      .chain_verify = chain_ok,
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
  uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
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
  uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
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
    if (!vcs_zcode_dht_service_handle_frame(to, to_peer, wire, len, now,
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

static int test_disabled_diagnostics(void) {
  int failures = 0;
  TEST("zcode dht service: disabled reasons and diagnostics expose no identity material") {
    char dir[] = "/tmp/zcl_dht_service_disabled_XXXXXX";
    ASSERT(mkdtemp(dir) != NULL);
    struct vcs_zcode_dht_service_params params = {
        .datadir = dir, .transport_enabled = false, .now_unix = 1000,
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
    vcs_zcode_dht_service_free(disabled, 1000);
    params.transport_enabled = true;
    disabled = vcs_zcode_dht_service_create(&params);
    ASSERT(disabled != NULL);
    vcs_zcode_dht_service_status(disabled, &status);
    ASSERT(!status.enabled);
    ASSERT_STR_EQ(status.disabled_reason, "IDENTITY_MATERIAL_UNAVAILABLE");
    vcs_zcode_dht_service_free(disabled, 1000);

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

int test_zcode_dht_service(void) {
  int failures = test_disabled_diagnostics();
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
                                     },
                                 bs = {.established = true, .generation = 42};
    memcpy(as.remote_static, bnoise, 32);
    memcpy(bs.remote_static, anoise, 32);
    memcpy(as.transcript_hash, transcript, 32);
    memcpy(bs.transcript_hash, transcript, 32);
    ASSERT(vcs_zcode_dht_service_session_open(a, 2, &as, 1001));
    ASSERT(vcs_zcode_dht_service_session_open(b, 1, &bs, 1001));

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
    ASSERT(!vcs_zcode_dht_service_handle_frame(b, 1, replay, replay_len, 1001,
                                               &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_REPLAY);

    uint8_t target[32];
    memset(target, 0x7a, 32);
    uint64_t lookup = 0;
    ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, 1002, &lookup));
    ASSERT(pump(a, b, 2, 1, 1002, NULL, NULL));
    ASSERT(pump(b, a, 1, 2, 1002, NULL, NULL));
    struct vcs_zcode_dht_lookup_result result;
    ASSERT(vcs_zcode_dht_service_lookup_poll(a, lookup, 1002, &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    ASSERT_EQ(result.count, 2);
    uint8_t d0[32], d1[32];
    vcs_zcode_dht_xor_distance(result.node_ids[0], target, d0);
    vcs_zcode_dht_xor_distance(result.node_ids[1], target, d1);
    ASSERT(memcmp(d0, d1, 32) <= 0);

    /* Exact bounds and established Noise are checked before identity or
     * query state, and an arbitrary NODES response is never admitted. */
    ASSERT(!vcs_zcode_dht_service_handle_frame(b, 99, replay, replay_len, 1002,
                                               &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_PLAINTEXT);
    uint8_t oversized[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES + 1];
    memcpy(oversized, replay, replay_len);
    oversized[replay_len] = 0;
    ASSERT(!vcs_zcode_dht_service_handle_frame(b, 1, oversized, replay_len + 1,
                                               1002, &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_MALFORMED);
    size_t forged_len = 0;
    ASSERT(signed_nodes(bdir, 42, transcript, 0xa1, oversized,
                        sizeof(oversized), &forged_len));
    ASSERT(!vcs_zcode_dht_service_handle_frame(a, 2, oversized, forged_len,
                                               1002, &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_UNSOLICITED);
    ASSERT(signed_find(adir, 43, transcript, 0xa2, 0x7b, oversized,
                       sizeof(oversized), &forged_len));
    ASSERT(!vcs_zcode_dht_service_handle_frame(b, 1, oversized, forged_len,
                                               1002, &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_SESSION);

    /* A response with valid authentication but poisoned ordering is
     * rejected without consuming its query; the later valid response is
     * then rejected under the exact 30-second deadline. */
    ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, 1003, &lookup));
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
    ASSERT(!vcs_zcode_dht_service_handle_frame(a, 2, poisoned, response_len,
                                               1003, &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_POISONED);
    /* The periodic sweep may retire the active slot before a late frame is
     * dispatched.  A bounded tombstone must preserve the exact EXPIRED
     * diagnosis instead of degrading it to UNSOLICITED. */
    vcs_zcode_dht_service_tick(a, 1034);
    ASSERT(!vcs_zcode_dht_service_handle_frame(a, 2, response, response_len,
                                               1034, &rejected));
    ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_EXPIRED);
    ASSERT(vcs_zcode_dht_service_lookup_poll(a, lookup, 1034, &result));
    ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_TIMEOUT);

    /* Four requests/s with burst eight is exact and independent of peer
     * lengths; a ninth same-second request is named rate-limit. */
    (void)drain(b);
    for (uint8_t i = 1; i <= 9; i++) {
      ASSERT(signed_find(adir, 42, transcript, (uint8_t)(0xb0 + i), 0x7c,
                         oversized, sizeof(oversized), &forged_len));
      bool accepted = vcs_zcode_dht_service_handle_frame(
          b, 1, oversized, forged_len, 2000, &rejected);
      if (i <= 8)
        ASSERT(accepted);
      else {
        ASSERT(!accepted);
        ASSERT_EQ(rejected, VCS_ZCODE_DHT_REJECT_RATE);
      }
    }
    ASSERT_EQ(drain(b), 8);

    vcs_zcode_dht_service_tick(a, 1008);
    vcs_zcode_dht_service_free(a, 1008);
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
      ASSERT(vcs_zcode_dht_service_lookup_begin(a, target, 1010, &ids[i]));
    ASSERT(!vcs_zcode_dht_service_lookup_begin(a, target, 1010, &lookup));
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_LOOKUPS; i++) {
      ASSERT(vcs_zcode_dht_service_lookup_poll(a, ids[i], 1010, &result));
      ASSERT_EQ(result.state, VCS_ZCODE_DHT_LOOKUP_COMPLETE);
    }

    /* Peer session slots are reusable after disconnect; churn cannot
     * permanently exhaust the 64-session authentication budget. */
    for (uint64_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
      ASSERT(vcs_zcode_dht_service_session_open(a, 100 + i, &as, 1011));
    ASSERT(!vcs_zcode_dht_service_session_open(a, 1000, &as, 1011));
    vcs_zcode_dht_service_session_close(a, 100, 42, 1011);
    ASSERT(vcs_zcode_dht_service_session_open(a, 1000, &as, 1011));

    vcs_zcode_dht_service_free(a, 1009);
    vcs_zcode_dht_service_free(b, 1009);

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
    vcs_zcode_dht_service_free(a, 1012);
    cleanup_fixture(adir);
    cleanup_fixture(bdir);
    PASS();
  }
_test_next:;
  return failures;
}
