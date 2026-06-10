/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the Phase 4b utxo_projection event-log consumer.
 *
 * Coverage matrix (per `docs/work/wt-phase4b-utxo-projection.md`):
 *   1. open_close_clean        — open empty, close, reopen, offset=0
 *   2. single_add_consumed     — 1 ADD, catch_up, count==1, get returns
 *   3. add_then_spend          — ADD + SPEND, catch_up, count==0
 *   4. replay_idempotent       — catch_up twice; second is a no-op
 *   5. commitment_canonical    — insertion order does not affect SHA3
 *   6. reorg_replace           — second ADD overrides + counts collision
 *   7. resume_from_partial     — N events, set offset mid-stream, reopen
 *
 * Scratch dirs under ./test-tmp/utxo_projection_<pid>_<tag>/ per the
 * project's no-/tmp convention.
 */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/utxo_projection.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define UP_CHECK(name, expr) do { \
    printf("utxo_projection: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int up_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void up_ensure_root(void)
{
    up_mkdir_p("./test-tmp");
}

static void make_txid(uint8_t txid[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++)
        txid[i] = (uint8_t)(seed + i);
}

/* Helper: append an EV_UTXO_ADD with a synthetic script `script_len`
 * bytes long whose contents are deterministic from the seed. */
static bool append_add(event_log_t *log, uint8_t seed, uint32_t vout,
                       int64_t value, uint32_t height, bool is_coinbase,
                       uint32_t script_len)
{
    uint8_t txid[32]; make_txid(txid, seed);
    uint8_t *script = NULL;
    if (script_len > 0) {
        script = malloc(script_len);  // raw-alloc-ok:test-only
        if (!script) return false;
        for (uint32_t k = 0; k < script_len; k++)
            script[k] = (uint8_t)((seed * 7 + k) & 0xFF);
    }

    struct ev_utxo_add_hdr hdr = {0};
    memcpy(hdr.txid, txid, 32);
    hdr.vout        = vout;
    hdr.value       = value;
    hdr.height      = height;
    hdr.is_coinbase = is_coinbase ? 1 : 0;
    hdr.script_len  = script_len;

    size_t cap = (size_t)EV_UTXO_ADD_HDR_WIRE_LEN + (size_t)script_len;
    uint8_t *buf = malloc(cap);  // raw-alloc-ok:test-only
    if (!buf) { free(script); return false; }
    size_t out_len = 0;
    bool ok = ev_utxo_add_serialize(&hdr, script, buf, cap, &out_len);
    if (ok) {
        uint64_t off = event_log_append(log, EV_UTXO_ADD, buf, out_len);
        ok = (off != UINT64_MAX);
    }
    free(buf);
    free(script);
    return ok;
}

static bool append_spend(event_log_t *log, uint8_t seed, uint32_t vout)
{
    struct ev_utxo_spend spend = {0};
    make_txid(spend.txid, seed);
    spend.vout = vout;
    uint8_t buf[EV_UTXO_SPEND_WIRE_LEN];
    if (!ev_utxo_spend_serialize(&spend, buf)) return false;
    uint64_t off = event_log_append(log, EV_UTXO_SPEND, buf, sizeof(buf));
    return off != UINT64_MAX;
}

/* ── Test 1: open_close_clean ──────────────────────────────────────── */

static int run_open_close_clean(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "utxo_projection","occ");
    up_mkdir_p(dir);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",          dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    UP_CHECK("occ: open empty log", log != NULL);
    if (!log) goto done;

    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    UP_CHECK("occ: open empty projection", p != NULL);
    UP_CHECK("occ: count == 0", utxo_projection_count(p) == 0);

    utxo_projection_close(p);
    /* Reopen — last_consumed_offset must still be 0 because we never
     * appended or called catch_up. */
    utxo_projection_t *p2 = utxo_projection_open(proj_path, log);
    UP_CHECK("occ: reopen", p2 != NULL);
    UP_CHECK("occ: count still 0 after reopen",
             utxo_projection_count(p2) == 0);
    utxo_projection_close(p2);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 2: single_add_consumed ───────────────────────────────────── */

static int run_single_add_consumed(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "utxo_projection","single");
    up_mkdir_p(dir);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",          dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    UP_CHECK("single: open log", log != NULL);
    if (!log) goto done;
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    UP_CHECK("single: open projection", p != NULL);
    if (!p) { event_log_close(log); goto done; }

    UP_CHECK("single: append EV_UTXO_ADD",
             append_add(log, 0x11, 3, 123456789LL, 42, false, 25));
    uint64_t off = utxo_projection_catch_up(p);
    UP_CHECK("single: catch_up != UINT64_MAX", off != UINT64_MAX);
    UP_CHECK("single: count == 1", utxo_projection_count(p) == 1);

    uint8_t txid[32]; make_txid(txid, 0x11);
    int64_t value = 0;
    uint8_t script[100] = {0};
    size_t script_len = 0;
    bool got = utxo_projection_get(p, txid, 3, &value,
                                    script, sizeof(script), &script_len);
    UP_CHECK("single: get returns true", got);
    UP_CHECK("single: value matches", value == 123456789LL);
    UP_CHECK("single: script_len matches", script_len == 25);
    bool sok = true;
    for (uint32_t k = 0; k < 25; k++)
        if (script[k] != (uint8_t)((0x11 * 7 + k) & 0xFF)) { sok = false; break; }
    UP_CHECK("single: script bytes match", sok);

    utxo_projection_close(p);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 3: add_then_spend ────────────────────────────────────────── */

static int run_add_then_spend(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "utxo_projection","spend");
    up_mkdir_p(dir);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",          dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    UP_CHECK("spend: open", log && p);
    if (!log || !p) goto done;

    UP_CHECK("spend: append ADD",   append_add(log, 0x22, 1, 5000, 100, false, 0));
    UP_CHECK("spend: append SPEND", append_spend(log, 0x22, 1));
    UP_CHECK("spend: catch_up", utxo_projection_catch_up(p) != UINT64_MAX);
    UP_CHECK("spend: count == 0", utxo_projection_count(p) == 0);
    uint8_t txid[32]; make_txid(txid, 0x22);
    UP_CHECK("spend: get returns false",
             !utxo_projection_get(p, txid, 1, NULL, NULL, 0, NULL));

    utxo_projection_close(p);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 4: replay_idempotent ─────────────────────────────────────── */

static int run_replay_idempotent(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "utxo_projection","replay");
    up_mkdir_p(dir);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",          dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    UP_CHECK("replay: open", log && p);
    if (!log || !p) goto done;

    /* 1000 ADDs + 500 SPENDs (every other ADD) → 500 live UTXOs. */
    const int N_ADD = 1000;
    for (int i = 0; i < N_ADD; i++)
        UP_CHECK("replay: append ADD",
                 append_add(log, (uint8_t)(i & 0xFF),
                            (uint32_t)(i >> 8),
                            (int64_t)(1000 + i), (uint32_t)i,
                            (i % 50) == 0, 10));
    /* SPEND the first 500 (matching txid seeds). */
    for (int i = 0; i < 500; i++)
        UP_CHECK("replay: append SPEND",
                 append_spend(log, (uint8_t)(i & 0xFF), (uint32_t)(i >> 8)));

    uint64_t off1 = utxo_projection_catch_up(p);
    UP_CHECK("replay: first catch_up OK", off1 != UINT64_MAX);
    UP_CHECK("replay: count == 500 after first", utxo_projection_count(p) == 500);

    uint8_t cmt1[32];
    UP_CHECK("replay: commitment OK", utxo_projection_commitment(p, cmt1) == 0);

    /* Second catch_up is a no-op (no new events). */
    uint64_t off2 = utxo_projection_catch_up(p);
    UP_CHECK("replay: second catch_up == first", off2 == off1);
    UP_CHECK("replay: count unchanged", utxo_projection_count(p) == 500);

    uint8_t cmt2[32];
    UP_CHECK("replay: second commitment OK", utxo_projection_commitment(p, cmt2) == 0);
    UP_CHECK("replay: idempotent commitment", memcmp(cmt1, cmt2, 32) == 0);

    utxo_projection_close(p);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 5: commitment_canonical ──────────────────────────────────── */

static int run_commitment_canonical(int *failures_out)
{
    int failures = 0;
    char dirA[256], dirB[256];
    test_fmt_tmpdir(dirA, sizeof(dirA), "utxo_projection", "cmtA");
    test_fmt_tmpdir(dirB, sizeof(dirB), "utxo_projection", "cmtB");
    up_mkdir_p(dirA);
    up_mkdir_p(dirB);

    char logA[512], projA[512], logB[512], projB[512];
    snprintf(logA,  sizeof(logA),  "%s/events.log",          dirA);
    snprintf(projA, sizeof(projA), "%s/utxo_projection.db", dirA);
    snprintf(logB,  sizeof(logB),  "%s/events.log",          dirB);
    snprintf(projB, sizeof(projB), "%s/utxo_projection.db", dirB);

    event_log_t *la = event_log_open(logA);
    event_log_t *lb = event_log_open(logB);
    utxo_projection_t *pa = utxo_projection_open(projA, la);
    utxo_projection_t *pb = utxo_projection_open(projB, lb);
    UP_CHECK("cmt: open all", la && lb && pa && pb);
    if (!la || !lb || !pa || !pb) goto done;

    /* Append the same 3 UTXOs in different orders. */
    UP_CHECK("cmt: A insert 1",
             append_add(la, 0xA1, 0,  100, 1, false, 5));
    UP_CHECK("cmt: A insert 2",
             append_add(la, 0xA2, 1,  200, 2, false, 7));
    UP_CHECK("cmt: A insert 3",
             append_add(la, 0xA3, 2,  300, 3, true,  3));

    UP_CHECK("cmt: B insert 3",
             append_add(lb, 0xA3, 2,  300, 3, true,  3));
    UP_CHECK("cmt: B insert 1",
             append_add(lb, 0xA1, 0,  100, 1, false, 5));
    UP_CHECK("cmt: B insert 2",
             append_add(lb, 0xA2, 1,  200, 2, false, 7));

    UP_CHECK("cmt: A catch_up", utxo_projection_catch_up(pa) != UINT64_MAX);
    UP_CHECK("cmt: B catch_up", utxo_projection_catch_up(pb) != UINT64_MAX);

    uint8_t ca[32], cb[32];
    UP_CHECK("cmt: A commitment", utxo_projection_commitment(pa, ca) == 0);
    UP_CHECK("cmt: B commitment", utxo_projection_commitment(pb, cb) == 0);
    UP_CHECK("cmt: canonical (different insert order, same commitment)",
             memcmp(ca, cb, 32) == 0);

    utxo_projection_close(pa);
    utxo_projection_close(pb);
    event_log_close(la);
    event_log_close(lb);
done:
    test_cleanup_tmpdir(dirA);
    test_cleanup_tmpdir(dirB);
    *failures_out += failures;
    return failures;
}

/* ── Test 6: reorg_replace ─────────────────────────────────────────── */

/* The dump exposes replace_collisions_total — but its raw counter lives
 * on the (opaque) handle. Use dump_state_json to read it. */
#include "json/json.h"

static int read_replace_collisions(void)
{
    struct json_value v = {0};
    if (!utxo_projection_dump_state_json(&v, NULL)) {
        json_free(&v);
        return -1;
    }
    const struct json_value *col = json_get(&v, "replace_collisions_total");
    int64_t n = col ? json_get_int(col) : -1;
    json_free(&v);
    return (int)n;
}

static int run_reorg_replace(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "utxo_projection","reorg");
    up_mkdir_p(dir);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",          dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    UP_CHECK("reorg: open", log && p);
    if (!log || !p) goto done;

    int before = read_replace_collisions();

    UP_CHECK("reorg: ADD v1", append_add(log, 0x33, 0, 1000, 1, false, 0));
    UP_CHECK("reorg: ADD v2", append_add(log, 0x33, 0, 2000, 2, false, 0));
    UP_CHECK("reorg: catch_up", utxo_projection_catch_up(p) != UINT64_MAX);
    UP_CHECK("reorg: count == 1 (replaced, not duplicated)",
             utxo_projection_count(p) == 1);

    uint8_t txid[32]; make_txid(txid, 0x33);
    int64_t value = 0;
    UP_CHECK("reorg: get returns second value",
             utxo_projection_get(p, txid, 0, &value, NULL, 0, NULL) &&
             value == 2000);

    int after = read_replace_collisions();
    UP_CHECK("reorg: replace_collisions_total ticked by 1",
             after >= 0 && before >= 0 && (after - before) == 1);

    utxo_projection_close(p);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 7: resume_from_partial ───────────────────────────────────── */

/* Stream until we hit `stop_after` events, return the offset of the
 * NEXT (not yet consumed) event. We use the same EVENT_LOG_FRAME_OVERHEAD
 * accounting as the projection. */
struct stop_ctx { int seen; int stop_after; uint64_t resume_off; };
static bool stop_cb(uint64_t offset, enum event_log_type type,
                    const void *payload, size_t len, void *user)
{
    (void)type; (void)payload;
    struct stop_ctx *c = user;
    c->resume_off = offset + EVENT_LOG_FRAME_OVERHEAD + (uint64_t)len;
    c->seen++;
    return c->seen < c->stop_after;
}

static int run_resume_from_partial(int *failures_out)
{
    int failures = 0;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "utxo_projection","resume");
    up_mkdir_p(dir);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",          dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    UP_CHECK("resume: open log", log != NULL);
    if (!log) goto done;

    /* Append 100 ADDs with distinct txid seeds so we can count. */
    enum { N = 100 };
    for (int i = 0; i < N; i++)
        UP_CHECK("resume: append",
                 append_add(log, (uint8_t)i, 0, (int64_t)(1000 + i),
                            (uint32_t)i, false, 4));

    /* Find the offset of event #50 (i.e., the resume point that skips
     * the first 50). Walking the stream this way mirrors what the
     * projection itself does. */
    struct stop_ctx ctx = { .seen = 0, .stop_after = 50, .resume_off = 0 };
    UP_CHECK("resume: stream walk to find offset",
             event_log_stream(log, 0, stop_cb, &ctx) == 0);
    UP_CHECK("resume: walked 50", ctx.seen == 50);
    UP_CHECK("resume: nonzero offset", ctx.resume_off > 0);

    /* Open the projection, then bump its last_consumed_offset to
     * `ctx.resume_off` via raw SQL (the public API doesn't expose it on
     * purpose). This simulates a crash-resume from offset 50. */
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    UP_CHECK("resume: open projection", p != NULL);
    if (!p) { event_log_close(log); goto done; }

    /* Seed projection_meta.last_consumed_offset using sqlite3 directly
     * (the projection ships its file path; we open a second handle just
     * for this test poke). */
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(proj_path, &db, SQLITE_OPEN_READWRITE, NULL);
    UP_CHECK("resume: poke db open", rc == SQLITE_OK);
    if (rc != SQLITE_OK) { utxo_projection_close(p); event_log_close(log); goto done; }
    char poke[256];
    snprintf(poke, sizeof(poke),
             "UPDATE projection_meta SET v='%" PRIu64 "' "
             "WHERE k='last_consumed_offset'",
             ctx.resume_off);
    char *err = NULL;
    rc = sqlite3_exec(db, poke, NULL, NULL, &err);
    UP_CHECK("resume: poke offset", rc == SQLITE_OK);
    if (err) sqlite3_free(err);
    sqlite3_close(db);

    /* Close + reopen the projection so it picks up the poked offset. */
    utxo_projection_close(p);
    utxo_projection_t *p2 = utxo_projection_open(proj_path, log);
    UP_CHECK("resume: reopen projection", p2 != NULL);
    if (!p2) { event_log_close(log); goto done; }

    UP_CHECK("resume: catch_up after offset poke",
             utxo_projection_catch_up(p2) != UINT64_MAX);
    /* Only the second-half 50 events should have been consumed. */
    UP_CHECK("resume: count == 50 (suffix only)",
             utxo_projection_count(p2) == 50);
    /* Spot-check: events seeded with i >= 50 are present, i < 50 are not. */
    uint8_t txid_hi[32]; make_txid(txid_hi, 75);
    uint8_t txid_lo[32]; make_txid(txid_lo, 10);
    UP_CHECK("resume: late seed present",
             utxo_projection_get(p2, txid_hi, 0, NULL, NULL, 0, NULL));
    UP_CHECK("resume: early seed absent",
             !utxo_projection_get(p2, txid_lo, 0, NULL, NULL, 0, NULL));

    utxo_projection_close(p2);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Entry point ───────────────────────────────────────────────────── */

int test_utxo_projection(void);
int test_utxo_projection(void)
{
    int failures = 0;
    up_ensure_root();

    printf("\n== test_utxo_projection ==\n");
    run_open_close_clean      (&failures);
    run_single_add_consumed   (&failures);
    run_add_then_spend        (&failures);
    run_replay_idempotent     (&failures);
    run_commitment_canonical  (&failures);
    run_reorg_replace         (&failures);
    run_resume_from_partial   (&failures);

    if (failures == 0)
        printf("test_utxo_projection: all OK\n");
    else
        printf("test_utxo_projection: %d FAILURE(S)\n", failures);
    return failures;
}
