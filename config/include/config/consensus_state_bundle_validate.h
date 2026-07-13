/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Internal external-bundle validation contract. */

#ifndef ZCL_CONSENSUS_STATE_BUNDLE_VALIDATE_H
#define ZCL_CONSENSUS_STATE_BUNDLE_VALIDATE_H

#include "config/consensus_state_snapshot_install.h"
#include "storage/consensus_state_bundle_codec.h"

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

/* `db` must be the separately opened read-only immutable bundle handle. Reads
 * and validates its complete transaction snapshot; never mutates it. */
bool consensus_state_bundle_validate(
    struct sqlite3 *db,
    struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_install_result *result);

#endif /* ZCL_CONSENSUS_STATE_BUNDLE_VALIDATE_H */
