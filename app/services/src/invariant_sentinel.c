// one-result-type-ok:fail-loud-predicates
/* E2 override: this module's public surface is pass/refuse PREDICATES
 * (check_pair) and best-effort sweeps. A refusal is not an error to
 * propagate — the reason travels via the typed blocker (PERMANENT,
 * named heights) + EV_OPERATOR_NEEDED + LOG_WARN, which is the whole
 * point of the fail-loud pack. zcl_result would duplicate that channel
 * with a code/message the callers must not branch on. */

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * invariant_sentinel — see services/invariant_sentinel.h.
 *
 * Crash-only throughout: a firing check refuses/holds + blocker + page;
 * the process and every other stage keep running. */

#include "services/invariant_sentinel.h"

#include "config/runtime.h"
#include "event/event.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_helpers.h"
#include "jobs/tip_finalize_stage.h"
#include "models/database.h"
#include "platform/time_compat.h"
#include "services/utxo_recovery_service.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "supervisors/domains.h"
#include "coins/utxo_commitment.h"
#include "json/json.h"
#include "util/ar_step_readonly.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/supervisor.h"
#include "validation/chain_linkage_check.h"

#include <inttypes.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SWEEP_PERIOD_SECS            ((int64_t)60)
#define COMMITMENT_AUDIT_PERIOD_SECS ((int64_t)3600)
/* I4.5: this many reorg_detected increments in one sweep window at an
 * unmoving tip_finalize cursor = the oscillation wedge signature. */
#define SWEEP_OSCILLATION_THRESHOLD  10

/* ── counters / state (all atomic or lock-guarded; dumped via JSON) ── */
static _Atomic uint64_t g_pair_checks_total = 0;
static _Atomic uint64_t g_pair_violations_total = 0;
static _Atomic uint64_t g_sweeps_total = 0;
static _Atomic uint64_t g_sweep_violations_total = 0;
static _Atomic int64_t  g_last_sweep_unix = 0;
static _Atomic uint64_t g_commitment_audits_total = 0;
static _Atomic uint64_t g_commitment_mismatches_total = 0;
static _Atomic uint64_t g_commitment_skipped_tip_moved = 0;
static _Atomic int64_t  g_last_audit_unix = 0;
static _Atomic bool     g_sweep_blocker_active = false;
static _Atomic bool     g_audit_blocker_active = false;

static pthread_mutex_t g_detail_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_last_sweep_detail[200];
static char g_last_pair_detail[200];

/* Cross-sweep memory for the oscillation check (single sweep thread). */
static uint64_t g_prev_reorg_total = 0;
static int64_t  g_prev_tip_finalize_cursor = -1;
static bool     g_prev_sweep_valid = false;

#ifdef ZCL_TESTING
static struct node_db *g_test_ndb;
#endif

static struct node_db *sentinel_ndb(void)
{
#ifdef ZCL_TESTING
    if (g_test_ndb)
        return g_test_ndb;
#endif
    return app_runtime_node_db();
}

/* Page once per fresh blocker write (the dedup discipline). */
static void sentinel_raise_blocker(const char *id, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void sentinel_raise_blocker(const char *id, const char *fmt, ...)
{
    char reason[BLOCKER_REASON_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);

    struct blocker_record rec;
    if (!blocker_init(&rec, id, "validation_pack", BLOCKER_PERMANENT,
                      reason))
        return;
    if (blocker_set(&rec) == 0)
        event_emitf(EV_OPERATOR_NEEDED, 0, "check=%s %s", id, reason);
    LOG_WARN("validation_pack", "[validation_pack] %s: %s", id, reason);
}

/* ── Check 3: authority-pair self-check ─────────────────────────── */

/* resolve(hash) in the blocks projection: 1 = found (*out_h set),
 * 0 = unknown, -1 = read error (treated as unknown by callers — a
 * broken read must not block publishes). */
static int pair_resolve_height(struct node_db *ndb, const uint8_t hash[32],
                               int64_t *out_h)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT height FROM blocks WHERE hash = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("validation_pack",
                 "[validation_pack] pair resolve prepare failed: %s",
                 sqlite3_errmsg(ndb->db));
        return -1; // raw-return-ok:warned-on-previous-line
    }
    sqlite3_bind_blob(st, 1, hash, 32, SQLITE_STATIC);
    int rc = AR_STEP_ROW_READONLY(st);
    int found = 0;
    if (rc == SQLITE_ROW) {
        *out_h = sqlite3_column_int64(st, 0);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

void invariant_sentinel_pair_violation(const char *site, int height,
                                       int64_t resolved_h)
{
    atomic_fetch_add(&g_pair_violations_total, 1);
    pthread_mutex_lock(&g_detail_lock);
    snprintf(g_last_pair_detail, sizeof(g_last_pair_detail),
             "site=%s published_h=%d resolved_h=%lld",
             site ? site : "?", height, (long long)resolved_h);
    pthread_mutex_unlock(&g_detail_lock);
    sentinel_raise_blocker("authority.pair_self_check",
                           "authority pair mismatch site=%s published_h=%d "
                           "resolved_h=%lld (write refused)",
                           site ? site : "?", height,
                           (long long)resolved_h);
}

bool invariant_sentinel_check_pair(struct node_db *ndb,
                                   const uint8_t hash[32], int height,
                                   const char *site)
{
    if (!hash) {
        LOG_WARN("validation_pack",
                 "[validation_pack] pair check NULL hash site=%s h=%d",
                 site ? site : "?", height);
        return true; /* malformed input is the caller's bug, not a hold */
    }
    if (height < 0)
        return true; /* tip reset (e.g. snapshot import) — not a publish */
    if (!ndb || !ndb->open || !ndb->db)
        return true; /* projection not wired (early boot / unit tests) */

    atomic_fetch_add(&g_pair_checks_total, 1);
    int64_t resolved_h = -1;
    int found = pair_resolve_height(ndb, hash, &resolved_h);
    if (found <= 0)
        return true; /* unknown hash (or read error): publishable */
    if (resolved_h == (int64_t)height)
        return true;

    invariant_sentinel_pair_violation(site, height, resolved_h);
    return false;
}

/* ── Check 4: window consistency sweep ──────────────────────────── */

void invariant_sentinel_sweep_evaluate(
    const struct invariant_sweep_inputs *in,
    struct invariant_sweep_verdict *out)
{
    memset(out, 0, sizeof(*out));
    out->first_bad_h = -1;

    /* I4.1 — pipeline ordering, restricted to the PROVEN pairs:
     *   tip_finalize <= utxo_apply   (step_finalize requires
     *                                 next_h < utxo_apply cursor)
     *   utxo_apply <= script_validate (apply consumes a script verdict
     *                                  row, written below that cursor)
     * The body_fetch / validate_headers cursors are deliberately NOT in
     * the ordering set: bodies and headers arrive through MULTIPLE
     * writers (direct P2P connect, legacy mirror, import), so a
     * downstream cursor legitimately overtakes body_fetch's own cursor
     * during live catch-up — proven by a false fire on the healthy
     * negative fixture (script_validate=3143207 > body_fetch=3143163
     * while the node was finalizing forward normally, 2026-06-11). */
    const struct { const char *a; int64_t va; const char *b; int64_t vb; }
    order[] = {
        { "tip_finalize", in->cur_tip_finalize,
          "utxo_apply", in->cur_utxo_apply },
        { "utxo_apply", in->cur_utxo_apply,
          "script_validate", in->cur_script_validate },
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (order[i].va < 0 || order[i].vb < 0)
            continue; /* unknown cursor: skip the pair */
        if (order[i].va > order[i].vb) {
            out->violated = true;
            snprintf(out->invariant, sizeof(out->invariant), "I4.1");
            out->first_bad_h = (int)order[i].vb;
            snprintf(out->detail, sizeof(out->detail),
                     "cursor ordering: %s=%lld > %s=%lld",
                     order[i].a, (long long)order[i].va,
                     order[i].b, (long long)order[i].vb);
            return;
        }
    }

    /* I4.3 — utxo_apply log contiguity: the contiguous ok=1 prefix must
     * reach cursor-1 (FIX-3 guarantees an anchor can never manufacture a
     * rowless hole; a frontier below cursor-1 is a real hole/ok=0 row). */
    if (in->ua_log_frontier_known && in->cur_utxo_apply > 0 &&
        (int64_t)in->ua_log_frontier < in->cur_utxo_apply - 1) {
        out->violated = true;
        snprintf(out->invariant, sizeof(out->invariant), "I4.3");
        out->first_bad_h = in->ua_log_frontier + 1;
        snprintf(out->detail, sizeof(out->detail),
                 "utxo_apply log hole: contiguous ok=1 prefix h=%d but "
                 "cursor=%lld (first hole h=%d)",
                 in->ua_log_frontier, (long long)in->cur_utxo_apply,
                 in->ua_log_frontier + 1);
        return;
    }

    /* I4.4 — Invariant B re-asserted every minute: coins_applied must not
     * exceed utxo_apply's OWN contiguous ok=1 prefix + 1 (the torn-coin
     * shape, measured against the correct authority — c8018a388). */
    if (in->coins_applied_found && in->ua_log_frontier_known &&
        in->coins_applied > in->ua_log_frontier + 1) {
        out->violated = true;
        snprintf(out->invariant, sizeof(out->invariant), "I4.4");
        out->first_bad_h = in->ua_log_frontier + 1;
        snprintf(out->detail, sizeof(out->detail),
                 "coin tear: coins_applied=%d > utxo_apply ok=1 prefix=%d+1",
                 in->coins_applied, in->ua_log_frontier);
        return;
    }

    /* I4.5 — tip_finalize oscillation: many reorg_detected increments in
     * one window while the finalize cursor did not move = the live-wedge
     * signature (today it only logs quietly). */
    if (in->prev_cur_tip_finalize >= 0 &&
        in->cur_tip_finalize == in->prev_cur_tip_finalize &&
        in->reorg_detected_total >= in->prev_reorg_detected_total &&
        in->reorg_detected_total - in->prev_reorg_detected_total >=
            SWEEP_OSCILLATION_THRESHOLD) {
        out->violated = true;
        snprintf(out->invariant, sizeof(out->invariant), "I4.5");
        out->first_bad_h = (int)in->cur_tip_finalize;
        snprintf(out->detail, sizeof(out->detail),
                 "tip_finalize oscillation: %llu reorg_detected in one "
                 "window at unmoving cursor=%lld",
                 (unsigned long long)(in->reorg_detected_total -
                                      in->prev_reorg_detected_total),
                 (long long)in->cur_tip_finalize);
        return;
    }
}

bool invariant_sentinel_sweep_once(void)
{
    sqlite3 *db = progress_store_db();
    if (!db)
        return false; /* store not open yet — not a violation */

    struct invariant_sweep_inputs in;
    memset(&in, 0, sizeof(in));
    in.cur_tip_finalize =
        (int64_t)stage_cursor_persisted(db, "tip_finalize", "sentinel");
    in.cur_utxo_apply =
        (int64_t)stage_cursor_persisted(db, "utxo_apply", "sentinel");
    in.cur_script_validate =
        (int64_t)stage_cursor_persisted(db, "script_validate", "sentinel");
    in.cur_body_fetch =
        (int64_t)stage_cursor_persisted(db, "body_fetch", "sentinel");
    in.cur_validate_headers =
        (int64_t)stage_cursor_persisted(db, "validate_headers", "sentinel");

    int32_t ua_frontier = 0;
    in.ua_log_frontier_known =
        in.cur_utxo_apply > 0 &&
        reducer_frontier_log_frontier(db, "utxo_apply_log", "utxo_apply",
                                      &ua_frontier);
    in.ua_log_frontier = ua_frontier;

    int32_t coins_applied = 0;
    bool coins_found = false;
    if (coins_kv_get_applied_height(db, &coins_applied, &coins_found)) {
        in.coins_applied = coins_applied;
        in.coins_applied_found = coins_found;
    }

    in.reorg_detected_total = tip_finalize_stage_reorg_detected_total();
    if (g_prev_sweep_valid) {
        in.prev_reorg_detected_total = g_prev_reorg_total;
        in.prev_cur_tip_finalize = g_prev_tip_finalize_cursor;
    } else {
        in.prev_reorg_detected_total = in.reorg_detected_total;
        in.prev_cur_tip_finalize = -1;
    }
    g_prev_reorg_total = in.reorg_detected_total;
    g_prev_tip_finalize_cursor = in.cur_tip_finalize;
    g_prev_sweep_valid = true;

    struct invariant_sweep_verdict v;
    invariant_sentinel_sweep_evaluate(&in, &v);

    atomic_fetch_add(&g_sweeps_total, 1);
    atomic_store(&g_last_sweep_unix, platform_time_wall_unix());

    if (v.violated) {
        atomic_fetch_add(&g_sweep_violations_total, 1);
        pthread_mutex_lock(&g_detail_lock);
        snprintf(g_last_sweep_detail, sizeof(g_last_sweep_detail),
                 "%s %s", v.invariant, v.detail);
        pthread_mutex_unlock(&g_detail_lock);
        atomic_store(&g_sweep_blocker_active, true);
        sentinel_raise_blocker("window.consistency", "%s %s",
                               v.invariant, v.detail);
        if (v.first_bad_h >= 0)
            chain_linkage_hold_set("window_sweep", v.first_bad_h, v.detail);
    } else if (atomic_load(&g_sweep_blocker_active)) {
        /* Self-clearing: a clean sweep releases the hold — repair jobs
         * may legitimately fix holes; crash-only and recovery-friendly. */
        atomic_store(&g_sweep_blocker_active, false);
        blocker_clear("window.consistency");
        chain_linkage_hold_clear("window_sweep");
        LOG_INFO("validation_pack",
                 "[validation_pack] window.consistency cleared by a clean "
                 "sweep");
    }
    return true;
}

/* ── Check 5: commitment audit (hourly) ─────────────────────────── */

/* 16 per-txid-prefix range counts: localizes WHERE the set diverged (a
 * keyspace-tail truncation names itself as zeroed high ranges). */
static void audit_log_range_counts(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT substr(hex(txid),1,1), COUNT(*) FROM utxos GROUP BY 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("validation_pack",
                 "[validation_pack] range count prepare failed: %s",
                 sqlite3_errmsg(db));
        return;
    }
    char line[400];
    size_t off = 0;
    line[0] = '\0';
    while (AR_STEP_ROW_READONLY(st) == SQLITE_ROW) {
        const unsigned char *nib = sqlite3_column_text(st, 0);
        int64_t cnt = sqlite3_column_int64(st, 1);
        int n = snprintf(line + off, sizeof(line) - off, "%s%s=%lld",
                         off ? " " : "", nib ? (const char *)nib : "?",
                         (long long)cnt);
        if (n < 0 || (size_t)n >= sizeof(line) - off)
            break;
        off += (size_t)n;
    }
    sqlite3_finalize(st);
    LOG_WARN("validation_pack",
             "[validation_pack] utxos per-txid-prefix range counts: %s",
             line);
}

bool invariant_sentinel_commitment_audit_once(void)
{
    struct node_db *ndb = sentinel_ndb();
    if (!ndb || !ndb->open || !ndb->db)
        return false;

    struct utxo_commitment saved, computed;
    memset(&saved, 0, sizeof(saved));
    memset(&computed, 0, sizeof(computed));
    if (!utxo_commitment_load_checkpoint(ndb->db, &saved))
        return false; /* no checkpoint to audit against — skip */

    /* Tip-moved discard: a pass concurrent with live applies would
     * compare a moving set against a fixed checkpoint. */
    int64_t tip_before = tip_finalize_stage_last_height();
    utxo_commitment_compute_db(ndb->db, &computed);
    int64_t tip_after = tip_finalize_stage_last_height();

    atomic_fetch_add(&g_commitment_audits_total, 1);
    atomic_store(&g_last_audit_unix, platform_time_wall_unix());

    if (tip_before != tip_after) {
        atomic_fetch_add(&g_commitment_skipped_tip_moved, 1);
        return true; /* discarded, no verdict */
    }

    if (utxo_commitment_equal(&saved, &computed)) {
        if (atomic_load(&g_audit_blocker_active)) {
            atomic_store(&g_audit_blocker_active, false);
            blocker_clear("coins.commitment_spot_check");
            chain_linkage_hold_clear("commitment_audit");
            LOG_INFO("validation_pack",
                     "[validation_pack] coins.commitment_spot_check cleared "
                     "by a clean audit");
        }
        return true;
    }

    /* Reuse the boot path's stale-vs-corruption classifier: a stale
     * checkpoint (set advanced since it was saved) is legitimate and
     * never fires. */
    if (!utxo_recovery_xor_mismatch_is_corruption_candidate(
            saved.count, computed.count))
        return true;

    atomic_fetch_add(&g_commitment_mismatches_total, 1);
    atomic_store(&g_audit_blocker_active, true);
    audit_log_range_counts(ndb->db);
    sentinel_raise_blocker(
        "coins.commitment_spot_check",
        "XOR commitment mismatch vs checkpoint: count_expected=%llu "
        "count_got=%llu (corruption candidate; range counts logged)",
        (unsigned long long)saved.count,
        (unsigned long long)computed.count);
    /* HOLD: a node with a corrupted coin set must not extend the chain.
     * Boundary = the applied frontier (everything at/above it is built on
     * the suspect set). */
    int64_t tip = tip_finalize_stage_last_height();
    if (tip >= 0)
        chain_linkage_hold_set("commitment_audit", (int)tip + 1,
                               "utxo commitment corruption candidate");
    return true;
}

/* ── supervisor children ────────────────────────────────────────── */

static struct liveness_contract g_sweep_contract;
static struct liveness_contract g_audit_contract;
static _Atomic supervisor_child_id g_sweep_id = SUPERVISOR_INVALID_ID;
static _Atomic supervisor_child_id g_audit_id = SUPERVISOR_INVALID_ID;
static _Atomic int64_t g_sweep_ticks = 0;
static _Atomic int64_t g_audit_ticks = 0;

static void sweep_tick(struct liveness_contract *c)
{
    (void)c;
    (void)invariant_sentinel_sweep_once();
    int64_t marker = atomic_fetch_add(&g_sweep_ticks, 1) + 1;
    supervisor_progress(atomic_load(&g_sweep_id), marker);
    supervisor_tick(atomic_load(&g_sweep_id));
}

static void audit_tick(struct liveness_contract *c)
{
    (void)c;
    (void)invariant_sentinel_commitment_audit_once();
    int64_t marker = atomic_fetch_add(&g_audit_ticks, 1) + 1;
    supervisor_progress(atomic_load(&g_audit_id), marker);
    supervisor_tick(atomic_load(&g_audit_id));
}

void invariant_sentinel_register(void)
{
    supervisor_domains_init();
    if (atomic_load(&g_sweep_id) == SUPERVISOR_INVALID_ID) {
        liveness_contract_init(&g_sweep_contract, "chain.invariant_sweep");
        atomic_store(&g_sweep_contract.period_secs, SWEEP_PERIOD_SECS);
        atomic_store(&g_sweep_contract.deadline_secs, (int64_t)0);
        atomic_store(&g_sweep_contract.progress_max_quiet_us, (int64_t)0);
        g_sweep_contract.on_tick = sweep_tick;
        g_sweep_contract.on_stall = NULL;
        supervisor_child_id id =
            supervisor_register_in_domain(g_chain_sup, &g_sweep_contract);
        atomic_store(&g_sweep_id, id);
        if (id == SUPERVISOR_INVALID_ID)
            LOG_WARN("validation_pack",
                     "[validation_pack] sweep register failed");
    }
    if (atomic_load(&g_audit_id) == SUPERVISOR_INVALID_ID) {
        liveness_contract_init(&g_audit_contract, "coins.commitment_audit");
        atomic_store(&g_audit_contract.period_secs,
                     COMMITMENT_AUDIT_PERIOD_SECS);
        atomic_store(&g_audit_contract.deadline_secs, (int64_t)0);
        atomic_store(&g_audit_contract.progress_max_quiet_us, (int64_t)0);
        g_audit_contract.on_tick = audit_tick;
        g_audit_contract.on_stall = NULL;
        supervisor_child_id id =
            supervisor_register_in_domain(g_chain_sup, &g_audit_contract);
        atomic_store(&g_audit_id, id);
        if (id == SUPERVISOR_INVALID_ID)
            LOG_WARN("validation_pack",
                     "[validation_pack] audit register failed");
    }
}

/* ── health + dump ──────────────────────────────────────────────── */

static const char *const k_pack_blockers[] = {
    "chain.linkage_violation",
    "chain.coinbase_label_mismatch",
    "authority.pair_self_check",
    "window.consistency",
    "coins.commitment_spot_check",
    "mirror.divergence_located",
    "seed.linkage_gate",
};

bool invariant_sentinel_healthy(char *detail, int detail_cap)
{
    if (detail && detail_cap > 0)
        detail[0] = '\0';
    bool ok = !chain_linkage_hold_active();
    for (size_t i = 0;
         i < sizeof(k_pack_blockers) / sizeof(k_pack_blockers[0]); i++) {
        if (blocker_exists(k_pack_blockers[i])) {
            ok = false;
            if (detail && detail_cap > 0 && !detail[0])
                snprintf(detail, (size_t)detail_cap, "%s",
                         k_pack_blockers[i]);
        }
    }
    return ok;
}

/* Counter accessors for the cross-module dump (seed gate + locator
 * register theirs through these setters to keep ONE dumper). */
static _Atomic uint64_t g_seed_gate_runs = 0;
static _Atomic uint64_t g_seed_gate_refusals = 0;
static _Atomic uint64_t g_locator_runs = 0;
static _Atomic int g_locator_first_div_h = -1;
static _Atomic uint64_t g_loader_height_fallbacks = 0;

void invariant_sentinel_note_seed_gate(bool refused)
{
    atomic_fetch_add(&g_seed_gate_runs, 1);
    if (refused)
        atomic_fetch_add(&g_seed_gate_refusals, 1);
}

void invariant_sentinel_note_locator(int first_div_h)
{
    atomic_fetch_add(&g_locator_runs, 1);
    if (first_div_h >= 0)
        atomic_store(&g_locator_first_div_h, first_div_h);
}

void invariant_sentinel_note_loader_height_fallback(void)
{
    atomic_fetch_add(&g_loader_height_fallbacks, 1);
}

bool invariant_sentinel_dump_state_json(struct json_value *out,
                                        const char *key)
{
    (void)key;
    json_set_object(out);

    bool hold_active = false;
    int refuse_from = -1;
    char ids[96], reason[CHAIN_HOLD_REASON_MAX];
    chain_linkage_hold_snapshot(&hold_active, &refuse_from, ids,
                                (int)sizeof(ids), reason,
                                (int)sizeof(reason));
    json_push_kv_bool(out, "hold_active", hold_active);
    json_push_kv_int(out, "hold_refuse_from_h", (int64_t)refuse_from);
    json_push_kv_str(out, "hold_check_ids", ids);
    json_push_kv_str(out, "hold_reason", reason);

    json_push_kv_int(out, "linkage_violations_total",
                     (int64_t)chain_linkage_violations_total());
    json_push_kv_int(out, "hold_refusals_total",
                     (int64_t)chain_linkage_hold_refusals_total());
    json_push_kv_int(out, "offtip_switches_total",
                     (int64_t)chain_linkage_offtip_switches_total());

    json_push_kv_int(out, "pair_checks_total",
                     (int64_t)atomic_load(&g_pair_checks_total));
    json_push_kv_int(out, "pair_violations_total",
                     (int64_t)atomic_load(&g_pair_violations_total));

    json_push_kv_int(out, "sweeps_total",
                     (int64_t)atomic_load(&g_sweeps_total));
    json_push_kv_int(out, "sweep_violations_total",
                     (int64_t)atomic_load(&g_sweep_violations_total));
    json_push_kv_int(out, "last_sweep_unix",
                     atomic_load(&g_last_sweep_unix));

    json_push_kv_int(out, "commitment_audits_total",
                     (int64_t)atomic_load(&g_commitment_audits_total));
    json_push_kv_int(out, "commitment_mismatches_total",
                     (int64_t)atomic_load(&g_commitment_mismatches_total));
    json_push_kv_int(out, "commitment_skipped_tip_moved",
                     (int64_t)atomic_load(&g_commitment_skipped_tip_moved));
    json_push_kv_int(out, "last_audit_unix",
                     atomic_load(&g_last_audit_unix));

    json_push_kv_int(out, "seed_gate_runs",
                     (int64_t)atomic_load(&g_seed_gate_runs));
    json_push_kv_int(out, "seed_gate_refusals",
                     (int64_t)atomic_load(&g_seed_gate_refusals));
    json_push_kv_int(out, "locator_runs",
                     (int64_t)atomic_load(&g_locator_runs));
    json_push_kv_int(out, "locator_first_div_h",
                     (int64_t)atomic_load(&g_locator_first_div_h));
    json_push_kv_int(out, "loader_height_fallbacks",
                     (int64_t)atomic_load(&g_loader_height_fallbacks));

    pthread_mutex_lock(&g_detail_lock);
    json_push_kv_str(out, "last_sweep_detail", g_last_sweep_detail);
    json_push_kv_str(out, "last_pair_detail", g_last_pair_detail);
    pthread_mutex_unlock(&g_detail_lock);

    struct json_value blockers = {0};
    json_set_object(&blockers);
    for (size_t i = 0;
         i < sizeof(k_pack_blockers) / sizeof(k_pack_blockers[0]); i++)
        json_push_kv_bool(&blockers, k_pack_blockers[i],
                          blocker_exists(k_pack_blockers[i]));
    json_push_kv(out, "blockers", &blockers);
    json_free(&blockers);

    char detail[BLOCKER_ID_MAX];
    json_push_kv_bool(out, "healthy",
                      invariant_sentinel_healthy(detail,
                                                 (int)sizeof(detail)));
    return true;
}

#ifdef ZCL_TESTING
void invariant_sentinel_reset_for_testing(void)
{
    g_test_ndb = NULL;
    atomic_store(&g_pair_checks_total, (uint64_t)0);
    atomic_store(&g_pair_violations_total, (uint64_t)0);
    atomic_store(&g_sweeps_total, (uint64_t)0);
    atomic_store(&g_sweep_violations_total, (uint64_t)0);
    atomic_store(&g_commitment_audits_total, (uint64_t)0);
    atomic_store(&g_commitment_mismatches_total, (uint64_t)0);
    atomic_store(&g_commitment_skipped_tip_moved, (uint64_t)0);
    atomic_store(&g_sweep_blocker_active, false);
    atomic_store(&g_audit_blocker_active, false);
    atomic_store(&g_seed_gate_runs, (uint64_t)0);
    atomic_store(&g_seed_gate_refusals, (uint64_t)0);
    atomic_store(&g_locator_runs, (uint64_t)0);
    atomic_store(&g_locator_first_div_h, -1);
    g_prev_sweep_valid = false;
    g_prev_reorg_total = 0;
    g_prev_tip_finalize_cursor = -1;
    pthread_mutex_lock(&g_detail_lock);
    g_last_sweep_detail[0] = '\0';
    g_last_pair_detail[0] = '\0';
    pthread_mutex_unlock(&g_detail_lock);
}

void invariant_sentinel_set_node_db_for_testing(struct node_db *ndb)
{
    g_test_ndb = ndb;
}
#endif
