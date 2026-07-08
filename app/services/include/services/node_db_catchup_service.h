/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* node_db_catchup_service — bulk-index blocks (sqlite_tip+1 → chain_tip)
 * into SQLite, optionally scanning for wallet transactions.
 *
 * This is the orchestration body lifted verbatim out of the sync
 * controller (sync_controller_catchup.c). The controller
 * (node_db_sync_catchup) keeps its parse/validate front matter and now
 * delegates the turbo-mode scope, DB verify, Sapling-tree init, the main
 * transaction/commit block loop, and turbo end to this service.
 *
 * Contract (LOCKED): returns a plain int — the number of blocks indexed,
 * or -1 on a setup failure. The single caller is the catchup job thread
 * (sync_controller_catchup_jobs.c), which stores the int into job->result.
 * Do NOT migrate to zcl_result: the recovery-primitive int contract is
 * consumed across the coins-wedge recovery surface and a result type buys
 * nothing here while forcing a job-struct rewrite. */

#ifndef ZCL_SERVICES_NODE_DB_CATCHUP_SERVICE_H
#define ZCL_SERVICES_NODE_DB_CATCHUP_SERVICE_H

struct node_db;
struct active_chain;
struct wallet;

#ifdef ZCL_TESTING
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
uint8_t *node_db_catchup_test_mmap_block_file_quiet(const char *datadir,
                                                    int file_num,
                                                    size_t *out_size,
                                                    int *out_errno);
bool node_db_catchup_test_sparse_prefix_can_advance(int indexed,
                                                    int total,
                                                    int lean_holes,
                                                    int first_hole_h,
                                                    int start,
                                                    int chain_tip,
                                                    int suspicious_holes,
                                                    int missing_file_holes,
                                                    int missing_index_holes,
                                                    bool proven_authority,
                                                    int32_t proven_applied);
#endif

/* Indexes blocks from (sqlite_tip+1) to chain_tip into SQLite. Also scans
 * for wallet transactions if a wallet is provided. Returns the number of
 * blocks indexed, or -1 on a setup failure (turbo enter / BEGIN / COMMIT).
 * Logs every failure path internally. */
int node_db_catchup_service_run(struct node_db *ndb,
                                const struct active_chain *chain,
                                const struct wallet *w,
                                const char *datadir);

#endif /* ZCL_SERVICES_NODE_DB_CATCHUP_SERVICE_H */
