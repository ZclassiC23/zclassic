/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: register and re-verify private seller content for paid delivery. */

#ifndef ZCL_SERVICE_FILE_MARKET_CONTENT_H
#define ZCL_SERVICE_FILE_MARKET_CONTENT_H

#include "base/result.h"
#include "models/market_content.h"
#include "net/file_market_delivery.h"

#include <stdint.h>

struct zcl_result file_market_content_register(
    struct node_db *ndb, const uint8_t offer_id[32], const char *content_path,
    int64_t now_unix, struct market_content_public_record *out);

/* Reopens the private regular file, reads exactly one registered chunk, and
 * verifies its digest. On success out->data is zcl_malloc-owned. */
struct zcl_result file_market_content_load_chunk(
    struct node_db *ndb, const uint8_t offer_id[32], uint32_t chunk_index,
    struct file_market_delivery_chunk *out);

#endif
