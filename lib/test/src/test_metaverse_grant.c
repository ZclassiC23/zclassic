/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Property grant engine tests — the rules (lib/metaverse) and the
 * PLAN → COMMIT → RECEIPT state machine (services/property_grant_service.h).
 *
 * ── The seam these tests drive, and why ───────────────────────────────────
 * Nothing is mocked except the two things the service declares as seams: the
 * property catalog (a separate lane owns the real one) and the clock. The
 * grant rules, the store, the receipt codec, the SHA3 hash chain, and the real
 * Ed25519 signer are all the shipped code. In particular the tamper cases
 * corrupt the REAL store through the service's declared test-only hook rather
 * than hand-building a fake chain — a tamper proof against a fake store proves
 * nothing about the real one.
 *
 * Proves:
 *  1. Codecs round-trip: property ids and action sets, including the rejects.
 *  2. PLAN FAILS FAST: an action outside the grant's set, a property outside
 *     scope, a wrong kind, a disallowed counterparty, a budget-exceeding value,
 *     value on a free action, the wrong holder, and both expiry forms are all
 *     refused at PLAN with their own named reason — not deferred to COMMIT.
 *  3. COMMIT RE-CHECKS: a property revised between plan and commit is rejected
 *     STALE_REVISION; a property whose controller changed is OWNER_MISMATCH; a
 *     grant revoked between plan and commit is GRANT_REVOKED; an absent catalog
 *     is CATALOG_UNAVAILABLE. The plan is never trusted.
 *  4. REVOCATION is total across a delegation subtree: revoking a parent makes
 *     every child fail ANCESTOR_REVOKED with no write to the child.
 *  5. IDEMPOTENCY: the same key committed twice returns the SAME receipt, does
 *     not debit twice, and does not append a second chain link.
 *  6. The receipt chain is TAMPER-EVIDENT: editing a stored receipt, re-signing
 *     it with a foreign key, and cutting a link are each detected and located.
 *  7. Delegation attenuates: depth, actions, budget, and scope may only narrow.
 *  8. The cumulative budget and the rate window both bind.
 *
 * House idiom note: exactly one TEST block per function. ASSERT jumps to
 * `_test_next`, and a function holding two TEST blocks would jump BACKWARD
 * into an already-run block. */

#include "test/test_core.h"

#include "crypto/ed25519.h"
#include "metaverse/property_grant.h"
#include "metaverse/property_id.h"
#include "metaverse/property_receipt.h"
#include "services/property_grant_service.h"

#include <stdio.h>
#include <string.h>

static const char *const k_holder = "t1MetaverseGrantHolderAccount0000000";
static const char *const k_other = "t1MetaverseSomeoneElseAccount0000000";
static const char *const k_buyer = "t1MetaverseBuyerCounterparty00000000";
static const char *const k_stranger = "t1MetaverseStrangerCounterparty00000";

/* ── Fakes for the two declared seams ───────────────────────────────────── */

static int64_t g_now = 1700000000;
static int64_t g_height = 900000;

static void fake_clock(int64_t *now_unix, int64_t *height, void *ctx)
{
    (void)ctx;
    if (now_unix) *now_unix = g_now;
    if (height) *height = g_height;
}

struct fake_catalog {
    struct metaverse_property_id id;
    char controller[METAVERSE_PRINCIPAL_MAX + 1];
    int64_t revision;
    bool present;
};

static struct fake_catalog g_cat;

static bool fake_lookup(const struct metaverse_property_id *id,
                        struct metaverse_catalog_view *out, void *ctx)
{
    (void)ctx;
    if (!g_cat.present) return false;
    if (!metaverse_property_id_equal(&g_cat.id, id)) return false;
    out->id = *id;
    snprintf(out->controller, sizeof(out->controller), "%s", g_cat.controller);
    out->revision = g_cat.revision;
    out->freshness_height = g_height;
    return true;
}

/* ── Fixture helpers ────────────────────────────────────────────────────── */

static struct metaverse_property_id make_id(enum metaverse_property_kind kind,
                                            uint8_t tag)
{
    struct metaverse_property_id id;
    memset(&id, 0, sizeof(id));
    id.kind = kind;
    for (size_t i = 0; i < METAVERSE_PROPERTY_ROOT_LEN; i++)
        id.root[i] = (uint8_t)(tag + i);
    return id;
}

/* Install the fakes and clear every store. Every case starts from here so no
 * case can pass because of a previous case's leftovers. */
static void fixture_reset(const struct metaverse_property_id *prop)
{
    property_grant_service_reset();
    g_now = 1700000000;
    g_height = 900000;

    memset(&g_cat, 0, sizeof(g_cat));
    if (prop) {
        g_cat.id = *prop;
        g_cat.present = true;
        g_cat.revision = 7;
        snprintf(g_cat.controller, sizeof(g_cat.controller), "%s", k_holder);
    }

    struct property_grant_env env;
    memset(&env, 0, sizeof(env));
    env.catalog_lookup = fake_lookup;
    env.clock = fake_clock;
    property_grant_service_configure(&env);

    uint8_t seed[32];
    memset(seed, 0xA5, sizeof(seed));
    property_grant_service_set_signing_seed(seed);
}

/* A root grant over exactly one property id. */
static struct metaverse_grant grant_over_id(
    const struct metaverse_property_id *id, metaverse_action_set actions,
    int64_t budget_zat)
{
    struct metaverse_grant g;
    memset(&g, 0, sizeof(g));
    snprintf(g.holder, sizeof(g.holder), "%s", k_holder);
    snprintf(g.issuer, sizeof(g.issuer), "%s", k_holder);
    g.scope_form = METAVERSE_SCOPE_IDS;
    g.ids[0] = *id;
    g.id_count = 1;
    g.actions = actions;
    g.max_value_zat = budget_zat;
    return g;
}

static struct metaverse_action_request request_for(
    const struct metaverse_property_id *id, enum metaverse_action a,
    const char *counterparty, int64_t value_zat)
{
    struct metaverse_action_request r;
    memset(&r, 0, sizeof(r));
    snprintf(r.actor, sizeof(r.actor), "%s", k_holder);
    r.property = *id;
    r.action = a;
    if (counterparty)
        snprintf(r.counterparty, sizeof(r.counterparty), "%s", counterparty);
    r.value_zat = value_zat;
    return r;
}

/* Commit `n` 1000-zatoshi BUYs, each under its own idempotency key. Returns 0
 * on success, -1 on the first refusal. */
static int commit_n(const struct metaverse_grant *g,
                    const struct metaverse_property_id *prop, int n)
{
    for (int i = 0; i < n; i++) {
        struct metaverse_action_request r =
            request_for(prop, METAVERSE_ACTION_BUY, NULL, 1000);
        struct property_grant_plan plan;
        if (property_grant_service_plan(g->grant_id, &r, &plan) !=
            PROPERTY_GRANT_OK)
            return -1;
        char key[32];
        snprintf(key, sizeof(key), "seq-%d", i);
        struct property_grant_commit_result res;
        if (property_grant_service_commit(plan.plan_id, key, &res) !=
            PROPERTY_GRANT_OK)
            return -1;
    }
    return 0;
}

#define ACT(a) metaverse_action_bit(METAVERSE_ACTION_##a)

/* ── 1. Codecs ──────────────────────────────────────────────────────────── */

static int t_id_roundtrip(void)
{
    int failures = 0;
    TEST("property id renders and re-parses to the same value") {
        struct metaverse_property_id id =
            make_id(METAVERSE_KIND_ZCODE_PACKAGE, 3);
        char text[METAVERSE_PROPERTY_ID_TEXT_MAX + 1];
        ASSERT(metaverse_property_id_render(&id, text, sizeof(text)));
        ASSERT(strncmp(text, "zcode-package:", 14) == 0);
        struct metaverse_property_id back;
        ASSERT(metaverse_property_id_parse(text, &back));
        ASSERT(metaverse_property_id_equal(&id, &back));
        PASS();
    } _test_next:;
    return failures;
}

static int t_id_rejects(void)
{
    int failures = 0;
    TEST("malformed property ids are rejected, not guessed") {
        struct metaverse_property_id out;
        ASSERT(!metaverse_property_id_parse("content", &out));
        ASSERT(!metaverse_property_id_parse("no-such-kind:00", &out));
        ASSERT(!metaverse_property_id_parse("content:beef", &out));
        ASSERT_EQ((int)metaverse_kind_parse("unknown"),
                  (int)METAVERSE_KIND_UNKNOWN);
        ASSERT_EQ((int)metaverse_kind_parse(NULL),
                  (int)METAVERSE_KIND_UNKNOWN);
        ASSERT_STR_EQ(metaverse_kind_token(METAVERSE_KIND_CONTRACT_SWAP),
                      "contract-swap");
        PASS();
    } _test_next:;
    return failures;
}

static int t_action_set_codec(void)
{
    int failures = 0;
    TEST("action sets round-trip and reject a typo whole") {
        metaverse_action_set set = 0;
        ASSERT(metaverse_action_set_parse("inspect,host,publish_revision",
                                          &set));
        ASSERT(set == (ACT(INSPECT) | ACT(HOST) | ACT(PUBLISH_REVISION)));
        char rendered[256];
        ASSERT(metaverse_action_set_render(set, rendered, sizeof(rendered)));
        ASSERT_STR_EQ(rendered, "inspect,host,publish-revision");

        /* One bad element fails the whole parse — a silently dropped element
         * narrows a grant the operator believed they had written. */
        metaverse_action_set bad = 0xFFFF;
        ASSERT(!metaverse_action_set_parse("inspect,hosst", &bad));

        metaverse_action_set empty = 0xFFFF;
        ASSERT(metaverse_action_set_parse("", &empty));
        ASSERT_EQ((int)empty, 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_value_actions(void)
{
    int failures = 0;
    TEST("value-moving actions are exactly the five that move value") {
        ASSERT(metaverse_action_moves_value(METAVERSE_ACTION_BUY));
        ASSERT(metaverse_action_moves_value(METAVERSE_ACTION_SELL));
        ASSERT(metaverse_action_moves_value(METAVERSE_ACTION_LEASE));
        ASSERT(metaverse_action_moves_value(METAVERSE_ACTION_TRANSFER));
        ASSERT(metaverse_action_moves_value(METAVERSE_ACTION_ACCEPT_PAYMENT));
        ASSERT(!metaverse_action_moves_value(METAVERSE_ACTION_INSPECT));
        ASSERT(!metaverse_action_moves_value(METAVERSE_ACTION_HOST));
        ASSERT(!metaverse_action_moves_value(METAVERSE_ACTION_DELEGATE));
        PASS();
    } _test_next:;
    return failures;
}

/* ── 2. PLAN fails fast ─────────────────────────────────────────────────── */

static int t_plan_action_not_granted(void)
{
    int failures = 0;
    TEST("an action outside the grant's set is refused at PLAN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 11);
        fixture_reset(&prop);
        struct metaverse_grant g =
            grant_over_id(&prop, ACT(INSPECT) | ACT(HOST), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);

        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_TRANSFER, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_ACTION_NOT_GRANTED);
        /* Fails fast means NOTHING was recorded. */
        struct metaverse_receipt rc[4];
        ASSERT_EQ((int)property_grant_service_receipts(g.grant_id, rc, 4), 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_plan_property_out_of_scope(void)
{
    int failures = 0;
    TEST("a property outside an id-scoped grant is refused at PLAN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 11);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(INSPECT), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_property_id other = make_id(METAVERSE_KIND_CONTENT, 99);
        struct metaverse_action_request r =
            request_for(&other, METAVERSE_ACTION_INSPECT, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_PROPERTY_OUT_OF_SCOPE);
        PASS();
    } _test_next:;
    return failures;
}

static int t_plan_kind_out_of_scope(void)
{
    int failures = 0;
    TEST("a kind-scoped grant refuses another kind at PLAN") {
        struct metaverse_property_id zc =
            make_id(METAVERSE_KIND_ZCODE_PACKAGE, 21);
        fixture_reset(&zc);
        struct metaverse_grant g;
        memset(&g, 0, sizeof(g));
        snprintf(g.holder, sizeof(g.holder), "%s", k_holder);
        g.scope_form = METAVERSE_SCOPE_KINDS;
        g.kinds = metaverse_kind_bit(METAVERSE_KIND_CONTENT);
        g.actions = ACT(INSPECT);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&zc, METAVERSE_ACTION_INSPECT, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_KIND_OUT_OF_SCOPE);
        PASS();
    } _test_next:;
    return failures;
}

static int t_plan_counterparty(void)
{
    int failures = 0;
    TEST("a counterparty outside the allowlist is refused at PLAN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 11);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(SELL), 500000);
        snprintf(g.counterparties[0], sizeof(g.counterparties[0]), "%s",
                 k_buyer);
        g.counterparty_count = 1;
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);

        struct property_grant_plan plan;
        struct metaverse_action_request bad =
            request_for(&prop, METAVERSE_ACTION_SELL, k_stranger, 1000);
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &bad, &plan),
                  (int)PROPERTY_GRANT_COUNTERPARTY_NOT_ALLOWED);
        /* The allowlisted counterparty passes, so the refusal was the
         * allowlist and not some unrelated part of the request. */
        struct metaverse_action_request ok =
            request_for(&prop, METAVERSE_ACTION_SELL, k_buyer, 1000);
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &ok, &plan),
                  (int)PROPERTY_GRANT_OK);
        PASS();
    } _test_next:;
    return failures;
}

static int t_plan_budget(void)
{
    int failures = 0;
    TEST("a budget-exceeding action is refused at PLAN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 11);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct property_grant_plan plan;
        struct metaverse_action_request over =
            request_for(&prop, METAVERSE_ACTION_BUY, NULL, 100001);
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &over, &plan),
                  (int)PROPERTY_GRANT_BUDGET_EXCEEDED);
        struct metaverse_action_request at_cap =
            request_for(&prop, METAVERSE_ACTION_BUY, NULL, 100000);
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &at_cap, &plan),
                  (int)PROPERTY_GRANT_OK);
        PASS();
    } _test_next:;
    return failures;
}

static int t_plan_value_shape(void)
{
    int failures = 0;
    TEST("value on a free action and a negative value each name themselves") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 11);
        fixture_reset(&prop);
        struct metaverse_grant g =
            grant_over_id(&prop, ACT(INSPECT) | ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);

        struct property_grant_plan plan;
        struct metaverse_action_request free_with_value =
            request_for(&prop, METAVERSE_ACTION_INSPECT, NULL, 5);
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &free_with_value,
                                                   &plan),
                  (int)PROPERTY_GRANT_VALUE_ON_FREE_ACTION);

        struct metaverse_action_request negative =
            request_for(&prop, METAVERSE_ACTION_BUY, NULL, -1);
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &negative,
                                                   &plan),
                  (int)PROPERTY_GRANT_VALUE_NEGATIVE);
        PASS();
    } _test_next:;
    return failures;
}

static int t_plan_wrong_holder(void)
{
    int failures = 0;
    TEST("an actor who is not the grant's holder is refused at PLAN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 11);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(INSPECT), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request imposter =
            request_for(&prop, METAVERSE_ACTION_INSPECT, NULL, 0);
        snprintf(imposter.actor, sizeof(imposter.actor), "%s", k_other);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &imposter,
                                                   &plan),
                  (int)PROPERTY_GRANT_WRONG_HOLDER);
        PASS();
    } _test_next:;
    return failures;
}

static int t_plan_expiry(void)
{
    int failures = 0;
    TEST("both expiry forms are enforced at PLAN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 11);
        fixture_reset(&prop);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_INSPECT, NULL, 0);
        struct property_grant_plan plan;

        struct metaverse_grant byh = grant_over_id(&prop, ACT(INSPECT), 0);
        byh.expires_height = g_height;   /* already reached */
        ASSERT_EQ((int)property_grant_service_mint(&byh),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)property_grant_service_plan(byh.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_GRANT_EXPIRED_HEIGHT);

        struct metaverse_grant byt = grant_over_id(&prop, ACT(INSPECT), 0);
        byt.expires_unix = g_now - 1;
        ASSERT_EQ((int)property_grant_service_mint(&byt),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)property_grant_service_plan(byt.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_GRANT_EXPIRED_TIME);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 3. COMMIT re-checks ────────────────────────────────────────────────── */

static int t_commit_stale_revision(void)
{
    int failures = 0;
    TEST("a property revised between PLAN and COMMIT is STALE_REVISION") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 31);
        fixture_reset(&prop);
        struct metaverse_grant g =
            grant_over_id(&prop, ACT(PUBLISH_REVISION), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);

        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_PUBLISH_REVISION, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)plan.property_revision, 7);

        /* Somebody else publishes a revision in between. */
        g_cat.revision = 8;

        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "k", &res),
                  (int)PROPERTY_GRANT_STALE_REVISION);
        struct metaverse_receipt rc[4];
        ASSERT_EQ((int)property_grant_service_receipts(g.grant_id, rc, 4), 0);

        /* Re-planning against the CURRENT revision commits cleanly, so the
         * rejection was the staleness and not a broken commit path. */
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)plan.property_revision, 8);
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "k2", &res),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)res.receipt.property_revision, 8);
        PASS();
    } _test_next:;
    return failures;
}

static int t_commit_owner_mismatch(void)
{
    int failures = 0;
    TEST("a property that changed controller is OWNER_MISMATCH") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 31);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(HOST), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);

        snprintf(g_cat.controller, sizeof(g_cat.controller), "%s", k_other);

        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "k", &res),
                  (int)PROPERTY_GRANT_OWNER_MISMATCH);
        PASS();
    } _test_next:;
    return failures;
}

static int t_commit_property_unknown(void)
{
    int failures = 0;
    TEST("a property that left the catalog is PROPERTY_UNKNOWN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 31);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(HOST), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        g_cat.present = false;
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "k", &res),
                  (int)PROPERTY_GRANT_PROPERTY_UNKNOWN);
        PASS();
    } _test_next:;
    return failures;
}

static int t_commit_no_catalog(void)
{
    int failures = 0;
    TEST("with no catalog installed COMMIT fails closed, PLAN still diagnoses") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 31);
        fixture_reset(&prop);
        struct property_grant_env env;
        memset(&env, 0, sizeof(env));
        env.clock = fake_clock;           /* no catalog_lookup at all */
        property_grant_service_configure(&env);

        struct metaverse_grant g = grant_over_id(&prop, ACT(HOST), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        ASSERT(!plan.catalog_seen);
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "k", &res),
                  (int)PROPERTY_GRANT_CATALOG_UNAVAILABLE);
        PASS();
    } _test_next:;
    return failures;
}

static int t_commit_plan_expired(void)
{
    int failures = 0;
    TEST("a plan that outlived its TTL is PLAN_EXPIRED") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 31);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(HOST), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        g_now += PROPERTY_GRANT_PLAN_TTL_SECONDS + 1;
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "k", &res),
                  (int)PROPERTY_GRANT_PLAN_EXPIRED);
        PASS();
    } _test_next:;
    return failures;
}

static int t_commit_unknown_plan(void)
{
    int failures = 0;
    TEST("an unknown plan id is PLAN_UNKNOWN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 31);
        fixture_reset(&prop);
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(
                      "ffffffffffffffffffffffffffffffff", "k", &res),
                  (int)PROPERTY_GRANT_PLAN_UNKNOWN);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 4. Revocation ──────────────────────────────────────────────────────── */

static int t_revoke_kills_grant(void)
{
    int failures = 0;
    TEST("commit succeeds, revoke, then the next attempt is GRANT_REVOKED") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 41);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(HOST), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);

        struct property_grant_plan plan;
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "one", &res),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)res.receipt.seq, 1);

        /* A plan taken BEFORE the revoke must also die at commit — this is the
         * whole reason COMMIT re-runs the capability check. */
        struct property_grant_plan pre;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &pre),
                  (int)PROPERTY_GRANT_OK);

        ASSERT_EQ((int)property_grant_service_revoke(g.grant_id),
                  (int)PROPERTY_GRANT_OK);

        ASSERT_EQ((int)property_grant_service_commit(pre.plan_id, "two", &res),
                  (int)PROPERTY_GRANT_GRANT_REVOKED);
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_GRANT_REVOKED);

        /* Exactly one receipt: the revoked attempts wrote nothing. */
        struct metaverse_receipt rc[4];
        ASSERT_EQ((int)property_grant_service_receipts(g.grant_id, rc, 4), 1);
        PASS();
    } _test_next:;
    return failures;
}

static int t_revoke_kills_subtree(void)
{
    int failures = 0;
    TEST("revoking a parent kills the delegated child at every stage") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 41);
        fixture_reset(&prop);
        struct metaverse_grant parent =
            grant_over_id(&prop, ACT(HOST) | ACT(DELEGATE), 100000);
        parent.delegation_allowed = true;
        parent.max_delegation_depth = 2;
        ASSERT_EQ((int)property_grant_service_mint(&parent),
                  (int)PROPERTY_GRANT_OK);

        struct metaverse_grant child = grant_over_id(&prop, ACT(HOST), 1000);
        ASSERT_EQ((int)property_grant_service_delegate(parent.grant_id, &child),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)child.depth, 1);
        ASSERT_EQ((int)child.lineage_count, 1);

        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);
        struct property_grant_plan child_plan;
        ASSERT_EQ((int)property_grant_service_plan(child.grant_id, &r,
                                                   &child_plan),
                  (int)PROPERTY_GRANT_OK);

        /* Revoke the PARENT; the child record is never touched. */
        ASSERT_EQ((int)property_grant_service_revoke(parent.grant_id),
                  (int)PROPERTY_GRANT_OK);
        struct metaverse_grant child_now;
        ASSERT_EQ((int)property_grant_service_get(child.grant_id, &child_now),
                  (int)PROPERTY_GRANT_OK);
        ASSERT(!child_now.revoked);

        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(child_plan.plan_id, "x",
                                                     &res),
                  (int)PROPERTY_GRANT_ANCESTOR_REVOKED);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(child.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_ANCESTOR_REVOKED);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 5. Idempotency ─────────────────────────────────────────────────────── */

static int t_idempotent_commit(void)
{
    int failures = 0;
    TEST("the same idempotency key returns the SAME receipt and charges once") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 51);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);

        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_BUY, NULL, 25000);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);

        struct property_grant_commit_result first;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "order-7",
                                                     &first),
                  (int)PROPERTY_GRANT_OK);
        ASSERT(!first.replayed);
        ASSERT_EQ((int)first.receipt.seq, 1);
        ASSERT_EQ((int)first.budget_remaining_zat, 75000);

        struct property_grant_commit_result second;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "order-7",
                                                     &second),
                  (int)PROPERTY_GRANT_OK);
        ASSERT(second.replayed);
        ASSERT_EQ((int)second.receipt.seq, (int)first.receipt.seq);
        ASSERT(memcmp(second.receipt.chain_hash, first.receipt.chain_hash,
                      METAVERSE_HASH_LEN) == 0);
        /* No double charge and no second chain link. */
        ASSERT_EQ((int)second.budget_remaining_zat, 75000);
        struct metaverse_grant after;
        ASSERT_EQ((int)property_grant_service_get(g.grant_id, &after),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)after.spent_zat, 25000);
        struct metaverse_receipt rc[8];
        ASSERT_EQ((int)property_grant_service_receipts(g.grant_id, rc, 8), 1);
        PASS();
    } _test_next:;
    return failures;
}

static int t_unkeyed_commit_not_replayable(void)
{
    int failures = 0;
    TEST("an un-keyed commit is not replay-protected and says so") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 51);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(HOST), 0);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "", &res),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "", &res),
                  (int)PROPERTY_GRANT_PLAN_ALREADY_COMMITTED);
        PASS();
    } _test_next:;
    return failures;
}

static int t_distinct_key_is_not_replay(void)
{
    int failures = 0;
    TEST("a different key on a committed plan is not a replay") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 51);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_BUY, NULL, 1000);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "a", &res),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "b", &res),
                  (int)PROPERTY_GRANT_PLAN_ALREADY_COMMITTED);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 6. Tamper evidence ─────────────────────────────────────────────────── */

static int t_tamper_edit_detected(void)
{
    int failures = 0;
    TEST("editing a stored receipt breaks the chain and is located") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 61);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        ASSERT_EQ(commit_n(&g, &prop, 3), 0);

        uint64_t bad = 0;
        ASSERT_EQ((int)property_grant_service_verify_chain(g.grant_id, &bad),
                  (int)METAVERSE_RECEIPT_OK);

        struct metaverse_receipt rc[8];
        ASSERT_EQ((int)property_grant_service_receipts(g.grant_id, rc, 8), 3);

        /* An auditor's-eye edit: change the amount and put it back. */
        struct metaverse_receipt edited = rc[1];
        edited.value_zat = 999999;
        ASSERT(property_grant_service_test_overwrite_receipt(g.grant_id, 2,
                                                             &edited));
        ASSERT_EQ((int)property_grant_service_verify_chain(g.grant_id, &bad),
                  (int)METAVERSE_RECEIPT_BODY_HASH_MISMATCH);
        ASSERT_EQ((int)bad, 2);
        PASS();
    } _test_next:;
    return failures;
}

static int t_tamper_foreign_signature(void)
{
    int failures = 0;
    TEST("re-signing a rewritten receipt with a foreign key is still caught") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 61);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        ASSERT_EQ(commit_n(&g, &prop, 2), 0);

        struct metaverse_receipt rc[8];
        ASSERT_EQ((int)property_grant_service_receipts(g.grant_id, rc, 8), 2);

        /* The forger rewrites the amount AND re-seals it properly with their
         * own key, so body_hash and chain_hash are internally consistent. Only
         * pinning the expected signer catches this. */
        uint8_t fseed[32], fpk[32], fsk[32];
        memset(fseed, 0x11, sizeof(fseed));
        ed25519_keypair(fpk, fsk, fseed);
        struct metaverse_receipt forged = rc[1];
        forged.value_zat = 42;
        ASSERT(metaverse_receipt_seal(&forged, rc[0].chain_hash, fsk, fpk));
        ASSERT(property_grant_service_test_overwrite_receipt(g.grant_id, 2,
                                                             &forged));

        uint64_t bad = 0;
        ASSERT_EQ((int)property_grant_service_verify_chain(g.grant_id, &bad),
                  (int)METAVERSE_RECEIPT_SIGNER_UNEXPECTED);
        ASSERT_EQ((int)bad, 2);
        PASS();
    } _test_next:;
    return failures;
}

static int t_tamper_cut_link(void)
{
    int failures = 0;
    TEST("cutting a link out of the chain is reported as CHAIN_BROKEN") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 61);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        ASSERT_EQ(commit_n(&g, &prop, 3), 0);

        struct metaverse_receipt rc[8];
        ASSERT_EQ((int)property_grant_service_receipts(g.grant_id, rc, 8), 3);

        /* Re-point receipt 3 at the FIRST receipt, as if #2 never existed, and
         * seal it correctly with the node's own key. Body and signature are
         * perfect; only the link is wrong. */
        uint8_t signer[METAVERSE_PUBKEY_LEN];
        ASSERT(property_grant_service_signer_pubkey(signer));
        uint8_t seed[32], pk[32], sk[32];
        memset(seed, 0xA5, sizeof(seed));
        ed25519_keypair(pk, sk, seed);
        ASSERT(memcmp(pk, signer, METAVERSE_PUBKEY_LEN) == 0);

        struct metaverse_receipt relinked = rc[2];
        ASSERT(metaverse_receipt_seal(&relinked, rc[0].chain_hash, sk, pk));
        ASSERT_EQ((int)metaverse_receipt_verify(&relinked, signer),
                  (int)METAVERSE_RECEIPT_OK);
        ASSERT(property_grant_service_test_overwrite_receipt(g.grant_id, 3,
                                                             &relinked));

        uint64_t bad = 0;
        ASSERT_EQ((int)property_grant_service_verify_chain(g.grant_id, &bad),
                  (int)METAVERSE_RECEIPT_CHAIN_BROKEN);
        ASSERT_EQ((int)bad, 3);
        PASS();
    } _test_next:;
    return failures;
}

static int t_chain_verifier_agrees(void)
{
    int failures = 0;
    TEST("the standalone chain verifier agrees, and catches a reorder") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 61);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 100000);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        ASSERT_EQ(commit_n(&g, &prop, 2), 0);

        struct metaverse_receipt rc[8];
        size_t n = property_grant_service_receipts(g.grant_id, rc, 8);
        ASSERT_EQ((int)n, 2);
        uint8_t signer[METAVERSE_PUBKEY_LEN];
        ASSERT(property_grant_service_signer_pubkey(signer));
        size_t bad_index = 99;
        ASSERT_EQ((int)metaverse_receipt_chain_verify(rc, n, signer,
                                                      &bad_index),
                  (int)METAVERSE_RECEIPT_OK);

        /* Reordering alone is caught, without touching a single byte. */
        struct metaverse_receipt swapped[2] = { rc[1], rc[0] };
        ASSERT_EQ((int)metaverse_receipt_chain_verify(swapped, 2, signer,
                                                      &bad_index),
                  (int)METAVERSE_RECEIPT_SEQ_OUT_OF_ORDER);
        ASSERT_EQ((int)bad_index, 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 7. Delegation attenuation ──────────────────────────────────────────── */

static int t_delegation_depth(void)
{
    int failures = 0;
    TEST("delegation beyond the declared max depth is refused") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 71);
        fixture_reset(&prop);
        struct metaverse_grant parent =
            grant_over_id(&prop, ACT(HOST) | ACT(DELEGATE), 100000);
        parent.delegation_allowed = true;
        parent.max_delegation_depth = 1;  /* children yes, grandchildren no */
        ASSERT_EQ((int)property_grant_service_mint(&parent),
                  (int)PROPERTY_GRANT_OK);

        struct metaverse_grant child =
            grant_over_id(&prop, ACT(HOST) | ACT(DELEGATE), 1000);
        child.delegation_allowed = true;
        child.max_delegation_depth = 1;
        ASSERT_EQ((int)property_grant_service_delegate(parent.grant_id, &child),
                  (int)PROPERTY_GRANT_OK);

        struct metaverse_grant grandchild = grant_over_id(&prop, ACT(HOST), 100);
        ASSERT_EQ((int)property_grant_service_delegate(child.grant_id,
                                                       &grandchild),
                  (int)PROPERTY_GRANT_DELEGATION_DEPTH_EXCEEDED);
        PASS();
    } _test_next:;
    return failures;
}

static int t_delegation_requires_right(void)
{
    int failures = 0;
    TEST("a grant without the DELEGATE action cannot mint a child") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 71);
        fixture_reset(&prop);
        struct metaverse_grant parent = grant_over_id(&prop, ACT(HOST), 100000);
        parent.delegation_allowed = true;  /* the flag alone is not enough */
        parent.max_delegation_depth = 2;
        ASSERT_EQ((int)property_grant_service_mint(&parent),
                  (int)PROPERTY_GRANT_OK);
        struct metaverse_grant child = grant_over_id(&prop, ACT(HOST), 10);
        ASSERT_EQ((int)property_grant_service_delegate(parent.grant_id, &child),
                  (int)PROPERTY_GRANT_DELEGATION_NOT_PERMITTED);
        PASS();
    } _test_next:;
    return failures;
}

static int t_delegation_attenuates(void)
{
    int failures = 0;
    TEST("a child may not hold an action, budget, or scope the parent lacks") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 71);
        fixture_reset(&prop);
        struct metaverse_grant parent =
            grant_over_id(&prop, ACT(HOST) | ACT(DELEGATE), 5000);
        parent.delegation_allowed = true;
        parent.max_delegation_depth = 2;
        ASSERT_EQ((int)property_grant_service_mint(&parent),
                  (int)PROPERTY_GRANT_OK);

        struct metaverse_grant wider_action =
            grant_over_id(&prop, ACT(HOST) | ACT(TRANSFER), 10);
        ASSERT_EQ((int)property_grant_service_delegate(parent.grant_id,
                                                       &wider_action),
                  (int)PROPERTY_GRANT_ACTION_NOT_GRANTED);

        struct metaverse_grant richer = grant_over_id(&prop, ACT(HOST), 5001);
        ASSERT_EQ((int)property_grant_service_delegate(parent.grant_id,
                                                       &richer),
                  (int)PROPERTY_GRANT_BUDGET_EXCEEDED);

        struct metaverse_property_id other = make_id(METAVERSE_KIND_CONTENT, 72);
        struct metaverse_grant elsewhere = grant_over_id(&other, ACT(HOST), 10);
        ASSERT_EQ((int)property_grant_service_delegate(parent.grant_id,
                                                       &elsewhere),
                  (int)PROPERTY_GRANT_PROPERTY_OUT_OF_SCOPE);

        /* A kind-wildcard child under an id-scoped parent is a WIDENING. */
        struct metaverse_grant kindwide;
        memset(&kindwide, 0, sizeof(kindwide));
        snprintf(kindwide.holder, sizeof(kindwide.holder), "%s", k_holder);
        kindwide.scope_form = METAVERSE_SCOPE_KINDS;
        kindwide.kinds = metaverse_kind_bit(METAVERSE_KIND_CONTENT);
        kindwide.actions = ACT(HOST);
        ASSERT_EQ((int)property_grant_service_delegate(parent.grant_id,
                                                       &kindwide),
                  (int)PROPERTY_GRANT_KIND_OUT_OF_SCOPE);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 8. Budget and rate window ──────────────────────────────────────────── */

static int t_budget_is_cumulative(void)
{
    int failures = 0;
    TEST("the budget is CUMULATIVE: repeated under-cap actions exhaust it") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 81);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 2500);
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);
        ASSERT_EQ(commit_n(&g, &prop, 2), 0);   /* 1000 + 1000 spent */

        struct metaverse_grant after;
        ASSERT_EQ((int)property_grant_service_get(g.grant_id, &after),
                  (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)after.spent_zat, 2000);
        ASSERT_EQ((int)metaverse_grant_budget_remaining(&after), 500);

        /* A third 1000 is under no per-action cap but over the ceiling. */
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_BUY, NULL, 1000);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_BUDGET_EXCEEDED);
        PASS();
    } _test_next:;
    return failures;
}

static int t_rate_window(void)
{
    int failures = 0;
    TEST("the rate window bounds action COUNT and rolls over") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 81);
        fixture_reset(&prop);
        struct metaverse_grant g = grant_over_id(&prop, ACT(BUY), 1000000);
        g.rate_limit = 2;
        g.rate_window_seconds = 60;
        ASSERT_EQ((int)property_grant_service_mint(&g), (int)PROPERTY_GRANT_OK);

        ASSERT_EQ(commit_n(&g, &prop, 2), 0);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_BUY, NULL, 1000);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_RATE_LIMITED);

        g_now += 61;
        ASSERT_EQ((int)property_grant_service_plan(g.grant_id, &r, &plan),
                  (int)PROPERTY_GRANT_OK);
        struct property_grant_commit_result res;
        ASSERT_EQ((int)property_grant_service_commit(plan.plan_id, "after",
                                                     &res),
                  (int)PROPERTY_GRANT_OK);
        PASS();
    } _test_next:;
    return failures;
}

static int t_unknown_and_malformed(void)
{
    int failures = 0;
    TEST("an unknown grant and a malformed grant each name themselves") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 81);
        fixture_reset(&prop);
        struct metaverse_action_request r =
            request_for(&prop, METAVERSE_ACTION_HOST, NULL, 0);
        struct property_grant_plan plan;
        ASSERT_EQ((int)property_grant_service_plan(
                      "00000000000000000000000000000000", &r, &plan),
                  (int)PROPERTY_GRANT_GRANT_UNKNOWN);

        struct metaverse_grant empty_scope;
        memset(&empty_scope, 0, sizeof(empty_scope));
        snprintf(empty_scope.holder, sizeof(empty_scope.holder), "%s",
                 k_holder);
        empty_scope.scope_form = METAVERSE_SCOPE_IDS;
        empty_scope.id_count = 0;   /* no ids under an id-scoped grant */
        ASSERT_EQ((int)property_grant_service_mint(&empty_scope),
                  (int)PROPERTY_GRANT_GRANT_MALFORMED);
        PASS();
    } _test_next:;
    return failures;
}

static int t_list_and_get(void)
{
    int failures = 0;
    TEST("list and get report what the store holds") {
        struct metaverse_property_id prop = make_id(METAVERSE_KIND_CONTENT, 81);
        fixture_reset(&prop);
        struct metaverse_grant a = grant_over_id(&prop, ACT(HOST), 0);
        struct metaverse_grant b = grant_over_id(&prop, ACT(INSPECT), 0);
        snprintf(b.holder, sizeof(b.holder), "%s", k_other);
        ASSERT_EQ((int)property_grant_service_mint(&a), (int)PROPERTY_GRANT_OK);
        ASSERT_EQ((int)property_grant_service_mint(&b), (int)PROPERTY_GRANT_OK);

        struct metaverse_grant rows[8];
        ASSERT_EQ((int)property_grant_service_list(NULL, rows, 8), 2);
        ASSERT_EQ((int)property_grant_service_list(k_holder, rows, 8), 1);
        ASSERT_STR_EQ(rows[0].grant_id, a.grant_id);
        struct metaverse_grant one;
        ASSERT_EQ((int)property_grant_service_get("deadbeef", &one),
                  (int)PROPERTY_GRANT_GRANT_UNKNOWN);
        PASS();
    } _test_next:;
    return failures;
}

static int t_reason_tokens_are_contract(void)
{
    int failures = 0;
    TEST("every reason has a distinct, stable token") {
        ASSERT_STR_EQ(property_grant_reason_token(PROPERTY_GRANT_OK), "OK");
        ASSERT_STR_EQ(property_grant_reason_token(PROPERTY_GRANT_STALE_REVISION),
                      "STALE_REVISION");
        ASSERT_STR_EQ(property_grant_reason_token(PROPERTY_GRANT_GRANT_REVOKED),
                      "GRANT_REVOKED");
        ASSERT_STR_EQ(
            property_grant_reason_token(PROPERTY_GRANT_ANCESTOR_REVOKED),
            "ANCESTOR_REVOKED");
        ASSERT_STR_EQ(property_grant_reason_token(PROPERTY_GRANT_REASON_COUNT),
                      "UNKNOWN_REASON");
        /* No two reasons share a token — a duplicate would make two different
         * refusals indistinguishable to an operator. */
        for (int i = 0; i < PROPERTY_GRANT_REASON_COUNT; i++) {
            for (int j = i + 1; j < PROPERTY_GRANT_REASON_COUNT; j++) {
                ASSERT(strcmp(property_grant_reason_token(
                                  (enum property_grant_reason)i),
                              property_grant_reason_token(
                                  (enum property_grant_reason)j)) != 0);
            }
        }
        /* And the verdict→reason map is total. */
        for (int v = 1; v < METAVERSE_GRANT_VERDICT_COUNT; v++) {
            ASSERT(property_grant_reason_from_verdict(
                       (enum metaverse_grant_verdict)v) != PROPERTY_GRANT_OK);
        }
        PASS();
    } _test_next:;
    return failures;
}

int test_metaverse_grant(void);
int test_metaverse_grant(void)
{
    printf("\n=== metaverse property grant tests ===\n");
    int failures = 0;

    failures += t_id_roundtrip();
    failures += t_id_rejects();
    failures += t_action_set_codec();
    failures += t_value_actions();

    failures += t_plan_action_not_granted();
    failures += t_plan_property_out_of_scope();
    failures += t_plan_kind_out_of_scope();
    failures += t_plan_counterparty();
    failures += t_plan_budget();
    failures += t_plan_value_shape();
    failures += t_plan_wrong_holder();
    failures += t_plan_expiry();

    failures += t_commit_stale_revision();
    failures += t_commit_owner_mismatch();
    failures += t_commit_property_unknown();
    failures += t_commit_no_catalog();
    failures += t_commit_plan_expired();
    failures += t_commit_unknown_plan();

    failures += t_revoke_kills_grant();
    failures += t_revoke_kills_subtree();

    failures += t_idempotent_commit();
    failures += t_unkeyed_commit_not_replayable();
    failures += t_distinct_key_is_not_replay();

    failures += t_tamper_edit_detected();
    failures += t_tamper_foreign_signature();
    failures += t_tamper_cut_link();
    failures += t_chain_verifier_agrees();

    failures += t_delegation_depth();
    failures += t_delegation_requires_right();
    failures += t_delegation_attenuates();

    failures += t_budget_is_cumulative();
    failures += t_rate_window();
    failures += t_unknown_and_malformed();
    failures += t_list_and_get();
    failures += t_reason_tokens_are_contract();

    property_grant_service_reset();
    printf("metaverse_grant: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}
