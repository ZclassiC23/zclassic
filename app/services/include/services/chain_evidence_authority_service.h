/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Evidence Controller — native-first chain-state state machine.
 *
 * This service sits above chain_state_repository.  The repository is
 * the single writer for concrete tip pointers; this controller is the
 * evidence gate that decides whether a transition is allowed and
 * persists the publishable chain-state evidence that explains why.
 */

#ifndef ZCL_SERVICES_CHAIN_EVIDENCE_AUTHORITY_SERVICE_H
#define ZCL_SERVICES_CHAIN_EVIDENCE_AUTHORITY_SERVICE_H

#include "core/uint256.h"
#include "chain/chain.h"
#include "services/chain_state_service.h"
#include "util/result.h"

#include <stdbool.h>
#include <stdint.h>

struct node_db;

enum chain_evidence_controller_state {
    CEC_EMPTY = 0,
    CEC_HEADERS_WORK_VALIDATED,
    CEC_SNAPSHOT_UTXO_HASH_VERIFIED,
    CEC_TIP_FOLLOWING,
    CEC_BACKGROUND_VALIDATING,
    CEC_FULLY_VALIDATED,
    CEC_CONTRADICTION_FROZEN,
    CEC_NUM_STATES
};

enum chain_evidence_controller_result {
    CEC_OK = 0,
    CEC_REJECTED_NULL_ARG,
    CEC_REJECTED_FROZEN,
    CEC_REJECTED_BAD_STATE,
    CEC_REJECTED_BAD_PROOF,
    CEC_REJECTED_INCOMPLETE_INDEX_EVIDENCE,
    CEC_REJECTED_UTXO_AHEAD_OF_INDEX,
    CEC_REJECTED_CSR,
    CEC_REJECTED_PERSIST,
};

enum chain_evidence_source_class {
    CEC_SOURCE_CLASS_UNKNOWN = 0,
    CEC_SOURCE_CLASS_NATIVE_P2P,
    CEC_SOURCE_CLASS_SNAPSHOT,
    CEC_SOURCE_CLASS_LOCAL_IMPORT,
    CEC_SOURCE_CLASS_LEGACY_ADVISORY,
};

enum chain_evidence_publish_state {
    CEC_PUBLISH_NOT_PUBLISHABLE = 0,
    CEC_PUBLISH_LOCAL_EVIDENCE,
    CEC_PUBLISH_FROZEN_CONTRADICTION,
};

struct chain_evidence_controller {
    struct node_db *ndb;                 /* non-owning */
    struct chain_state_repository *csr;  /* non-owning */
    enum chain_evidence_controller_state state;
    char contradiction_reason[192];
};

struct chain_evidence_record {
    enum chain_evidence_source_class source_class;
    enum chain_evidence_publish_state publish_state;
    bool header_ancestry_linked;
    bool chainwork_recomputed;
    bool nakamoto_selected_best_work;
    bool block_bytes_hash_checked;
    bool utxo_sha3_verified;
    bool mmb_flyclient_proof_verified;
    bool chunk_hash_coverage_verified;
    bool full_validation_complete;
};

struct chain_evidence_controller_snapshot_meta {
    int32_t anchor_height;
    struct uint256 anchor_hash;
    struct uint256 utxo_sha3;
    uint64_t utxo_count;
    uint8_t chainwork[32];
    uint8_t mmb_root[32];
    uint32_t finality_depth;
    uint32_t schema_version;
    const char *producer;
    struct chain_evidence_record verified;
};

struct chain_evidence_controller_tip_request {
    struct block_index *new_tip;
    int utxo_max_height;
    bool update_header_tip;
    const char *reason;
    struct chain_evidence_record verified;
};

struct chain_evidence_controller_view {
    enum chain_evidence_controller_state state;
    int active_tip_height;
    int header_tip_height;
    int persisted_active_tip_height;
    int snapshot_anchor_height;
    int background_validation_height;
    int utxo_max_height;
    int coins_best_block_height;
    int sqlite_max_height;
    struct uint256 active_tip_hash;
    struct uint256 header_tip_hash;
    struct uint256 persisted_active_tip_hash;
    struct uint256 coins_best_block_hash;
    bool has_active_tip_hash;
    bool has_header_tip_hash;
    bool has_persisted_active_tip_hash;
    bool has_coins_best_block_hash;
    enum chain_evidence_source_class active_tip_source_class;
    enum chain_evidence_publish_state publish_state;
    bool missing_active_tip_evidence;
    bool publish_state_not_local;
    bool active_tip_hash_mismatch;
    bool csr_cursor_mismatch;
    bool repaired_active_tip_evidence;
    struct chain_evidence_record block_index_evidence_state;
    struct chain_evidence_record active_tip_evidence;
    struct chain_evidence_record snapshot_evidence;
    struct chain_evidence_record header_chain_evidence;
    char health_reason[128];
    char contradiction_reason[192];
};

const char *chain_evidence_controller_state_name(enum chain_evidence_controller_state state);
const char *chain_evidence_controller_result_name(enum chain_evidence_controller_result result);
const char *chain_evidence_source_class_name(enum chain_evidence_source_class source);
const char *chain_evidence_publish_state_name(enum chain_evidence_publish_state state);

void chain_evidence_controller_init(struct chain_evidence_controller *authority,
                         struct node_db *ndb,
                         struct chain_state_repository *csr);

enum chain_evidence_controller_state chain_evidence_controller_load_state(
    struct chain_evidence_controller *authority);

enum chain_evidence_controller_result chain_evidence_controller_import_snapshot_evidence(
    struct chain_evidence_controller *authority,
    const struct chain_evidence_controller_snapshot_meta *snapshot);

enum chain_evidence_controller_result chain_evidence_controller_promote_tip(
    struct chain_evidence_controller *authority,
    const struct chain_evidence_controller_tip_request *request);

/* Guard A (coins-durability): the genuine applied coins frontier is the height
 * of the block whose hash is coins_best_block (the connect_block authority).
 * cec.coins_best_block_height must NEVER be persisted above it, or recovery
 * anchors promoted ahead of the applied coins poison MAX(ok=1) (the
 * coins-durability desync). Returns min(requested_height, frontier); refusal-
 * only — returns requested_height unchanged when coins_best_block is zero or
 * unresolvable (fresh datadir / snapshot anchor), so a legitimate advance is
 * never blocked. On the normal forward path coins==tip so this is a no-op. */
int chain_evidence_clamp_coins_height_to_frontier(
    struct chain_evidence_controller *authority, int requested_height);



enum chain_evidence_controller_result chain_evidence_controller_mark_fully_validated(
    struct chain_evidence_controller *authority,
    const struct uint256 *utxo_sha3);

void chain_evidence_controller_freeze(struct chain_evidence_controller *authority,
                           const char *reason);

void chain_evidence_controller_snapshot(struct chain_evidence_controller *authority,
                             struct chain_evidence_controller_view *out);

struct zcl_result chain_evidence_controller_mark_block_evidence(
    struct chain_evidence_controller *authority,
    const struct uint256 *block_hash,
    const struct chain_evidence_record *evidence);

bool chain_evidence_record_has_block_index_required(
    const struct chain_evidence_record *evidence);

bool chain_evidence_record_has_snapshot_required(
    const struct chain_evidence_record *evidence);

#ifdef ZCL_TESTING
void chain_evidence_controller_test_fail_commit_after_csr(bool fail);
/* Clears the process-lifetime startup-reconcile once-guard so each test
 * case gets a fresh reconcile run (production never resets it). */
void chain_evidence_controller_test_reset_startup_reconcile(void);
#endif

#endif /* ZCL_SERVICES_CHAIN_EVIDENCE_AUTHORITY_SERVICE_H */
