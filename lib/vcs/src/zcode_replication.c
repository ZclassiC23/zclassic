/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Evaluate bounded signed provider and storage-ACK evidence. */
#include "vcs/zcode_replication.h"

#include <string.h>

static bool seen32(const uint8_t values[][32], size_t count,
                   const uint8_t value[32]) {
  for (size_t i = 0; i < count; i++)
    if (memcmp(values[i], value, 32) == 0)
      return true;
  return false;
}

void vcs_zcode_replication_evaluate(
    const struct vcs_zcode_dht_record *records, size_t count,
    const char *namespace_name, const uint8_t transport_root[32],
    uint64_t now_unix, struct vcs_zcode_replication_status *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  if (!records || !namespace_name || !transport_root)
    return;
  uint8_t providers[VCS_ZCODE_REPLICATION_TARGET][32];
  uint8_t acknowledgers[VCS_ZCODE_REPLICATION_TARGET][32];
  uint8_t groups[VCS_ZCODE_REPLICATION_TARGET][32];
  size_t provider_count = 0, ack_count = 0, group_count = 0;
  for (size_t i = 0; i < count; i++) {
    const struct vcs_zcode_dht_record *record = &records[i];
    if (strcmp(record->namespace_name, namespace_name) != 0 ||
        memcmp(record->transport_root, transport_root, 32) != 0 ||
        now_unix < record->not_before)
      continue;
    if (record->kind == VCS_ZCODE_DHT_RECORD_PROVIDER &&
        now_unix < record->expiry && provider_count < VCS_ZCODE_REPLICATION_TARGET &&
        !seen32(providers, provider_count, record->provider_node_id)) {
      memcpy(providers[provider_count++], record->provider_node_id, 32);
    } else if (record->kind == VCS_ZCODE_DHT_RECORD_STORAGE_ACK) {
      if (now_unix >= record->expiry) {
        out->expired_acks++;
        continue;
      }
      if (ack_count >= VCS_ZCODE_REPLICATION_TARGET ||
          seen32(acknowledgers, ack_count, record->provider_node_id))
        continue;
      memcpy(acknowledgers[ack_count++], record->provider_node_id, 32);
      if (group_count < VCS_ZCODE_REPLICATION_TARGET &&
          !seen32(groups, group_count, record->owner_group))
        memcpy(groups[group_count++], record->owner_group, 32);
    }
  }
  out->provider_hints = provider_count;
  out->valid_acks = ack_count;
  out->declared_owner_groups = group_count;
  out->durable = ack_count >= VCS_ZCODE_REPLICATION_DURABLE_ACKS &&
                 group_count >= VCS_ZCODE_REPLICATION_DURABLE_GROUPS;
}
