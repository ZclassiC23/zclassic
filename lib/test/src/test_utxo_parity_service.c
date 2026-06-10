/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_utxo_parity_service — drives the standing UTXO parity service through
 * its FIXTURE reference (no live oracle, no sockets). The teeth are a paired
 * negative/positive control:
 *   CASE 1  same-height MATCH    → no operator page (subset-complete sentinel)
 *   CASE 2  same-height MISMATCH → persists drift, Condition pages
 *   CASE 3  COARSE reference     → never pages, clears any stale drift flag
 *   CASE 4  height skew          → never pages (LOCAL_ONLY)
 *   CASE 5  disabled tick        → no-op
 *   CASE 6  enabled tick         → waits while reorg-unsafe, then checks at the
 *                                  true applied height (exercises the live
 *                                  supervised path, not just the comparator)
 *
 * The same-height discipline (CASE 1 vs CASE 2 at one height) is the
 * subset-complete sentinel: a MATCH that pages, or a MISMATCH that does not,
 * fails the negative control.
 */

#include "test/test_helpers.h"

#include "services/utxo_parity_service.h"
#include "services/utxo_reference_source.h"
#include "services/utxo_audit_service.h"
#include "conditions/utxo_drift_detected.h"
#include "coins/utxo_commitment.h"
#include "config/runtime.h"
#include "models/database.h"
#include "event/event.h"
#include "framework/condition.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The fixture reference is height-independent, so the parity tick path
 * (which reads the live applied height) is exercised via the synchronous
 * utxo_parity_check_height(); the height we pass is the same-height label. */
#define PARITY_TEST_HEIGHT 3078015

static _Atomic int g_op_events;

static void op_obs(enum event_type type, uint32_t peer_id,
                   const void *payload, uint32_t payload_len, void *ctx)
{
    (void)type; (void)peer_id; (void)payload; (void)payload_len; (void)ctx;
    atomic_fetch_add(&g_op_events, 1);
}

static bool insert_test_utxo(struct node_db *ndb, uint8_t tag)
{
    sqlite3_stmt *s = NULL;
    const char *sql =
        "INSERT INTO utxos (txid,vout,value,script,script_type,"
        "address_hash,height,is_coinbase) VALUES (?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL) != SQLITE_OK)
        return false;

    uint8_t txid[32] = {0};
    uint8_t script[3] = {0x51, tag, 0xac};
    uint8_t addr[20] = {0};
    txid[0] = tag;
    addr[0] = tag;

    bool ok = sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int(s, 2, 0) == SQLITE_OK &&
              sqlite3_bind_int64(s, 3, 1000 + tag) == SQLITE_OK &&
              sqlite3_bind_blob(s, 4, script, sizeof(script), SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int(s, 5, 1) == SQLITE_OK &&
              sqlite3_bind_blob(s, 6, addr, sizeof(addr), SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int(s, 7, 100 + tag) == SQLITE_OK &&
              sqlite3_bind_int(s, 8, 0) == SQLITE_OK &&
              sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

static void hash_to_hex(const uint8_t hash[32], char out[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
}

/* Open a temp node.db with one UTXO and return its local commitment hex. */
static bool open_ndb_with_local(struct node_db *ndb, char *path_io,
                                char local_hex[65])
{
    int fd = mkstemp(path_io);
    if (fd < 0)
        return false;
    close(fd);
    if (!node_db_open(ndb, path_io))
        return false;
    if (!insert_test_utxo(ndb, 0x24))
        return false;
    uint8_t local_hash[32];
    uint64_t count = 0;
    utxo_commitment_sha3_compute(ndb->db, local_hash, &count);
    hash_to_hex(local_hash, local_hex);
    return true;
}

static void cleanup_case(struct node_db *ndb, const char *path)
{
    event_clear_observers(EV_OPERATOR_NEEDED);
    condition_engine_reset_for_testing();
    utxo_drift_detected_test_reset();
    utxo_parity_reset_for_test();
    node_db_close(ndb);
    unlink(path);
    atomic_store(&g_op_events, 0);
}

/* Shared per-case wiring: parity service + reference + drift Condition. */
static void wire_case(struct node_db *ndb,
                      const struct utxo_reference_source *src)
{
    struct utxo_parity_config cfg = {
        .enabled = true, .finality_depth = 0, .max_checks_per_tick = 1,
    };
    utxo_parity_reset_for_test();
    utxo_parity_init(&cfg, ndb);
    utxo_parity_set_reference_source(src);

    condition_engine_reset_for_testing();
    utxo_drift_detected_test_reset();
    utxo_drift_detected_test_set_node_db(ndb);
    register_utxo_drift_detected();

    atomic_store(&g_op_events, 0);
    event_observe(EV_OPERATOR_NEEDED, op_obs, NULL);
}

static int case_match_no_page(void)
{
    int failures = 0;
    TEST("utxo_parity: same-height MATCH does not page (negative control)") {
        char path[] = "/tmp/zcl23-parity-match-XXXXXX";
        struct node_db ndb;
        char local_hex[65];
        ASSERT(open_ndb_with_local(&ndb, path, local_hex));

        struct utxo_reference_source src;
        struct utxo_reference_source_fixture fx;
        utxo_reference_source_fixture_init(&src, &fx, "unit-fixture",
                                           local_hex, PARITY_TEST_HEIGHT, true);
        wire_case(&ndb, &src);

        /* Exercise the observer/marker path as well. */
        event_emitf(EV_CHAIN_TIP_COMMIT, 0, "from=%d to=%d reason=test",
                    PARITY_TEST_HEIGHT - 1, PARITY_TEST_HEIGHT);

        struct utxo_audit_result res;
        ASSERT(utxo_parity_check_height(PARITY_TEST_HEIGHT, &res).ok);
        ASSERT(res.status == UTXO_AUDIT_MATCH);

        int64_t drift = -1;
        ASSERT(node_db_state_get_int(&ndb, "utxo_drift_detected", &drift));
        ASSERT(drift == 0);

        condition_engine_tick();
        ASSERT(atomic_load(&g_op_events) == 0);
        ASSERT(condition_engine_get_active_count() == 0);

        cleanup_case(&ndb, path);
        PASS();
    } _test_next:;
    return failures;
}

static int case_mismatch_pages(void)
{
    int failures = 0;
    TEST("utxo_parity: same-height MISMATCH persists drift and pages") {
        char path[] = "/tmp/zcl23-parity-drift-XXXXXX";
        struct node_db ndb;
        char local_hex[65];
        ASSERT(open_ndb_with_local(&ndb, path, local_hex));

        /* Nibble-flip the local hash → a same-height byte mismatch. */
        char remote_hex[65];
        memcpy(remote_hex, local_hex, 65);
        remote_hex[0] = remote_hex[0] == '0' ? '1' : '0';

        struct utxo_reference_source src;
        struct utxo_reference_source_fixture fx;
        utxo_reference_source_fixture_init(&src, &fx, "unit-fixture",
                                           remote_hex, PARITY_TEST_HEIGHT, true);
        wire_case(&ndb, &src);

        struct utxo_audit_result res;
        ASSERT(utxo_parity_check_height(PARITY_TEST_HEIGHT, &res).ok);
        ASSERT(res.status == UTXO_AUDIT_DRIFT);

        int64_t drift = 0;
        ASSERT(node_db_state_get_int(&ndb, "utxo_drift_detected", &drift));
        ASSERT(drift == 1);

        /* Condition picks up the persisted flag and pages on the first tick
         * (max_attempts=1, remedy FAILED). */
        condition_engine_tick();
        ASSERT(atomic_load(&g_op_events) >= 1);
        ASSERT(condition_engine_get_unresolved_count() == 1);

        /* Full loop: clear the flag → the witness clears the Condition. */
        ASSERT(node_db_state_set_int(&ndb, "utxo_drift_detected", 0));
        condition_engine_tick();
        ASSERT(condition_engine_get_active_count() == 0);

        cleanup_case(&ndb, path);
        PASS();
    } _test_next:;
    return failures;
}

static int case_coarse_never_pages(void)
{
    int failures = 0;
    TEST("utxo_parity: coarse reference never pages and clears stale drift") {
        char path[] = "/tmp/zcl23-parity-coarse-XXXXXX";
        struct node_db ndb;
        char local_hex[65];
        ASSERT(open_ndb_with_local(&ndb, path, local_hex));

        /* Pre-seed a stale drift flag from a prior exact divergence. The
         * coarse check must CLEAR it (a height confirmation cannot prove
         * bytes, but it must not let an old drift keep paging). */
        ASSERT(node_db_state_set_int(&ndb, "utxo_drift_detected", 1));

        struct utxo_reference_source src;
        struct utxo_reference_source_fixture fx;
        utxo_reference_source_fixture_init(&src, &fx, "coarse-fixture",
                                           "", PARITY_TEST_HEIGHT, false);
        wire_case(&ndb, &src);

        struct utxo_audit_result res;
        ASSERT(utxo_parity_check_height(PARITY_TEST_HEIGHT, &res).ok);
        ASSERT(res.status == UTXO_AUDIT_MATCH); /* heights agree, height-only */
        ASSERT(res.drift_detected == false);

        int64_t drift = -1;
        ASSERT(node_db_state_get_int(&ndb, "utxo_drift_detected", &drift));
        ASSERT(drift == 0); /* coarse explicitly cleared the stale flag */

        condition_engine_tick();
        ASSERT(atomic_load(&g_op_events) == 0);
        ASSERT(condition_engine_get_active_count() == 0);

        cleanup_case(&ndb, path);
        PASS();
    } _test_next:;
    return failures;
}

static int case_height_skew_no_page(void)
{
    int failures = 0;
    TEST("utxo_parity: exact reference at a different height never DRIFTs") {
        char path[] = "/tmp/zcl23-parity-skew-XXXXXX";
        struct node_db ndb;
        char local_hex[65];
        ASSERT(open_ndb_with_local(&ndb, path, local_hex));

        /* An exact reference whose hash differs AND whose height differs from
         * the local applied height: the same-height guard must record this as
         * LOCAL_ONLY (height skew), never a byte DRIFT — the structural
         * false-DRIFT defense. */
        char remote_hex[65];
        memcpy(remote_hex, local_hex, 65);
        remote_hex[0] = remote_hex[0] == '0' ? '1' : '0';

        struct utxo_reference_source src;
        struct utxo_reference_source_fixture fx;
        utxo_reference_source_fixture_init(&src, &fx, "skew-fixture",
                                           remote_hex,
                                           PARITY_TEST_HEIGHT - 50, true);
        wire_case(&ndb, &src);

        struct utxo_audit_result res;
        ASSERT(utxo_parity_check_height(PARITY_TEST_HEIGHT, &res).ok);
        ASSERT(res.status == UTXO_AUDIT_LOCAL_ONLY);
        ASSERT(res.drift_detected == false);

        int64_t drift = -1;
        /* utxo_audit_local persists 'utxo_drift_detected' only on the
         * compare_remote path, so the skew branch leaves it untouched —
         * verify it is not raised. */
        if (node_db_state_get_int(&ndb, "utxo_drift_detected", &drift))
            ASSERT(drift == 0);

        condition_engine_tick();
        ASSERT(atomic_load(&g_op_events) == 0);
        ASSERT(condition_engine_get_active_count() == 0);

        cleanup_case(&ndb, path);
        PASS();
    } _test_next:;
    return failures;
}

static int case_dormant(void)
{
    int failures = 0;
    TEST("utxo_parity: tick is a no-op when disabled or without a frontier") {
        char path[] = "/tmp/zcl23-parity-dormant-XXXXXX";
        struct node_db ndb;
        char local_hex[65];
        ASSERT(open_ndb_with_local(&ndb, path, local_hex));

        /* Disabled config + no reference: the supervised tick must do nothing
         * (no checks, no page), even if a frontier marker exists. */
        struct utxo_parity_config cfg = {
            .enabled = false, .finality_depth = 100, .max_checks_per_tick = 1,
        };
        utxo_parity_reset_for_test();
        utxo_parity_init(&cfg, &ndb);
        utxo_parity_set_frontier_for_test(PARITY_TEST_HEIGHT);

        atomic_store(&g_op_events, 0);
        event_observe(EV_OPERATOR_NEEDED, op_obs, NULL);

        utxo_parity_tick_once();
        ASSERT(atomic_load(&g_op_events) == 0);

        cleanup_case(&ndb, path);
        PASS();
    } _test_next:;
    return failures;
}

/* Exercise the live supervised tick (not just the synchronous comparator):
 * it must WAIT while the live applied height is still reorg-unsafe, then run a
 * same-height check at the TRUE applied height once it is reorg-safe. This is
 * the regression guard for the same-height label fix — the tick must never
 * compare the live set against a reference at a relabeled stable_ceiling. */
static int case_enabled_tick_checks_at_applied(void)
{
    int failures = 0;
    TEST("utxo_parity: enabled tick waits while reorg-unsafe, checks at applied") {
        char path[] = "/tmp/zcl23-parity-tick-XXXXXX";
        struct node_db ndb;
        char local_hex[65];
        ASSERT(open_ndb_with_local(&ndb, path, local_hex));

        /* The single test UTXO sets the live applied height. */
        int32_t applied = (int32_t)app_runtime_node_db_utxo_max_height(&ndb);
        ASSERT(applied > 0);

        /* Exact fixture AT the applied height with the matching local hash. */
        struct utxo_reference_source src;
        struct utxo_reference_source_fixture fx;
        utxo_reference_source_fixture_init(&src, &fx, "tick-fixture",
                                           local_hex, applied, true);
        wire_case(&ndb, &src);  /* finality_depth defaults to 100 */
        setenv("ZCL_PARITY_ENABLE", "1", 1);

        /* Reorg-UNSAFE: applied sits ABOVE frontier - finality_depth, so there
         * is no reorg-safe height the live set reflects → the tick must not
         * run any check (no audit state written). */
        utxo_parity_set_frontier_for_test(applied + 50);  /* ceiling = applied-50 */
        utxo_parity_tick_once();
        int64_t h = -1;
        ASSERT(!node_db_state_get_int(&ndb, "utxo_audit_last_height", &h));

        /* Reorg-SAFE: frontier far enough ahead that applied <= ceiling → the
         * tick checks at exactly the applied height (MATCH, no page). */
        utxo_parity_set_frontier_for_test(applied + 200); /* ceiling = applied+100 */
        utxo_parity_tick_once();
        unsetenv("ZCL_PARITY_ENABLE");

        ASSERT(node_db_state_get_int(&ndb, "utxo_audit_last_height", &h));
        ASSERT(h == applied);  /* checked at the TRUE applied height, not a clamp */
        int64_t drift = -1;
        ASSERT(node_db_state_get_int(&ndb, "utxo_drift_detected", &drift));
        ASSERT(drift == 0);

        condition_engine_tick();
        ASSERT(atomic_load(&g_op_events) == 0);
        ASSERT(condition_engine_get_active_count() == 0);

        cleanup_case(&ndb, path);
        PASS();
    } _test_next:;
    return failures;
}

int test_utxo_parity_service(void)
{
    int failures = 0;
    /* Enable synchronous event dispatch (and start from a clean observer
     * table) so the EV_OPERATOR_NEEDED page reaches our observer even under
     * ZCL_TEST_ONLY, which skips the full-harness event_log_init. */
    event_log_init();
    failures += case_match_no_page();
    failures += case_mismatch_pages();
    failures += case_coarse_never_pages();
    failures += case_height_skew_no_page();
    failures += case_dormant();
    failures += case_enabled_tick_checks_at_applied();
    return failures;
}
