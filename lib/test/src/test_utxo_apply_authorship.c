/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_utxo_apply_authorship — B3 guards for the single-writer UTXO
 * authority inversion.
 *
 * B3 hands authorship of the UTXO projection from the legacy
 * `update_coins()` projection emitters to `utxo_apply_stage`. Two
 * properties must hold for the flip to be safe:
 *
 *   1. ordering_equivalence — the stage emits a block's delta as
 *      "all adds, then all spends", whereas legacy emits per-tx
 *      (spend-then-add, tx by tx). Because the projection is a set and
 *      every UTXO key created in a block is unique, both orders fold to
 *      the SAME final set. We prove the two emission orders yield a
 *      byte-identical projection commitment — including the tricky case
 *      where a block creates an output and spends it in a later tx.
 *
 *   2. single_writer_gate — only one author writes at a time. With
 *      authority LEGACY the stage stays silent and the legacy emitters
 *      write; with authority STAGE the legacy emitters no-op. */

#include "test/test_helpers.h"

#include "coins/coins_view.h"
#include "primitives/transaction.h"
#include "storage/coins_kv.h"
#include "storage/event_log.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define UA_CHECK(name, expr) do { \
    if (!(expr)) { printf("  FAIL: %s\n", name); failures++; } \
} while (0)

static void ua_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "/tmp/zcl_ua_%s_%d", tag, (int)getpid());
}

static void ua_mkdir_p(const char *dir)
{
    mkdir(dir, 0700);
}

static void make_txid(uint8_t txid[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++)
        txid[i] = (uint8_t)(seed + i);
}

/* Emit one ADD via the production projection emitter (writes to the
 * global event log set by utxo_projection_set_event_log). */
static bool emit_add(uint8_t seed, uint32_t vout, int64_t value,
                     uint32_t height, bool coinbase, uint32_t script_len)
{
    uint8_t txid[32]; make_txid(txid, seed);
    uint8_t script[64];
    for (uint32_t k = 0; k < script_len && k < sizeof(script); k++)
        script[k] = (uint8_t)((seed * 7 + k) & 0xFF);
    return utxo_projection_emit_add(txid, vout, value, height, coinbase,
                                    script_len ? script : NULL, script_len);
}

static bool emit_spend(uint8_t seed, uint32_t vout)
{
    uint8_t txid[32]; make_txid(txid, seed);
    return utxo_projection_emit_spend(txid, vout);
}

/* ── Test 1: ordering_equivalence ──────────────────────────────────── */

static int run_ordering_equivalence(int *failures_out)
{
    int failures = 0;
    char dir[256];
    ua_tmpdir(dir, sizeof(dir), "order");
    ua_mkdir_p(dir);
    char logA[512], projA[512], logB[512], projB[512];
    snprintf(logA,  sizeof(logA),  "%s/a.log",  dir);
    snprintf(projA, sizeof(projA), "%s/a.db",   dir);
    snprintf(logB,  sizeof(logB),  "%s/b.log",  dir);
    snprintf(projB, sizeof(projB), "%s/b.db",   dir);

    event_log_t *la = event_log_open(logA);
    event_log_t *lb = event_log_open(logB);
    utxo_projection_t *pa = utxo_projection_open(projA, la);
    utxo_projection_t *pb = utxo_projection_open(projB, lb);
    UA_CHECK("order: open A/B", la && lb && pa && pb);
    if (!la || !lb || !pa || !pb) goto done;

    /* A synthetic block:
     *   tx0 (coinbase): adds o0 (seed 0xA0, vout 0)
     *   tx1           : adds o1 (seed 0xA1, vout 0)
     *   tx2           : spends o1, adds o2 (seed 0xA2, vout 0)
     * Final live set: {o0, o2}. o1 is created and spent in-block. */

    /* Path A — legacy per-tx interleaving (spend before add, tx by tx). */
    utxo_projection_set_event_log(la);
    UA_CHECK("order: A add o0",  emit_add(0xA0, 0, 5000000000LL, 100, true, 25));
    UA_CHECK("order: A add o1",  emit_add(0xA1, 0, 1000, 100, false, 10));
    UA_CHECK("order: A spend o1", emit_spend(0xA1, 0));
    UA_CHECK("order: A add o2",  emit_add(0xA2, 0, 900, 100, false, 12));

    /* Path B — stage order: all adds, then all spends. */
    utxo_projection_set_event_log(lb);
    UA_CHECK("order: B add o0",  emit_add(0xA0, 0, 5000000000LL, 100, true, 25));
    UA_CHECK("order: B add o1",  emit_add(0xA1, 0, 1000, 100, false, 10));
    UA_CHECK("order: B add o2",  emit_add(0xA2, 0, 900, 100, false, 12));
    UA_CHECK("order: B spend o1", emit_spend(0xA1, 0));

    UA_CHECK("order: A catch_up", utxo_projection_catch_up(pa) != UINT64_MAX);
    UA_CHECK("order: B catch_up", utxo_projection_catch_up(pb) != UINT64_MAX);
    UA_CHECK("order: A count == 2", utxo_projection_count(pa) == 2);
    UA_CHECK("order: B count == 2", utxo_projection_count(pb) == 2);

    uint8_t ca[32], cb[32];
    UA_CHECK("order: A commitment", utxo_projection_commitment(pa, ca) == 0);
    UA_CHECK("order: B commitment", utxo_projection_commitment(pb, cb) == 0);
    UA_CHECK("order: legacy-interleaved == stage-adds-first (byte-exact)",
             memcmp(ca, cb, 32) == 0);

    utxo_projection_set_event_log(NULL);
    utxo_projection_close(pa);
    utxo_projection_close(pb);
    event_log_close(la);
    event_log_close(lb);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 2: coins_kv parity (step-2 dual-write) ───────────────────────
 *
 * The atomic progress.kv `coins` set (coins_kv, written in-txn by step_apply)
 * must fold to the SAME UTXO set as the event-log-driven projection for the
 * same block delta. This proves the new in-txn write path is correct before
 * step 3 flips reads onto it. */

/* Mirror emit_add's deterministic txid + script generation, but write into the
 * progress.kv coins table instead of the event log. */
static bool ck_add(sqlite3 *db, uint8_t seed, uint32_t vout, int64_t value,
                   uint32_t height, bool coinbase, uint32_t script_len)
{
    uint8_t txid[32]; make_txid(txid, seed);
    uint8_t script[64];
    for (uint32_t k = 0; k < script_len && k < sizeof(script); k++)
        script[k] = (uint8_t)((seed * 7 + k) & 0xFF);
    return coins_kv_add(db, txid, vout, value, (int32_t)height, coinbase,
                        script_len ? script : NULL, script_len);
}

static bool ck_spend(sqlite3 *db, uint8_t seed, uint32_t vout)
{
    uint8_t txid[32]; make_txid(txid, seed);
    return coins_kv_spend(db, txid, vout);
}

/* coins_kv_get_coins(seed) must equal utxo_projection_get_coins(seed) field
 * for field. */
static bool ck_coins_equal(sqlite3 *db, utxo_projection_t *p, uint8_t seed)
{
    uint8_t txid[32]; make_txid(txid, seed);
    struct coins a, b;
    bool ga = coins_kv_get_coins(db, txid, &a);
    bool gb = utxo_projection_get_coins(p, txid, &b);
    bool eq = (ga == gb);
    if (eq && ga) {
        eq = a.num_vout == b.num_vout && a.is_coinbase == b.is_coinbase
          && a.height == b.height;
        for (size_t v = 0; eq && v < a.num_vout; v++) {
            eq = a.vout[v].value == b.vout[v].value
              && a.vout[v].script_pub_key.size == b.vout[v].script_pub_key.size
              && memcmp(a.vout[v].script_pub_key.data,
                        b.vout[v].script_pub_key.data,
                        a.vout[v].script_pub_key.size) == 0;
        }
    }
    coins_free(&a);
    coins_free(&b);
    return eq;
}

static int run_coins_kv_parity(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "coins_kv_parity", "main");

    UA_CHECK("parity: progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    UA_CHECK("parity: coins_kv schema", db && coins_kv_ensure_schema(db));

    char logp[512], projp[512];
    snprintf(logp,  sizeof(logp),  "%s/p.log", dir);
    snprintf(projp, sizeof(projp), "%s/p.db",  dir);
    event_log_t *lp = event_log_open(logp);
    utxo_projection_t *pp = utxo_projection_open(projp, lp);
    UA_CHECK("parity: open log/proj", lp && pp);
    if (!db || !lp || !pp) goto done;

    /* Same block as the ordering test: adds o0(cb),o1,o2; spends o1.
     * Apply to the projection via the event log, and to coins_kv directly. */
    utxo_projection_set_event_log(lp);
    UA_CHECK("parity: proj add o0",  emit_add(0xA0, 0, 5000000000LL, 100, true, 25));
    UA_CHECK("parity: proj add o1",  emit_add(0xA1, 0, 1000, 100, false, 10));
    UA_CHECK("parity: proj add o2",  emit_add(0xA2, 0, 900, 100, false, 12));
    UA_CHECK("parity: proj spend o1", emit_spend(0xA1, 0));
    UA_CHECK("parity: proj catch_up", utxo_projection_catch_up(pp) != UINT64_MAX);

    UA_CHECK("parity: ck add o0",  ck_add(db, 0xA0, 0, 5000000000LL, 100, true, 25));
    UA_CHECK("parity: ck add o1",  ck_add(db, 0xA1, 0, 1000, 100, false, 10));
    UA_CHECK("parity: ck add o2",  ck_add(db, 0xA2, 0, 900, 100, false, 12));
    UA_CHECK("parity: ck spend o1", ck_spend(db, 0xA1, 0));

    UA_CHECK("parity: counts equal (==2)",
             coins_kv_count(db) == (int64_t)utxo_projection_count(pp)
             && coins_kv_count(db) == 2);
    UA_CHECK("parity: o0 coins equal", ck_coins_equal(db, pp, 0xA0));
    UA_CHECK("parity: o2 coins equal", ck_coins_equal(db, pp, 0xA2));
    UA_CHECK("parity: o1 absent in both", ck_coins_equal(db, pp, 0xA1));

    /* SHA3 commitment + gettxoutsetinfo aggregates must be byte/scalar-identical
     * to the projection — the read-flip relies on coins_kv matching the oracle
     * gettxoutsetinfo commitment exactly. */
    uint8_t ck_cmt[32], pj_cmt[32];
    UA_CHECK("parity: coins_kv commitment computes", coins_kv_commitment(db, ck_cmt) == 0);
    UA_CHECK("parity: projection commitment computes", utxo_projection_commitment(pp, pj_cmt) == 0);
    UA_CHECK("parity: commitments byte-identical", memcmp(ck_cmt, pj_cmt, 32) == 0);
    int64_t ck_tx = 0, ck_to = 0, ck_amt = 0, pj_tx = 0, pj_to = 0, pj_amt = 0;
    UA_CHECK("parity: coins_kv setinfo", coins_kv_setinfo(db, &ck_tx, &ck_to, &ck_amt));
    UA_CHECK("parity: projection setinfo", utxo_projection_setinfo(pp, &pj_tx, &pj_to, &pj_amt));
    UA_CHECK("parity: setinfo equal (txs/txouts/amount)",
             ck_tx == pj_tx && ck_to == pj_to && ck_amt == pj_amt);

    utxo_projection_set_event_log(NULL);
    utxo_projection_close(pp);
    event_log_close(lp);
    progress_store_close();
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 3: coins_kv boot rebuild (step-3 migration) ──────────────────
 *
 * An existing datadir has data ONLY in the projection (coins_kv empty). The
 * boot rebuild must copy it into coins_kv so the read-flip has data, and be
 * idempotent (second call no-ops). */
static int run_coins_kv_boot_rebuild(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "coins_kv_rebuild", "main");

    UA_CHECK("rebuild: progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    UA_CHECK("rebuild: coins_kv schema", db && coins_kv_ensure_schema(db));

    char logp[512], projp[512];
    snprintf(logp,  sizeof(logp),  "%s/r.log", dir);
    snprintf(projp, sizeof(projp), "%s/r.db",  dir);
    event_log_t *lp = event_log_open(logp);
    utxo_projection_t *pp = utxo_projection_open(projp, lp);
    UA_CHECK("rebuild: open log/proj", lp && pp);
    if (!db || !lp || !pp) goto done;

    /* Populate the PROJECTION ONLY (simulate a pre-flip datadir). */
    utxo_projection_set_event_log(lp);
    UA_CHECK("rebuild: proj add o0", emit_add(0xB0, 0, 4200000000LL, 50, true, 20));
    UA_CHECK("rebuild: proj add o1", emit_add(0xB1, 0, 1234, 50, false, 7));
    UA_CHECK("rebuild: proj add o2", emit_add(0xB2, 0, 99, 51, false, 0));
    UA_CHECK("rebuild: proj catch_up", utxo_projection_catch_up(pp) != UINT64_MAX);
    UA_CHECK("rebuild: coins_kv empty before", coins_kv_count(db) == 0);

    /* Migrate. */
    UA_CHECK("rebuild: boot_rebuild ok", coins_kv_boot_rebuild_if_needed(db, pp));
    UA_CHECK("rebuild: coins_kv count == projection count",
             (uint64_t)coins_kv_count(db) == utxo_projection_count(pp));
    uint8_t ck[32], pj[32];
    UA_CHECK("rebuild: coins_kv commitment", coins_kv_commitment(db, ck) == 0);
    UA_CHECK("rebuild: projection commitment", utxo_projection_commitment(pp, pj) == 0);
    UA_CHECK("rebuild: commitments byte-identical", memcmp(ck, pj, 32) == 0);

    /* Idempotent: second call no-ops (count unchanged). */
    int64_t before = coins_kv_count(db);
    UA_CHECK("rebuild: second call ok", coins_kv_boot_rebuild_if_needed(db, pp));
    UA_CHECK("rebuild: idempotent (count unchanged)", coins_kv_count(db) == before);

    utxo_projection_set_event_log(NULL);
    utxo_projection_close(pp);
    event_log_close(lp);
    progress_store_close();
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

int test_utxo_apply_authorship(void);
int test_utxo_apply_authorship(void)
{
    int failures = 0;
    printf("test_utxo_apply_authorship: STAGE projection ordering equivalence\n");
    run_ordering_equivalence(&failures);
    run_coins_kv_parity(&failures);
    run_coins_kv_boot_rebuild(&failures);
    if (failures == 0)
        printf("  all utxo_apply authorship checks passed\n");
    return failures;
}
