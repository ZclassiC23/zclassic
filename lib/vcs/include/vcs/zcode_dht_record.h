/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical signed provider, pointer, and storage-ack DHT records. */

#ifndef ZCL_VCS_ZCODE_DHT_RECORD_H
#define ZCL_VCS_ZCODE_DHT_RECORD_H

#include "vcs/zcode_dht.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_DHT_RECORD_VERSION 1u
#define VCS_ZCODE_DHT_RECORD_NAMESPACE_BYTES 32u
#define VCS_ZCODE_DHT_RECORD_NAMESPACE_MAX 31u
#define VCS_ZCODE_DHT_RECORD_SIGNATURE_BYTES 64u
#define VCS_ZCODE_DHT_RECORD_WIRE_BYTES                                  \
  (12u + VCS_ZCODE_DHT_RECORD_NAMESPACE_BYTES + 5u * 32u + 3u * 8u +    \
   VCS_ZCODE_DHT_DELEGATION_WIRE_BYTES +                                \
   VCS_ZCODE_DHT_RECORD_SIGNATURE_BYTES)
#define VCS_ZCODE_DHT_PROVIDER_MAX_SECONDS UINT64_C(7200)
#define VCS_ZCODE_DHT_POINTER_MAX_SECONDS UINT64_C(604800)
#define VCS_ZCODE_DHT_STORAGE_ACK_MAX_SECONDS UINT64_C(604800)
#define VCS_ZCODE_DHT_RECORD_SIGNATURE_DOMAIN "zcl.zcode.dht.record.v1"

enum vcs_zcode_dht_record_kind {
  VCS_ZCODE_DHT_RECORD_PROVIDER = 1,
  VCS_ZCODE_DHT_RECORD_POINTER = 2,
  VCS_ZCODE_DHT_RECORD_STORAGE_ACK = 3,
};

enum vcs_zcode_dht_record_error {
  VCS_ZCODE_DHT_RECORD_OK = 0,
  VCS_ZCODE_DHT_RECORD_NULL,
  VCS_ZCODE_DHT_RECORD_SIZE,
  VCS_ZCODE_DHT_RECORD_MAGIC,
  VCS_ZCODE_DHT_RECORD_VERSION_ERROR,
  VCS_ZCODE_DHT_RECORD_KIND,
  VCS_ZCODE_DHT_RECORD_NAMESPACE,
  VCS_ZCODE_DHT_RECORD_ROOT,
  VCS_ZCODE_DHT_RECORD_OWNER_GROUP,
  VCS_ZCODE_DHT_RECORD_SEQUENCE,
  VCS_ZCODE_DHT_RECORD_WINDOW,
  VCS_ZCODE_DHT_RECORD_NOT_YET_VALID,
  VCS_ZCODE_DHT_RECORD_EXPIRED,
  VCS_ZCODE_DHT_RECORD_DELEGATION,
  VCS_ZCODE_DHT_RECORD_NETWORK,
  VCS_ZCODE_DHT_RECORD_PROVIDER_ID,
  VCS_ZCODE_DHT_RECORD_SIGNER,
  VCS_ZCODE_DHT_RECORD_SIGNATURE,
  VCS_ZCODE_DHT_RECORD_CHAIN,
};

const char *vcs_zcode_dht_record_error_string(
    enum vcs_zcode_dht_record_error error);

/* namespace is canonical lower-case ASCII with a zero tail. PROVIDER and
 * STORAGE_ACK address transport_root directly and require semantic_root=0.
 * POINTER binds semantic_root -> transport_root. owner_group is present only
 * on STORAGE_ACK and is explicitly declared diversity, never operator proof. */
struct vcs_zcode_dht_record {
  enum vcs_zcode_dht_record_kind kind;
  char namespace_name[VCS_ZCODE_DHT_RECORD_NAMESPACE_BYTES];
  uint8_t network_genesis[32];
  uint8_t semantic_root[32];
  uint8_t transport_root[32];
  uint8_t provider_node_id[32];
  uint8_t owner_group[32];
  uint64_t sequence;
  uint64_t not_before;
  uint64_t expiry;
  struct vcs_zcode_dht_delegation delegation;
  uint8_t signature[VCS_ZCODE_DHT_RECORD_SIGNATURE_BYTES];
};

struct vcs_zcode_dht_record_verify_context {
  uint8_t network_genesis[32];
  uint64_t now_unix;
  vcs_zcode_dht_chain_verify_fn chain_verify;
  void *chain_ctx;
};

/* Sign a fully populated record with the delegated online seed. The seed's
 * public key must equal delegation.online_pubkey. */
enum vcs_zcode_dht_record_error vcs_zcode_dht_record_sign(
    struct vcs_zcode_dht_record *record, const uint8_t online_seed[32]);

enum vcs_zcode_dht_record_error vcs_zcode_dht_record_encode(
    const struct vcs_zcode_dht_record *record,
    uint8_t wire[VCS_ZCODE_DHT_RECORD_WIRE_BYTES]);

/* Bounds and canonical structure are checked before delegation/signature/chain
 * work. `out` is zero on every rejection. */
enum vcs_zcode_dht_record_error vcs_zcode_dht_record_parse(
    const uint8_t *wire, size_t wire_len,
    const struct vcs_zcode_dht_record_verify_context *verify,
    struct vcs_zcode_dht_record *out);

/* True only for two distinct, valid-looking records occupying the same signed
 * sequence slot. Callers retain both as equivocation evidence. */
bool vcs_zcode_dht_record_conflicts(
    const struct vcs_zcode_dht_record *a,
    const struct vcs_zcode_dht_record *b);

#endif /* ZCL_VCS_ZCODE_DHT_RECORD_H */
