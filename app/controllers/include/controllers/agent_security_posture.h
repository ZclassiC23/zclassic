/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_AGENT_SECURITY_POSTURE_H
#define ZCL_CONTROLLERS_AGENT_SECURITY_POSTURE_H

#include <stdbool.h>
#include <stdint.h>

struct json_value;
struct node_db;

struct agent_security_posture {
    bool review_required;
    bool node_db_available;
    bool progress_store_available;
    bool snapshot_evidence_present;
    bool trusted_state_present;
    bool snapshot_full_validation_complete;
    bool snapshot_utxo_sha3_verified;
    bool snapshot_flyclient_verified;
    bool snapshot_chunk_hash_coverage_verified;
    bool anchor_cursor_known;
    bool anchor_history_complete;
    bool anchor_backfill_gap;
    bool nullifier_cursor_known;
    bool nullifier_history_complete;
    bool nullifier_backfill_gap;
    int snapshot_anchor_height;
    int64_t sprout_anchor_activation_cursor;
    int64_t sapling_anchor_activation_cursor;
    int64_t nullifier_activation_cursor;
    char status[48];
    char bootstrap_model[64];
    char full_history_validation_state[48];
    char anchor_history_state[64];
    char nullifier_history_state[64];
    char next_action[96];
};

void agent_security_posture_collect(struct agent_security_posture *out,
                                    struct node_db *ndb);
void agent_push_security_posture_json(struct json_value *out, const char *key,
                                      struct node_db *ndb);

#endif /* ZCL_CONTROLLERS_AGENT_SECURITY_POSTURE_H */
