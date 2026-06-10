/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_DB_WALLET_SCAN_H
#define ZCL_DB_WALLET_SCAN_H

#include "models/database.h"
#include "validation/chainstate.h"
#include <stdbool.h>

struct wallet;

/* Fast wallet rescan using pre-built chain index.
 *
 * Scans blocks from start_height to end_height, using the active chain's
 * block_index entries for file locations. Finds wallet transactions using
 * a hash table of address hashes (O(1) lookup vs O(n) keystore scan).
 * Tracks UTXOs in memory and bulk-writes results to SQLite.
 *
 * Returns the number of wallet transactions found, or -1 on error. */
int wallet_scan_blocks(struct node_db *ndb,
                       const struct active_chain *chain,
                       const struct wallet *w,
                       const char *datadir,
                       int start_height,
                       int end_height);

#endif
