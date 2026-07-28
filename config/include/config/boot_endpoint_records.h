/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot-time wiring of SIGNED ENDPOINT RECORDS (zid/zendp.h "ZIDE",
 * carried as blobs by vcs/zendp_swarm.h).
 *
 * Two jobs, both of which can only be done at the composition root:
 *
 *  1. Close the chain binding. vcs/zendp_swarm.h names a port for
 *     "is this master key anchored on-chain, at what height, and is it
 *     still live?"; the answer lives in db_zid_identity_find
 *     (app/models), far above lib/vcs. This file registers the
 *     implementation. Until it is registered, zendp fails CLOSED:
 *     every record is refused ZENDP_ERR_NO_ANCHOR_LOOKUP.
 *
 *  2. Project the verified records into peer discovery as an
 *     ADDITIONAL source — never a replacement for, and never a filter
 *     on, any source already wired. */

#ifndef ZCL_CONFIG_BOOT_ENDPOINT_RECORDS_H
#define ZCL_CONFIG_BOOT_ENDPOINT_RECORDS_H

#include "net/onion_discovery.h"

#include <stddef.h>

/* Register the on-chain identity lookup with vcs/zendp_swarm. Safe to
 * call before node.db is open: the lookup itself reports "could not
 * ask" separately from "the chain said no". */
void boot_endpoint_records_register(void);

/* An onion_signed_peer_source_fn over the verified endpoint-record
 * directory: only records inside their own signed validity window and
 * backed by an ACTIVE on-chain anchor, projected through the same v3
 * hostname rule every other source passes. Returns 0 when nothing has
 * been accepted, which is the state of a node that has never seen a
 * record. */
int boot_endpoint_record_peers(void *ctx, struct onion_peer *out, size_t max);

#endif
