/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_FILE_OFFER_H
#define ZCL_DB_MODEL_FILE_OFFER_H

#include "models/database.h"
#include "models/activerecord.h"
#include "net/file_market.h"
#include <stdbool.h>

/* ActiveRecord model: FileOffer (ZCL Market gossip)
 *
 * The record type is `struct file_offer` (defined in
 * net/file_market.h) — the same struct the P2P gossip layer and
 * SQLite persistence both use. The validator + callbacks live in
 * app/models/src/file_offer.c.
 *
 * Validation (db_file_offer_validate):
 *   - root_hash:    must be non-zero (a SHA3-256 manifest hash)
 *   - filename:     non-empty
 *   - size_bytes:   non-zero (no zero-size files in the market)
 *   - num_chunks:   positive (must cover at least one chunk)
 *   - price_per_mb: non-negative
 *   - z_addr:       non-zero (Sapling payment address must be set)
 *   - peer_port:    non-zero
 *   - last_seen:    non-negative
 *   - ttl:          1..FILE_MARKET_MAX_TTL inclusive
 */

struct ar_callbacks *db_file_offer_callbacks(void);
bool db_file_offer_validate(const struct file_offer *offer,
                            struct ar_errors *errors);
bool db_file_offer_save(struct node_db *ndb,
                        const struct file_offer *offer);
int db_file_offer_list(struct node_db *ndb,
                       struct file_offer *out, size_t max);
bool db_file_offer_find(struct node_db *ndb,
                        const uint8_t root_hash[32],
                        struct file_offer *out);
int db_file_offer_prune(struct node_db *ndb, int64_t max_age);

#endif
