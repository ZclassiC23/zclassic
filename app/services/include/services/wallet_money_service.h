/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Current, identity-bound custody snapshot from existing wallet authorities. */

#ifndef ZCL_SERVICES_WALLET_MONEY_SERVICE_H
#define ZCL_SERVICES_WALLET_MONEY_SERVICE_H

#include "base/result.h"
#include "models/wallet_identity.h"

#include <stdbool.h>
#include <stdint.h>

struct json_value;
struct main_state;
struct node_db;

#define WALLET_MONEY_REASON_MAX 160

struct wallet_money_snapshot {
    struct wallet_identity_row identity;
    char wallet_scope[5];
    char status[16];                 /* CURRENT | UNKNOWN | STALE | CONFLICTED */
    bool complete;
    char reason[WALLET_MONEY_REASON_MAX + 1];

    int64_t confirmed_zat;
    int64_t pending_zat;
    int64_t encumbered_zat;
    int64_t intent_reserved_zat;
    int64_t lifetime_lab_spent_zat;
    int64_t agent_available_zat;

    int32_t tip_height;
    uint8_t tip_hash[32];
    int64_t observed_at;
    uint8_t snapshot_root[32];
};

/* Compose identity + vault readers + intent reservations + chain tip. No
 * independent balance arithmetic is stored; every call re-reads authorities. */
struct zcl_result wallet_money_snapshot_build(
    struct node_db *ndb, struct main_state *main_state,
    const char *wallet_scope, struct wallet_money_snapshot *out);

struct zcl_result wallet_money_snapshot_to_json(
    const struct wallet_money_snapshot *snapshot, struct json_value *out);

#endif
