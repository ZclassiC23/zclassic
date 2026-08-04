/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Generic local-sovereignty decisions for discovered objects. */

#ifndef ZCL_VCS_ZCODE_SOVEREIGNTY_POLICY_H
#define ZCL_VCS_ZCODE_SOVEREIGNTY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define VCS_ZCODE_SOVEREIGNTY_SERVICE_TYPE_BYTES 32u
#define VCS_ZCODE_SOVEREIGNTY_CLASSIFICATION_BYTES 32u

enum vcs_zcode_sovereignty_action {
  VCS_ZCODE_SOVEREIGNTY_DISCOVER = 0,
  VCS_ZCODE_SOVEREIGNTY_FETCH,
  VCS_ZCODE_SOVEREIGNTY_STORE,
  VCS_ZCODE_SOVEREIGNTY_INDEX,
  VCS_ZCODE_SOVEREIGNTY_SERVE,
  VCS_ZCODE_SOVEREIGNTY_FORWARD,
  VCS_ZCODE_SOVEREIGNTY_EXECUTE,
  VCS_ZCODE_SOVEREIGNTY_ACTION_COUNT
};

/* All fields are public object metadata. `publisher_zid` is the signed master
 * identity, never a private key. Zero means that dimension is unknown. */
struct vcs_zcode_sovereignty_subject {
  uint8_t semantic_root[32];
  uint8_t transport_root[32];
  uint8_t package_root[32];
  uint8_t publisher_zid[32];
  char service_type[VCS_ZCODE_SOVEREIGNTY_SERVICE_TYPE_BYTES];
  char local_classification[VCS_ZCODE_SOVEREIGNTY_CLASSIFICATION_BYTES];
};

typedef bool (*vcs_zcode_sovereignty_decide_fn)(
    void *ctx, enum vcs_zcode_sovereignty_action action,
    const struct vcs_zcode_sovereignty_subject *subject);

#endif /* ZCL_VCS_ZCODE_SOVEREIGNTY_POLICY_H */
