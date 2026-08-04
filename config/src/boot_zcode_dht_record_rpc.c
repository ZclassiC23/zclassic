/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Capability-owned public lifecycle for iterative DHT records. */

#include "config/boot_zcode_dht.h"

#include "base/hex.h"
#include "crypto/random_secret.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "rpc/server.h"
#include "util/sync.h"

#include <stdatomic.h>
#include <string.h>

#define RECORD_PUBLIC_LOOKUPS_MAX 32u
#define RECORD_PUBLIC_TOKEN_BYTES 16u
#define RECORD_PUBLIC_ACTIVE_GRACE_S 5u
#define RECORD_PUBLIC_RESULT_RETENTION_S 30u

struct public_record_lookup {
  bool used, cached;
  uint8_t lookup_token[RECORD_PUBLIC_TOKEN_BYTES];
  uint8_t owner_token[RECORD_PUBLIC_TOKEN_BYTES];
  uint64_t service_operation_id, service_generation, expires_mono;
  struct vcs_zcode_dht_record_discovery_result result;
};

static zcl_mutex_t g_record_public_lock;
static _Atomic int g_record_public_lock_state;
static struct public_record_lookup g_record_public[RECORD_PUBLIC_LOOKUPS_MAX];

static struct vcs_zcode_dht_time record_public_now(void) {
  return (struct vcs_zcode_dht_time){
      .wall_unix = (uint64_t)platform_time_wall_time_t(),
      .monotonic_s = (uint64_t)(platform_time_monotonic_ms() / 1000),
  };
}

static void record_public_lock(void) {
  if (atomic_load_explicit(&g_record_public_lock_state,
                           memory_order_acquire) != 2) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &g_record_public_lock_state, &expected, 1, memory_order_acq_rel,
            memory_order_acquire)) {
      zcl_mutex_init(&g_record_public_lock);
      atomic_store_explicit(&g_record_public_lock_state, 2,
                            memory_order_release);
    } else {
      while (atomic_load_explicit(&g_record_public_lock_state,
                                  memory_order_acquire) != 2)
        ;
    }
  }
  zcl_mutex_lock(&g_record_public_lock);
}

static bool record_token_equal(
    const uint8_t a[RECORD_PUBLIC_TOKEN_BYTES],
    const uint8_t b[RECORD_PUBLIC_TOKEN_BYTES]) {
  uint8_t difference = 0;
  for (size_t i = 0; i < RECORD_PUBLIC_TOKEN_BYTES; i++)
    difference |= a[i] ^ b[i];
  return difference == 0;
}

static struct public_record_lookup *record_public_find_locked(
    const uint8_t lookup_token[RECORD_PUBLIC_TOKEN_BYTES],
    const uint8_t owner_token[RECORD_PUBLIC_TOKEN_BYTES]) {
  for (size_t i = 0; i < RECORD_PUBLIC_LOOKUPS_MAX; i++)
    if (g_record_public[i].used &&
        record_token_equal(g_record_public[i].lookup_token, lookup_token) &&
        record_token_equal(g_record_public[i].owner_token, owner_token))
      return &g_record_public[i];
  return NULL;
}

static void record_public_cleanup_locked(uint64_t monotonic_s) {
  for (size_t i = 0; i < RECORD_PUBLIC_LOOKUPS_MAX; i++) {
    struct public_record_lookup *entry = &g_record_public[i];
    if (!entry->used || monotonic_s < entry->expires_mono)
      continue;
    if (!entry->cached)
      (void)boot_zcode_dht_record_discovery_cancel(
          entry->service_operation_id, entry->service_generation);
    memset(entry, 0, sizeof(*entry));
  }
}

void boot_zcode_dht_record_public_tick(uint64_t monotonic_s) {
  record_public_lock();
  record_public_cleanup_locked(monotonic_s);
  zcl_mutex_unlock(&g_record_public_lock);
}

void boot_zcode_dht_record_public_reset(void) {
  record_public_lock();
  memset(g_record_public, 0, sizeof(g_record_public));
  zcl_mutex_unlock(&g_record_public_lock);
}

static const struct json_value *record_rpc_input(
    const struct json_value *params) {
  const struct json_value *first =
      params && json_size(params) ? json_at(params, 0) : NULL;
  return first && first->type == JSON_OBJ ? first : NULL;
}

static const char *record_input_str(const struct json_value *in,
                                    const char *key) {
  const struct json_value *value = in ? json_get(in, key) : NULL;
  return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool record_input_root(const struct json_value *in, const char *key,
                              uint8_t out[32]) {
  const char *hex = record_input_str(in, key);
  memset(out, 0, 32);
  return hex && strlen(hex) == 64 && zcl_hex_decode_lower(hex, out, 32);
}

static bool record_input_namespace(const struct json_value *in, char out[32]) {
  const char *name = record_input_str(in, "namespace");
  size_t length = name ? strlen(name) : 0;
  memset(out, 0, 32);
  if (!length || length > VCS_ZCODE_DHT_RECORD_NAMESPACE_MAX)
    return false;
  for (size_t i = 0; i < length; i++)
    if (!((name[i] >= 'a' && name[i] <= 'z') ||
          (name[i] >= '0' && name[i] <= '9') || name[i] == '.' ||
          name[i] == '-' || name[i] == '_'))
      return false;
  memcpy(out, name, length);
  return true;
}

static enum vcs_zcode_dht_record_kind record_input_kind(
    const struct json_value *in) {
  const char *kind = record_input_str(in, "kind");
  if (kind && strcmp(kind, "provider") == 0)
    return VCS_ZCODE_DHT_RECORD_PROVIDER;
  if (kind && strcmp(kind, "pointer") == 0)
    return VCS_ZCODE_DHT_RECORD_POINTER;
  if (kind && strcmp(kind, "storage_ack") == 0)
    return VCS_ZCODE_DHT_RECORD_STORAGE_ACK;
  return 0;
}

static bool record_parse_selector(
    const struct json_value *in,
    struct vcs_zcode_dht_record_selector *selector) {
  memset(selector, 0, sizeof(*selector));
  selector->kind = record_input_kind(in);
  if (!selector->kind ||
      !record_input_namespace(in, selector->namespace_name))
    return false;
  const char *root_key = selector->kind == VCS_ZCODE_DHT_RECORD_POINTER
                             ? "semantic_root"
                             : "transport_root";
  return record_input_root(in, root_key, selector->root);
}

static bool record_parse_capability(
    const struct json_value *in,
    uint8_t lookup_token[RECORD_PUBLIC_TOKEN_BYTES],
    uint8_t owner_token[RECORD_PUBLIC_TOKEN_BYTES]) {
  const char *lookup = record_input_str(in, "lookup_id");
  const char *owner = record_input_str(in, "owner_token");
  return lookup && owner &&
         strlen(lookup) == RECORD_PUBLIC_TOKEN_BYTES * 2 &&
         strlen(owner) == RECORD_PUBLIC_TOKEN_BYTES * 2 &&
         zcl_hex_decode_lower(lookup, lookup_token,
                              RECORD_PUBLIC_TOKEN_BYTES) &&
         zcl_hex_decode_lower(owner, owner_token, RECORD_PUBLIC_TOKEN_BYTES);
}

static void record_rpc_error(struct json_value *result, const char *code,
                             const char *message) {
  json_set_object(result);
  json_push_kv_bool(result, "ok", false);
  json_push_kv_str(result, "code", code);
  json_push_kv_str(result, "message", message);
}

static const char *record_kind_name(enum vcs_zcode_dht_record_kind kind) {
  if (kind == VCS_ZCODE_DHT_RECORD_PROVIDER)
    return "provider";
  if (kind == VCS_ZCODE_DHT_RECORD_POINTER)
    return "pointer";
  return kind == VCS_ZCODE_DHT_RECORD_STORAGE_ACK ? "storage_ack" : "unknown";
}

static void record_row_json(struct json_value *row,
                            const struct vcs_zcode_dht_record *record) {
  char semantic[65], transport[65], provider[65], owner[65], publisher[65];
  zcl_hex_encode(record->semantic_root, 32, semantic);
  zcl_hex_encode(record->transport_root, 32, transport);
  zcl_hex_encode(record->provider_node_id, 32, provider);
  zcl_hex_encode(record->owner_group, 32, owner);
  zcl_hex_encode(record->delegation.doc.master_pubkey, 32, publisher);
  json_set_object(row);
  json_push_kv_str(row, "kind", record_kind_name(record->kind));
  json_push_kv_str(row, "namespace", record->namespace_name);
  json_push_kv_str(row, "semantic_root", semantic);
  json_push_kv_str(row, "transport_root", transport);
  json_push_kv_str(row, "provider_node_id", provider);
  json_push_kv_str(row, "publisher_zid", publisher);
  json_push_kv_str(row, "owner_group", owner);
  json_push_kv_int(row, "sequence", (int64_t)record->sequence);
  json_push_kv_int(row, "not_before", (int64_t)record->not_before);
  json_push_kv_int(row, "expiry", (int64_t)record->expiry);
  json_push_kv_bool(row, "possession_proof", false);
  json_push_kv_bool(row, "declared_diversity_only", true);
}

static const char *record_state_name(
    enum vcs_zcode_dht_record_operation_state state) {
  if (state == VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE)
    return "complete";
  if (state == VCS_ZCODE_DHT_RECORD_OPERATION_TIMEOUT)
    return "timeout";
  if (state == VCS_ZCODE_DHT_RECORD_OPERATION_REJECTED)
    return "rejected";
  return "pending";
}

static void record_result_json(
    struct json_value *result,
    const struct vcs_zcode_dht_record_discovery_result *discovery) {
  bool successful =
      discovery->state == VCS_ZCODE_DHT_RECORD_OPERATION_PENDING ||
      discovery->state == VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE;
  json_set_object(result);
  json_push_kv_bool(result, "ok", successful);
  json_push_kv_str(result, "state", record_state_name(discovery->state));
  json_push_kv_bool(result, "local_projection", false);
  json_push_kv_int(result, "routing_rounds", discovery->routing_rounds);
  json_push_kv_int(result, "xor_progress", discovery->xor_progress);
  json_push_kv_int(result, "nodes_queried", discovery->nodes_queried);
  if (!successful) {
    bool timeout =
        discovery->state == VCS_ZCODE_DHT_RECORD_OPERATION_TIMEOUT;
    json_push_kv_str(result, "code",
                     timeout ? "LOOKUP_TIMEOUT" : "LOOKUP_REJECTED");
    json_push_kv_str(result, "message",
                     timeout ? "record discovery reached its bounded deadline"
                             : "record discovery was interrupted");
  }
  json_push_kv_int(result, "count", discovery->record_count);
  struct json_value rows;
  json_init(&rows);
  json_set_array(&rows);
  for (uint32_t i = 0; i < discovery->record_count; i++) {
    struct json_value row;
    json_init(&row);
    record_row_json(&row, &discovery->records[i]);
    json_push_back(&rows, &row);
    json_free(&row);
  }
  json_push_kv(result, "records", &rows);
  json_free(&rows);
  struct json_value conflicts;
  json_init(&conflicts);
  json_set_array(&conflicts);
  uint32_t conflict_count = 0;
  for (uint32_t i = 0; i < discovery->record_count; i++) {
    bool conflict = false;
    for (uint32_t j = 0; j < i; j++)
      if (discovery->records[i].sequence == discovery->records[j].sequence &&
          memcmp(discovery->records[i].provider_node_id,
                 discovery->records[j].provider_node_id, 32) == 0) {
        conflict = true;
        break;
      }
    if (!conflict)
      continue;
    struct json_value row;
    json_init(&row);
    record_row_json(&row, &discovery->records[i]);
    json_push_back(&conflicts, &row);
    json_free(&row);
    conflict_count++;
  }
  json_push_kv_int(result, "conflict_count", conflict_count);
  json_push_kv(result, "conflicts", &conflicts);
  json_free(&conflicts);
}

static bool rpc_record_begin(const struct json_value *params, bool help,
                             struct json_value *result) {
  if (help) {
    json_set_str(result,
                 "zcode_dht_record_begin {kind,namespace,matching_root}");
    return true;
  }
  const struct json_value *in = record_rpc_input(params);
  struct vcs_zcode_dht_record_selector selector;
  if (!record_parse_selector(in, &selector)) {
    record_rpc_error(
        result, "INVALID_SELECTOR",
        "kind, canonical namespace and matching 64-hex root required");
    return true;
  }
  uint8_t tokens[RECORD_PUBLIC_TOKEN_BYTES * 2];
  if (!zcl_random_secret_bytes(tokens, sizeof(tokens),
                               "zcode_dht_public_record_lookup")) {
    record_rpc_error(result, "LOOKUP_ID_UNAVAILABLE",
                     "secure lookup capability generation failed");
    return true;
  }
  struct vcs_zcode_dht_time now = record_public_now();
  record_public_lock();
  record_public_cleanup_locked(now.monotonic_s);
  struct public_record_lookup *entry = NULL;
  bool collision = false;
  for (size_t i = 0; i < RECORD_PUBLIC_LOOKUPS_MAX; i++) {
    if (!g_record_public[i].used && !entry)
      entry = &g_record_public[i];
    if (g_record_public[i].used &&
        record_token_equal(g_record_public[i].lookup_token, tokens))
      collision = true;
  }
  uint64_t internal_id = 0, generation = 0;
  bool began = entry && !collision &&
               boot_zcode_dht_record_discovery_begin(
                   &selector, now, &internal_id, &generation);
  if (!began) {
    zcl_mutex_unlock(&g_record_public_lock);
    record_rpc_error(
        result, "LOOKUP_UNAVAILABLE",
        "DHT is disabled or its bounded discovery queue is full");
    return true;
  }
  memset(entry, 0, sizeof(*entry));
  entry->used = true;
  memcpy(entry->lookup_token, tokens, RECORD_PUBLIC_TOKEN_BYTES);
  memcpy(entry->owner_token, tokens + RECORD_PUBLIC_TOKEN_BYTES,
         RECORD_PUBLIC_TOKEN_BYTES);
  entry->service_operation_id = internal_id;
  entry->service_generation = generation;
  entry->expires_mono = now.monotonic_s + VCS_ZCODE_DHT_LOOKUP_CEILING_S +
                        VCS_ZCODE_DHT_SERVICE_QUERY_TIMEOUT_S +
                        RECORD_PUBLIC_ACTIVE_GRACE_S;
  char lookup_hex[RECORD_PUBLIC_TOKEN_BYTES * 2 + 1];
  char owner_hex[RECORD_PUBLIC_TOKEN_BYTES * 2 + 1];
  zcl_hex_encode(entry->lookup_token, RECORD_PUBLIC_TOKEN_BYTES, lookup_hex);
  zcl_hex_encode(entry->owner_token, RECORD_PUBLIC_TOKEN_BYTES, owner_hex);
  zcl_mutex_unlock(&g_record_public_lock);
  json_set_object(result);
  json_push_kv_bool(result, "ok", true);
  json_push_kv_str(result, "state", "pending");
  json_push_kv_str(result, "lookup_id", lookup_hex);
  json_push_kv_str(result, "owner_token", owner_hex);
  json_push_kv_int(result, "expires_in_seconds",
                   VCS_ZCODE_DHT_LOOKUP_CEILING_S +
                       VCS_ZCODE_DHT_SERVICE_QUERY_TIMEOUT_S +
                       RECORD_PUBLIC_ACTIVE_GRACE_S);
  return true;
}

static bool rpc_record_poll(const struct json_value *params, bool help,
                            struct json_value *result) {
  if (help) {
    json_set_str(result, "zcode_dht_record_poll {lookup_id,owner_token}");
    return true;
  }
  const struct json_value *in = record_rpc_input(params);
  uint8_t lookup_token[RECORD_PUBLIC_TOKEN_BYTES];
  uint8_t owner_token[RECORD_PUBLIC_TOKEN_BYTES];
  if (!record_parse_capability(in, lookup_token, owner_token)) {
    record_rpc_error(
        result, "INVALID_LOOKUP_CAPABILITY",
        "lookup_id and owner_token must be canonical 32-hex values");
    return true;
  }
  struct vcs_zcode_dht_time now = record_public_now();
  record_public_lock();
  record_public_cleanup_locked(now.monotonic_s);
  struct public_record_lookup *entry =
      record_public_find_locked(lookup_token, owner_token);
  if (!entry) {
    zcl_mutex_unlock(&g_record_public_lock);
    record_rpc_error(
        result, "LOOKUP_UNKNOWN",
        "lookup capability is unknown, expired, or not owned");
    return true;
  }
  if (!entry->cached && !boot_zcode_dht_record_discovery_poll(
                            entry->service_operation_id,
                            entry->service_generation, now, &entry->result)) {
    memset(entry, 0, sizeof(*entry));
    zcl_mutex_unlock(&g_record_public_lock);
    record_rpc_error(result, "LOOKUP_INTERRUPTED",
                     "DHT service restarted during record discovery");
    return true;
  }
  if (entry->result.state != VCS_ZCODE_DHT_RECORD_OPERATION_PENDING &&
      !entry->cached) {
    entry->cached = true;
    entry->expires_mono =
        now.monotonic_s + RECORD_PUBLIC_RESULT_RETENTION_S;
  }
  struct vcs_zcode_dht_record_discovery_result snapshot = entry->result;
  zcl_mutex_unlock(&g_record_public_lock);
  record_result_json(result, &snapshot);
  return true;
}

static bool rpc_record_cancel(const struct json_value *params, bool help,
                              struct json_value *result) {
  if (help) {
    json_set_str(result, "zcode_dht_record_cancel {lookup_id,owner_token}");
    return true;
  }
  const struct json_value *in = record_rpc_input(params);
  uint8_t lookup_token[RECORD_PUBLIC_TOKEN_BYTES];
  uint8_t owner_token[RECORD_PUBLIC_TOKEN_BYTES];
  if (!record_parse_capability(in, lookup_token, owner_token)) {
    record_rpc_error(
        result, "INVALID_LOOKUP_CAPABILITY",
        "lookup_id and owner_token must be canonical 32-hex values");
    return true;
  }
  struct vcs_zcode_dht_time now = record_public_now();
  record_public_lock();
  record_public_cleanup_locked(now.monotonic_s);
  struct public_record_lookup *entry =
      record_public_find_locked(lookup_token, owner_token);
  if (!entry) {
    zcl_mutex_unlock(&g_record_public_lock);
    record_rpc_error(
        result, "LOOKUP_UNKNOWN",
        "lookup capability is unknown, expired, or not owned");
    return true;
  }
  if (!entry->cached)
    (void)boot_zcode_dht_record_discovery_cancel(
        entry->service_operation_id, entry->service_generation);
  memset(entry, 0, sizeof(*entry));
  zcl_mutex_unlock(&g_record_public_lock);
  json_set_object(result);
  json_push_kv_bool(result, "ok", true);
  json_push_kv_bool(result, "canceled", true);
  return true;
}

void boot_zcode_dht_record_register_rpc(struct rpc_table *table) {
  const struct rpc_command commands[] = {
      {"zcode", "zcode_dht_record_begin", rpc_record_begin, true},
      {"zcode", "zcode_dht_record_poll", rpc_record_poll, true},
      {"zcode", "zcode_dht_record_cancel", rpc_record_cancel, true},
  };
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    rpc_table_must_append(table, &commands[i]);
}
