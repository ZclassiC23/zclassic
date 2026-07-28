/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Onion peer discovery contract for net-layer bootstrapping. */

#ifndef ZCL_NET_ONION_DISCOVERY_H
#define ZCL_NET_ONION_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct onion_peer {
    char hostname[64];
    int height;
};

typedef int (*onion_peer_discover_fn)(const char *datadir,
                                      struct onion_peer *out,
                                      size_t max);

/* ── v3 hostname shape ──────────────────────────────────────────────
 *
 * Hostnames reaching this layer are attacker-controlled (on-chain
 * OP_RETURN payloads, peer-served directory JSON). Only the exact Tor v3
 * shape — 56 base32 [a-z2-7] chars + ".onion", 62 bytes total — may reach
 * HTML, JSON, the peer_directory table, or a fetch. Single definition
 * shared by the directory server and the connman seed walker so the two
 * can never drift apart. */
bool onion_hostname_is_valid_v3(const char *h);

/* Is `name` safe to render as a label for an endpoint (HTML page, JSON
 * document)? A RENDER guard only: bounded length, lowercase alphanumeric
 * and hyphen. Registry validity belongs to the on-chain ZNAM fold that
 * writes znam_names and is NOT re-decided here — lib/net ranks below
 * lib/znam, and a second answer to "is this a legal name" is a second
 * truth waiting to drift. Kept at or tighter than the registry rule, so it
 * can only ever withhold a label, never invent one. */
bool onion_directory_label_is_renderable(const char *name);

/* ── /directory.json onion-field scanner ────────────────────────────
 *
 * Advance *cursor past the next `"onion":"<host>"` field and copy <host>
 * (NUL-terminated) into out. Malformed, over-long, or unterminated fields
 * are SKIPPED, not fatal: one bad record from a hostile peer must not
 * truncate the scan of the good ones. Returns false only when no further
 * field exists. Shape validation is the CALLER's job — this is a parser,
 * not a filter. */
bool onion_directory_scan_next_onion(const char **cursor,
                                     char *out, size_t out_len);

/* ── Directory freshness ledger ─────────────────────────────────────
 *
 * peer_directory rows carry an age. Rows we have never reached and have
 * not seen advertised for ONION_DIRECTORY_MAX_AGE_SECS are deleted; rows
 * that survive are served WITH their age so a consumer judges freshness
 * itself rather than trusting a bare list.
 *
 * A directory record is a HINT about WHERE to look, never proof of WHO is
 * there (docs/work/NAT_AND_ONION_TRANSPORT.md). The three observation
 * kinds keep that distinction in the data:
 *
 *   ADVERTISED   — a peer's directory listed this onion. Creates the row
 *                  and refreshes last_seen. Never touches last_success:
 *                  being named by someone else is not contact.
 *   REACHED      — WE fetched /directory.json from it. Refreshes both
 *                  last_seen and last_success, bumps dial_success_count.
 *   UNREACHABLE  — our fetch failed. Bumps dial_fail_count on an EXISTING
 *                  row only; never inserts, never refreshes last_seen. A
 *                  failed dial carries no identity (the discipline
 *                  peers_projection's census fold already enforces).
 */
enum onion_directory_observation {
    ONION_DIR_ADVERTISED  = 0,
    ONION_DIR_REACHED     = 1,
    ONION_DIR_UNREACHABLE = 2,
};

/* Create peer_directory (and idempotently add its freshness columns) on an
 * already-open node.db handle. `sqlite3` is forward-declared so a consumer
 * of this contract does not have to include sqlite3.h. */
struct sqlite3;
void onion_directory_ensure_table(struct sqlite3 *db);

/* Rows unseen this long are expired (72 h). */
#define ONION_DIRECTORY_MAX_AGE_SECS 259200

/* Minimum wall-clock gap between opportunistic expiry sweeps (10 min). */
#define ONION_DIRECTORY_EXPIRE_INTERVAL_SECS 600

/* Record one observation of `onion` into datadir's peer_directory. height
 * <= 0 leaves the stored height alone. Silently no-ops on a NULL datadir,
 * a hostname failing onion_hostname_is_valid_v3(), or an unopenable
 * database — discovery bookkeeping never fails a caller's dial path.
 * Opportunistically runs the expiry sweep, throttled to at most one per
 * ONION_DIRECTORY_EXPIRE_INTERVAL_SECS. */
void onion_directory_observe(const char *datadir, const char *onion,
                             enum onion_directory_observation obs,
                             int height);

/* Delete non-self rows whose last_seen is older than now_unix -
 * max_age_secs. Returns rows deleted, or -1 on error. The self row is
 * never expired: this node's own address does not go stale to itself. */
int onion_directory_expire(const char *datadir, int64_t now_unix,
                           int64_t max_age_secs);

/* Resolve the ZNAM name registered on-chain for a .onion target (the
 * znam_names projection, target_type ZNAM_TYPE_ONION). Matches the stored
 * target with and without the ".onion" suffix. Returns true and fills out
 * when a name exists; false (out set to "") otherwise, including when the
 * projection table does not exist yet. */
bool onion_directory_name_for(const char *datadir, const char *onion,
                              char *out, size_t out_len);

/* Same join against an already-open node.db handle — for a page that
 * resolves a name per row and must not reopen the database each time. */
bool onion_directory_name_for_db(struct sqlite3 *db, const char *onion,
                                 char *out, size_t out_len);

#endif
