/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal seam: active-tip evidence reconstruction. Implemented in
 * chain_evidence_reconstruct.c; called only by the controller's
 * reconcile_startup path. Not part of the public service API.
 */

#ifndef ZCL_SERVICES_CHAIN_EVIDENCE_RECONSTRUCT_H
#define ZCL_SERVICES_CHAIN_EVIDENCE_RECONSTRUCT_H

#include "services/chain_evidence_authority_service.h"

/* Reconstruct publishable LOCAL_IMPORT evidence for a hash-consistent
 * active tip recovered from disk. Recomputes ancestry/chainwork when
 * recoverable; returns true and persists the evidence. On a genuine
 * contradiction returns false and fills reason_out (always non-empty) with
 * the precise invariant that failed, so the caller can freeze with a named
 * reason. */
bool cec_reconstruct_active_tip_evidence(
    struct chain_evidence_controller *authority,
    struct block_index *active_tip,
    const struct chain_state_view *csv,
    char reason_out[192]);

#endif /* ZCL_SERVICES_CHAIN_EVIDENCE_RECONSTRUCT_H */
