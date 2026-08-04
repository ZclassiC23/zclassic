/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Bounded, explicitly declared replication accounting. */
#ifndef ZCL_VCS_ZCODE_REPLICATION_H
#define ZCL_VCS_ZCODE_REPLICATION_H

#include "vcs/zcode_dht_record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_REPLICATION_TARGET 8u
#define VCS_ZCODE_REPLICATION_DURABLE_ACKS 5u
#define VCS_ZCODE_REPLICATION_DURABLE_GROUPS 3u

struct vcs_zcode_replication_status {
  size_t provider_hints;
  size_t valid_acks;
  size_t declared_owner_groups;
  size_t expired_acks;
  bool durable;
};

/* Records must already have passed signature/network authorization. Provider
 * IDs and owner-group declarations are de-duplicated. `durable` means only
 * five live ACKs across three declared groups; it is never proof of separate
 * operators, machines or failure domains. */
void vcs_zcode_replication_evaluate(
    const struct vcs_zcode_dht_record *records, size_t count,
    const char *namespace_name, const uint8_t transport_root[32],
    uint64_t now_unix, struct vcs_zcode_replication_status *out);

#endif
