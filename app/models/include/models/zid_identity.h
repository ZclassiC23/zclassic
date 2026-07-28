/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sovereign-identity anchor projection (zid_identities) — the node's
 * answer to "is this 32-byte master ed25519 key anchored on-chain, by
 * whom, at what height, and is it still valid?".
 *
 * One row per master public key, keyed by the key itself. Derived purely
 * from confirmed on-chain data; rebuildable; never consulted by
 * consensus. This header is the READ API every later surface (chain
 * ingestion, native commands, the explorer) calls — nothing else should
 * hand-roll SQL against zid_identities.
 *
 * Field semantics:
 *   master_pubkey     the identity's ed25519 master public key (lib/zid) —
 *                     the primary key, so a re-anchor of the same key
 *                     overwrites in place rather than forking the row.
 *   anchor_txid       txid of the transaction that anchored the key.
 *   anchor_height     height of the block that confirmed anchor_txid.
 *   status            "active" | "rotated" | "revoked". A rotated identity
 *                     is not gone — it points at its successor; a revoked
 *                     one is dead with no successor.
 *   successor_pubkey  set EXACTLY when status is "rotated" (has_successor
 *                     is the in-memory presence bit; the column is NULL
 *                     otherwise).
 *   source            "znam_text" (anchored via a ZNAM text record, in
 *                     which case `name` carries that name) or
 *                     "zid_overlay" (anchored via the zid OP_RETURN
 *                     overlay, in which case `name` is empty).
 *   owner_address     the P2PKH signer that anchored the row, when known.
 *   updated_height    height at which this row last changed — the anchor
 *                     height for a fresh row, the rotation/revocation
 *                     height afterwards.
 *
 * Threading contract: every write goes through db_zid_identity_save
 * (AR_ADHOC_SAVE — a locally-prepared statement per call, INSERT OR
 * REPLACE on the primary key), so it is idempotent and safe from any
 * thread. The read helpers prepare their own statements too and hold no
 * cached cursor, so they never park a statement on the shared WAL
 * connection.
 */

#ifndef ZCL_DB_MODEL_ZID_IDENTITY_H
#define ZCL_DB_MODEL_ZID_IDENTITY_H

#include "models/database.h"
#include "models/activerecord.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ZNAM names are 1-63 chars; +1 for the terminating NUL. */
#define ZID_IDENTITY_NAME_MAX    64
/* Longest address form the node produces is a 78-char Sapling zs1. */
#define ZID_IDENTITY_ADDRESS_MAX 96
#define ZID_IDENTITY_STATUS_MAX  16
#define ZID_IDENTITY_SOURCE_MAX  16

#define ZID_IDENTITY_STATUS_ACTIVE   "active"
#define ZID_IDENTITY_STATUS_ROTATED  "rotated"
#define ZID_IDENTITY_STATUS_REVOKED  "revoked"

#define ZID_IDENTITY_SOURCE_ZNAM_TEXT  "znam_text"
#define ZID_IDENTITY_SOURCE_ZID_OVERLAY "zid_overlay"

struct zid_identity {
    uint8_t master_pubkey[32];
    uint8_t anchor_txid[32];
    int32_t anchor_height;
    char    status[ZID_IDENTITY_STATUS_MAX];
    uint8_t successor_pubkey[32];
    bool    has_successor;   /* true iff the column is non-NULL */
    char    source[ZID_IDENTITY_SOURCE_MAX];
    char    name[ZID_IDENTITY_NAME_MAX];          /* "" when unnamed */
    char    owner_address[ZID_IDENTITY_ADDRESS_MAX]; /* "" when unknown */
    int32_t updated_height;
};

struct ar_callbacks *db_zid_identity_callbacks(void);

/* Rejects: an unknown status/source literal, a successor_pubkey whose
 * presence disagrees with status=="rotated" (present iff rotated), a
 * present-but-all-zero successor_pubkey, and negative heights. */
bool db_zid_identity_validate(const struct zid_identity *r,
                              struct ar_errors *errors);

/* INSERT OR REPLACE keyed by master_pubkey — idempotent, any thread. */
bool db_zid_identity_save(struct node_db *ndb, const struct zid_identity *row);

/* ── Read API ──────────────────────────────────────────────────────
 *
 * The resolve surface everything else calls. `find`/`find_by_name`
 * return false when no row matches (a legitimate negative answer, not an
 * error). `out` is only written on a true return. */

bool db_zid_identity_find(struct node_db *ndb, const uint8_t pubkey[32],
                          struct zid_identity *out);

/* Resolve by ZNAM name (source=="znam_text" rows). Names are unique in
 * ZNAM, so at most one row matches; ties break on the newest anchor. */
bool db_zid_identity_find_by_name(struct node_db *ndb, const char *name,
                                  struct zid_identity *out);

/* Newest-anchor-first page. Returns the number of rows written to `out`
 * (<= max). max<=0 or a negative offset yields 0. */
int db_zid_identity_list(struct node_db *ndb, struct zid_identity *out,
                         int max, int offset);

int64_t db_zid_identity_count(struct node_db *ndb);
int64_t db_zid_identity_count_by_status(struct node_db *ndb,
                                        const char *status);

/* Drop every row — the projection is rebuildable from block history. */
bool db_zid_identity_truncate(struct node_db *ndb);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe.
 * key: NULL/"" for totals; a 64-hex master pubkey or a ZNAM name to
 * resolve one identity. */
struct json_value;
bool zid_identity_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_DB_MODEL_ZID_IDENTITY_H */
