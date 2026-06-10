/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_SNAPSHOT_MANIFEST_H
#define ZCL_SERVICES_SNAPSHOT_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#include "util/result.h"

struct byte_stream;

enum snapshot_manifest_result {
    SNAPSHOT_MANIFEST_OK = 0,
    SNAPSHOT_MANIFEST_NULL_ARG,
    SNAPSHOT_MANIFEST_TRUNCATED,
    SNAPSHOT_MANIFEST_TRAILING_BYTES,
    SNAPSHOT_MANIFEST_RANGE,
    SNAPSHOT_MANIFEST_STALE_SCHEMA,
    SNAPSHOT_MANIFEST_UNFINAL,
    SNAPSHOT_MANIFEST_WEAK_WORK,
    SNAPSHOT_MANIFEST_NO_MMR,
    SNAPSHOT_MANIFEST_NO_MMB,
    SNAPSHOT_MANIFEST_NOT_AHEAD,
};

struct snapshot_manifest {
    int32_t  height;
    uint8_t  block_hash[32];
    uint8_t  utxo_root[32];
    uint8_t  mmr_root[32];
    uint8_t  mmb_root[32];
    uint8_t  chain_work[32];
    uint64_t num_utxos;
    uint64_t total_bytes;
    uint32_t protocol_version;
    uint32_t snapshot_schema_version;
    int32_t  peer_tip_height;
};

struct zcl_result snapshot_manifest_parse(struct snapshot_manifest *out,
                                struct byte_stream *s,
                                enum snapshot_manifest_result *result);
enum snapshot_manifest_result snapshot_manifest_validate_offer(
    const struct snapshot_manifest *m,
    int32_t our_height);
enum snapshot_manifest_result snapshot_manifest_validate_recovery(
    const struct snapshot_manifest *m,
    int32_t target_height);
const char *snapshot_manifest_result_name(
    enum snapshot_manifest_result result);

#endif /* ZCL_SERVICES_SNAPSHOT_MANIFEST_H */
