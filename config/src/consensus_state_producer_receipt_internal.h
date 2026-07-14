/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Internal contract shared by the producer-receipt TUs
 * (consensus_state_producer_receipt.c + _corpus.c). */

#ifndef ZCL_CONSENSUS_STATE_PRODUCER_RECEIPT_INTERNAL_H
#define ZCL_CONSENSUS_STATE_PRODUCER_RECEIPT_INTERNAL_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

#define PRODUCER_RECEIPT_SUBSYS "consensus_producer_receipt"

/* Recompute the genesis..height header corpus digest and confirm the tip hash.
 * MUST stay byte-identical to prove_header_chain() in
 * consensus_state_snapshot_export_proof.c — the exporter compares the receipt's
 * chain_corpus_digest against its own recomputation. The producer-receipt test
 * runs the real exporter, so any drift here fails the test. */
bool producer_receipt_header_corpus_digest(sqlite3 *db, int32_t height,
                                           const uint8_t expected_hash[32],
                                           uint8_t out[32]);

#endif /* ZCL_CONSENSUS_STATE_PRODUCER_RECEIPT_INTERNAL_H */
