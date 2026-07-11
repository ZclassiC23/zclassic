/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Focused unit tests for the pure, deterministic predicate layer of
 * chain_frontier_snapshot_service.c: chain_frontier_snapshot_values_known,
 * chain_frontier_snapshot_bindings_known, chain_frontier_snapshot_consistent,
 * chain_frontier_snapshot_equal, and chain_frontier_authority_source_name.
 *
 * These five exports take/compare a caller-owned `struct
 * chain_frontier_snapshot` value type with no I/O, no clock, and no global
 * state (the stateful collector chain_frontier_snapshot_collect() is the
 * sibling exercised elsewhere via a live main_state; it is out of scope
 * here). Every test below hand-builds the struct fixture, so the suite is
 * fully deterministic and needs no datadir/DB/network. */

#include "test/test_helpers.h"

#include "services/chain_frontier_snapshot_service.h"

#include <string.h>

/* A fully "known + consistent" baseline snapshot: every gate that
 * chain_frontier_snapshot_consistent() checks is satisfied. Individual
 * tests flip exactly one field off the baseline to prove that gate is
 * load-bearing (an AND, not silently ignored). */
static void baseline_snapshot(struct chain_frontier_snapshot *s)
{
    memset(s, 0, sizeof(*s));
    s->context_known = true;
    s->hstar_published = true;
    s->authority_pair_known = true;
    s->durable_authority_known = true;
    s->authority_matches_served = true;
    s->authority_source = CHAIN_FRONTIER_AUTHORITY_RUNTIME_PUBLICATION;
    s->ancestry_known = true;
    s->served_ancestor_indexed = true;
    s->indexed_ancestor_header = true;
    s->work_known = true;
    s->work_monotone = true;
    s->validity_known = true;
    s->validity_sufficient = true;
    s->failure_free = true;

    struct chain_frontier_value *vals[3] = {&s->served, &s->indexed,
                                            &s->header};
    for (int i = 0; i < 3; i++) {
        vals[i]->height_known = true;
        vals[i]->binding_known = true;
        vals[i]->status_known = true;
        vals[i]->validity_sufficient = true;
        vals[i]->failure_free = true;
        vals[i]->height = 100 + i;
        vals[i]->status = 0;
        snprintf(vals[i]->hash, sizeof(vals[i]->hash), "hash%d", i);
        snprintf(vals[i]->chain_work, sizeof(vals[i]->chain_work),
                 "work%d", i);
    }
}

/* ── chain_frontier_snapshot_values_known ─────────────────────────── */

static int t_values_known_baseline_true(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_values_known: baseline true") {
        baseline_snapshot(&s);
        ASSERT(chain_frontier_snapshot_values_known(&s));
    } TEST_END
    return failures;
}

static int t_values_known_null_false(void)
{
    int failures = 0;
    TEST_CASE("chain_frontier_snapshot_values_known: NULL snapshot false") {
        ASSERT(!chain_frontier_snapshot_values_known(NULL));
    } TEST_END
    return failures;
}

static int t_values_known_context_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_values_known: !context_known false") {
        baseline_snapshot(&s);
        s.context_known = false;
        ASSERT(!chain_frontier_snapshot_values_known(&s));
    } TEST_END
    return failures;
}

static int t_values_known_served_height_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_values_known: served height unknown "
              "false") {
        baseline_snapshot(&s);
        s.served.height_known = false;
        ASSERT(!chain_frontier_snapshot_values_known(&s));
    } TEST_END
    return failures;
}

/* ── chain_frontier_snapshot_bindings_known ───────────────────────── */

static int t_bindings_known_baseline_true(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_bindings_known: baseline true") {
        baseline_snapshot(&s);
        ASSERT(chain_frontier_snapshot_bindings_known(&s));
    } TEST_END
    return failures;
}

static int t_bindings_known_inherits_values_known(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_bindings_known: inherits "
              "values_known gate") {
        baseline_snapshot(&s);
        s.indexed.height_known = false;
        ASSERT(!chain_frontier_snapshot_bindings_known(&s));
    } TEST_END
    return failures;
}

static int t_bindings_known_per_value_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_bindings_known: binding_known gate "
              "per value") {
        baseline_snapshot(&s);
        s.header.binding_known = false;
        ASSERT(!chain_frontier_snapshot_bindings_known(&s));
    } TEST_END
    return failures;
}

/* ── chain_frontier_snapshot_consistent ───────────────────────────── */

static int t_consistent_baseline_true(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_consistent: baseline true") {
        baseline_snapshot(&s);
        ASSERT(chain_frontier_snapshot_consistent(&s));
    } TEST_END
    return failures;
}

static int t_consistent_inherits_bindings_known(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_consistent: inherits "
              "bindings_known gate") {
        baseline_snapshot(&s);
        s.served.binding_known = false;
        ASSERT(!chain_frontier_snapshot_consistent(&s));
    } TEST_END
    return failures;
}

static int t_consistent_authority_mismatch_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_consistent: authority mismatch "
              "false") {
        baseline_snapshot(&s);
        s.authority_matches_served = false;
        ASSERT(!chain_frontier_snapshot_consistent(&s));
    } TEST_END
    return failures;
}

static int t_consistent_ancestry_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_consistent: ancestry unknown false") {
        baseline_snapshot(&s);
        s.ancestry_known = false;
        ASSERT(!chain_frontier_snapshot_consistent(&s));
    } TEST_END
    return failures;
}

static int t_consistent_work_monotone_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_consistent: work not monotone "
              "false") {
        baseline_snapshot(&s);
        s.work_monotone = false;
        ASSERT(!chain_frontier_snapshot_consistent(&s));
    } TEST_END
    return failures;
}

static int t_consistent_validity_sufficient_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_consistent: validity insufficient "
              "false") {
        baseline_snapshot(&s);
        s.validity_sufficient = false;
        ASSERT(!chain_frontier_snapshot_consistent(&s));
    } TEST_END
    return failures;
}

static int t_consistent_failure_free_gate(void)
{
    int failures = 0;
    struct chain_frontier_snapshot s;
    TEST_CASE("chain_frontier_snapshot_consistent: not failure_free "
              "false") {
        baseline_snapshot(&s);
        s.failure_free = false;
        ASSERT(!chain_frontier_snapshot_consistent(&s));
    } TEST_END
    return failures;
}

/* ── chain_frontier_snapshot_equal ────────────────────────────────── */

static int t_equal_identical_true(void)
{
    int failures = 0;
    struct chain_frontier_snapshot a, b;
    TEST_CASE("chain_frontier_snapshot_equal: identical snapshots equal") {
        baseline_snapshot(&a);
        baseline_snapshot(&b);
        ASSERT(chain_frontier_snapshot_equal(&a, &b));
    } TEST_END
    return failures;
}

static int t_equal_top_level_scalar_diff(void)
{
    int failures = 0;
    struct chain_frontier_snapshot a, b;
    TEST_CASE("chain_frontier_snapshot_equal: differing top-level scalar "
              "not equal") {
        baseline_snapshot(&a);
        baseline_snapshot(&b);
        b.authority_source = CHAIN_FRONTIER_AUTHORITY_DURABLE_TIP_FINALIZE_LOG;
        ASSERT(!chain_frontier_snapshot_equal(&a, &b));
    } TEST_END
    return failures;
}

static int t_equal_nested_hash_diff(void)
{
    int failures = 0;
    struct chain_frontier_snapshot a, b;
    TEST_CASE("chain_frontier_snapshot_equal: differing nested value "
              "(hash) not equal") {
        baseline_snapshot(&a);
        baseline_snapshot(&b);
        snprintf(b.served.hash, sizeof(b.served.hash), "different-hash");
        ASSERT(!chain_frontier_snapshot_equal(&a, &b));
    } TEST_END
    return failures;
}

static int t_equal_nested_height_diff(void)
{
    int failures = 0;
    struct chain_frontier_snapshot a, b;
    TEST_CASE("chain_frontier_snapshot_equal: differing nested value "
              "(height) not equal") {
        baseline_snapshot(&a);
        baseline_snapshot(&b);
        b.header.height = a.header.height + 1;
        ASSERT(!chain_frontier_snapshot_equal(&a, &b));
    } TEST_END
    return failures;
}

static int t_equal_null_args(void)
{
    int failures = 0;
    struct chain_frontier_snapshot a;
    TEST_CASE("chain_frontier_snapshot_equal: NULL args not equal") {
        baseline_snapshot(&a);
        ASSERT(!chain_frontier_snapshot_equal(NULL, &a));
        ASSERT(!chain_frontier_snapshot_equal(&a, NULL));
        ASSERT(!chain_frontier_snapshot_equal(NULL, NULL));
    } TEST_END
    return failures;
}

/* ── chain_frontier_authority_source_name ─────────────────────────── */

static int t_authority_source_name_known_values(void)
{
    int failures = 0;
    TEST_CASE("chain_frontier_authority_source_name: known values map to "
              "stable strings") {
        ASSERT_STR_EQ(
            chain_frontier_authority_source_name(
                CHAIN_FRONTIER_AUTHORITY_RUNTIME_PUBLICATION),
            "tip_finalize_runtime_publication");
        ASSERT_STR_EQ(
            chain_frontier_authority_source_name(
                CHAIN_FRONTIER_AUTHORITY_DURABLE_TIP_FINALIZE_LOG),
            "durable_tip_finalize_log");
        ASSERT_STR_EQ(
            chain_frontier_authority_source_name(
                CHAIN_FRONTIER_AUTHORITY_NONE),
            "unavailable");
    } TEST_END
    return failures;
}

static int t_authority_source_name_unknown_value(void)
{
    int failures = 0;
    TEST_CASE("chain_frontier_authority_source_name: unknown value falls "
              "back to unavailable") {
        ASSERT_STR_EQ(
            chain_frontier_authority_source_name(
                (enum chain_frontier_authority_source)999),
            "unavailable");
    } TEST_END
    return failures;
}

int test_chain_frontier_snapshot_service(void)
{
    int failures = 0;

    failures += t_values_known_baseline_true();
    failures += t_values_known_null_false();
    failures += t_values_known_context_gate();
    failures += t_values_known_served_height_gate();

    failures += t_bindings_known_baseline_true();
    failures += t_bindings_known_inherits_values_known();
    failures += t_bindings_known_per_value_gate();

    failures += t_consistent_baseline_true();
    failures += t_consistent_inherits_bindings_known();
    failures += t_consistent_authority_mismatch_gate();
    failures += t_consistent_ancestry_gate();
    failures += t_consistent_work_monotone_gate();
    failures += t_consistent_validity_sufficient_gate();
    failures += t_consistent_failure_free_gate();

    failures += t_equal_identical_true();
    failures += t_equal_top_level_scalar_diff();
    failures += t_equal_nested_hash_diff();
    failures += t_equal_nested_height_diff();
    failures += t_equal_null_args();

    failures += t_authority_source_name_known_values();
    failures += t_authority_source_name_unknown_value();

    return failures;
}
