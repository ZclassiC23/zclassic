/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Contained validation of the canonical external consensus-state bundle. */

#ifndef ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_H
#define ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_H

#include <stdbool.h>
#include <stdint.h>

#include "storage/consensus_state_bundle_codec.h"
#include "util/result.h"

struct sqlite3;
struct consensus_state_artifact_evidence;
struct consensus_state_publication_decision_record;

/* Legacy USS v1-v3 artifacts may be converted by separate import adapters.
 * They are never active schemas and are never passed directly to this API. */
enum consensus_state_install_failpoint {
    CONSENSUS_INSTALL_FAIL_NONE = 0,
    CONSENSUS_INSTALL_FAIL_AFTER_BUNDLE_OPEN,
    CONSENSUS_INSTALL_FAIL_AFTER_BUNDLE_VALIDATE,
};

struct consensus_state_snapshot_install_request {
    /* Immutable external SQLite bundle. It holds `bundle_meta`, `coins`,
     * `anchors`, and `nullifiers`; rows are streamed by SQLite, never loaded
     * into process memory or staged inside the active progress.kv WAL. */
    const char *bundle_path;
    /* Caller assertion only. It catches selecting the wrong artifact but is
     * not local-chain proof and never authorizes activation. The protected
     * adapter will replace this pair with opaque digest-bound evidence after
     * resolving selected-chain ancestry and the Sapling header root. */
    int32_t expected_height;
    uint8_t expected_block_hash[32];
    enum consensus_state_install_failpoint failpoint;
};

enum consensus_state_install_status {
    CONSENSUS_INSTALL_REFUSED = 0,
    CONSENSUS_INSTALL_INJECTED_FAILURE = 1,
    CONSENSUS_INSTALL_STORE_ERROR = 2,
    CONSENSUS_INSTALL_VERIFIED_CONTAINED = 3,
    /* ACTIVATE mode terminal: the complete bundle (coins+anchors+nullifiers+
     * cursors) was atomically installed into the live progress store and its
     * recomputed UTXO root/count matched the manifest. */
    CONSENSUS_INSTALL_ACTIVATED = 4,
    /* SQLite returned an error at a commit/rollback boundary and the caller
     * cannot prove which generation is durable. The preinstall backup is the
     * recovery authority; callers must terminate and inspect/restore it. */
    CONSENSUS_INSTALL_COMMIT_OUTCOME_UNKNOWN = 5,
};

struct consensus_state_install_result {
    enum consensus_state_install_status status;
    bool history_complete;
    bool source_clean;
    uint8_t validation_profile;
    int32_t height;
    char reason[192];
};

/* A candidate is a complete, immutable progress-store generation built from
 * already-admitted artifact evidence. It is deliberately NOT the active
 * progress.kv, progress_store_open() refuses this schema if manually renamed,
 * and this API has no activation/promotion operation. */
#define CONSENSUS_STATE_CANDIDATE_SCHEMA \
    "zcl.consensus_state_candidate.v1"

enum consensus_state_candidate_failpoint {
    CONSENSUS_CANDIDATE_FAIL_NONE = 0,
    CONSENSUS_CANDIDATE_FAIL_AFTER_STAGING_OPEN,
    CONSENSUS_CANDIDATE_FAIL_AFTER_SCHEMA,
    CONSENSUS_CANDIDATE_FAIL_AFTER_COINS,
    CONSENSUS_CANDIDATE_FAIL_AFTER_ANCHORS,
    CONSENSUS_CANDIDATE_FAIL_AFTER_NULLIFIERS,
    CONSENSUS_CANDIDATE_FAIL_AFTER_PROVENANCE,
    CONSENSUS_CANDIDATE_FAIL_AFTER_COMMIT,
    CONSENSUS_CANDIDATE_FAIL_AFTER_REOPEN,
};

enum consensus_state_candidate_status {
    CONSENSUS_CANDIDATE_REFUSED = 0,
    CONSENSUS_CANDIDATE_INJECTED_FAILURE = 1,
    CONSENSUS_CANDIDATE_OUTPUT_ERROR = 2,
    CONSENSUS_CANDIDATE_VERIFIED_CONTAINED = 3,
};

struct consensus_state_candidate_request {
    /* Borrowed directory capability. output_name must be one normalized path
     * component, must not name the active progress.kv family, and must not
     * already exist. Candidate publication uses no-replace. */
    int output_dir_fd;
    const char *output_name;
    enum consensus_state_candidate_failpoint failpoint;
};

struct consensus_state_candidate_result {
    enum consensus_state_candidate_status status;
    bool source_clean;
    uint8_t validation_profile;
    int32_t height;
    uint64_t utxo_count;
    uint64_t anchor_count;
    uint64_t nullifier_count;
    uint8_t artifact_digest[32];
    uint8_t candidate_file_digest[32];
    char reason[192];
};

/* Opaque evidence over one exact, validated bundle file description. The
 * implementation keeps both the O_NOFOLLOW descriptor and its immutable
 * SQLite read transaction open until free. Callers can inspect a copy of the
 * validated manifest, but cannot manufacture evidence from manifest fields.
 *
 * This is artifact admission evidence only. It is deliberately not selected-
 * chain evidence, lane authority, or publication permission. */
struct zcl_result consensus_state_artifact_evidence_open(
    const char *bundle_path,
    struct consensus_state_artifact_evidence **out);
void consensus_state_artifact_evidence_free(
    struct consensus_state_artifact_evidence *evidence);
bool consensus_state_artifact_evidence_manifest_copy(
    const struct consensus_state_artifact_evidence *evidence,
    struct consensus_state_bundle_manifest *out);
bool consensus_state_artifact_evidence_digest(
    const struct consensus_state_artifact_evidence *evidence,
    uint8_t out[32]);
/* Re-hash the complete pinned SQLite file and recheck its descriptor metadata.
 * Required immediately before and after any long evidence-capture operation. */
bool consensus_state_artifact_evidence_revalidate(
    const struct consensus_state_artifact_evidence *evidence);
/* Domain-separated binding of logical artifact digest, complete-file SHA3,
 * and the exact local file identity admitted by the validator. */
bool consensus_state_artifact_evidence_receipt_digest(
    const struct consensus_state_artifact_evidence *evidence,
    uint8_t out[32]);

/* Stream a history-complete admitted bundle into a separate FULL-durable
 * SQLite progress generation.  The builder closes and independently reopens
 * the staged inode, verifies component parity and reducer conventions, then
 * links the read-only candidate at output_name with no replacement.  Every
 * pre-publication failure removes staging and leaves no final output.  The
 * active progress store is never opened or mutated by this operation. */
bool consensus_state_snapshot_candidate_build(
    const struct consensus_state_artifact_evidence *evidence,
    const struct consensus_state_candidate_request *request,
    struct consensus_state_candidate_result *result);

#ifdef ZCL_TESTING
/* One-shot race hook invoked after immutable validation/digest and immediately
 * before the final name/sidecar recheck. */
void consensus_state_snapshot_candidate_test_set_before_link_hook(
    void (*hook)(void *), void *ctx);
#endif

/* Validate the external zcl.consensus_state_bundle.v1 without publishing it.
 * This deliberately returns VERIFIED_CONTAINED for a valid bundle: the final
 * atomic publisher remains unavailable until proof authority, rollback state,
 * and kill/reopen evidence are durably bound. */
bool consensus_state_snapshot_install(
    struct sqlite3 *progress_db,
    const struct consensus_state_snapshot_install_request *request,
    struct consensus_state_install_result *result);

/* ── ACTIVATE mode (the sovereign shielded-state cure — consumer side) ──────
 *
 * PRODUCTION CONTAINMENT: this transaction engine is currently enabled only
 * under ZCL_TESTING for hermetic copy-proof. A bundle's producer/source fields
 * and receipt digests are carried by that same untrusted bundle; selected-chain
 * H/hash/Sapling-frontier binding does not authenticate UTXOs, Sprout history,
 * historical Sapling anchors, or nullifiers because ZClassic headers commit
 * none of those sets. Production calls therefore return
 * VERIFIED_CONTAINED before touching the store. Remove that containment only
 * after an independently replay-bound receipt outside the artifact is wired
 * and adversarially tested.
 *
 * Atomically install a complete, history-complete bundle's coins + Sprout/
 * Sapling anchors + nullifiers + the 8 reducer stage cursors into the LIVE
 * progress store, mirroring boot_refold_from_anchor_reset's cutover discipline
 * (cursors forced to the bundle height, coins_applied_height = height+1,
 * tip_finalize seeded AT the height) — BUT installing the complete shielded
 * history with activation cursor 0 instead of the empty-anchor/positive-cursor
 * reset that produced the anchor_backfill_gap wedge. The whole install is ONE
 * BEGIN IMMEDIATE transaction so it lands or rolls back atomically. All three
 * destination commitments, the in-transaction tip seed, and exact terminal
 * H-star/served/coins frontiers are required BEFORE COMMIT. The durable ADMIT's
 * frontier is re-captured under that same lock, so a stale CAS cannot apply.
 * Mixed provenance is forbidden: the bundle is the atomic unit — coins,
 * anchors, and nullifiers are all replaced together, never installed beside a
 * borrowed set. Superseded full-replay, nullifier-backfill, and refold session
 * markers are cleared in the same transaction.
 *
 * A physically restorable prior generation of the progress store is captured
 * with `VACUUM INTO` from the already-open singleton through the classified
 * directory capability while holding the process transaction lock. The code
 * then immediately takes `BEGIN IMMEDIATE` and requires SQLite's data-version,
 * total-change counter, and file identity to remain unchanged across that
 * boundary, before the first cutover write. It is independently reopened,
 * quick-checked, sidecar-checked, file-fsynced, and parent-directory-fsynced
 * before cutover, so an operator who later decides to abort a committed install
 * can restore the exact prior logical generation. (The immutable-
 * generation machinery in
 * consensus_state_snapshot_candidate.c builds a FORWARD generation from a bundle
 * and never snapshots the CURRENT store, so it cannot serve as the prior-gen
 * restore.) `result->prior_generation_path` names it.
 *
 * Fail-closed: every refusal sets a typed status + named reason. A SQLite
 * commit boundary whose outcome cannot be proven returns the distinct
 * COMMIT_OUTCOME_UNKNOWN terminal and points at the durable backup; it never
 * claims rollback. Returns true ONLY on a fully activated + verified install
 * (status ACTIVATED). */
struct consensus_state_activate_request {
    const char *bundle_path;
    /* Mandatory exact artifact receipt from the ADMIT decision. Activation
     * reopens bundle_path, recomputes this descriptor/file/inode binding, and
     * refuses unless it is byte-for-byte identical. Height/hash alone cannot
     * authorize state because ZClassic headers do not commit the state sets. */
    uint8_t expected_artifact_receipt_digest[32];
    int32_t expected_height;
    uint8_t expected_block_hash[32];
    /* The exact durable ADMIT record. Activation revalidates its digest and
     * all artifact/height/hash bindings, then checks its expected frontier
     * inside the same progress-store transaction that performs the cutover. */
    const struct consensus_state_publication_decision_record
        *publication_decision;
    /* Retained directory capability classified by the boot adapter. It must
     * be the capability through which the singleton progress store was
     * opened; the rollback generation is created and fsynced relative to it.
     * datadir_display is never authority and is used only in operator text. */
    int datadir_fd;
    const char *datadir_display;
};

struct consensus_state_activate_result {
    enum consensus_state_install_status status;
    bool activated;
    int32_t height;
    int32_t hstar;                  /* durable H* after install (cursors-reported) */
    int32_t coins_applied_height;   /* utxo_apply next-height frontier after install */
    uint64_t utxo_count;
    uint64_t anchor_count;
    uint64_t nullifier_count;
    char prior_generation_path[256];
    char reason[192];
};

bool consensus_state_snapshot_install_activate(
    struct sqlite3 *progress_db,
    const struct consensus_state_activate_request *request,
    struct consensus_state_activate_result *result);

#ifdef ZCL_TESTING
/* Production containment remains the default even in the test binary. The
 * hermetic activation transaction is reachable only while this explicit seam
 * is enabled by the copy-proof test. */
void consensus_state_snapshot_install_activate_test_set_independent_authority(
    bool available);
/* One-shot race hook after the durable prior-generation copy but immediately
 * before BEGIN IMMEDIATE. Used to prove the data-version fence catches an
 * out-of-band SQLite commit in that narrow boundary. */
void consensus_state_snapshot_install_activate_test_set_after_backup_hook(
    void (*hook)(void *), void *ctx);
/* One-shot race hook after the bundle stream and before the source-evidence
 * revalidation/COMMIT boundary. */
void consensus_state_snapshot_install_activate_test_set_after_stream_hook(
    void (*hook)(void *), void *ctx);
/* One-shot pre-seed failpoint. It fires inside the open cutover transaction
 * and proves every streamed state/schema/marker write rolls back together. */
void consensus_state_snapshot_install_activate_test_fail_seed_once(void);
/* One-shot failure after the seed has written its log/meta witnesses but before
 * terminal verification/COMMIT. Proves those writes join the cutover rollback. */
void consensus_state_snapshot_install_activate_test_fail_after_seed_once(void);
#endif

#endif /* ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_H */
