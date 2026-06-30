/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sibling-private declarations shared between proof_validate_stage.c (the Job
 * state owner) and proof_validate_stage_dump.c (the read-only zcl_state JSON
 * dump). Not a public header.
 */

#ifndef ZCL_JOBS_PROOF_VALIDATE_STAGE_INTERNAL_H
#define ZCL_JOBS_PROOF_VALIDATE_STAGE_INTERNAL_H

#include "util/stage.h"

#include <stdint.h>

stage_t *proof_validate_stage_handle(void);
int64_t proof_validate_stage_last_step_unix(void);
int64_t proof_validate_stage_last_blocked_unix(void);
int64_t proof_validate_stage_last_advance_height(void);

#endif /* ZCL_JOBS_PROOF_VALIDATE_STAGE_INTERNAL_H */
