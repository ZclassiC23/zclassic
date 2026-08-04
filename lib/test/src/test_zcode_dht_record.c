/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_core.h"

#include "base/hex.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "support/cleanse.h"
#include "vcs/zcode_dht_record.h"
#include "vcs/zcode_dht_record_store.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct record_fixture {
  uint8_t online_seed[32];
  struct vcs_zcode_dht_record_verify_context verify;
  struct vcs_zcode_dht_delegation delegation;
  uint8_t node_id[32];
};

static void rf_fill(uint8_t *out, size_t n, uint8_t value)
{
  memset(out, value, n);
}

static bool rf_chain_accept(void *ctx,
                            const struct vcs_zcode_dht_delegation *delegation)
{
  int *calls = ctx;
  (*calls)++;
  return delegation->beacon_height == 120;
}

static bool rf_init(struct record_fixture *f, int *chain_calls)
{
  memset(f, 0, sizeof(*f));
  uint8_t online_pub[32], online_secret[32], noise[32], beacon[32], master[32];
  rf_fill(f->online_seed, 32, 0x22);
  ed25519_keypair(online_pub, online_secret, f->online_seed);
  memory_cleanse(online_secret, sizeof(online_secret));
  rf_fill(f->verify.network_genesis, 32, 0x01);
  rf_fill(noise, 32, 0x33);
  rf_fill(beacon, 32, 0x44);
  rf_fill(master, 32, 0x55);
  f->verify.now_unix = 1500;
  f->verify.chain_verify = rf_chain_accept;
  f->verify.chain_ctx = chain_calls;
  if (vcs_zcode_dht_delegation_sign(
          &f->delegation, f->verify.network_genesis, online_pub, noise, 120,
          beacon, 1000, 3000, 7, master) != VCS_ZCODE_DHT_DELEGATION_OK)
    return false;
  return vcs_zcode_dht_delegation_node_id(f->node_id, &f->delegation);
}

static void rf_record(struct record_fixture *f,
                      struct vcs_zcode_dht_record *record,
                      enum vcs_zcode_dht_record_kind kind)
{
  memset(record, 0, sizeof(*record));
  record->kind = kind;
  (void)snprintf(record->namespace_name, sizeof(record->namespace_name),
                 "science.study");
  memcpy(record->network_genesis, f->verify.network_genesis, 32);
  rf_fill(record->transport_root, 32, 0x71);
  memcpy(record->provider_node_id, f->node_id, 32);
  record->sequence = 11;
  record->not_before = 1200;
  record->expiry = 1800;
  record->delegation = f->delegation;
  if (kind == VCS_ZCODE_DHT_RECORD_POINTER)
    rf_fill(record->semantic_root, 32, 0x61);
  if (kind == VCS_ZCODE_DHT_RECORD_STORAGE_ACK)
    rf_fill(record->owner_group, 32, 0x81);
}

static int test_record_roundtrip(void)
{
  int failures = 0;
  TEST("zcode dht record: all signed kinds round-trip canonically") {
    struct record_fixture f;
    int chain_calls = 0;
    ASSERT(rf_init(&f, &chain_calls));
    for (int kind = VCS_ZCODE_DHT_RECORD_PROVIDER;
         kind <= VCS_ZCODE_DHT_RECORD_STORAGE_ACK; kind++) {
      struct vcs_zcode_dht_record record, parsed;
      rf_record(&f, &record, (enum vcs_zcode_dht_record_kind)kind);
      ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
                VCS_ZCODE_DHT_RECORD_OK);
      uint8_t wire[VCS_ZCODE_DHT_RECORD_WIRE_BYTES];
      ASSERT_EQ(vcs_zcode_dht_record_encode(&record, wire),
                VCS_ZCODE_DHT_RECORD_OK);
      if (kind == VCS_ZCODE_DHT_RECORD_PROVIDER) {
        uint8_t digest[32];
        char digest_hex[65];
        sha3_256(wire, sizeof(wire), digest);
        zcl_hex_encode(digest, sizeof(digest), digest_hex);
        ASSERT(strcmp(digest_hex,
                      "284d3f369bf3dd2644e4843f310b8bba1c4f64a4d081269f"
                      "5460dee197092839") == 0);
      }
      ASSERT_EQ(vcs_zcode_dht_record_parse(wire, sizeof(wire), &f.verify,
                                           &parsed),
                VCS_ZCODE_DHT_RECORD_OK);
      ASSERT_EQ((int)parsed.kind, kind);
      ASSERT_EQ(parsed.sequence, 11);
      ASSERT(memcmp(parsed.provider_node_id, f.node_id, 32) == 0);
      ASSERT(strcmp(parsed.namespace_name, "science.study") == 0);
    }
    ASSERT_EQ(chain_calls, 3);
    PASS();
  }
  _test_next:;
  return failures;
}

static int test_record_shape_and_windows(void)
{
  int failures = 0;
  TEST("zcode dht record: canonical roots, namespace and windows fail closed") {
    struct record_fixture f;
    int chain_calls = 0;
    ASSERT(rf_init(&f, &chain_calls));
    struct vcs_zcode_dht_record record;
    rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_PROVIDER);
    record.semantic_root[0] = 1;
    ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
              VCS_ZCODE_DHT_RECORD_ROOT);
    rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_POINTER);
    memset(record.semantic_root, 0, 32);
    ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
              VCS_ZCODE_DHT_RECORD_ROOT);
    rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_STORAGE_ACK);
    memset(record.owner_group, 0, 32);
    ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
              VCS_ZCODE_DHT_RECORD_OWNER_GROUP);
    rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_PROVIDER);
    (void)snprintf(record.namespace_name, sizeof(record.namespace_name),
                   "Science.Bad");
    ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
              VCS_ZCODE_DHT_RECORD_NAMESPACE);
    rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_PROVIDER);
    record.expiry = record.not_before + VCS_ZCODE_DHT_PROVIDER_MAX_SECONDS + 1;
    ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
              VCS_ZCODE_DHT_RECORD_WINDOW);
    PASS();
  }
  _test_next:;
  return failures;
}

static int test_record_adversarial(void)
{
  int failures = 0;
  TEST("zcode dht record: bounds, tamper, network and signer reject to zero") {
    struct record_fixture f;
    int chain_calls = 0;
    ASSERT(rf_init(&f, &chain_calls));
    struct vcs_zcode_dht_record record;
    rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_POINTER);
    ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
              VCS_ZCODE_DHT_RECORD_OK);
    uint8_t wire[VCS_ZCODE_DHT_RECORD_WIRE_BYTES + 1];
    ASSERT_EQ(vcs_zcode_dht_record_encode(&record, wire),
              VCS_ZCODE_DHT_RECORD_OK);
    struct vcs_zcode_dht_record parsed, zero;
    memset(&zero, 0, sizeof(zero));
    memset(&parsed, 0xa5, sizeof(parsed));
    ASSERT_EQ(vcs_zcode_dht_record_parse(wire, VCS_ZCODE_DHT_RECORD_WIRE_BYTES - 1,
                                         &f.verify, &parsed),
              VCS_ZCODE_DHT_RECORD_SIZE);
    ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
    ASSERT_EQ(chain_calls, 0);
    wire[80] ^= 1;
    ASSERT_EQ(vcs_zcode_dht_record_parse(wire, VCS_ZCODE_DHT_RECORD_WIRE_BYTES,
                                         &f.verify, &parsed),
              VCS_ZCODE_DHT_RECORD_SIGNATURE);
    ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
    wire[80] ^= 1;
    struct vcs_zcode_dht_record_verify_context wrong = f.verify;
    wrong.network_genesis[0] ^= 1;
    ASSERT_EQ(vcs_zcode_dht_record_parse(wire, VCS_ZCODE_DHT_RECORD_WIRE_BYTES,
                                         &wrong, &parsed),
              VCS_ZCODE_DHT_RECORD_NETWORK);
    uint8_t wrong_seed[32];
    rf_fill(wrong_seed, 32, 0x23);
    ASSERT_EQ(vcs_zcode_dht_record_sign(&record, wrong_seed),
              VCS_ZCODE_DHT_RECORD_SIGNER);
    PASS();
  }
  _test_next:;
  return failures;
}

static int test_record_conflicts(void)
{
  int failures = 0;
  TEST("zcode dht record: conflicting valid pointer slots are preserved") {
    struct record_fixture f;
    int chain_calls = 0;
    ASSERT(rf_init(&f, &chain_calls));
    struct vcs_zcode_dht_record a, b;
    rf_record(&f, &a, VCS_ZCODE_DHT_RECORD_POINTER);
    b = a;
    b.transport_root[0] ^= 1;
    ASSERT(vcs_zcode_dht_record_conflicts(&a, &b));
    b = a;
    b.sequence++;
    ASSERT(!vcs_zcode_dht_record_conflicts(&a, &b));
    b = a;
    b.semantic_root[0] ^= 1;
    ASSERT(!vcs_zcode_dht_record_conflicts(&a, &b));
    ASSERT(!vcs_zcode_dht_record_conflicts(&a, &a));
    PASS();
  }
  _test_next:;
  return failures;
}

static void rf_cleanup_store(const char *datadir)
{
  char path[512];
  (void)snprintf(path, sizeof(path), "%s/%s", datadir,
                 VCS_ZCODE_DHT_RECORD_STORE_FILE);
  (void)unlink(path);
  (void)snprintf(path, sizeof(path), "%s/zcode/dht", datadir);
  (void)rmdir(path);
  (void)snprintf(path, sizeof(path), "%s/zcode", datadir);
  (void)rmdir(path);
  (void)rmdir(datadir);
}

static int test_record_store_restart(void)
{
  int failures = 0;
  TEST("zcode dht records: conflicts persist canonically across cold restart") {
    struct record_fixture f;
    int chain_calls = 0;
    ASSERT(rf_init(&f, &chain_calls));
    struct vcs_zcode_dht_record a, b;
    rf_record(&f, &a, VCS_ZCODE_DHT_RECORD_POINTER);
    ASSERT_EQ(vcs_zcode_dht_record_sign(&a, f.online_seed),
              VCS_ZCODE_DHT_RECORD_OK);
    b = a;
    b.transport_root[0] ^= 1;
    ASSERT_EQ(vcs_zcode_dht_record_sign(&b, f.online_seed),
              VCS_ZCODE_DHT_RECORD_OK);
    struct vcs_zcode_dht_record_store *before =
        vcs_zcode_dht_record_store_create(f.verify.network_genesis);
    struct vcs_zcode_dht_record_store *after =
        vcs_zcode_dht_record_store_create(f.verify.network_genesis);
    ASSERT(before != NULL && after != NULL);
    ASSERT_EQ(vcs_zcode_dht_record_store_put(before, &a, 1500),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    ASSERT_EQ(vcs_zcode_dht_record_store_put(before, &a, 1500),
              VCS_ZCODE_DHT_RECORD_STORE_DUPLICATE);
    ASSERT_EQ(vcs_zcode_dht_record_store_put(before, &b, 1500),
              VCS_ZCODE_DHT_RECORD_STORE_CONFLICT);
    ASSERT_EQ(vcs_zcode_dht_record_store_count(before), 2);

    char datadir[] = "test-tmp/zcode_dht_records_XXXXXX";
    ASSERT(mkdtemp(datadir) != NULL);
    char error[160] = {0};
    ASSERT_EQ(vcs_zcode_dht_record_store_save(before, datadir, error,
                                               sizeof(error)),
              VCS_ZCODE_DHT_RECORD_STORE_OK);
    ASSERT_EQ(vcs_zcode_dht_record_store_load(after, datadir, &f.verify,
                                               error, sizeof(error)),
              VCS_ZCODE_DHT_RECORD_STORE_OK);
    ASSERT_EQ(vcs_zcode_dht_record_store_count(after), 2);
    uint8_t before_digest[32], after_digest[32];
    vcs_zcode_dht_record_store_digest(before, before_digest);
    vcs_zcode_dht_record_store_digest(after, after_digest);
    ASSERT(memcmp(before_digest, after_digest, 32) == 0);
    struct vcs_zcode_dht_record found[2];
    ASSERT_EQ(vcs_zcode_dht_record_store_query(
                  after, VCS_ZCODE_DHT_RECORD_POINTER, "science.study",
                  a.semantic_root, found, 2),
              2);
    ASSERT(vcs_zcode_dht_record_conflicts(&found[0], &found[1]));

    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", datadir,
                   VCS_ZCODE_DHT_RECORD_STORE_FILE);
    struct stat st;
    ASSERT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    ASSERT(fd >= 0);
    uint8_t byte = 0;
    ASSERT(pread(fd, &byte, 1, VCS_ZCODE_DHT_RECORD_STORE_HEADER_BYTES + 20) ==
           1);
    byte ^= 1;
    ASSERT(pwrite(fd, &byte, 1,
                  VCS_ZCODE_DHT_RECORD_STORE_HEADER_BYTES + 20) == 1);
    ASSERT(close(fd) == 0);
    ASSERT_EQ(vcs_zcode_dht_record_store_load(after, datadir, &f.verify,
                                               error, sizeof(error)),
              VCS_ZCODE_DHT_RECORD_STORE_CORRUPT);
    ASSERT_EQ(vcs_zcode_dht_record_store_count(after), 2);
    ASSERT_EQ(vcs_zcode_dht_record_store_save(before, datadir, error,
                                               sizeof(error)),
              VCS_ZCODE_DHT_RECORD_STORE_OK);
    struct vcs_zcode_dht_record_verify_context expired_verify = f.verify;
    expired_verify.now_unix = 1800;
    ASSERT_EQ(vcs_zcode_dht_record_store_load(after, datadir, &expired_verify,
                                               error, sizeof(error)),
              VCS_ZCODE_DHT_RECORD_STORE_OK);
    ASSERT_EQ(vcs_zcode_dht_record_store_count(after), 0);
    vcs_zcode_dht_record_store_free(after);
    vcs_zcode_dht_record_store_free(before);
    rf_cleanup_store(datadir);
    PASS();
  }
  _test_next:;
  return failures;
}

static int test_record_store_caps(void)
{
  int failures = 0;
  TEST("zcode dht records: root, provider and conflict caps are exact") {
    struct record_fixture f;
    int chain_calls = 0;
    ASSERT(rf_init(&f, &chain_calls));
    struct vcs_zcode_dht_record_store *store =
        vcs_zcode_dht_record_store_create(f.verify.network_genesis);
    ASSERT(store != NULL);
    struct vcs_zcode_dht_record record;
    for (size_t i = 0; i <= VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_ROOT; i++) {
      rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_POINTER);
      (void)snprintf(record.namespace_name, sizeof(record.namespace_name),
                     "science.root.%zu", i);
      record.transport_root[0] = (uint8_t)(i + 1);
      ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
                VCS_ZCODE_DHT_RECORD_OK);
      enum vcs_zcode_dht_record_store_result result =
          vcs_zcode_dht_record_store_put(store, &record, 1500);
      ASSERT_EQ(result, i < VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_ROOT
                            ? VCS_ZCODE_DHT_RECORD_STORE_ADDED
                            : VCS_ZCODE_DHT_RECORD_STORE_ROOT_CAP);
    }
    vcs_zcode_dht_record_store_free(store);

    store = vcs_zcode_dht_record_store_create(f.verify.network_genesis);
    ASSERT(store != NULL);
    for (size_t i = 0; i <= VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_PROVIDER; i++) {
      rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_PROVIDER);
      record.transport_root[0] = (uint8_t)(i & 0xffu);
      record.transport_root[1] = (uint8_t)(i >> 8);
      ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
                VCS_ZCODE_DHT_RECORD_OK);
      enum vcs_zcode_dht_record_store_result result =
          vcs_zcode_dht_record_store_put(store, &record, 1500);
      ASSERT_EQ(result, i < VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_PROVIDER
                            ? VCS_ZCODE_DHT_RECORD_STORE_ADDED
                            : VCS_ZCODE_DHT_RECORD_STORE_PROVIDER_CAP);
    }
    vcs_zcode_dht_record_store_free(store);

    store = vcs_zcode_dht_record_store_create(f.verify.network_genesis);
    ASSERT(store != NULL);
    for (size_t i = 0; i <= VCS_ZCODE_DHT_RECORD_STORE_MAX_CONFLICTS; i++) {
      rf_record(&f, &record, VCS_ZCODE_DHT_RECORD_POINTER);
      record.transport_root[0] = (uint8_t)(i + 1);
      ASSERT_EQ(vcs_zcode_dht_record_sign(&record, f.online_seed),
                VCS_ZCODE_DHT_RECORD_OK);
      enum vcs_zcode_dht_record_store_result result =
          vcs_zcode_dht_record_store_put(store, &record, 1500);
      ASSERT_EQ(result, i == 0
                            ? VCS_ZCODE_DHT_RECORD_STORE_ADDED
                            : i < VCS_ZCODE_DHT_RECORD_STORE_MAX_CONFLICTS
                                  ? VCS_ZCODE_DHT_RECORD_STORE_CONFLICT
                                  : VCS_ZCODE_DHT_RECORD_STORE_CONFLICT_CAP);
    }
    vcs_zcode_dht_record_store_free(store);
    PASS();
  }
  _test_next:;
  return failures;
}

static int test_record_store_sequence_and_expiry(void)
{
  int failures = 0;
  TEST("zcode dht records: sequence replay and expiry fail closed") {
    struct record_fixture f;
    int chain_calls = 0;
    ASSERT(rf_init(&f, &chain_calls));
    struct vcs_zcode_dht_record current, stale, next;
    rf_record(&f, &current, VCS_ZCODE_DHT_RECORD_PROVIDER);
    ASSERT_EQ(vcs_zcode_dht_record_sign(&current, f.online_seed),
              VCS_ZCODE_DHT_RECORD_OK);
    stale = current;
    stale.sequence--;
    ASSERT_EQ(vcs_zcode_dht_record_sign(&stale, f.online_seed),
              VCS_ZCODE_DHT_RECORD_OK);
    next = current;
    next.sequence++;
    ASSERT_EQ(vcs_zcode_dht_record_sign(&next, f.online_seed),
              VCS_ZCODE_DHT_RECORD_OK);
    struct vcs_zcode_dht_record_store *store =
        vcs_zcode_dht_record_store_create(f.verify.network_genesis);
    ASSERT(store != NULL);
    ASSERT_EQ(vcs_zcode_dht_record_store_put(store, &current, 1500),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    ASSERT_EQ(vcs_zcode_dht_record_store_put(store, &stale, 1500),
              VCS_ZCODE_DHT_RECORD_STORE_STALE);
    ASSERT_EQ(vcs_zcode_dht_record_store_put(store, &next, 1500),
              VCS_ZCODE_DHT_RECORD_STORE_ADDED);
    ASSERT_EQ(vcs_zcode_dht_record_store_count(store), 1);
    ASSERT_EQ(vcs_zcode_dht_record_store_put(store, &current, 1800),
              VCS_ZCODE_DHT_RECORD_STORE_EXPIRED);
    vcs_zcode_dht_record_store_free(store);
    PASS();
  }
  _test_next:;
  return failures;
}

int test_zcode_dht_record(void)
{
  int failures = 0;
  failures += test_record_roundtrip();
  failures += test_record_shape_and_windows();
  failures += test_record_adversarial();
  failures += test_record_conflicts();
  failures += test_record_store_restart();
  failures += test_record_store_sequence_and_expiry();
  failures += test_record_store_caps();
  printf("=== zcode_dht_record: %d failures ===\n", failures);
  return failures;
}
