/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Durable, encrypted transaction-intent plan records. */

#ifndef ZCL_MODELS_VAULT_INTENT_H
#define ZCL_MODELS_VAULT_INTENT_H

#include "models/activerecord.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "models/wallet_identity.h"

struct node_db;

#define VAULT_INTENT_PAYLOAD_MAX 16416
#define VAULT_INTENT_ERROR_MAX 63
#define VAULT_INTENT_RAW_MAX 200000

enum vault_intent_state {
    VAULT_INTENT_PLANNED = 0,
    VAULT_INTENT_PROVING = 1,
    VAULT_INTENT_MEMPOOL_ACCEPTED = 2,
    VAULT_INTENT_CONFIRMED = 3,
    VAULT_INTENT_FINALIZED = 4,
    VAULT_INTENT_REORGED = 5,
    VAULT_INTENT_CONFLICTED = 6,
    VAULT_INTENT_EXPIRED = 7,
    VAULT_INTENT_FAILED = 8
};

enum vault_intent_route {
    VAULT_INTENT_ROUTE_PRIVATE = 1,
    VAULT_INTENT_ROUTE_SHIELD = 2,
    VAULT_INTENT_ROUTE_UNSHIELD = 3,
    VAULT_INTENT_ROUTE_TRANSPARENT = 4,
    VAULT_INTENT_ROUTE_MIXED = 5
};

struct vault_intent_row {
    uint8_t plan_id[32];
    uint8_t digest[32];
    enum vault_intent_state state;
    enum vault_intent_route route;
    int64_t created_at;
    int64_t expires_at;
    int32_t anchor_height;
    uint8_t anchor_hash[32];
    uint8_t encrypted_payload[VAULT_INTENT_PAYLOAD_MAX];
    size_t encrypted_payload_len;
    bool has_txid;
    uint8_t txid[32];
    int32_t confirm_height;
    bool has_confirm_hash;
    uint8_t confirm_hash[32];
    char error_code[VAULT_INTENT_ERROR_MAX + 1];
    int64_t updated_at;
    char wallet_scope[5];
    char wallet_instance_id[WALLET_INSTANCE_ID_HEX_LEN + 1];
    char wallet_genesis[WALLET_GENESIS_HEX_LEN + 1];
    bool has_snapshot_root;
    uint8_t snapshot_root[32];
    int64_t recipient_value_zat;
    int64_t max_fee_zat;
    int64_t reserved_zat;
};

bool vault_intent_validate(const struct vault_intent_row *row,
                           struct ar_errors *errors);
bool vault_intent_save(struct node_db *ndb, const struct vault_intent_row *row);
/* Atomically check the wallet-wide reservation ceiling and insert the plan.
 * Dev additionally enforces reserve + lifetime lab allocation in the same
 * BEGIN IMMEDIATE as the insert. */
bool vault_intent_reserve(struct node_db *ndb,
                          const struct vault_intent_row *row,
                          int64_t confirmed_zat);
bool vault_intent_find(struct node_db *ndb, const uint8_t plan_id[32],
                       struct vault_intent_row *out);
int vault_intent_list(struct node_db *ndb, struct vault_intent_row *out,
                      size_t max);
bool vault_intent_claim_commit(struct node_db *ndb,
                               const uint8_t plan_id[32], int64_t now_unix);
bool vault_intent_reclaim_proving(struct node_db *ndb,
                                  const uint8_t plan_id[32],
                                  int64_t stale_before_unix,
                                  int64_t now_unix);
bool vault_intent_set_state(struct node_db *ndb, const uint8_t plan_id[32],
                            enum vault_intent_state state,
                            const uint8_t txid[32], const char *error_code,
                            int64_t now_unix);
bool vault_intent_set_confirmation(
    struct node_db *ndb, const uint8_t plan_id[32],
    enum vault_intent_state state, int32_t confirm_height,
    const uint8_t confirm_hash[32], int64_t now_unix);
bool vault_intent_expire_due(struct node_db *ndb, int64_t now_unix);
bool vault_intent_store_raw(struct node_db *ndb, const uint8_t plan_id[32],
                            const uint8_t *raw_tx, size_t raw_tx_len);
bool vault_intent_load_raw(struct node_db *ndb, const uint8_t plan_id[32],
                           uint8_t *out, size_t out_cap, size_t *out_len);
bool vault_intent_has_raw(struct node_db *ndb, const uint8_t plan_id[32]);
int64_t vault_intent_reserved_total(struct node_db *ndb,
                                    const char *wallet_scope,
                                    const char *wallet_instance_id);
const char *vault_intent_state_name(enum vault_intent_state state);

#endif
