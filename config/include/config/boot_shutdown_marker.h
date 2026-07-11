/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONFIG_BOOT_SHUTDOWN_MARKER_H
#define ZCL_CONFIG_BOOT_SHUTDOWN_MARKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clean-shutdown marker lifecycle.
 *
 * detect_unclean() runs during boot: it observes the previous run's marker and
 * node.db WAL, emits EV_CRASH_RECOVERY_START when the WAL exists without the
 * clean marker, then unlinks the marker so only a full future shutdown can
 * recreate it. Before unlinking it caches the marker's content-binding (see
 * below) in memory so node_db_open can consult it.
 *
 * write_clean() runs last during shutdown. A true return means the marker was
 * written; callers may ignore false to preserve the existing best-effort
 * shutdown posture.
 *
 * ── Tier-2 fast restart: verified-clean quick_check skip ──────────────
 *
 * A warm boot spends ~9s in `PRAGMA quick_check` on node.db even when the DB
 * was checkpointed and closed cleanly. write_clean() records a content binding
 * for node.db into the marker (marker format v2 below). On the next boot,
 * node_db_open() may SKIP quick_check iff:
 *   1. the previous shutdown wrote a v2 binding (wal_checkpointed=1), AND
 *   2. node.db on disk is byte-consistent with that binding (size + the SQLite
 *      header file-change-counter both match), AND
 *   3. no node.db-wal is present (or it is zero bytes).
 * Any parse failure, any mismatch, or any WAL present → quick_check runs
 * exactly as before (the dirty path is bit-identical to today). The binding is
 * single-use: detect_unclean unlinks the on-disk marker and the in-memory cache
 * is cleared the first time the skip decision is consumed, so a subsequent
 * crash-boot can never reuse a stale binding.
 *
 * Marker format v2 (text, one key=value per line):
 *   <unix_seconds>              (line 1 — legacy presence timestamp, kept)
 *   magic=ZCLSHUT
 *   version=2
 *   node_db_size=<bytes>
 *   node_db_change_counter=<u32>        (header bytes 24..27, big-endian)
 *   node_db_version_valid_for=<u32>     (header bytes 92..95, big-endian)
 *   schema_version=<int>
 *   wal_checkpointed=1
 */
bool boot_shutdown_marker_detect_unclean(const char *datadir);
bool boot_shutdown_marker_write_clean(const char *datadir);

/* Pristine on-disk identity of a node.db file, captured BEFORE any sqlite
 * handle is opened (so it reflects exactly what the previous shutdown left). */
struct node_db_file_identity {
    bool     present;             /* file exists and header is a SQLite db */
    int64_t  size;                /* st_size of node.db                    */
    uint32_t change_counter;      /* header bytes 24..27, big-endian       */
    uint32_t version_valid_for;   /* header bytes 92..95, big-endian       */
    bool     wal_present;         /* node.db-wal exists with size > 0      */
};

/* Parsed content binding from a v2 marker. */
struct shutdown_clean_binding {
    bool     valid;               /* a complete v2 binding was parsed      */
    int64_t  node_db_size;
    uint32_t change_counter;
    uint32_t version_valid_for;
    int      schema_version;

    /* ── Tier-2 P2 fast-restart bindings (marker v3 OPTIONAL fields) ──────
     * Present iff the marker carried `fast_restart=1` and every fr_* field
     * below parsed. Independent of the v2 node.db-identity binding above:
     * the quick_check skip (P1) only needs the v2 fields; the forward-pass /
     * reconcile / chain_restore_finalize skip (P2) additionally needs these.
     * A marker written by a pre-P2 binary has valid=true, fr_valid=false —
     * quick_check may still be skipped, the fast-restart path is not taken. */
    bool     fr_valid;
    int64_t  fr_tip_height;
    uint8_t  fr_tip_hash[32];
    int64_t  fr_coins_best_height;
    uint8_t  fr_coins_best_hash[32];
    int64_t  fr_block_index_count;
    int64_t  fr_mmb_leaves;
    int64_t  fr_sapling_ckpt_height;
};

/* Facts captured at CLEAN SHUTDOWN, baked into the v3 marker so the next boot
 * can verify-then-trust the persisted state. All heights/counts are the
 * durable, on-disk-consistent values as of the final flush. */
struct fast_restart_shutdown_facts {
    bool     valid;               /* set false to write a v2-only marker    */
    int64_t  tip_height;
    uint8_t  tip_hash[32];
    int64_t  coins_best_height;
    uint8_t  coins_best_hash[32];
    int64_t  block_index_count;
    int64_t  mmb_leaves;
    int64_t  sapling_ckpt_height;
};

/* Record the fast-restart facts to bake into the NEXT clean-shutdown marker.
 * Called from the shutdown path just before boot_shutdown_marker_write_clean.
 * Pass valid=false (or never call it) to keep the marker v2-only. */
void boot_shutdown_marker_set_fast_restart_facts(
    const struct fast_restart_shutdown_facts *facts);

/* Copy the fast-restart binding parsed from THIS boot's marker (cached by
 * detect_unclean before it unlinked the on-disk file). Returns true iff a
 * complete v3 fast-restart binding was present. Non-consuming (peek): the
 * on-disk marker is already single-use via detect_unclean's unlink, so within
 * one process the cache may be read more than once. */
bool boot_shutdown_marker_peek_fast_restart_binding(
    struct shutdown_clean_binding *out);

/* Read node.db's pristine identity from `node_db_path`. Safe before any sqlite
 * open. Returns false (out->present=false) if the file is missing or not a
 * SQLite database. */
bool node_db_file_identity_read(const char *node_db_path,
                                struct node_db_file_identity *out);

/* Pure functions (unit-tested; no I/O). */
int  boot_shutdown_marker_format(char *buf, size_t n, int64_t unix_seconds,
                                 const struct shutdown_clean_binding *b);
bool boot_shutdown_marker_parse(const char *text, size_t len,
                                struct shutdown_clean_binding *out);
bool boot_shutdown_marker_can_skip(const struct shutdown_clean_binding *b,
                                   const struct node_db_file_identity *cur);

/* Record the schema version to bake into the NEXT clean-shutdown marker.
 * Called once after node.db migration completes. */
void boot_shutdown_marker_set_schema_version(int schema_version);

/* node_db_open's quick_check-skip probe (matches the
 * node_db_quick_check_skip_probe_fn signature in models/database.h). Reads the
 * pristine identity of `node_db_path`, compares it against the cached
 * clean-shutdown binding, and returns true to skip quick_check. Single-use:
 * clears the cache on first call. */
bool boot_shutdown_marker_quick_check_probe(const char *node_db_path);

/* True if the probe decided to skip quick_check this boot (a background
 * re-check should still run once). */
bool boot_shutdown_marker_quick_check_was_skipped(void);

/* Test-only: reset all module-level cache/state. */
void boot_shutdown_marker_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONFIG_BOOT_SHUTDOWN_MARKER_H */
