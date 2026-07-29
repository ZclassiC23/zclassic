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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Register the on-chain identity lookup with vcs/zendp_swarm. Safe to
 * call before node.db is open: the lookup itself reports "could not
 * ask" separately from "the chain said no". */
void boot_endpoint_records_register(void);

/* THE ONE mapping from a zid_identities projection row to an anchor
 * verdict, over whichever node.db the caller has: the running node
 * passes its runtime handle, the CLI (tools/command/native_zendp_command.c)
 * passes an ad-hoc READONLY one. Exported rather than copied — a second
 * copy of "which status literal means ACTIVE" is a second answer to the
 * only question this whole subsystem asks.
 *
 * Returns false when the question could not be ASKED (no node.db, or a
 * status literal this build does not know), which is never the same as
 * "no such identity" — that is a true return carrying
 * ZENDP_ANCHOR_ABSENT. */
struct node_db;
struct zendp_anchor;
bool boot_endpoint_anchor_from_db(struct node_db *ndb,
                                  const uint8_t pubkey[32],
                                  struct zendp_anchor *out);

/* Load every record filed under <datadir>/zcode/endpoints/ into the
 * process-wide directory, re-running the WHOLE pipeline on each — the
 * files are witnesses, never the authority for what a record says, and
 * a key that was ACTIVE when the file was written may have been revoked
 * since. A record that does not resolve to an ACTIVE on-chain anchor is
 * DISCARDED here, exactly as it would be on the wire.
 *
 * CALL THIS ON THE BOOT THREAD ONLY. It reads files and does one
 * node.db lookup per record. The discovery projection
 * (boot_endpoint_record_peers) runs on the shared supervisor tick
 * runner via onion_directory_tick, and a blocking DB read there is how
 * this node has been killed by its own watchdog before — so the load is
 * a one-shot at start and never lazy off the discovery path.
 *
 * Returns the number of records installed. Bounded by ZENDP_DIR_MAX;
 * a NULL or missing directory is 0, not an error (the state of a node
 * that has never accepted a record). */
int boot_endpoint_records_load(const char *datadir);

/* An onion_signed_peer_source_fn over the verified endpoint-record
 * directory: only records inside their own signed validity window and
 * backed by an ACTIVE on-chain anchor, projected through the same v3
 * hostname rule every other source passes. Returns 0 when nothing has
 * been accepted, which is the state of a node that has never seen a
 * record. */
int boot_endpoint_record_peers(void *ctx, struct onion_peer *out, size_t max);

#endif
