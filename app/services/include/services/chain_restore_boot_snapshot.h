/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain restore boot snapshot — diagnostic state captured during restore. */

#ifndef ZCL_CHAIN_RESTORE_BOOT_SNAPSHOT_H
#define ZCL_CHAIN_RESTORE_BOOT_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

struct chain_integrity_result;
struct chain_restore_plan;
struct json_value;

/* Captures the most recent post-restore integrity-check result plus
 * the most recent backfill counters. Reads via
 * chain_restore_get_boot_snapshot are atomic enough for diagnostic
 * use (single struct copy; no locking). */
struct chain_restore_boot_snapshot {
    bool   has_data;
    int64_t boot_time;
    bool   integrity_ok;
    int    zero_nbits_count;
    int    active_chain_holes;
    int    active_chain_mismatches;
    int    tip_window_holes;
    int    tip_height;
    int    first_nbits_zero_height;
    int    first_hole_height;
    int    first_mismatch_height;
    int    first_tip_window_hole;
    bool   backfill_ran;
    int    backfill_fixed;
    int    backfill_read_errors;
    int    backfill_off_chain_cleared;
    bool   plan_recorded;
    int    plan_next_state;
    int    plan_anchor_height;
    bool   plan_should_skip_activate;
    char   plan_reason[160];
    bool   csr_consistency_checked;
    bool   csr_consistent;
    int    csr_tip_height;
    int    csr_header_height;
    bool   snapshot_imported_pre_restore;
    int64_t snapshot_imported_utxos;
    int64_t snapshot_imported_height;

    /* Tier-2 P2 fast restart: which boot path was taken this run and why. */
    bool    fast_restart_evaluated;   /* the P2 gate ran this boot           */
    bool    fast_restart_taken;       /* verify-then-trust skip fired         */
    int64_t fast_restart_tip_height;  /* installed tip height (fast path)     */
    char    fast_restart_reason[96];  /* named failing binding / "all-…"      */
};

void chain_restore_get_boot_snapshot(struct chain_restore_boot_snapshot *out);
void chain_restore_record_plan_result(const struct chain_restore_plan *p);
void chain_restore_record_integrity_result(
    const struct chain_integrity_result *r);
void chain_restore_record_backfill_result(int fixed,
                                          int read_errors,
                                          int off_chain_cleared);
void chain_restore_record_csr_consistency(bool consistent,
                                          int tip_height,
                                          int header_height);
void chain_restore_record_snapshot_import(bool ok,
                                          int64_t utxo_count,
                                          int64_t snap_height);
/* Record the Tier-2 P2 fast-restart verdict for `zclassic23 dumpstate boot`. */
void chain_restore_record_fast_restart(bool taken,
                                       int64_t tip_height,
                                       const char *reason);
bool chain_restore_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_CHAIN_RESTORE_BOOT_SNAPSHOT_H */
