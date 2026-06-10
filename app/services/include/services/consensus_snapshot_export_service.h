/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Consensus snapshot export service.
 *
 * Exports public consensus tables from node.db to consensus_snapshot.db.
 * SECURITY: excludes wallet_keys, wallet_utxos, wallet_sapling_keys,
 * wallet_sapling_notes, node_state, and other private operator tables.
 */

#ifndef ZCL_SERVICES_CONSENSUS_SNAPSHOT_EXPORT_SERVICE_H
#define ZCL_SERVICES_CONSENSUS_SNAPSHOT_EXPORT_SERVICE_H

#include "util/result.h"

struct zcl_result consensus_snapshot_export_service_run(const char *datadir);

#endif /* ZCL_SERVICES_CONSENSUS_SNAPSHOT_EXPORT_SERVICE_H */
