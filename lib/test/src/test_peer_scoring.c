/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for lib/net/src/peer_scoring.c — the typed offence layer that
 * wraps peer_misbehaving(). Covers config, offence-name lookup, record
 * semantics, is_trusted_peer() guard, decay, reset, should_ban, and
 * env-var overrides.
 */

#include "test/test_helpers.h"
#include "net/peer_scoring.h"
#include "net/net.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ── Fixture helpers ─────────────────────────────────────── */

/* Construct a minimal p2p_node on the stack. peer_misbehaving() only
 * reads node->misbehavior, ->disconnect, ->id, ->addr, ->addr_name, and
 * ->whitelisted on the score path. The ban path also calls ban_addr()
 * which uses nm->banned under nm->cs_banned — we zero the manager and
 * accept that the ban-list path grows via realloc(NULL). */
static void setup_node(struct p2p_node *node, const char *name, bool whitelisted)
{
    memset(node, 0, sizeof(*node));
    snprintf(node->addr_name, sizeof(node->addr_name), "%s", name);
    node->whitelisted = whitelisted;
    /* Make the peer look non-localhost. The IPv4-mapped prefix check in
     * is_trusted_peer() matches bytes 10..12 = {0xff, 0xff, 127}; use
     * 1.2.3.4 to stay clear of it. */
    node->addr.svc.addr.ip[10] = 0xff;
    node->addr.svc.addr.ip[11] = 0xff;
    node->addr.svc.addr.ip[12] = 1;
    node->addr.svc.addr.ip[13] = 2;
    node->addr.svc.addr.ip[14] = 3;
    node->addr.svc.addr.ip[15] = 4;
}

static void setup_localhost(struct p2p_node *node)
{
    memset(node, 0, sizeof(*node));
    snprintf(node->addr_name, sizeof(node->addr_name), "localhost");
    /* IPv4-mapped 127.0.0.1 — matches is_trusted_peer()'s prefix. */
    node->addr.svc.addr.ip[10] = 0xff;
    node->addr.svc.addr.ip[11] = 0xff;
    node->addr.svc.addr.ip[12] = 127;
    node->addr.svc.addr.ip[13] = 0;
    node->addr.svc.addr.ip[14] = 0;
    node->addr.svc.addr.ip[15] = 1;
}

static void setup_manager(struct net_manager *nm)
{
    memset(nm, 0, sizeof(*nm));
    /* cs_banned is touched only if a ban fires. Zero-initialised pthread
     * mutexes behave like PTHREAD_MUTEX_INITIALIZER on glibc. */
}

/* ── Test cases (one TEST per function to avoid label collisions) ── */

static int test_defaults(void)
{
    int failures = 0;
    unsetenv("ZCL_PEER_BAN_THRESHOLD");
    unsetenv("ZCL_PEER_BAN_HOURS");
    unsetenv("ZCL_PEER_SCORE_DECAY_PER_MIN");
    peer_scoring_init();
    TEST("peer_scoring: defaults match historical 100/24h/1") {
        ASSERT_EQ(peer_scoring_ban_threshold(), 100);
        ASSERT_EQ(peer_scoring_ban_hours(), 24);
        ASSERT_EQ(peer_scoring_decay_rate(), 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_env_overrides(void)
{
    int failures = 0;
    TEST("peer_scoring: env vars override defaults") {
        setenv("ZCL_PEER_BAN_THRESHOLD", "250", 1);
        setenv("ZCL_PEER_BAN_HOURS", "48", 1);
        setenv("ZCL_PEER_SCORE_DECAY_PER_MIN", "5", 1);
        peer_scoring_init();
        ASSERT_EQ(peer_scoring_ban_threshold(), 250);
        ASSERT_EQ(peer_scoring_ban_hours(), 48);
        ASSERT_EQ(peer_scoring_decay_rate(), 5);

        unsetenv("ZCL_PEER_BAN_THRESHOLD");
        unsetenv("ZCL_PEER_BAN_HOURS");
        unsetenv("ZCL_PEER_SCORE_DECAY_PER_MIN");
        peer_scoring_init();
        PASS();
    } _test_next:;
    return failures;
}

static int test_invalid_env(void)
{
    int failures = 0;
    TEST("peer_scoring: invalid env falls back to defaults") {
        setenv("ZCL_PEER_BAN_THRESHOLD", "0", 1);         /* below min */
        setenv("ZCL_PEER_BAN_HOURS", "garbage", 1);
        setenv("ZCL_PEER_SCORE_DECAY_PER_MIN", "-3", 1);  /* below min */
        peer_scoring_init();
        ASSERT_EQ(peer_scoring_ban_threshold(), 100);
        ASSERT_EQ(peer_scoring_ban_hours(), 24);
        ASSERT_EQ(peer_scoring_decay_rate(), 1);

        unsetenv("ZCL_PEER_BAN_THRESHOLD");
        unsetenv("ZCL_PEER_BAN_HOURS");
        unsetenv("ZCL_PEER_SCORE_DECAY_PER_MIN");
        peer_scoring_init();
        PASS();
    } _test_next:;
    return failures;
}

static int test_offence_names(void)
{
    int failures = 0;
    TEST("peer_scoring: offence names are human-readable") {
        ASSERT_STR_EQ(peer_offence_name(PEER_OFFENCE_NONE), "none");
        ASSERT_STR_EQ(peer_offence_name(PEER_OFFENCE_TIMEOUT), "timeout");
        ASSERT_STR_EQ(peer_offence_name(PEER_OFFENCE_INVALID_MESSAGE),
                      "invalid_message");
        ASSERT_STR_EQ(peer_offence_name(PEER_OFFENCE_FLOOD), "flood");
        ASSERT_STR_EQ(peer_offence_name(PEER_OFFENCE_INVALID_HEADER),
                      "invalid_header");
        ASSERT_STR_EQ(peer_offence_name(PEER_OFFENCE_INVALID_BLOCK),
                      "invalid_block");
        PASS();
    } _test_next:;
    return failures;
}

static int test_record_increments(void)
{
    int failures = 0;
    TEST("peer_scoring: record increments by offence weight") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_5", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_MESSAGE, "ctx");
        ASSERT_EQ(atomic_load(&node.misbehavior), 10);
        peer_scoring_record(&nm, &node, PEER_OFFENCE_TIMEOUT, "ctx");
        ASSERT_EQ(atomic_load(&node.misbehavior), 15);
        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_HEADER, "ctx");
        ASSERT_EQ(atomic_load(&node.misbehavior), 65);
        PASS();
    } _test_next:;
    return failures;
}

static int test_none_noop(void)
{
    int failures = 0;
    TEST("peer_scoring: NONE offence is a no-op") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_none", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_NONE, "no-op");
        ASSERT_EQ(atomic_load(&node.misbehavior), 0);
        ASSERT(!node.disconnect);
        PASS();
    } _test_next:;
    return failures;
}

static int test_autoban_single_hit(void)
{
    int failures = 0;
    TEST("peer_scoring: INVALID_BLOCK auto-bans at 100") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_autoban", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_BLOCK, "bad");
        ASSERT_EQ(atomic_load(&node.misbehavior), 100);
        ASSERT(node.disconnect);
        ASSERT(peer_scoring_should_ban(&node));
        free(nm.banned);
        PASS();
    } _test_next:;
    return failures;
}

static int test_accumulated_hits(void)
{
    int failures = 0;
    TEST("peer_scoring: small hits accumulate to ban") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_accum", false);

        /* 5 × FLOOD(20) = 100 → banned */
        for (int i = 0; i < 5; i++)
            peer_scoring_record(&nm, &node, PEER_OFFENCE_FLOOD, "spam");
        ASSERT_EQ(atomic_load(&node.misbehavior), 100);
        ASSERT(node.disconnect);
        free(nm.banned);
        PASS();
    } _test_next:;
    return failures;
}

static int test_localhost_exempt(void)
{
    int failures = 0;
    TEST("peer_scoring: localhost peers are exempt") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_localhost(&node);

        for (int i = 0; i < 10; i++)
            peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_BLOCK, "bad");
        /* is_trusted_peer() short-circuits before increment. */
        ASSERT_EQ(atomic_load(&node.misbehavior), 0);
        ASSERT(!node.disconnect);
        ASSERT(!peer_scoring_should_ban(&node));
        PASS();
    } _test_next:;
    return failures;
}

static int test_whitelist_exempt(void)
{
    int failures = 0;
    TEST("peer_scoring: whitelisted peers are exempt") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_wl", true);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_BLOCK, "bad");
        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_BLOCK, "bad");
        ASSERT_EQ(atomic_load(&node.misbehavior), 0);
        ASSERT(!node.disconnect);
        PASS();
    } _test_next:;
    return failures;
}

static int test_linear_decay(void)
{
    int failures = 0;
    TEST("peer_scoring: linear decay subtracts rate × minutes") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_decay", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_HEADER, "ctx");
        ASSERT_EQ(atomic_load(&node.misbehavior), 50);

        /* First decay call primes the anchor without awarding decay. */
        int64_t t0 = 1000000000LL * 1000;
        peer_scoring_decay(&node, t0);
        ASSERT_EQ(atomic_load(&node.misbehavior), 50);

        /* 10 minutes later at default rate 1: should drop by 10. */
        int post = peer_scoring_decay(&node, t0 + 10 * 60 * 1000);
        ASSERT_EQ(post, 40);
        ASSERT_EQ(atomic_load(&node.misbehavior), 40);
        PASS();
    } _test_next:;
    return failures;
}

static int test_decay_floors_zero(void)
{
    int failures = 0;
    TEST("peer_scoring: decay floors at zero") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_floor", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_MESSAGE, "x");
        ASSERT_EQ(atomic_load(&node.misbehavior), 10);

        int64_t t0 = 2000000000LL * 1000;
        peer_scoring_decay(&node, t0);
        int post = peer_scoring_decay(&node, t0 + 60 * 60 * 1000);
        ASSERT_EQ(post, 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_sub_minute_decay(void)
{
    int failures = 0;
    TEST("peer_scoring: sub-minute decay preserves anchor") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_sub", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_HEADER, "x");
        int64_t t0 = 1700000000LL * 1000;
        peer_scoring_decay(&node, t0);

        /* 30s later → still below one minute; no decay and anchor stays. */
        peer_scoring_decay(&node, t0 + 30 * 1000);
        ASSERT_EQ(atomic_load(&node.misbehavior), 50);

        /* Another 30s → 60s from the original anchor → one point drop. */
        int post = peer_scoring_decay(&node, t0 + 60 * 1000);
        ASSERT_EQ(post, 49);
        PASS();
    } _test_next:;
    return failures;
}

static int test_reset(void)
{
    int failures = 0;
    TEST("peer_scoring: reset zeroes the score") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_reset", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_FLOOD, "x");
        peer_scoring_record(&nm, &node, PEER_OFFENCE_FLOOD, "x");
        ASSERT_EQ(atomic_load(&node.misbehavior), 40);

        peer_scoring_reset(&node);
        ASSERT_EQ(atomic_load(&node.misbehavior), 0);
        ASSERT(!peer_scoring_should_ban(&node));
        PASS();
    } _test_next:;
    return failures;
}

static int test_should_ban_pure(void)
{
    int failures = 0;
    TEST("peer_scoring: should_ban does not mutate") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_pure", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_FLOOD, "x");
        int before = atomic_load(&node.misbehavior);
        bool banned = peer_scoring_should_ban(&node);
        int after = atomic_load(&node.misbehavior);
        ASSERT_EQ(before, after);
        ASSERT(!banned); /* 20 < 100 */
        PASS();
    } _test_next:;
    return failures;
}

static int test_custom_threshold(void)
{
    int failures = 0;
    TEST("peer_scoring: custom threshold bans earlier") {
        setenv("ZCL_PEER_BAN_THRESHOLD", "30", 1);
        peer_scoring_init();

        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_custom", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_FLOOD, "x");  /* 20 */
        ASSERT(!node.disconnect);
        peer_scoring_record(&nm, &node, PEER_OFFENCE_FLOOD, "x");  /* 40, past 30 */
        ASSERT(node.disconnect);
        ASSERT(peer_scoring_should_ban(&node));
        free(nm.banned);

        unsetenv("ZCL_PEER_BAN_THRESHOLD");
        peer_scoring_init();
        PASS();
    } _test_next:;
    return failures;
}

static int test_good_interaction(void)
{
    int failures = 0;
    TEST("peer_scoring: well-behaved peer stays at zero") {
        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_good", false);

        int64_t t0 = 1800000000LL * 1000;
        for (int i = 0; i < 50; i++)
            peer_scoring_on_good_interaction(&node, t0 + i * 60 * 1000);
        ASSERT_EQ(atomic_load(&node.misbehavior), 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_decay_disabled(void)
{
    int failures = 0;
    TEST("peer_scoring: decay=0 leaves score alone") {
        setenv("ZCL_PEER_SCORE_DECAY_PER_MIN", "0", 1);
        peer_scoring_init();

        struct net_manager nm;
        struct p2p_node node;
        setup_manager(&nm);
        setup_node(&node, "test_peer_disabled", false);

        peer_scoring_record(&nm, &node, PEER_OFFENCE_INVALID_HEADER, "x");
        int64_t t0 = 1900000000LL * 1000;
        peer_scoring_decay(&node, t0);
        peer_scoring_decay(&node, t0 + 10 * 60 * 1000);
        ASSERT_EQ(atomic_load(&node.misbehavior), 50);

        unsetenv("ZCL_PEER_SCORE_DECAY_PER_MIN");
        peer_scoring_init();
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ───────────────────────────────────────── */

int test_peer_scoring(void);

int test_peer_scoring(void)
{
    int failures = 0;

    printf("\n=== peer_scoring ===\n");

    /* Snapshot env so test_peer_scoring doesn't leak config to suites
     * that run after it. */
    const char *orig_threshold = getenv("ZCL_PEER_BAN_THRESHOLD");
    const char *orig_hours = getenv("ZCL_PEER_BAN_HOURS");
    const char *orig_decay = getenv("ZCL_PEER_SCORE_DECAY_PER_MIN");
    char *saved_threshold = orig_threshold ? strdup(orig_threshold) : NULL;
    char *saved_hours = orig_hours ? strdup(orig_hours) : NULL;
    char *saved_decay = orig_decay ? strdup(orig_decay) : NULL;

    failures += test_defaults();
    failures += test_env_overrides();
    failures += test_invalid_env();
    failures += test_offence_names();
    failures += test_record_increments();
    failures += test_none_noop();
    failures += test_autoban_single_hit();
    failures += test_accumulated_hits();
    failures += test_localhost_exempt();
    failures += test_whitelist_exempt();
    failures += test_linear_decay();
    failures += test_decay_floors_zero();
    failures += test_sub_minute_decay();
    failures += test_reset();
    failures += test_should_ban_pure();
    failures += test_custom_threshold();
    failures += test_good_interaction();
    failures += test_decay_disabled();

    /* Restore env so other suites see the same state they started with. */
    if (saved_threshold) {
        setenv("ZCL_PEER_BAN_THRESHOLD", saved_threshold, 1);
        free(saved_threshold);
    } else {
        unsetenv("ZCL_PEER_BAN_THRESHOLD");
    }
    if (saved_hours) {
        setenv("ZCL_PEER_BAN_HOURS", saved_hours, 1);
        free(saved_hours);
    } else {
        unsetenv("ZCL_PEER_BAN_HOURS");
    }
    if (saved_decay) {
        setenv("ZCL_PEER_SCORE_DECAY_PER_MIN", saved_decay, 1);
        free(saved_decay);
    } else {
        unsetenv("ZCL_PEER_SCORE_DECAY_PER_MIN");
    }
    peer_scoring_init();

    return failures;
}
