/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for body_history — "which block bodies BELOW my tip am I missing?"
 *
 * Two of these tests exist because of a specific failure. A previous attempt
 * at this fix was rejected by its own reviewer with:
 *
 *     "the fix contains the same fail-open defect it was written to cure:
 *      an unreadable block index publishes as 'no hole'."
 *
 * So the census has THREE outcomes, and the tests below hunt specifically
 * for a collapse back to two:
 *
 *   test_bh_unreadable_index_is_not_no_hole  — every height indeterminate
 *      must land on UNKNOWN with unmeasured_count == the whole window, and
 *      must NOT report COMPLETE, must NOT report proven, and must not be
 *      distinguishable-only-by-a-zero from a clean node.
 *   test_bh_partial_read_does_not_certify    — half the window readable and
 *      fully held still is not COMPLETE.
 *   test_bh_at_tip_requires_proven_history   — the AT_TIP planner refuses
 *      the transition on UNKNOWN exactly as hard as on INCOMPLETE.
 *
 * The positive direction is covered too: a deliberate hole below the tip is
 * found, counted, located, and handed to the real download manager.
 */

#include "test/test_core.h"

#include "storage/body_history.h"
#include "storage/body_coverage.h"
#include "storage/progress_store.h"
#include "sync/sync_planner.h"
#include "net/download.h"
#include "json/json.h"
#include "core/uint256.h"

#include <sqlite3.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── A synthetic chain the probe can read ───────────────────────── */

/* Models the two things a real probe distinguishes: whether the height is
 * indexed at all (an unreadable/absent block-index entry) and, if it is,
 * whether the body is on disk. */
struct bh_fake_chain {
    int64_t  height_count;
    uint8_t *indexed;    /* 0 = index entry unreadable (INDETERMINATE) */
    uint8_t *have_data;  /* 1 = BLOCK_HAVE_DATA set */
    uint64_t probe_calls;
};

static void bh_fake_hash(struct uint256 *out, int64_t h)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < 8; i++)
        out->data[i] = (uint8_t)((uint64_t)h >> (8 * i));
    out->data[31] = 0xAB;
}

static bool bh_fake_chain_init(struct bh_fake_chain *c, int64_t n)
{
    memset(c, 0, sizeof(*c));
    c->height_count = n;
    c->indexed = calloc((size_t)n, 1);
    c->have_data = calloc((size_t)n, 1);
    if (!c->indexed || !c->have_data) {
        free(c->indexed);
        free(c->have_data);
        memset(c, 0, sizeof(*c));
        return false;
    }
    for (int64_t h = 0; h < n; h++) {
        c->indexed[h] = 1;
        c->have_data[h] = 1;
    }
    return true;
}

static void bh_fake_chain_free(struct bh_fake_chain *c)
{
    free(c->indexed);
    free(c->have_data);
    memset(c, 0, sizeof(*c));
}

/* Mirrors gf_history_probe in app/services/src/gap_fill_service.c: an
 * unreadable index entry yields INDETERMINATE, never MISSING and never
 * HAVE. */
static enum body_history_probe bh_fake_probe(int64_t height,
                                             struct uint256 *out_hash,
                                             void *ctx)
{
    struct bh_fake_chain *c = (struct bh_fake_chain *)ctx;
    if (!c)
        return BODY_HISTORY_PROBE_INDETERMINATE;
    c->probe_calls++;
    if (height < 0 || height >= c->height_count)
        return BODY_HISTORY_PROBE_INDETERMINATE;
    if (!c->indexed[height])
        return BODY_HISTORY_PROBE_INDETERMINATE;
    if (c->have_data[height])
        return BODY_HISTORY_PROBE_HAVE;
    if (out_hash)
        bh_fake_hash(out_hash, height);
    return BODY_HISTORY_PROBE_MISSING;
}

/* Drive the census over [0, tip] to completion (bounded passes), folding
 * into the caller's maps. Returns total heights handed back as MISSING. */
static int64_t bh_run_full_census(struct body_history_census *census,
                                  struct body_coverage_map *held,
                                  struct body_coverage_map *measured,
                                  struct bh_fake_chain *chain,
                                  int64_t tip,
                                  int64_t budget,
                                  struct uint256 *collect_h,
                                  int32_t *collect_n,
                                  size_t collect_cap,
                                  size_t *collected_out)
{
    int64_t total_missing = 0;
    size_t collected = 0;
    uint8_t *classes = malloc((size_t)budget);
    struct uint256 *hashes = malloc((size_t)budget * sizeof(*hashes));
    if (!classes || !hashes) {
        free(classes);
        free(hashes);
        if (collected_out) *collected_out = 0;
        return -1;
    }

    /* Bounded by construction: one pass per budget-sized window, plus slack.
     * A runaway cursor fails the loop cap rather than hanging the suite. */
    int64_t max_passes = (tip / budget) + 8;
    for (int64_t p = 0; p < max_passes; p++) {
        int64_t lo = 0, hi = 0;
        if (!body_history_census_plan(census, 0, tip, budget, &lo, &hi))
            break;
        size_t n = body_history_census_probe_window(lo, hi, bh_fake_probe,
                                                    chain, classes, hashes,
                                                    (size_t)budget);
        struct body_history_pass_result res;
        if (!body_history_census_fold(census, held, measured, lo, classes, n,
                                      &res))
            break;
        body_history_census_advance(census, lo, hi);
        total_missing += res.missing;
        if (collect_h && collect_n && collected < collect_cap) {
            collected += body_history_census_collect_missing(
                lo, classes, hashes, n,
                collect_h + collected, collect_n + collected,
                collect_cap - collected);
        }
        if (census->sweeps_completed > 0)
            break;
    }

    free(classes);
    free(hashes);
    if (collected_out) *collected_out = collected;
    return total_missing;
}

/* ── 1. The rejected defect: unreadable index != no hole ────────── */

static int test_bh_unreadable_index_is_not_no_hole(void)
{
    int failures = 0;
    TEST("unreadable index reports could-not-determine, never 'no hole'") {
        struct bh_fake_chain chain;
        ASSERT(bh_fake_chain_init(&chain, 500));
        /* The WHOLE window is unreadable — the exact case that must not
         * publish as a clean bill of health. */
        for (int64_t h = 0; h < 500; h++)
            chain.indexed[h] = 0;

        struct body_coverage_map held, measured;
        body_coverage_init(&held);
        body_coverage_init(&measured);
        struct body_history_census census;
        body_history_census_init(&census);

        int64_t missing = bh_run_full_census(&census, &held, &measured,
                                             &chain, 499, 64,
                                             NULL, NULL, 0, NULL);
        ASSERT(missing == 0);              /* nothing was found missing ... */
        ASSERT(census.heights_indeterminate == 500); /* ... because nothing
                                                      * could be read */
        ASSERT(census.heights_examined == 0);

        /* Neither map may have absorbed an unreadable height. */
        ASSERT(body_coverage_total_covered(&measured) == 0);
        ASSERT(body_coverage_total_covered(&held) == 0);

        struct body_history_verdict v;
        ASSERT(body_history_evaluate(&held, &measured, 0, 499, &v));

        /* THE assertion. missing_count is 0 — and that must NOT read as
         * "no hole". */
        ASSERT(v.missing_count == 0);
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);
        ASSERT(v.status != BODY_HISTORY_COMPLETE);
        ASSERT(!body_history_verdict_is_proven(&v));
        ASSERT(v.unmeasured_count == 500);
        ASSERT(v.lowest_unmeasured == 0);
        ASSERT(v.lowest_missing == -1);
        ASSERT(strcmp(body_history_status_name(v.status), "unknown") == 0);

        /* And it must be DISTINGUISHABLE from a genuinely complete node,
         * not merely "also zero missing". */
        struct body_coverage_map full_held, full_measured;
        body_coverage_init(&full_held);
        body_coverage_init(&full_measured);
        ASSERT(body_coverage_insert(&full_held, 0, 499));
        ASSERT(body_coverage_insert(&full_measured, 0, 499));
        struct body_history_verdict good;
        ASSERT(body_history_evaluate(&full_held, &full_measured, 0, 499,
                                     &good));
        ASSERT(good.status == BODY_HISTORY_COMPLETE);
        ASSERT(body_history_verdict_is_proven(&good));
        ASSERT(good.missing_count == v.missing_count); /* same zero ... */
        ASSERT(good.status != v.status);               /* ... different state */
        ASSERT(good.unmeasured_count != v.unmeasured_count);
        body_coverage_free(&full_held);
        body_coverage_free(&full_measured);

        body_coverage_free(&held);
        body_coverage_free(&measured);
        bh_fake_chain_free(&chain);
        PASS();
    } _test_next:;
    return failures;
}

/* A probe that cannot run at all — the caller could not even reach the
 * index. Must be indistinguishable in OUTCOME from the unreadable case
 * above, and must never leave a slot looking answered. */
static int test_bh_null_probe_leaves_everything_unmeasured(void)
{
    int failures = 0;
    TEST("a probe that cannot run leaves every height unmeasured") {
        uint8_t classes[128];
        struct uint256 hashes[128];
        /* Poison the buffers with the HAVE value first: if probe_window
         * ever skipped its pre-fill, this would sail through as coverage. */
        memset(classes, BODY_HISTORY_PROBE_HAVE, sizeof(classes));
        memset(hashes, 0xFF, sizeof(hashes));

        size_t n = body_history_census_probe_window(0, 127, NULL, NULL,
                                                    classes, hashes, 128);
        ASSERT(n == 0);
        for (size_t i = 0; i < 128; i++)
            ASSERT(classes[i] == BODY_HISTORY_PROBE_INDETERMINATE);

        struct body_coverage_map held, measured;
        body_coverage_init(&held);
        body_coverage_init(&measured);
        struct body_history_census census;
        body_history_census_init(&census);

        struct body_history_pass_result res;
        /* Fold the full poisoned window even though probe_window reported
         * 0: a caller that trusts its own cap must still measure nothing. */
        ASSERT(body_history_census_fold(&census, &held, &measured, 0,
                                        classes, 128, &res));
        ASSERT(res.indeterminate == 128);
        ASSERT(res.examined == 0);
        ASSERT(body_coverage_total_covered(&measured) == 0);

        struct body_history_verdict v;
        ASSERT(body_history_evaluate(&held, &measured, 0, 127, &v));
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);
        ASSERT(v.unmeasured_count == 128);
        ASSERT(!body_history_verdict_is_proven(&v));

        body_coverage_free(&held);
        body_coverage_free(&measured);
        PASS();
    } _test_next:;
    return failures;
}

/* Half readable and every readable height held: still not COMPLETE. This is
 * the subtler shape of the same defect — a census that stopped early and
 * certified what it happened to see. */
static int test_bh_partial_read_does_not_certify(void)
{
    int failures = 0;
    TEST("a partially readable window never certifies complete") {
        struct body_coverage_map held, measured;
        body_coverage_init(&held);
        body_coverage_init(&measured);
        /* Probed and held: [500, 999]. Never probed: [0, 499]. */
        ASSERT(body_coverage_insert(&held, 500, 999));
        ASSERT(body_coverage_insert(&measured, 500, 999));

        struct body_history_verdict v;
        ASSERT(body_history_evaluate(&held, &measured, 0, 999, &v));
        ASSERT(v.missing_count == 0);
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);
        ASSERT(!body_history_verdict_is_proven(&v));
        ASSERT(v.unmeasured_count == 500);
        ASSERT(v.lowest_unmeasured == 0);
        ASSERT(v.held_count == 500);

        /* Narrow the question to only what WAS measured and it is complete —
         * proving the UNKNOWN above is about scope, not about a broken
         * count. */
        struct body_history_verdict narrow;
        ASSERT(body_history_evaluate(&held, &measured, 500, 999, &narrow));
        ASSERT(narrow.status == BODY_HISTORY_COMPLETE);
        ASSERT(body_history_verdict_is_proven(&narrow));

        body_coverage_free(&held);
        body_coverage_free(&measured);
        PASS();
    } _test_next:;
    return failures;
}

/* Bad arguments and allocation-free failure paths all land on UNKNOWN. */
static int test_bh_evaluate_failure_paths_are_unknown(void)
{
    int failures = 0;
    TEST("every evaluate failure path publishes unknown, not clean") {
        struct body_coverage_map held, measured;
        body_coverage_init(&held);
        body_coverage_init(&measured);
        ASSERT(body_coverage_insert(&held, 0, 99));
        ASSERT(body_coverage_insert(&measured, 0, 99));

        struct body_history_verdict v;

        /* Poison first so a function that returns early without resetting
         * would be caught. */
        memset(&v, 0xEE, sizeof(v));
        ASSERT(!body_history_evaluate(NULL, &measured, 0, 99, &v));
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);
        ASSERT(!body_history_verdict_is_proven(&v));

        memset(&v, 0xEE, sizeof(v));
        ASSERT(!body_history_evaluate(&held, NULL, 0, 99, &v));
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);

        memset(&v, 0xEE, sizeof(v));
        ASSERT(!body_history_evaluate(&held, &measured, -1, 99, &v));
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);

        memset(&v, 0xEE, sizeof(v));
        ASSERT(!body_history_evaluate(&held, &measured, 100, 50, &v));
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);
        ASSERT(v.missing_count == 0);
        ASSERT(v.lowest_missing == -1);

        /* A zero-initialized verdict — the shape every uninitialized caller
         * gets — is UNKNOWN, not COMPLETE. */
        struct body_history_verdict zero;
        memset(&zero, 0, sizeof(zero));
        ASSERT(zero.status == BODY_HISTORY_UNKNOWN);
        ASSERT(!body_history_verdict_is_proven(&zero));

        body_coverage_free(&held);
        body_coverage_free(&measured);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 2. The positive direction: a real hole below the tip ───────── */

static int test_bh_below_tip_hole_found_and_enqueued(void)
{
    int failures = 0;
    TEST("a deliberate hole below the tip is found, located and enqueued") {
        /* 3,000 heights; bodies missing for [40, 60] — below the tip, which
         * is precisely the region gap_fill_compute_window can never reach
         * (its window is [tip+1, best_header] by construction). */
        struct bh_fake_chain chain;
        ASSERT(bh_fake_chain_init(&chain, 3000));
        for (int64_t h = 40; h <= 60; h++)
            chain.have_data[h] = 0;

        struct body_coverage_map held, measured;
        body_coverage_init(&held);
        body_coverage_init(&measured);
        struct body_history_census census;
        body_history_census_init(&census);

        struct uint256 collected_h[64];
        int32_t collected_n[64];
        size_t collected = 0;
        int64_t missing = bh_run_full_census(&census, &held, &measured,
                                             &chain, 2999, 256,
                                             collected_h, collected_n, 64,
                                             &collected);
        ASSERT(missing == 21);
        ASSERT(census.heights_examined == 3000);
        ASSERT(census.heights_indeterminate == 0);
        ASSERT(census.sweeps_completed == 1);

        struct body_history_verdict v;
        ASSERT(body_history_evaluate(&held, &measured, 0, 2999, &v));
        ASSERT(v.status == BODY_HISTORY_INCOMPLETE);
        ASSERT(!body_history_verdict_is_proven(&v));
        ASSERT(v.missing_count == 21);
        ASSERT(v.lowest_missing == 40);
        ASSERT(v.unmeasured_count == 0);
        ASSERT(v.held_count == 2979);

        /* Every missing height was collected for the download manager. */
        ASSERT(collected == 21);

        /* And it goes through the EXISTING download primitives, not a new
         * path: dl_queue_blocks accepts all 21 and hands them to a peer in
         * ascending height order. */
        struct download_manager dm;
        dl_init(&dm);
        size_t added = dl_queue_blocks(&dm, collected_h, collected_n,
                                       collected);
        ASSERT(added == 21);
        uint64_t in_flight = 0, queued = 0;
        dl_get_stats(&dm, NULL, NULL, NULL, &in_flight, &queued);
        ASSERT(queued == 21);

        struct uint256 out[32];
        size_t assigned = dl_assign_to_peer(&dm, 1, out, 21);
        ASSERT(assigned == 21);
        struct uint256 want40;
        bh_fake_hash(&want40, 40);
        ASSERT(uint256_eq(&out[0], &want40)); /* lowest missing goes first */
        dl_free(&dm);

        /* Bodies land: re-running the census now certifies the window. */
        for (int64_t h = 40; h <= 60; h++)
            chain.have_data[h] = 1;
        body_history_census_init(&census);
        (void)bh_run_full_census(&census, &held, &measured, &chain, 2999,
                                 256, NULL, NULL, 0, NULL);
        struct body_history_verdict after;
        ASSERT(body_history_evaluate(&held, &measured, 0, 2999, &after));
        ASSERT(after.status == BODY_HISTORY_COMPLETE);
        ASSERT(body_history_verdict_is_proven(&after));
        ASSERT(after.missing_count == 0);
        ASSERT(after.unmeasured_count == 0);

        body_coverage_free(&held);
        body_coverage_free(&measured);
        bh_fake_chain_free(&chain);
        PASS();
    } _test_next:;
    return failures;
}

/* The owner's node in miniature: genesis held, one contiguous hole covering
 * almost everything, and the top few thousand heights held. */
static int test_bh_one_contiguous_hole_from_height_one(void)
{
    int failures = 0;
    TEST("genesis plus a tail, hole from height 1 — the live-node shape") {
        struct bh_fake_chain chain;
        ASSERT(bh_fake_chain_init(&chain, 4000));
        for (int64_t h = 1; h <= 3800; h++)
            chain.have_data[h] = 0;

        struct body_coverage_map held, measured;
        body_coverage_init(&held);
        body_coverage_init(&measured);
        struct body_history_census census;
        body_history_census_init(&census);

        int64_t missing = bh_run_full_census(&census, &held, &measured,
                                             &chain, 3999, 512,
                                             NULL, NULL, 0, NULL);
        ASSERT(missing == 3800);

        struct body_history_verdict v;
        ASSERT(body_history_evaluate(&held, &measured, 0, 3999, &v));
        ASSERT(v.status == BODY_HISTORY_INCOMPLETE);
        ASSERT(v.missing_count == 3800);
        ASSERT(v.lowest_missing == 1);
        ASSERT(v.held_count == 200);   /* genesis + [3801, 3999] */
        ASSERT(v.unmeasured_count == 0);

        body_coverage_free(&held);
        body_coverage_free(&measured);
        bh_fake_chain_free(&chain);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 3. The census is bounded and resumable ─────────────────────── */

static int test_bh_census_is_bounded_and_resumable(void)
{
    int failures = 0;
    TEST("census walks a bounded window per pass and resumes downward") {
        struct body_history_census c;
        body_history_census_init(&c);

        int64_t lo = 0, hi = 0;
        ASSERT(body_history_census_plan(&c, 0, 9999, 1000, &lo, &hi));
        ASSERT(hi == 9999 && lo == 9000);   /* walks DOWN from the tip */
        body_history_census_advance(&c, lo, hi);
        ASSERT(c.passes == 1);

        ASSERT(body_history_census_plan(&c, 0, 9999, 1000, &lo, &hi));
        ASSERT(hi == 8999 && lo == 8000);
        body_history_census_advance(&c, lo, hi);

        /* Walk it out; the last window clamps to window_lo and the sweep
         * wraps exactly once. */
        for (int i = 0; i < 8; i++) {
            ASSERT(body_history_census_plan(&c, 0, 9999, 1000, &lo, &hi));
            body_history_census_advance(&c, lo, hi);
        }
        ASSERT(lo == 0);
        ASSERT(c.sweeps_completed == 1);
        ASSERT(c.passes == 10);

        /* A pass never exceeds its budget, even on a huge window. */
        struct body_history_census big;
        body_history_census_init(&big);
        ASSERT(body_history_census_plan(&big, 0, 3196956,
                                        BODY_HISTORY_CENSUS_BUDGET,
                                        &lo, &hi));
        ASSERT(hi - lo + 1 == BODY_HISTORY_CENSUS_BUDGET);

        /* Unusable windows are refused rather than silently "done". */
        ASSERT(!body_history_census_plan(&big, 0, -1, 4096, &lo, &hi));
        ASSERT(!body_history_census_plan(&big, 5, 1, 4096, &lo, &hi));
        ASSERT(!body_history_census_plan(&big, 0, 100, 0, &lo, &hi));
        ASSERT(!body_history_census_plan(NULL, 0, 100, 10, &lo, &hi));
        PASS();
    } _test_next:;
    return failures;
}

/* ── 4. "At tip" is unsayable while history is unproven ─────────── */

static int test_bh_at_tip_requires_proven_history(void)
{
    int failures = 0;
    TEST("at-tip is refused on both incomplete AND unknown history") {
        struct sync_tip_state_evaluation eval;

        /* A textbook at-tip situation: every height agrees, peers present,
         * nothing queued, nothing in flight, nothing pending. The ONLY
         * variable below is the body-history verdict. */
        #define BH_PLAN(status_) \
            syncsvc_plan_periodic_tip_state(&eval, SYNC_BLOCKS_DOWNLOAD, \
                                            true, 1000, 1000, 1000, 1000, \
                                            3, 0, 0, 0, (status_))

        BH_PLAN(BODY_HISTORY_COMPLETE);
        ASSERT(eval.should_set_at_tip);      /* the only value that passes */

        BH_PLAN(BODY_HISTORY_INCOMPLETE);
        ASSERT(!eval.should_set_at_tip);     /* known hole */

        BH_PLAN(BODY_HISTORY_UNKNOWN);
        ASSERT(!eval.should_set_at_tip);     /* could not determine —
                                              * refused just as hard */

        /* The zero value is the fail-closed default: a caller that forgot to
         * set the argument gets a refusal, not a free pass. */
        enum body_history_status defaulted = 0;
        BH_PLAN(defaulted);
        ASSERT(!eval.should_set_at_tip);

        #undef BH_PLAN
        PASS();
    } _test_next:;
    return failures;
}

/* ── 5. Singleton, persistence, dumper ──────────────────────────── */

static int test_bh_singleton_defaults_to_unknown(void)
{
    int failures = 0;
    TEST("the global verdict starts unknown and is never restored as proven") {
        body_history_reset();

        struct body_history_verdict v;
        memset(&v, 0xEE, sizeof(v));
        ASSERT(!body_history_get_verdict(&v));   /* nothing published */
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);
        ASSERT(!body_history_is_proven());

        /* Publishing a complete verdict is the only way to become proven. */
        struct body_history_verdict good;
        memset(&good, 0, sizeof(good));
        good.status = BODY_HISTORY_COMPLETE;
        body_history_publish(&good);
        ASSERT(body_history_is_proven());

        /* Publishing NULL publishes ignorance — it does not leave the last
         * good news standing. */
        body_history_publish(NULL);
        ASSERT(!body_history_is_proven());
        ASSERT(body_history_get_verdict(&v));    /* published, but unknown */
        ASSERT(v.status == BODY_HISTORY_UNKNOWN);

        body_history_reset();
        ASSERT(!body_history_is_proven());
        PASS();
    } _test_next:;
    return failures;
}

static int test_bh_persistence_never_restores_a_verdict(void)
{
    int failures = 0;
    TEST("a restart resumes the cursor but must re-earn 'complete'") {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "body_history", "main");
        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(progress_meta_table_ensure(db));

        body_history_reset();
        body_history_global_lock();
        ASSERT(body_coverage_insert(body_history_global_measured(), 100, 999));
        body_history_global_census()->cursor = 555;
        body_history_global_census()->cursor_valid = true;
        body_history_global_unlock();

        struct body_history_verdict good;
        memset(&good, 0, sizeof(good));
        good.status = BODY_HISTORY_COMPLETE;
        body_history_publish(&good);
        ASSERT(body_history_is_proven());
        ASSERT(body_history_save(db));

        /* Simulate the restart. */
        body_history_reset();
        ASSERT(!body_history_is_proven());
        ASSERT(body_history_load(db));

        body_history_global_lock();
        int64_t measured = body_coverage_total_covered(
            body_history_global_measured());
        int64_t cursor = body_history_global_census()->cursor;
        body_history_global_unlock();
        ASSERT(measured == 900);   /* the measured map came back ... */
        ASSERT(cursor == 555);     /* ... and so did the cursor ... */
        ASSERT(!body_history_is_proven()); /* ... but NOT the verdict */

        body_history_reset();
        progress_store_close();
        PASS();
    } _test_next:;
    return failures;
}

static int test_bh_dump_state_json_separates_the_three(void)
{
    int failures = 0;
    TEST("dumpstate body_history publishes unknown as a status, not a gap") {
        body_history_reset();

        /* Unknown: the dump must carry status=unknown, proven=false, and a
         * blocker id — not an absent field an operator could read as fine. */
        struct json_value v = {0};
        json_set_object(&v);
        ASSERT(body_history_dump_state_json(&v, NULL));
        const struct json_value *status = json_get(&v, "status");
        const struct json_value *proven = json_get(&v, "proven");
        const struct json_value *missing = json_get(&v, "missing_count");
        const struct json_value *unmeasured = json_get(&v, "unmeasured_count");
        const struct json_value *blocker = json_get(&v, "blocker_id");
        ASSERT(status && strcmp(json_get_str(status), "unknown") == 0);
        ASSERT(proven && json_get_bool(proven) == false);
        ASSERT(missing != NULL);      /* present, and zero */
        ASSERT(unmeasured != NULL);   /* the field that tells them apart */
        ASSERT(blocker &&
               strcmp(json_get_str(blocker),
                      BODY_HISTORY_UNPROVEN_BLOCKER) == 0);
        json_free(&v);

        /* Incomplete: counts and the lowest missing height are carried. */
        struct body_history_verdict inc;
        memset(&inc, 0, sizeof(inc));
        inc.status = BODY_HISTORY_INCOMPLETE;
        inc.window_lo = 0;
        inc.window_hi = 3196956;
        inc.missing_count = 3155842;
        inc.lowest_missing = 1;
        inc.unmeasured_count = 0;
        inc.lowest_unmeasured = -1;
        body_history_publish(&inc);

        struct json_value v2 = {0};
        json_set_object(&v2);
        ASSERT(body_history_dump_state_json(&v2, NULL));
        const struct json_value *s2 = json_get(&v2, "status");
        const struct json_value *m2 = json_get(&v2, "missing_count");
        const struct json_value *l2 = json_get(&v2, "lowest_missing");
        ASSERT(s2 && strcmp(json_get_str(s2), "incomplete") == 0);
        ASSERT(m2 && json_get_int(m2) == 3155842);
        ASSERT(l2 && json_get_int(l2) == 1);
        json_free(&v2);

        /* Complete: proven, and no blocker id. */
        struct body_history_verdict comp;
        memset(&comp, 0, sizeof(comp));
        comp.status = BODY_HISTORY_COMPLETE;
        comp.lowest_missing = -1;
        comp.lowest_unmeasured = -1;
        body_history_publish(&comp);

        struct json_value v3 = {0};
        json_set_object(&v3);
        ASSERT(body_history_dump_state_json(&v3, NULL));
        const struct json_value *s3 = json_get(&v3, "status");
        const struct json_value *p3 = json_get(&v3, "proven");
        const struct json_value *b3 = json_get(&v3, "blocker_id");
        ASSERT(s3 && strcmp(json_get_str(s3), "complete") == 0);
        ASSERT(p3 && json_get_bool(p3) == true);
        ASSERT(b3 && json_get_str(b3)[0] == '\0');
        json_free(&v3);

        body_history_reset();
        PASS();
    } _test_next:;
    return failures;
}

int test_body_history(void)
{
    int failures = 0;
    failures += test_bh_unreadable_index_is_not_no_hole();
    failures += test_bh_null_probe_leaves_everything_unmeasured();
    failures += test_bh_partial_read_does_not_certify();
    failures += test_bh_evaluate_failure_paths_are_unknown();
    failures += test_bh_below_tip_hole_found_and_enqueued();
    failures += test_bh_one_contiguous_hole_from_height_one();
    failures += test_bh_census_is_bounded_and_resumable();
    failures += test_bh_at_tip_requires_proven_history();
    failures += test_bh_singleton_defaults_to_unknown();
    failures += test_bh_persistence_never_restores_a_verdict();
    failures += test_bh_dump_state_json_separates_the_three();
    return failures;
}
