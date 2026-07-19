/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * consensus_db — the dedicated on-disk home for the reducer's consensus
 * kernel: the tables that MUST commit atomically with each other on the
 * reducer batch-commit path (coins / sprout_anchors / sapling_anchors /
 * anchor_state / nullifiers) together with the progress-cursor rows they
 * are committed with (stage_cursor / progress_meta).
 *
 * WHY A SEPARATE FILE. Today every one of those tables lives in progress.kv,
 * the SAME SQLite file that the projection co-writers (address_index,
 * txindex, created_outputs, census, topology, …) also write. Because a WAL
 * database serialises all writers on ONE journal, a projection fold and the
 * reducer's fsync-bearing batch commit contend for the same write lock — the
 * writer-contention incident class. consensus.db gives the reducer its own
 * WAL so its commit/fsync path stops sharing a journal with the lagging
 * projection writers.
 *
 * KEY INVARIANT (the whole reason the atomic set is enumerated as ONE list):
 * SQLite WAL mode does NOT guarantee cross-database atomicity for a
 * transaction spanning ATTACHed files. Every transaction that must be atomic
 * therefore lives entirely in ONE file. The five kernel tables plus the two
 * progress-cursor tables are the reducer's single atomic set, so they move as
 * ONE unit into consensus.db — never split across files.
 *
 * This module owns the one-time, O(state) (never O(chain)) migration that
 * copies that atomic set out of an existing progress.kv into a fresh
 * consensus.db and PROVES the copy before anyone trusts it: row counts on
 * every kernel table AND the canonical SHA3-256 UTXO commitment over `coins`
 * must match byte-for-byte. On ANY mismatch it refuses — it removes the
 * partial consensus.db and returns false with a typed message — so boot can
 * never proceed on half-migrated consensus state (fail fast, never silently
 * proceed). It is idempotent: a no-op once consensus.db exists, and a clean
 * success when there is no populated progress.kv to migrate (a fresh node,
 * where boot creates consensus.db from scratch). */

#ifndef ZCL_STORAGE_CONSENSUS_DB_H
#define ZCL_STORAGE_CONSENSUS_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sqlite3;

/* THE name for the dedicated consensus-kernel store. No version suffix. */
#define CONSENSUS_DB_FILENAME "consensus.db"

/* The kernel atomic set: the exact tables that commit atomically together on
 * the reducer batch-commit path. Order is stable and used by
 * consensus_db_kernel_stats::table_rows. */
#define CONSENSUS_DB_KERNEL_TABLE_COUNT 7

/* Integrity fingerprint of the consensus kernel in one open database:
 *   coins_commit — the canonical SHA3-256 UTXO commitment over `coins`
 *                  (utxo_commitment.h serialisation; bit-identical to
 *                  coins_kv_commitment / utxo_projection_commitment).
 *   table_rows   — durable row count of each kernel table, indexed by the
 *                  stable CONSENSUS_DB_KERNEL_TABLE_COUNT ordering. */
struct consensus_db_kernel_stats {
    uint8_t coins_commit[32];
    int64_t table_rows[CONSENSUS_DB_KERNEL_TABLE_COUNT];
};

/* Read the kernel fingerprint of `db`. Requires the `coins` table to exist
 * (the marker of an initialised kernel store); returns false if it does not.
 * A missing OPTIONAL kernel table counts as 0 rows. `db` must NOT have the
 * coins_ram overlay bound (this reads the on-disk `coins` table directly, as
 * it does at boot before the overlay is allocated). errbuf may be NULL. */
bool consensus_db_read_kernel_stats(struct sqlite3 *db,
                                    struct consensus_db_kernel_stats *out,
                                    char *errbuf, size_t errcap);

/* Byte-for-byte equality of two kernel fingerprints. On mismatch returns
 * false and, when errbuf is non-NULL, writes a message naming the diverging
 * component (contains "sha3" for the coins commitment and/or "count" plus the
 * table name for a row count). */
bool consensus_db_kernel_stats_match(const struct consensus_db_kernel_stats *a,
                                     const struct consensus_db_kernel_stats *b,
                                     char *errbuf, size_t errcap);

/* One-time, O(state) migration of the consensus kernel atomic set out of
 * `<datadir>/progress.kv` into a fresh `<datadir>/consensus.db`.
 *
 *   - Idempotent: returns true immediately if consensus.db already exists.
 *   - Fresh node: returns true (no consensus.db created) if there is no
 *     progress.kv, or the progress.kv present has no `coins` table (nothing
 *     to migrate — boot creates consensus.db from scratch).
 *   - Otherwise copies the atomic set into consensus.db.tmp, verifies the
 *     copied fingerprint EQUALS the source fingerprint, and only then renames
 *     it into place.
 *   - On ANY integrity mismatch (or copy/verify failure) it removes the
 *     partial consensus.db.tmp, leaves progress.kv untouched, and returns
 *     false with a typed message. It NEVER leaves a half-migrated
 *     consensus.db behind.
 *
 * progress.kv is left fully intact — this call only PRODUCES consensus.db;
 * dropping the migrated tables from progress.kv belongs to the caller-flip
 * that repoints the kernel handle, not to the migration primitive. errbuf may
 * be NULL. */
bool consensus_db_migrate_from_progress(const char *datadir,
                                        char *errbuf, size_t errcap);

#endif /* ZCL_STORAGE_CONSENSUS_DB_H */
