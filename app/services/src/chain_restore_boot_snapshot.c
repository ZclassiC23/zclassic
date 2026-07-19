/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain restore boot snapshot — diagnostic state captured during restore. */

// one-result-type-ok:diagnostic-recorder — this file is a pure in-memory
// recorder of restore diagnostics: every chain_restore_record_*() and the
// getter return void (they cannot fail; they just stamp a static struct).
// The single bool return, chain_restore_dump_state_json(), is the
// standardized *_dump_state_json introspection contract (see CLAUDE.md
// "Adding state introspection") whose bool signature is fixed across the
// codebase and dispatched from diagnostics_controller — not a fallible
// service result. No service surface to migrate.

#include "services/chain_restore_boot_snapshot.h"
#include "services/chain_restore_integrity.h"
#include "services/chain_restore_planner.h"
#include "services/binary_staleness_service.h"
#include "platform/time_compat.h"
#include "util/boot_scan.h"
#include "json/json.h"

#include <string.h>

static struct chain_restore_boot_snapshot g_chain_restore_boot_snapshot;

static const char *chain_restore_state_name(int s)
{
    switch ((enum chain_restore_state)s) {
    case CHAIN_RESTORE_UNRESOLVED:      return "UNRESOLVED";
    case CHAIN_RESTORE_FOUND_IN_INDEX:  return "FOUND_IN_INDEX";
    case CHAIN_RESTORE_ANCHOR_CREATED:  return "ANCHOR_CREATED";
    case CHAIN_RESTORE_RESOLVED:        return "RESOLVED";
    case CHAIN_RESTORE_FAILED:          return "FAILED";
    }
    return "UNKNOWN";
}

void chain_restore_record_plan_result(const struct chain_restore_plan *p)
{
    if (!p) return;
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.plan_recorded = true;
    g_chain_restore_boot_snapshot.plan_next_state = (int)p->next_state;
    g_chain_restore_boot_snapshot.plan_anchor_height = p->anchor_height;
    g_chain_restore_boot_snapshot.plan_should_skip_activate =
        p->should_skip_activate;
    size_t n = strnlen(p->reason, sizeof(p->reason));
    if (n >= sizeof(g_chain_restore_boot_snapshot.plan_reason))
        n = sizeof(g_chain_restore_boot_snapshot.plan_reason) - 1;
    memcpy(g_chain_restore_boot_snapshot.plan_reason, p->reason, n);
    g_chain_restore_boot_snapshot.plan_reason[n] = '\0';
}

void chain_restore_record_integrity_result(
    const struct chain_integrity_result *r)
{
    if (!r) return;
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.integrity_ok = r->ok;
    g_chain_restore_boot_snapshot.zero_nbits_count = r->zero_nbits_count;
    g_chain_restore_boot_snapshot.active_chain_holes = r->active_chain_holes;
    g_chain_restore_boot_snapshot.active_chain_mismatches =
        r->active_chain_mismatches;
    g_chain_restore_boot_snapshot.tip_window_holes = r->tip_window_holes;
    g_chain_restore_boot_snapshot.tip_height = r->tip_height;
    g_chain_restore_boot_snapshot.first_nbits_zero_height =
        r->first_nbits_zero_height;
    g_chain_restore_boot_snapshot.first_hole_height = r->first_hole_height;
    g_chain_restore_boot_snapshot.first_mismatch_height =
        r->first_mismatch_height;
    g_chain_restore_boot_snapshot.first_tip_window_hole =
        r->first_tip_window_hole;
}

void chain_restore_record_backfill_result(int fixed,
                                          int read_errors,
                                          int off_chain_cleared)
{
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.backfill_ran = true;
    g_chain_restore_boot_snapshot.backfill_fixed = fixed;
    g_chain_restore_boot_snapshot.backfill_read_errors = read_errors;
    g_chain_restore_boot_snapshot.backfill_off_chain_cleared =
        off_chain_cleared;
}

void chain_restore_record_csr_consistency(bool consistent,
                                          int tip_height,
                                          int header_height)
{
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.csr_consistency_checked = true;
    g_chain_restore_boot_snapshot.csr_consistent = consistent;
    g_chain_restore_boot_snapshot.csr_tip_height = tip_height;
    g_chain_restore_boot_snapshot.csr_header_height = header_height;
}

void chain_restore_record_snapshot_import(bool ok,
                                          int64_t utxo_count,
                                          int64_t snap_height)
{
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.snapshot_imported_pre_restore = ok;
    g_chain_restore_boot_snapshot.snapshot_imported_utxos = utxo_count;
    g_chain_restore_boot_snapshot.snapshot_imported_height = snap_height;
}

void chain_restore_record_fast_restart(bool taken,
                                       int64_t tip_height,
                                       const char *reason)
{
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.fast_restart_evaluated = true;
    g_chain_restore_boot_snapshot.fast_restart_taken = taken;
    g_chain_restore_boot_snapshot.fast_restart_tip_height = tip_height;
    if (reason) {
        size_t n = strnlen(reason,
                           sizeof(g_chain_restore_boot_snapshot.fast_restart_reason) - 1);
        memcpy(g_chain_restore_boot_snapshot.fast_restart_reason, reason, n);
        g_chain_restore_boot_snapshot.fast_restart_reason[n] = '\0';
    }
}

void chain_restore_get_boot_snapshot(struct chain_restore_boot_snapshot *out)
{
    if (!out) return;
    *out = g_chain_restore_boot_snapshot;
}

bool chain_restore_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    const struct chain_restore_boot_snapshot *s =
        &g_chain_restore_boot_snapshot;
    json_set_object(out);
    json_push_kv_bool(out, "has_data", s->has_data);
    json_push_kv_int(out, "boot_time", s->boot_time);
    json_push_kv_bool(out, "integrity_ok", s->integrity_ok);
    json_push_kv_int(out, "zero_nbits_count",
                     s->zero_nbits_count);
    json_push_kv_int(out, "active_chain_holes",
                     s->active_chain_holes);
    json_push_kv_int(out, "active_chain_mismatches",
                     s->active_chain_mismatches);
    json_push_kv_int(out, "tip_window_holes",
                     s->tip_window_holes);
    json_push_kv_int(out, "tip_height", s->tip_height);
    json_push_kv_int(out, "first_nbits_zero_height",
                     s->first_nbits_zero_height);
    json_push_kv_int(out, "first_hole_height",
                     s->first_hole_height);
    json_push_kv_int(out, "first_mismatch_height",
                     s->first_mismatch_height);
    json_push_kv_int(out, "first_tip_window_hole",
                     s->first_tip_window_hole);
    json_push_kv_bool(out, "backfill_ran", s->backfill_ran);
    json_push_kv_int(out, "backfill_fixed", s->backfill_fixed);
    json_push_kv_int(out, "backfill_read_errors",
                     s->backfill_read_errors);
    json_push_kv_int(out, "backfill_off_chain_cleared",
                     s->backfill_off_chain_cleared);
    json_push_kv_bool(out, "plan_recorded", s->plan_recorded);
    json_push_kv_str(out, "plan_next_state",
                     chain_restore_state_name(s->plan_next_state));
    json_push_kv_int(out, "plan_anchor_height",
                     s->plan_anchor_height);
    json_push_kv_bool(out, "plan_should_skip_activate",
                      s->plan_should_skip_activate);
    json_push_kv_str(out, "plan_reason", s->plan_reason);
    json_push_kv_bool(out, "csr_consistency_checked",
                      s->csr_consistency_checked);
    json_push_kv_bool(out, "csr_consistent", s->csr_consistent);
    json_push_kv_int(out, "csr_tip_height", s->csr_tip_height);
    json_push_kv_int(out, "csr_header_height",
                     s->csr_header_height);
    json_push_kv_bool(out, "snapshot_imported_pre_restore",
                      s->snapshot_imported_pre_restore);
    json_push_kv_int(out, "snapshot_imported_utxos",
                     s->snapshot_imported_utxos);
    json_push_kv_int(out, "snapshot_imported_height",
                     s->snapshot_imported_height);
    json_push_kv_bool(out, "fast_restart_evaluated",
                      s->fast_restart_evaluated);
    json_push_kv_bool(out, "fast_restart_taken", s->fast_restart_taken);
    json_push_kv_int(out, "fast_restart_tip_height",
                     s->fast_restart_tip_height);
    json_push_kv_str(out, "fast_restart_reason", s->fast_restart_reason);

    /* O(delta) witness — per-step boot data-scan iteration counts (see
     * util/boot_scan.h). Each key is a named boot scanner; the value is how
     * many rows/entries it touched. A step whose count tracks chain length
     * instead of the delta is an O(chain) boot regression the operator can see
     * here without attaching a profiler. */
    struct json_value scan;
    json_init(&scan);
    boot_scan_dump_json(&scan);
    json_push_kv(out, "scan_counters", &scan);
    json_free(&scan);

    /* Stale-binary detection (see services/binary_staleness_service.h):
     * does the on-disk binary at this process's exec path still match
     * what's actually running? Folded into this existing "boot" dumper
     * rather than registered as its own top-level dumpstate subsystem —
     * see CLAUDE.md "Adding state introspection". */
    struct json_value bin_stale;
    json_init(&bin_stale);
    binary_staleness_dump_state_json(&bin_stale, NULL);
    json_push_kv(out, "binary_staleness", &bin_stale);
    json_free(&bin_stale);
    return true;
}
