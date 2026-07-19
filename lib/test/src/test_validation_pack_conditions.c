/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Hermetic engine tests for three fail-loud validation-pack Conditions that
 * had NO detect/remedy/witness-level coverage: they were previously only
 * name-checked in test_condition_engine.c's registry-enumeration assertion
 * (register_all exposes the current self-heal set), never driven through a
 * detect -> remedy -> witness cycle.
 *
 *   - mirror_divergence_located (check 6): PERMANENT, no auto-repair.
 *   - tip_label_divergence (checks 1+2): PERMANENT; remedy wires the
 *     window_rebuild seam (re-derive [refuse_from, tip] + release the linkage
 *     holds), which needs the chain_linkage_hold latch this registry-only
 *     fixture does not raise, so here it reports FAILED. OR-compound detect
 *     predicate over two independent blockers.
 *   - state_window_inconsistent (checks 4+5): the ONLY one of the three
 *     with a real auto-terminating remedy arm (coins.commitment_spot_check)
 *     alongside a permanently-manual arm (window.consistency) — and its
 *     OR-compound detect means a partial remedy (only one arm cleared)
 *     must NOT be reported as a witnessed success.
 *
 * All three conditions read/write only the process-global typed-blocker
 * registry (lib/util/src/blocker.c) — no node.db, no live node, no clock
 * injection needed. Driven through the REAL condition engine
 * (condition_engine_tick) exactly like test_sticky_conditions.c, so the
 * assertions exercise the engine's honest-witness downgrade path
 * (COND_REMEDY_OK -> COND_REMEDY_UNWITNESSED when the witness still sees
 * the symptom), not just the bare detect() predicate. */

#include "test/test_helpers.h"

#include "coins/utxo_commitment.h"
#include "framework/condition.h"
#include "models/database.h"
#include "platform/clock.h"
#include "services/invariant_sentinel.h"
#include "util/blocker.h"
#include "validation/chain_linkage_check.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

/* ── Injected wall clock (same technique as test_sticky_conditions.c) ──
 * Every condition here polls on a real poll_secs/backoff_secs cadence. The
 * FIRST-ever tick after registration (last_poll_unix == 0) always runs
 * detect() regardless of poll_secs, but once that first tick has run (e.g.
 * to prove "no false-positive with nothing set"), the engine's poll gate
 * (`!active && now - last_poll < poll_secs`) would silently skip detect()
 * on the very next tick unless real wall-clock time actually advances by
 * poll_secs. Two condition_engine_tick() calls issued microseconds apart
 * in this test would otherwise land in the SAME wall second and the second
 * one would be silently swallowed — not a bug in the condition, a gate in
 * the test's clock. Installing an injected wall-clock source and stepping
 * it forward between ticks makes the poll/backoff cadence deterministic
 * instead of racing the real clock. Monotonic stays the real syscall (the
 * source's own reader would recurse otherwise). */
static _Atomic int64_t g_vpc_inj_wall_unix;
static int64_t vpc_inj_wall(void *user) { (void)user; return atomic_load(&g_vpc_inj_wall_unix); }
static int64_t vpc_inj_mono(void *user)
{
    (void)user;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)  // platform-ok:test-injected-wall-clock-source-must-not-recurse-monotonic
        return 0;
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}
static struct platform_clock_source g_vpc_inj_src = {
    .monotonic_us = vpc_inj_mono, .wall_unix = vpc_inj_wall };

static void vpc_clock_install(void)
{
    atomic_store(&g_vpc_inj_wall_unix, 1700000000);
    platform_clock_set_source(&g_vpc_inj_src);
}

/* Step the injected wall clock forward well past any poll_secs/backoff_secs
 * used by these three conditions (max backoff_secs=600) so the next tick's
 * poll/backoff gate never blocks on real-time drift. */
static void vpc_clock_step(void)
{
    atomic_fetch_add(&g_vpc_inj_wall_unix, 1000);
}

static void vpc_clock_clear(void)
{
    platform_clock_clear_source();
}

/* No header exists for these three conditions (see app/conditions/include/
 * conditions/ — only conditions with test-visible internal state get one).
 * Forward-declare the register_* entry points the same way
 * test_utxo_activation_paused.c does for block_failed_mask_at_tip. */
void register_mirror_divergence_located(void);
void register_tip_label_divergence(void);
void register_state_window_inconsistent(void);

/* Minimal utxo-row fixture, mirroring test_invariant_sentinel.c's
 * isn_insert_utxo: this test needs a REAL commitment-audit raise (not a
 * synthetic blocker_set) so g_isn_audit_blocker_active is genuinely true —
 * only then does invariant_sentinel_clear_commitment_blocker() (called by
 * the state_window_inconsistent remedy) actually clear anything. */
static bool vpc_insert_utxo(struct node_db *ndb, int salt)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT OR REPLACE INTO utxos"
        "(txid,vout,value,script,script_type,address_hash,height,is_coinbase)"
        " VALUES(?,?,?,?,0,?,?,0)";
    if (sqlite3_prepare_v2(ndb->db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    uint8_t txid[32];
    memset(txid, (uint8_t)(0x20 + salt), 32);
    static const uint8_t script[4] = { 0x76, 0xa9, 0x14, 0x00 };
    static const uint8_t addr[20] = {0};
    sqlite3_bind_blob(st, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, 0);
    sqlite3_bind_int64(st, 3, 60000 + salt);
    sqlite3_bind_blob(st, 4, script, sizeof(script), SQLITE_STATIC);
    sqlite3_bind_blob(st, 5, addr, sizeof(addr), SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, 2000 + salt);
    int rc = sqlite3_step(st);  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

/* Legitimately raise coins.commitment_spot_check via the real audit path
 * (2 consecutive shrink-verdict sweeps — the streak gate) so the raise
 * carries the audit module's own internal "diagnostic is genuinely
 * latched" bookkeeping, exactly as production does. Returns true once the
 * blocker is confirmed active. */
static bool vpc_raise_commitment_blocker_via_real_audit(struct node_db *ndb)
{
    for (int i = 0; i < 8; i++)
        if (!vpc_insert_utxo(ndb, i))
            return false;
    struct utxo_commitment uc;
    utxo_commitment_compute_db(ndb->db, &uc);
    if (!utxo_commitment_save_checkpoint(ndb->db, &uc) || uc.count != 8)
        return false;
    if (!invariant_sentinel_commitment_audit_once())
        return false; /* baseline pass: matching set, no fire */

    bool truncated = sqlite3_exec(ndb->db,
        "DELETE FROM utxos WHERE rowid IN "
        "(SELECT rowid FROM utxos LIMIT 4)",
        NULL, NULL, NULL) == SQLITE_OK;
    if (!truncated)
        return false;
    if (!invariant_sentinel_commitment_audit_once())
        return false; /* streak 1: gate swallows the single shrink */
    if (!invariant_sentinel_commitment_audit_once())
        return false; /* streak 2: fires the diagnostic blocker */
    return blocker_exists("coins.commitment_spot_check");
}

#define VPC_CHECK(name, expr) do {                                  \
    printf("validation_pack_conditions: %s... ", (name));           \
    if (expr) printf("OK\n");                                       \
    else { printf("FAIL\n"); failures++; }                          \
} while (0)

static void vpc_fixture_reset(void)
{
    condition_engine_reset_for_testing();
    blocker_reset_for_testing();
}

int test_validation_pack_conditions(void);
int test_validation_pack_conditions(void)
{
    int failures = 0;

    /* ───────────────── mirror_divergence_located ─────────────────
     * detect := blocker_exists("mirror.divergence_located");
     * remedy always returns FAILED (no automated repair seam yet);
     * witness := !detect() (honest re-read of the same blocker). */
    {
        vpc_fixture_reset();
        vpc_clock_install();
        register_mirror_divergence_located();

        condition_engine_tick();
        VPC_CHECK("mirror_divergence: no false-positive with no blocker",
                  condition_engine_get_active_count() == 0);
        vpc_clock_step();

        struct blocker_record r;
        VPC_CHECK("mirror_divergence: blocker_init ok",
                  blocker_init(&r, "mirror.divergence_located", "test",
                               BLOCKER_PERMANENT,
                               "bisected divergence fixture"));
        VPC_CHECK("mirror_divergence: blocker_set ok", blocker_set(&r) >= 0);

        condition_engine_tick();   /* detect -> remedy(FAILED) -> witness(false) */
        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(
            "mirror_divergence_located", &snap);
        VPC_CHECK("mirror_divergence: detected + stays active (no auto-repair)",
                  got && snap.currently_active);
        VPC_CHECK("mirror_divergence: remedy honestly reports FAILED",
                  got && snap.last_outcome == COND_REMEDY_FAILED);
        VPC_CHECK("mirror_divergence: at least one remedy attempt recorded",
                  got && snap.attempts >= 1);

        /* Only a REAL repair (operator / window_rebuild) clears the blocker;
         * simulate that here and confirm the witness (not a self-clear)
         * is what ends the episode. */
        blocker_clear("mirror.divergence_located");
        condition_engine_tick();
        VPC_CHECK("mirror_divergence: clears once the blocker is genuinely gone",
                  condition_engine_get_active_count() == 0);

        vpc_clock_clear();
        vpc_fixture_reset();
    }

    /* ───────────────── tip_label_divergence ─────────────────
     * detect := blocker_exists("chain.linkage_violation") ||
     *           blocker_exists("chain.coinbase_label_mismatch");
     * OR-compound over two independently-raised blockers. The remedy wires the
     * window_rebuild seam (re-derive [refuse_from, tip] via stage_rederive_range
     * + release the linkage holds), but here the fixture raises the blockers
     * DIRECTLY without the chain_linkage_hold_raise latch, so
     * chain_linkage_hold_refuse_from() is -1: the remedy has no rebuild window to
     * open and honestly reports FAILED. The witness must stay false while EITHER
     * arm is still set. */
    {
        vpc_fixture_reset();
        vpc_clock_install();
        register_tip_label_divergence();

        condition_engine_tick();
        VPC_CHECK("tip_label: no false-positive with no blocker",
                  condition_engine_get_active_count() == 0);
        vpc_clock_step();

        /* Raise only the coinbase-label-mismatch arm. */
        struct blocker_record r;
        VPC_CHECK("tip_label: coinbase_label_mismatch blocker_init ok",
                  blocker_init(&r, "chain.coinbase_label_mismatch", "test",
                               BLOCKER_PERMANENT, "label mismatch fixture"));
        VPC_CHECK("tip_label: coinbase_label_mismatch blocker_set ok",
                  blocker_set(&r) >= 0);

        condition_engine_tick();
        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(
            "tip_label_divergence", &snap);
        VPC_CHECK("tip_label: single-arm detect fires + stays active",
                  got && snap.currently_active);
        VPC_CHECK("tip_label: remedy reports FAILED without a hold-latch "
                  "rebuild window",
                  got && snap.last_outcome == COND_REMEDY_FAILED);

        /* Also raise the linkage-violation arm while the first is still set:
         * the OR-compound predicate must remain true. */
        struct blocker_record r2;
        VPC_CHECK("tip_label: linkage_violation blocker_init ok",
                  blocker_init(&r2, "chain.linkage_violation", "test",
                               BLOCKER_PERMANENT, "linkage fixture"));
        VPC_CHECK("tip_label: linkage_violation blocker_set ok",
                  blocker_set(&r2) >= 0);

        /* Clear ONLY the first arm; the second still holds detect() true, so
         * the honest witness must NOT report a clear. */
        blocker_clear("chain.coinbase_label_mismatch");
        condition_engine_tick();
        VPC_CHECK("tip_label: still active while the OTHER arm remains set",
                  condition_engine_get_active_count() == 1);

        /* Clear the remaining arm: only now must the episode end. */
        blocker_clear("chain.linkage_violation");
        condition_engine_tick();
        VPC_CHECK("tip_label: clears only once BOTH arms are gone",
                  condition_engine_get_active_count() == 0);

        vpc_clock_clear();
        vpc_fixture_reset();
    }

    /* ───────────────── state_window_inconsistent ─────────────────
     * detect := blocker_exists("window.consistency") ||
     *           blocker_exists("coins.commitment_spot_check");
     * remedy: the commitment_spot_check arm is auto-terminating (releases
     * the diagnostic and returns OK); window.consistency has no repair seam
     * (FAILED). The honest-witness contract under test: when BOTH arms are
     * set and remedy only clears the commitment arm, the OR-compound
     * detect() is still true, so the engine must downgrade the reported
     * COND_REMEDY_OK to COND_REMEDY_UNWITNESSED rather than lie about a
     * full clear. */
    {
        /* (A) window.consistency ONLY: no auto-repair seam -> stays active
         * until a real repair (simulated) clears it. */
        vpc_fixture_reset();
        vpc_clock_install();
        register_state_window_inconsistent();

        condition_engine_tick();
        VPC_CHECK("state_window: no false-positive with no blocker",
                  condition_engine_get_active_count() == 0);
        vpc_clock_step();

        struct blocker_record rw;
        VPC_CHECK("state_window: window.consistency blocker_init ok",
                  blocker_init(&rw, "window.consistency", "test",
                               BLOCKER_PERMANENT, "window fixture"));
        VPC_CHECK("state_window: window.consistency blocker_set ok",
                  blocker_set(&rw) >= 0);

        condition_engine_tick();
        struct condition_runtime_snapshot snap_a;
        bool got_a = condition_engine_get_registered_snapshot(
            "state_window_inconsistent", &snap_a);
        VPC_CHECK("state_window: window.consistency alone stays active",
                  got_a && snap_a.currently_active);
        VPC_CHECK("state_window: window.consistency arm has NO auto-repair (FAILED)",
                  got_a && snap_a.last_outcome == COND_REMEDY_FAILED);

        blocker_clear("window.consistency");
        condition_engine_tick();
        VPC_CHECK("state_window: clears once window.consistency is genuinely gone",
                  condition_engine_get_active_count() == 0);
        vpc_clock_clear();

        /* (B) coins.commitment_spot_check ONLY, raised via the REAL audit
         * path (so the auto-terminating remedy's dependency,
         * g_isn_audit_blocker_active, is genuinely set — a synthetic
         * blocker_set() alone would make the owner-hook clear a no-op):
         * the remedy clears the blocker itself and the witness confirms it
         * in the SAME tick — a genuine one-shot self-cure, not a false
         * green. */
        vpc_fixture_reset();
        invariant_sentinel_reset_for_testing();
        chain_linkage_reset_for_testing();
        register_state_window_inconsistent();

        /* cleared_count lives in the condition's own static struct (not the
         * test-scoped registry array condition_engine_reset_for_testing()
         * clears), so it is a running total across this whole test's
         * episodes (block (A) above already produced one witnessed clear).
         * Baseline it here and assert the RELATIVE +1 from this specific
         * self-cure, not an absolute value. */
        struct condition_runtime_snapshot snap_b0;
        (void)condition_engine_get_registered_snapshot(
            "state_window_inconsistent", &snap_b0);
        int cleared_before_b = snap_b0.cleared_count;

        struct node_db ndb_b;
        bool dbok_b = node_db_open(&ndb_b, ":memory:");
        VPC_CHECK("state_window: audit fixture node_db opens", dbok_b);
        if (dbok_b) {
            invariant_sentinel_set_node_db_for_testing(&ndb_b);
            VPC_CHECK("state_window: commitment_spot_check genuinely raised "
                      "via real audit (2-shrink streak)",
                      vpc_raise_commitment_blocker_via_real_audit(&ndb_b));

            condition_engine_tick(); /* detect -> remedy clears it -> witness OK */
            VPC_CHECK("state_window: commitment-only arm self-cures in one tick",
                      condition_engine_get_active_count() == 0);
            VPC_CHECK("state_window: remedy genuinely cleared the blocker",
                      !blocker_exists("coins.commitment_spot_check"));
            struct condition_runtime_snapshot snap_b;
            bool got_b = condition_engine_get_registered_snapshot(
                "state_window_inconsistent", &snap_b);
            VPC_CHECK("state_window: cleared_count advanced on the genuine cure",
                      got_b && snap_b.cleared_count == cleared_before_b + 1);

            node_db_close(&ndb_b);
        }

        /* (C) BOTH arms set (commitment arm raised via the real audit path
         * again): remedy clears only the commitment arm, but
         * window.consistency keeps the OR-compound detect() true. The
         * engine's honest-witness downgrade must fire: a remedy that
         * returned OK but did not clear the OBSERVABLE symptom is reported
         * as UNWITNESSED, never as a false OK. */
        vpc_fixture_reset();
        invariant_sentinel_reset_for_testing();
        chain_linkage_reset_for_testing();
        register_state_window_inconsistent();

        struct node_db ndb_c;
        bool dbok_c = node_db_open(&ndb_c, ":memory:");
        VPC_CHECK("state_window: both-arm audit fixture node_db opens", dbok_c);
        if (dbok_c) {
            invariant_sentinel_set_node_db_for_testing(&ndb_c);
            VPC_CHECK("state_window: both-arm commitment_spot_check "
                      "genuinely raised via real audit",
                      vpc_raise_commitment_blocker_via_real_audit(&ndb_c));

            struct blocker_record rw2;
            VPC_CHECK("state_window: both-arm window.consistency blocker_init ok",
                      blocker_init(&rw2, "window.consistency", "test",
                                   BLOCKER_PERMANENT,
                                   "window fixture (both-arm)"));
            VPC_CHECK("state_window: both-arm window.consistency blocker_set ok",
                      blocker_set(&rw2) >= 0);

            condition_engine_tick();
            VPC_CHECK("state_window: remedy cleared the commitment arm",
                      !blocker_exists("coins.commitment_spot_check"));
            VPC_CHECK("state_window: window.consistency arm untouched "
                      "(no repair seam)",
                      blocker_exists("window.consistency"));
            struct condition_runtime_snapshot snap_c;
            bool got_c = condition_engine_get_registered_snapshot(
                "state_window_inconsistent", &snap_c);
            VPC_CHECK("state_window: partial clear stays ACTIVE "
                      "(compound detect still true)",
                      got_c && snap_c.currently_active);
            VPC_CHECK("state_window: honest downgrade — OK-but-unwitnessed, "
                      "NEVER reported as a full clear",
                      got_c && snap_c.last_outcome == COND_REMEDY_UNWITNESSED);
            VPC_CHECK("state_window: NOT counted as a genuine clear yet",
                      condition_engine_get_active_count() == 1);

            /* Now clear the remaining arm: only then does the episode end. */
            blocker_clear("window.consistency");
            condition_engine_tick();
            VPC_CHECK("state_window: clears only once BOTH arms are truly gone",
                      condition_engine_get_active_count() == 0);

            node_db_close(&ndb_c);
        }

        vpc_fixture_reset();
        invariant_sentinel_reset_for_testing();
        chain_linkage_reset_for_testing();
    }

    printf("\n=== validation_pack_conditions: %d failure(s) ===\n", failures);
    return failures;
}
