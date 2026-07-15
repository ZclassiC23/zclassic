/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Producer-owned durable source receipt for the full-history
 * zcl.consensus_state_bundle.v1 exporter.
 *
 * The contained exporter (consensus_state_snapshot_export.c) refuses to
 * publish unless the frozen producer progress.kv carries a
 * consensus_state_source_receipt singleton that is bound to the ACTUAL running
 * executable (running_binary_digest == SHA3(/proc/self/exe)), the genesis->H*
 * header corpus (chain_corpus_digest), and the exact fold cursor (H*+1). This
 * module lets a -mint-anchor producer honestly earn that receipt:
 *
 *   begin()    - at producer start, before the fold. Records the running
 *                executable, start time, and the source-tree identity claim,
 *                and publishes the source-epoch digest into progress_meta so
 *                every fold stage stamps its rows with the same epoch (the
 *                binding the exporter's stage-row proof requires). Idempotent:
 *                a resume by the SAME running binary with the SAME claim is a
 *                no-op; a different binary/claim/profile refuses (fail closed).
 *
 *   finalize() - at producer completion (fold reached the target height/hash),
 *                verifies the running executable still owns the start session,
 *                binds the receipt to the completed generation (height, hash,
 *                and fold cursor),
 *                and writes the consensus_state_source_receipt singleton the
 *                exporter reads. A missing/foreign start session refuses.
 *
 * The source-tree/toolchain/build-inputs fields are honest producer CLAIMS,
 * not an independent rebuild proof; their authority comes from the known
 * running executable that emitted and durably recorded them (see
 * storage/consensus_state_bundle_codec.h).
 */

#ifndef ZCL_CONFIG_CONSENSUS_STATE_PRODUCER_RECEIPT_H
#define ZCL_CONFIG_CONSENSUS_STATE_PRODUCER_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

/* Producer datadirs are bounded before any path composition or status text
 * rendering. The limit is intentionally below the platform path ceiling and
 * matches the progress-store path contract. */
#define CONSENSUS_STATE_PRODUCER_DATADIR_MAX 1024U

/* Start-of-fold ownership marker. `validation_profile` is one of
 * CONSENSUS_STATE_VALIDATION_FULL / _CHECKPOINT_FOLD
 * (storage/consensus_state_bundle_codec.h). Returns false and fills `err` on
 * any refusal (missing 64-hex source identity, foreign existing session,
 * profile mismatch, store error). */
bool consensus_state_producer_receipt_begin(sqlite3 *pdb,
                                            uint8_t validation_profile,
                                            char *err, size_t err_size);

/* Completion finalization bound to the folded (height, block_hash). Requires a
 * matching start session written by the SAME running executable. Writes the
 * consensus_state_source_receipt singleton the exporter admits. An exact retry
 * is idempotent; any different or malformed finalized row is immutable
 * conflicting evidence and is never replaced. Returns false and fills `err`
 * on any refusal. */
bool consensus_state_producer_receipt_finalize(sqlite3 *pdb, int32_t height,
                                               const uint8_t block_hash[32],
                                               char *err, size_t err_size);

/* Read-only producer status. Opens <datadir>/progress.kv READONLY (no writes,
 * no migrations — safe against a running producer over WAL) and reports the
 * fold session/receipt lifecycle plus the reducer stage cursors, for the
 * `zclassic23 ops producer status` operator command. NEVER mutates state. */
struct producer_status_read {
    bool    progress_kv_present;   /* <datadir>/progress.kv exists and opened */
    bool    session_open;          /* a producer session row exists (fold started) */
    bool    receipt_finalized;     /* full receipt parsed + digests reproduced */
    int64_t utxo_apply_cursor;     /* stage_cursor 'utxo_apply' (-1 if absent) */
    int64_t tip_finalize_cursor;   /* stage_cursor 'tip_finalize' (-1 if absent) */
    int64_t fold_cursor;           /* receipt fold cursor H*+1 (-1 if not finalized) */
    int64_t start_time_us;         /* session start (0 if no session) */
    int     validation_profile;    /* session/receipt validation profile (-1 if none) */
    char    receipt_schema[64];    /* exact v1/v2 session or receipt schema */
    char    source_tree_root[65];  /* lowercase hex source claim ("" if none) */
    char    source_epoch_digest[65]; /* lowercase hex epoch ("" if none) */
    char    producer_commit[64];   /* legacy v1 Git claim ("" for v2/none) */
};

/* Fill *out from <datadir>/progress.kv. ENOENT is a successful absent status;
 * an opened old/empty store may omit optional projection tables. Other I/O,
 * schema, SQL, type, or identity errors fail closed with `err` instead of
 * being reported as absence. Datadirs must contain fewer than
 * CONSENSUS_STATE_PRODUCER_DATADIR_MAX bytes. Node-free. */
bool consensus_state_producer_status_read(const char *datadir,
                                          struct producer_status_read *out,
                                          char *err, size_t err_size);

#ifdef ZCL_TESTING
/* Deterministic hermetic override for the source-identity claim so a test does
 * not depend on the build's baked source identity. `source_id` must be 64
 * lowercase hex (a SHA-256 identity) or begin() rejects it like a real
 * unstamped build. `source_clean` exercises the legacy-named completeness
 * column; production v2 writers always set it true after exact capture and do
 * not derive it from Git cleanliness. Pass source_id=NULL to clear. */
void consensus_state_producer_receipt_test_set_identity(const char *source_id,
                                                        bool source_clean);
#endif

#endif /* ZCL_CONFIG_CONSENSUS_STATE_PRODUCER_RECEIPT_H */
