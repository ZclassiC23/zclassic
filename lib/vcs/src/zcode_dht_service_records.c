/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Bounded signed-record operations inside the authenticated DHT. */

#include "zcode_dht_service_internal.h"

#include "crypto/sha3.h"

#include <string.h>

bool vcs_zcode_dht_message_is_request(enum vcs_zcode_dht_msg_kind kind)
{
  return kind == VCS_ZCODE_DHT_MSG_FIND_NODE ||
         kind == VCS_ZCODE_DHT_MSG_FIND_RECORD ||
         kind == VCS_ZCODE_DHT_MSG_STORE_RECORD;
}

const uint8_t *vcs_zcode_dht_message_query_id(
    const struct vcs_zcode_dht_msg *message)
{
  switch (message->kind) {
  case VCS_ZCODE_DHT_MSG_FIND_NODE: return message->find_node.query_id;
  case VCS_ZCODE_DHT_MSG_NODES: return message->nodes.query_id;
  case VCS_ZCODE_DHT_MSG_FIND_RECORD: return message->find_record.query_id;
  case VCS_ZCODE_DHT_MSG_RECORDS: return message->records.query_id;
  case VCS_ZCODE_DHT_MSG_STORE_RECORD: return message->store_record.query_id;
  case VCS_ZCODE_DHT_MSG_STORE_RESULT: return message->store_result.query_id;
  }
  return NULL;
}

const struct vcs_zcode_dht_delegation *vcs_zcode_dht_message_delegation(
    const struct vcs_zcode_dht_msg *message)
{
  switch (message->kind) {
  case VCS_ZCODE_DHT_MSG_FIND_NODE: return &message->find_node.delegation;
  case VCS_ZCODE_DHT_MSG_NODES: return &message->nodes.delegation;
  case VCS_ZCODE_DHT_MSG_FIND_RECORD:
    return &message->find_record.delegation;
  case VCS_ZCODE_DHT_MSG_RECORDS: return &message->records.delegation;
  case VCS_ZCODE_DHT_MSG_STORE_RECORD:
    return &message->store_record.delegation;
  case VCS_ZCODE_DHT_MSG_STORE_RESULT:
    return &message->store_result.delegation;
  }
  return NULL;
}

uint64_t vcs_zcode_dht_message_generation(
    const struct vcs_zcode_dht_msg *message)
{
  switch (message->kind) {
  case VCS_ZCODE_DHT_MSG_FIND_NODE:
    return message->find_node.session_generation;
  case VCS_ZCODE_DHT_MSG_NODES: return message->nodes.session_generation;
  case VCS_ZCODE_DHT_MSG_FIND_RECORD:
    return message->find_record.session_generation;
  case VCS_ZCODE_DHT_MSG_RECORDS: return message->records.session_generation;
  case VCS_ZCODE_DHT_MSG_STORE_RECORD:
    return message->store_record.session_generation;
  case VCS_ZCODE_DHT_MSG_STORE_RESULT:
    return message->store_result.session_generation;
  }
  return 0;
}

bool vcs_zcode_dht_response_matches_query(
    enum vcs_zcode_dht_msg_kind message_kind, enum query_kind query_kind)
{
  if (message_kind == VCS_ZCODE_DHT_MSG_NODES)
    return query_kind == QUERY_BOOTSTRAP || query_kind == QUERY_LOOKUP ||
           query_kind == QUERY_PROBE;
  if (message_kind == VCS_ZCODE_DHT_MSG_RECORDS)
    return query_kind == QUERY_RECORD_LOOKUP;
  return message_kind == VCS_ZCODE_DHT_MSG_STORE_RESULT &&
         query_kind == QUERY_RECORD_STORE;
}

static struct service_peer *records_peer_find(
    struct vcs_zcode_dht_service *service, uint64_t peer_id)
{
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PEERS; i++)
    if (service->peers[i].used && service->peers[i].peer_id == peer_id)
      return &service->peers[i];
  return NULL;
}

static struct service_record_operation *records_operation_find(
    struct vcs_zcode_dht_service *service, uint64_t id)
{
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_RECORD_OPERATIONS; i++)
    if (service->record_operations[i].used &&
        service->record_operations[i].id == id)
      return &service->record_operations[i];
  return NULL;
}

static bool records_outbound_push(struct vcs_zcode_dht_service *service,
                                  uint64_t peer_id, const uint8_t *wire,
                                  size_t wire_len)
{
  if (!wire || !wire_len || wire_len > VCS_ZCODE_DHT_MAX_FRAME_BYTES ||
      service->outbound_count >= VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND)
    return false;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_OUTBOUND; i++)
    if (!service->outbound[i].used) {
      service->outbound[i].used = true;
      service->outbound[i].peer_id = peer_id;
      service->outbound[i].len = wire_len;
      memcpy(service->outbound[i].wire, wire, wire_len);
      service->outbound_count++;
      return true;
    }
  return false;
}

static bool records_query_id(struct vcs_zcode_dht_service *service,
                             const struct service_peer *peer,
                             uint8_t out[16])
{
  uint8_t digest[32];
  struct sha3_256_ctx sha;
  service->serial++;
  sha3_256_init(&sha);
  sha3_256_write(&sha, (const uint8_t *)"zcl.dht.record.query.v1", 24);
  sha3_256_write(&sha, service->self_id, 32);
  sha3_256_write(&sha, (const uint8_t *)&peer->peer_id, 8);
  sha3_256_write(&sha, (const uint8_t *)&peer->session.generation, 8);
  sha3_256_write(&sha, (const uint8_t *)&service->serial, 8);
  sha3_256_finalize(&sha, digest);
  memcpy(out, digest, 16);
  return true;
}

static bool records_selector_equal(
    const struct vcs_zcode_dht_record_selector *a,
    const struct vcs_zcode_dht_record_selector *b)
{
  return a->kind == b->kind &&
         memcmp(a->namespace_name, b->namespace_name,
                VCS_ZCODE_DHT_RECORD_NAMESPACE_BYTES) == 0 &&
         memcmp(a->root, b->root, 32) == 0;
}

static void records_subject(const struct vcs_zcode_dht_record *record,
                            struct vcs_zcode_sovereignty_subject *subject)
{
  memset(subject, 0, sizeof(*subject));
  memcpy(subject->semantic_root, record->semantic_root, 32);
  memcpy(subject->transport_root, record->transport_root, 32);
  memcpy(subject->publisher_zid, record->delegation.doc.master_pubkey, 32);
  memcpy(subject->service_type, record->namespace_name,
         VCS_ZCODE_DHT_RECORD_NAMESPACE_BYTES);
}

static bool records_policy_allows(
    const struct vcs_zcode_dht_service *service,
    enum vcs_zcode_sovereignty_action action,
    const struct vcs_zcode_dht_record *record)
{
  struct vcs_zcode_sovereignty_subject subject;
  records_subject(record, &subject);
  return service->policy_decide &&
         service->policy_decide(service->policy_ctx, action, &subject);
}

static struct service_record_operation *records_operation_allocate(
    struct vcs_zcode_dht_service *service,
    enum service_record_operation_kind kind, uint64_t *id_out)
{
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_RECORD_OPERATIONS; i++) {
    struct service_record_operation *operation =
        &service->record_operations[i];
    if (operation->used)
      continue;
    memset(operation, 0, sizeof(*operation));
    operation->used = true;
    operation->kind = kind;
    operation->state = VCS_ZCODE_DHT_RECORD_OPERATION_PENDING;
    operation->id = service->next_record_operation_id++;
    if (!operation->id)
      operation->id = service->next_record_operation_id++;
    *id_out = operation->id;
    return operation;
  }
  return NULL;
}

static struct service_query *records_query_allocate(
    struct vcs_zcode_dht_service *service, struct service_peer *peer,
    enum query_kind kind, uint64_t operation_id,
    struct vcs_zcode_dht_time now)
{
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++) {
    struct service_query *query = &service->queries[i];
    if (query->used)
      continue;
    memset(query, 0, sizeof(*query));
    query->used = true;
    query->kind = kind;
    query->peer_id = peer->peer_id;
    query->generation = peer->session.generation;
    query->deadline_mono =
        now.monotonic_s + VCS_ZCODE_DHT_SERVICE_QUERY_TIMEOUT_S;
    query->record_operation_id = operation_id;
    if (!records_query_id(service, peer, query->id)) {
      memset(query, 0, sizeof(*query));
      return NULL;
    }
    return query;
  }
  return NULL;
}

static void fill_find_auth(struct vcs_zcode_dht_msg_find_record *message,
                           const struct vcs_zcode_dht_service *service,
                           const struct service_peer *peer,
                           const struct service_query *query)
{
  message->session_generation = peer->session.generation;
  memcpy(message->sender_node_id, service->self_id, 32);
  memcpy(message->query_id, query->id, 16);
  message->delegation = service->delegation;
}

bool vcs_zcode_dht_service_record_query_begin(
    struct vcs_zcode_dht_service *service, uint64_t peer_id,
    const struct vcs_zcode_dht_record_selector *selector,
    struct vcs_zcode_dht_time now, uint64_t *operation_id_out)
{
  if (!service || !service->enabled || !selector || !operation_id_out)
    return false;
  *operation_id_out = 0;
  struct service_peer *peer = records_peer_find(service, peer_id);
  if (!peer || !peer->connected || !peer->authenticated)
    return false;
  uint64_t operation_id = 0;
  struct service_record_operation *operation = records_operation_allocate(
      service, SERVICE_RECORD_LOOKUP, &operation_id);
  if (!operation)
    return false;
  operation->selector = *selector;
  struct service_query *query = records_query_allocate(
      service, peer, QUERY_RECORD_LOOKUP, operation_id, now);
  if (!query) {
    memset(operation, 0, sizeof(*operation));
    return false;
  }
  query->record_selector = *selector;
  struct vcs_zcode_dht_msg_find_record message;
  memset(&message, 0, sizeof(message));
  fill_find_auth(&message, service, peer, query);
  message.selector = *selector;
  uint8_t wire[VCS_ZCODE_DHT_FIND_RECORD_WIRE_BYTES];
  size_t wire_len = 0;
  if (vcs_zcode_dht_msg_serialize_find_record(
          &message, peer->session.transcript_hash, service->online_seed, wire,
          sizeof(wire), &wire_len) != VCS_ZCODE_DHT_OK ||
      !records_outbound_push(service, peer_id, wire, wire_len)) {
    memset(query, 0, sizeof(*query));
    memset(operation, 0, sizeof(*operation));
    return false;
  }
  service->find_record_sent++;
  *operation_id_out = operation_id;
  return true;
}

bool vcs_zcode_dht_service_record_store_begin(
    struct vcs_zcode_dht_service *service, uint64_t peer_id,
    const struct vcs_zcode_dht_record *record,
    struct vcs_zcode_dht_time now, uint64_t *operation_id_out)
{
  if (!service || !service->enabled || !record || !operation_id_out)
    return false;
  *operation_id_out = 0;
  struct service_peer *peer = records_peer_find(service, peer_id);
  if (!peer || !peer->connected || !peer->authenticated)
    return false;
  uint8_t record_wire[VCS_ZCODE_DHT_RECORD_WIRE_BYTES];
  if (vcs_zcode_dht_record_encode(record, record_wire) !=
          VCS_ZCODE_DHT_RECORD_OK ||
      memcmp(record->network_genesis, service->genesis, 32) != 0)
    return false;
  uint64_t operation_id = 0;
  struct service_record_operation *operation = records_operation_allocate(
      service, SERVICE_RECORD_STORE, &operation_id);
  if (!operation)
    return false;
  struct service_query *query = records_query_allocate(
      service, peer, QUERY_RECORD_STORE, operation_id, now);
  if (!query) {
    memset(operation, 0, sizeof(*operation));
    return false;
  }
  sha3_256(record_wire, sizeof(record_wire), query->record_digest);
  struct vcs_zcode_dht_msg_store_record message;
  memset(&message, 0, sizeof(message));
  message.session_generation = peer->session.generation;
  memcpy(message.sender_node_id, service->self_id, 32);
  memcpy(message.query_id, query->id, 16);
  message.delegation = service->delegation;
  message.record = *record;
  uint8_t wire[VCS_ZCODE_DHT_STORE_RECORD_WIRE_BYTES];
  size_t wire_len = 0;
  if (vcs_zcode_dht_msg_serialize_store_record(
          &message, peer->session.transcript_hash, service->online_seed, wire,
          sizeof(wire), &wire_len) != VCS_ZCODE_DHT_OK ||
      !records_outbound_push(service, peer_id, wire, wire_len)) {
    memset(query, 0, sizeof(*query));
    memset(operation, 0, sizeof(*operation));
    return false;
  }
  service->store_record_sent++;
  *operation_id_out = operation_id;
  return true;
}

bool vcs_zcode_dht_service_record_operation_poll(
    struct vcs_zcode_dht_service *service, uint64_t operation_id,
    struct vcs_zcode_dht_time now,
    struct vcs_zcode_dht_record_operation_result *out)
{
  if (!service || !out)
    return false;
  vcs_zcode_dht_service_tick(service, now);
  struct service_record_operation *operation =
      records_operation_find(service, operation_id);
  if (!operation)
    return false;
  memset(out, 0, sizeof(*out));
  out->state = operation->state;
  out->store_status = operation->store_status;
  out->record_count = operation->record_count;
  memcpy(out->records, operation->records,
         operation->record_count * sizeof(*operation->records));
  if (operation->state != VCS_ZCODE_DHT_RECORD_OPERATION_PENDING)
    memset(operation, 0, sizeof(*operation));
  return true;
}

bool vcs_zcode_dht_service_record_operation_cancel(
    struct vcs_zcode_dht_service *service, uint64_t operation_id)
{
  if (!service)
    return false;
  struct service_record_operation *operation =
      records_operation_find(service, operation_id);
  if (!operation)
    return false;
  for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_ACTIVE_QUERIES; i++)
    if (service->queries[i].used &&
        service->queries[i].record_operation_id == operation_id)
      memset(&service->queries[i], 0, sizeof(service->queries[i]));
  memset(operation, 0, sizeof(*operation));
  return true;
}

enum vcs_zcode_dht_record_store_result vcs_zcode_dht_service_record_admit(
    struct vcs_zcode_dht_service *service,
    const struct vcs_zcode_dht_record *record, struct vcs_zcode_dht_time now)
{
  if (!service || !service->enabled || !service->record_store)
    return VCS_ZCODE_DHT_RECORD_STORE_INVALID;
  if (!records_policy_allows(service, VCS_ZCODE_SOVEREIGNTY_STORE, record) ||
      !records_policy_allows(service, VCS_ZCODE_SOVEREIGNTY_INDEX, record))
    return VCS_ZCODE_DHT_RECORD_STORE_INVALID;
  enum vcs_zcode_dht_record_store_result result =
      vcs_zcode_dht_record_store_put(service->record_store, record,
                                     now.wall_unix);
  if (result == VCS_ZCODE_DHT_RECORD_STORE_ADDED ||
      result == VCS_ZCODE_DHT_RECORD_STORE_CONFLICT) {
    service->records_dirty = true;
    if (!service->persistence_dirty)
      service->dirty_since_mono = now.monotonic_s;
    service->persistence_dirty = true;
    service->persistence_generation++;
  }
  return result;
}

size_t vcs_zcode_dht_service_record_local_query(
    const struct vcs_zcode_dht_service *service, uint64_t now_unix,
    const struct vcs_zcode_dht_record_selector *selector,
    struct vcs_zcode_dht_record *out, size_t out_capacity)
{
  if (!service || !service->record_store || !selector)
    return 0;
  struct vcs_zcode_dht_record candidates[
      VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_ROOT];
  size_t found = vcs_zcode_dht_record_store_query(
      service->record_store, selector->kind, selector->namespace_name,
      selector->root, now_unix, candidates,
      VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_ROOT);
  if (found > VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_ROOT)
    found = VCS_ZCODE_DHT_RECORD_STORE_MAX_PER_ROOT;
  size_t allowed = 0;
  for (size_t i = 0; i < found; i++) {
    if (!records_policy_allows(service, VCS_ZCODE_SOVEREIGNTY_DISCOVER,
                               &candidates[i]))
      continue;
    if (allowed < out_capacity)
      out[allowed] = candidates[i];
    allowed++;
  }
  return allowed;
}

static bool records_publish_build(
    struct vcs_zcode_dht_service *service,
    const struct vcs_zcode_dht_publish_spec *spec,
    struct vcs_zcode_dht_record *record, uint8_t token[32])
{
  if (!service || !service->enabled || !service->record_store || !spec ||
      !record || !token)
    return false;
  memset(record, 0, sizeof(*record));
  record->kind = spec->kind;
  memcpy(record->namespace_name, spec->namespace_name,
         sizeof(record->namespace_name));
  memcpy(record->network_genesis, service->genesis, 32);
  memcpy(record->semantic_root, spec->semantic_root, 32);
  memcpy(record->transport_root, spec->transport_root, 32);
  memcpy(record->provider_node_id, service->self_id, 32);
  memcpy(record->owner_group, spec->owner_group, 32);
  record->sequence = spec->sequence;
  record->not_before = spec->not_before;
  record->expiry = spec->expiry;
  record->delegation = service->delegation;
  if (vcs_zcode_dht_record_sign(record, service->online_seed) !=
      VCS_ZCODE_DHT_RECORD_OK)
    return false;
  uint8_t wire[VCS_ZCODE_DHT_RECORD_WIRE_BYTES], store_digest[32];
  if (vcs_zcode_dht_record_encode(record, wire) !=
      VCS_ZCODE_DHT_RECORD_OK)
    return false;
  vcs_zcode_dht_record_store_digest(service->record_store, store_digest);
  struct sha3_256_ctx sha;
  sha3_256_init(&sha);
  sha3_256_write(&sha, (const uint8_t *)"zcl.dht.publish.plan.v1", 23);
  sha3_256_write(&sha, service->genesis, 32);
  sha3_256_write(&sha, store_digest, 32);
  sha3_256_write(&sha, wire, sizeof(wire));
  sha3_256_finalize(&sha, token);
  return true;
}

bool vcs_zcode_dht_service_record_publish_plan(
    struct vcs_zcode_dht_service *service,
    const struct vcs_zcode_dht_publish_spec *spec, uint8_t plan_token[32],
    struct vcs_zcode_dht_record *record_out)
{
  if (plan_token)
    memset(plan_token, 0, 32);
  if (record_out)
    memset(record_out, 0, sizeof(*record_out));
  return records_publish_build(service, spec, record_out, plan_token);
}

enum vcs_zcode_dht_record_store_result
vcs_zcode_dht_service_record_publish_commit(
    struct vcs_zcode_dht_service *service,
    const struct vcs_zcode_dht_publish_spec *spec,
    const uint8_t plan_token[32], struct vcs_zcode_dht_time now,
    struct vcs_zcode_dht_record *record_out)
{
  uint8_t expected[32], difference = 0;
  struct vcs_zcode_dht_record record;
  if (!plan_token || !records_publish_build(service, spec, &record, expected))
    return VCS_ZCODE_DHT_RECORD_STORE_INVALID;
  for (size_t i = 0; i < 32; i++)
    difference |= expected[i] ^ plan_token[i];
  if (difference)
    return VCS_ZCODE_DHT_RECORD_STORE_STALE;
  enum vcs_zcode_dht_record_store_result result =
      vcs_zcode_dht_service_record_admit(service, &record, now);
  if (record_out && (result == VCS_ZCODE_DHT_RECORD_STORE_ADDED ||
                     result == VCS_ZCODE_DHT_RECORD_STORE_DUPLICATE ||
                     result == VCS_ZCODE_DHT_RECORD_STORE_CONFLICT))
    *record_out = record;
  if (result == VCS_ZCODE_DHT_RECORD_STORE_ADDED ||
      result == VCS_ZCODE_DHT_RECORD_STORE_CONFLICT) {
    for (size_t i = 0; i < VCS_ZCODE_DHT_SERVICE_MAX_PUBLICATIONS; i++)
      if (!service->publications[i].used) {
        memset(&service->publications[i], 0, sizeof(service->publications[i]));
        service->publications[i].used = true;
        service->publications[i].record = record;
        break;
      }
    vcs_zcode_dht_service_publication_schedule(service, now);
  }
  return result;
}

void vcs_zcode_dht_service_publication_schedule(
    struct vcs_zcode_dht_service *service, struct vcs_zcode_dht_time now)
{
  if (!service || !service->enabled)
    return;
  for (size_t j = 0; j < VCS_ZCODE_DHT_SERVICE_MAX_PUBLICATIONS; j++) {
    struct service_publication *publication = &service->publications[j];
    if (!publication->used)
      continue;
    if (now.wall_unix >= publication->record.expiry ||
        publication->attempts >= VCS_ZCODE_DHT_SERVICE_MAX_PUBLICATIONS) {
      memset(publication, 0, sizeof(*publication));
      continue;
    }
    if (!records_policy_allows(service, VCS_ZCODE_SOVEREIGNTY_FORWARD,
                               &publication->record))
      continue;
    for (size_t p = 0; p < VCS_ZCODE_DHT_SERVICE_MAX_PEERS &&
                       publication->attempts < VCS_ZCODE_DHT_SERVICE_MAX_PUBLICATIONS;
         p++) {
      uint8_t mask = (uint8_t)(1u << (p & 7u));
      size_t byte = p >> 3;
      if ((publication->attempted_peer_slots[byte] & mask) != 0 ||
          !service->peers[p].used || !service->peers[p].connected ||
          !service->peers[p].authenticated)
        continue;
      uint64_t operation_id = 0;
      if (!vcs_zcode_dht_service_record_store_begin(
              service, service->peers[p].peer_id, &publication->record, now,
              &operation_id))
        return; /* shared three-query budget is full; next tick resumes */
      struct service_record_operation *operation =
          records_operation_find(service, operation_id);
      if (operation)
        operation->detached = true;
      publication->attempted_peer_slots[byte] |= mask;
      publication->attempts++;
    }
  }
}

static bool reply_records(struct vcs_zcode_dht_service *service,
                          struct service_peer *peer,
                          const struct vcs_zcode_dht_msg_find_record *request,
                          uint64_t now_unix)
{
  struct vcs_zcode_dht_msg_records response;
  memset(&response, 0, sizeof(response));
  response.session_generation = peer->session.generation;
  memcpy(response.sender_node_id, service->self_id, 32);
  memcpy(response.query_id, request->query_id, 16);
  response.delegation = service->delegation;
  response.selector = request->selector;
  size_t count = vcs_zcode_dht_service_record_local_query(
      service, now_unix, &request->selector, response.records,
      VCS_ZCODE_DHT_RECORDS_PER_FRAME);
  if (count > VCS_ZCODE_DHT_RECORDS_PER_FRAME)
    count = VCS_ZCODE_DHT_RECORDS_PER_FRAME;
  for (size_t i = 0; i < count; i++)
    if (records_policy_allows(service, VCS_ZCODE_SOVEREIGNTY_SERVE,
                              &response.records[i]))
      response.records[response.record_count++] = response.records[i];
  uint8_t wire[VCS_ZCODE_DHT_RECORDS_MAX_WIRE_BYTES];
  size_t wire_len = 0;
  if (vcs_zcode_dht_msg_serialize_records(
          &response, peer->session.transcript_hash, service->online_seed, wire,
          sizeof(wire), &wire_len) != VCS_ZCODE_DHT_OK ||
      !records_outbound_push(service, peer->peer_id, wire, wire_len))
    return false;
  service->records_sent++;
  return true;
}

static enum vcs_zcode_dht_store_status store_status(
    enum vcs_zcode_dht_record_store_result result)
{
  if (result == VCS_ZCODE_DHT_RECORD_STORE_ADDED)
    return VCS_ZCODE_DHT_STORE_STORED;
  if (result == VCS_ZCODE_DHT_RECORD_STORE_DUPLICATE)
    return VCS_ZCODE_DHT_STORE_DUPLICATE;
  if (result == VCS_ZCODE_DHT_RECORD_STORE_CONFLICT)
    return VCS_ZCODE_DHT_STORE_CONFLICT;
  return VCS_ZCODE_DHT_STORE_REJECTED;
}

static bool reply_store_result(
    struct vcs_zcode_dht_service *service, struct service_peer *peer,
    const struct vcs_zcode_dht_msg_store_record *request,
    enum vcs_zcode_dht_store_status status)
{
  uint8_t record_wire[VCS_ZCODE_DHT_RECORD_WIRE_BYTES];
  if (vcs_zcode_dht_record_encode(&request->record, record_wire) !=
      VCS_ZCODE_DHT_RECORD_OK)
    return false;
  struct vcs_zcode_dht_msg_store_result response;
  memset(&response, 0, sizeof(response));
  response.session_generation = peer->session.generation;
  memcpy(response.sender_node_id, service->self_id, 32);
  memcpy(response.query_id, request->query_id, 16);
  response.delegation = service->delegation;
  response.status = status;
  sha3_256(record_wire, sizeof(record_wire), response.record_digest);
  uint8_t wire[VCS_ZCODE_DHT_STORE_RESULT_WIRE_BYTES];
  size_t wire_len = 0;
  if (vcs_zcode_dht_msg_serialize_store_result(
          &response, peer->session.transcript_hash, service->online_seed, wire,
          sizeof(wire), &wire_len) != VCS_ZCODE_DHT_OK ||
      !records_outbound_push(service, peer->peer_id, wire, wire_len))
    return false;
  service->store_result_sent++;
  return true;
}

bool vcs_zcode_dht_service_records_handle(
    struct vcs_zcode_dht_service *service, struct service_peer *peer,
    struct service_query *query, const struct vcs_zcode_dht_msg *message,
    struct vcs_zcode_dht_time now,
    enum vcs_zcode_dht_reject_reason *rejected_out)
{
  if (rejected_out)
    *rejected_out = VCS_ZCODE_DHT_REJECT_CAP;
  if (message->kind == VCS_ZCODE_DHT_MSG_FIND_RECORD) {
    service->find_record_received++;
    return reply_records(service, peer, &message->find_record, now.wall_unix);
  }
  if (message->kind == VCS_ZCODE_DHT_MSG_STORE_RECORD) {
    if (!records_policy_allows(service, VCS_ZCODE_SOVEREIGNTY_DISCOVER,
                               &message->store_record.record)) {
      if (rejected_out)
        *rejected_out = VCS_ZCODE_DHT_REJECT_UNAUTHORIZED;
      return false;
    }
    if (peer->record_admissions >=
        VCS_ZCODE_DHT_SERVICE_MAX_RECORDS_PER_PEER)
      return false;
    peer->record_admissions++;
    enum vcs_zcode_dht_record_store_result admitted =
        vcs_zcode_dht_service_record_admit(service,
                                           &message->store_record.record, now);
    enum vcs_zcode_dht_store_status status = store_status(admitted);
    if (status == VCS_ZCODE_DHT_STORE_REJECTED) {
      if (rejected_out)
        *rejected_out = admitted == VCS_ZCODE_DHT_RECORD_STORE_STALE
                            ? VCS_ZCODE_DHT_REJECT_REPLAY
                            : admitted == VCS_ZCODE_DHT_RECORD_STORE_EXPIRED
                                  ? VCS_ZCODE_DHT_REJECT_EXPIRED
                                  : admitted ==
                                            VCS_ZCODE_DHT_RECORD_STORE_INVALID
                                        ? VCS_ZCODE_DHT_REJECT_POISONED
                                        : VCS_ZCODE_DHT_REJECT_CAP;
      return false;
    }
    service->store_record_received++;
    return reply_store_result(service, peer, &message->store_record, status);
  }
  if (!query)
    return false;
  struct service_record_operation *operation =
      records_operation_find(service, query->record_operation_id);
  if (!operation)
    return false;
  if (message->kind == VCS_ZCODE_DHT_MSG_RECORDS) {
    if (query->kind != QUERY_RECORD_LOOKUP ||
        !records_selector_equal(&query->record_selector,
                                &message->records.selector)) {
      if (rejected_out)
        *rejected_out = VCS_ZCODE_DHT_REJECT_POISONED;
      return false;
    }
    operation->record_count = message->records.record_count;
    memcpy(operation->records, message->records.records,
           operation->record_count * sizeof(*operation->records));
    operation->state = VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE;
    service->records_received++;
    if (operation->detached)
      memset(operation, 0, sizeof(*operation));
    return true;
  }
  if (message->kind == VCS_ZCODE_DHT_MSG_STORE_RESULT) {
    if (query->kind != QUERY_RECORD_STORE ||
        memcmp(query->record_digest, message->store_result.record_digest, 32) !=
            0) {
      if (rejected_out)
        *rejected_out = VCS_ZCODE_DHT_REJECT_POISONED;
      return false;
    }
    operation->store_status = message->store_result.status;
    operation->state = message->store_result.status ==
                               VCS_ZCODE_DHT_STORE_REJECTED
                           ? VCS_ZCODE_DHT_RECORD_OPERATION_REJECTED
                           : VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE;
    service->store_result_received++;
    if (operation->detached)
      memset(operation, 0, sizeof(*operation));
    return true;
  }
  return false;
}

void vcs_zcode_dht_service_record_query_finish(
    struct vcs_zcode_dht_service *service, const struct service_query *query,
    enum query_outcome outcome)
{
  if (!service || !query ||
      (query->kind != QUERY_RECORD_LOOKUP &&
       query->kind != QUERY_RECORD_STORE) ||
      outcome == QUERY_OUTCOME_RESPONSE)
    return;
  struct service_record_operation *operation =
      records_operation_find(service, query->record_operation_id);
  if (operation) {
    if (operation->detached)
      memset(operation, 0, sizeof(*operation));
    else
      operation->state = outcome == QUERY_OUTCOME_EXPIRED
                             ? VCS_ZCODE_DHT_RECORD_OPERATION_TIMEOUT
                             : VCS_ZCODE_DHT_RECORD_OPERATION_REJECTED;
  }
}
