/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Property grant service — the store, the environment seams, grant lifecycle
 * (mint / delegate / revoke / list), and PLAN. COMMIT, the receipt chain, and
 * chain verification live in property_grant_commit.c over the private store
 * API in property_grant_store.h; the two files share one lock and one set of
 * bounded arrays, and nothing outside them touches the arrays.
 *
 * The decision rules are all in lib/metaverse and are pure. Everything in this
 * file is store, clock, and refusal plumbing. See the public header for why
 * COMMIT re-checks rather than trusting its plan. */

// one-result-type-ok:grant-decision — this service has no bool/errno failure
// surface to converge: every fallible entry point already returns
// `enum property_grant_reason`, a closed taxonomy where the refusal IS the
// answer (GRANT_REVOKED, BUDGET_EXCEEDED, STALE_REVISION, …) and the tokens are
// contract asserted by test_metaverse_grant. Wrapping that in struct zcl_result
// would put the reason in a free-text string and lose the exhaustive
// verdict→reason map. The four remaining bool exports are not that surface:
// set_signing_seed / signer_pubkey are key-material predicates, and
// pg_collect_ancestors / pg_ensure_key are private store helpers shared with
// property_grant_commit.c through property_grant_store.h.

#include "services/property_grant_service.h"

#include "base/hex.h"
#include "base/log_macros.h"
#include "crypto/random_secret.h"
#include "platform/clock.h"
#include "property_grant_store.h"

#include <stdio.h>
#include <string.h>

#define PG_LOG "metaverse.grant_service"

/* ── The store ──────────────────────────────────────────────────────────── */

struct property_grant_store g_pg_store = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static const char *const k_reason_tokens[PROPERTY_GRANT_REASON_COUNT] = {
    "OK",
    "BAD_ARGS",
    "STORE_FULL",
    "GRANT_UNKNOWN",
    "GRANT_EXISTS",
    "GRANT_MALFORMED",
    "GRANT_REVOKED",
    "ANCESTOR_REVOKED",
    "GRANT_EXPIRED_HEIGHT",
    "GRANT_EXPIRED_TIME",
    "WRONG_HOLDER",
    "ACTION_NOT_GRANTED",
    "PROPERTY_OUT_OF_SCOPE",
    "KIND_OUT_OF_SCOPE",
    "COUNTERPARTY_NOT_ALLOWED",
    "VALUE_NEGATIVE",
    "VALUE_ON_FREE_ACTION",
    "BUDGET_EXCEEDED",
    "RATE_LIMITED",
    "DELEGATION_NOT_PERMITTED",
    "DELEGATION_DEPTH_EXCEEDED",
    "PLAN_UNKNOWN",
    "PLAN_EXPIRED",
    "PLAN_ALREADY_COMMITTED",
    "CATALOG_UNAVAILABLE",
    "PROPERTY_UNKNOWN",
    "OWNER_MISMATCH",
    "STALE_REVISION",
    "RECEIPT_SEAL_FAILED",
    "SIGNING_KEY_UNAVAILABLE",
};

const char *property_grant_reason_token(enum property_grant_reason r)
{
    if (r < 0 || r >= PROPERTY_GRANT_REASON_COUNT) return "UNKNOWN_REASON";
    return k_reason_tokens[r];
}

/* The static-scope block of the taxonomy is laid out to mirror
 * enum metaverse_grant_verdict one-for-one, and this table is the proof: a
 * verdict added to the rules without a matching reason here becomes a compile
 * error, not a silent "UNKNOWN_REASON" in an operator's error body. */
static const enum property_grant_reason
    k_verdict_map[METAVERSE_GRANT_VERDICT_COUNT] = {
    [METAVERSE_GRANT_OK] = PROPERTY_GRANT_OK,
    [METAVERSE_GRANT_BAD_ARGS] = PROPERTY_GRANT_BAD_ARGS,
    [METAVERSE_GRANT_MALFORMED] = PROPERTY_GRANT_GRANT_MALFORMED,
    [METAVERSE_GRANT_REVOKED] = PROPERTY_GRANT_GRANT_REVOKED,
    [METAVERSE_GRANT_ANCESTOR_REVOKED] = PROPERTY_GRANT_ANCESTOR_REVOKED,
    [METAVERSE_GRANT_EXPIRED_HEIGHT] = PROPERTY_GRANT_GRANT_EXPIRED_HEIGHT,
    [METAVERSE_GRANT_EXPIRED_TIME] = PROPERTY_GRANT_GRANT_EXPIRED_TIME,
    [METAVERSE_GRANT_WRONG_HOLDER] = PROPERTY_GRANT_WRONG_HOLDER,
    [METAVERSE_GRANT_ACTION_NOT_GRANTED] = PROPERTY_GRANT_ACTION_NOT_GRANTED,
    [METAVERSE_GRANT_PROPERTY_OUT_OF_SCOPE] =
        PROPERTY_GRANT_PROPERTY_OUT_OF_SCOPE,
    [METAVERSE_GRANT_KIND_OUT_OF_SCOPE] = PROPERTY_GRANT_KIND_OUT_OF_SCOPE,
    [METAVERSE_GRANT_COUNTERPARTY_NOT_ALLOWED] =
        PROPERTY_GRANT_COUNTERPARTY_NOT_ALLOWED,
    [METAVERSE_GRANT_VALUE_NEGATIVE] = PROPERTY_GRANT_VALUE_NEGATIVE,
    [METAVERSE_GRANT_VALUE_ON_FREE_ACTION] =
        PROPERTY_GRANT_VALUE_ON_FREE_ACTION,
    [METAVERSE_GRANT_BUDGET_EXCEEDED] = PROPERTY_GRANT_BUDGET_EXCEEDED,
    [METAVERSE_GRANT_RATE_LIMITED] = PROPERTY_GRANT_RATE_LIMITED,
    [METAVERSE_GRANT_DELEGATION_NOT_PERMITTED] =
        PROPERTY_GRANT_DELEGATION_NOT_PERMITTED,
    [METAVERSE_GRANT_DELEGATION_DEPTH_EXCEEDED] =
        PROPERTY_GRANT_DELEGATION_DEPTH_EXCEEDED,
};

enum property_grant_reason property_grant_reason_from_verdict(
    enum metaverse_grant_verdict v)
{
    if (v < 0 || v >= METAVERSE_GRANT_VERDICT_COUNT)
        return PROPERTY_GRANT_BAD_ARGS;
    return k_verdict_map[v];
}

/* ── Environment ────────────────────────────────────────────────────────── */

/* Wall clock in seconds; height 0 means "unknown", which cannot prove expiry.
 * clock_now_wall_ms is the platform boundary, so this file has no raw clock. */
static void default_clock(int64_t *now_unix, int64_t *height, void *ctx)
{
    (void)ctx;
    if (now_unix) *now_unix = clock_now_wall_ms() / 1000;
    if (height) *height = 0;
}

void property_grant_service_configure(const struct property_grant_env *env)
{
    pthread_mutex_lock(&g_pg_store.lock);
    if (env) {
        g_pg_store.env = *env;
    } else {
        memset(&g_pg_store.env, 0, sizeof(g_pg_store.env));
    }
    if (!g_pg_store.env.clock) g_pg_store.env.clock = default_clock;
    pthread_mutex_unlock(&g_pg_store.lock);
}

void pg_now(int64_t *now_unix, int64_t *height)
{
    metaverse_clock_fn fn = g_pg_store.env.clock;
    if (!fn) fn = default_clock;
    int64_t t = 0, h = 0;
    fn(&t, &h, g_pg_store.env.clock_ctx);
    if (now_unix) *now_unix = t;
    if (height) *height = h;
}

void property_grant_service_reset(void)
{
    pthread_mutex_lock(&g_pg_store.lock);
    memset(g_pg_store.grants, 0, sizeof(g_pg_store.grants));
    memset(g_pg_store.grant_used, 0, sizeof(g_pg_store.grant_used));
    memset(g_pg_store.plans, 0, sizeof(g_pg_store.plans));
    memset(g_pg_store.plan_used, 0, sizeof(g_pg_store.plan_used));
    memset(g_pg_store.receipts, 0, sizeof(g_pg_store.receipts));
    g_pg_store.receipt_count = 0;
    memset(g_pg_store.sk, 0, sizeof(g_pg_store.sk));
    memset(g_pg_store.pk, 0, sizeof(g_pg_store.pk));
    g_pg_store.key_set = false;
    pthread_mutex_unlock(&g_pg_store.lock);
}

/* ── Ids ────────────────────────────────────────────────────────────────── */

/* 128 bits rendered as 32 lowercase hex chars — the same shape and hygiene as
 * an agent session id. Fails closed if the CSPRNG fails: a predictable grant id
 * is a guessable handle onto someone else's authority. */
static bool draw_id(char *out, size_t out_cap)
{
    if (!out || out_cap < 33)
        LOG_FAIL(PG_LOG, "draw id: buffer %zu < 33", out_cap);
    uint8_t raw[16];
    if (!zcl_random_secret_bytes(raw, sizeof(raw), "metaverse_grant_id"))
        LOG_FAIL(PG_LOG, "draw id: CSPRNG failed");
    zcl_hex_encode(raw, sizeof(raw), out);  /* base/hex.h: the one hex codec */
    return true;
}

/* ── Store lookups (lock held by caller) ────────────────────────────────── */

struct metaverse_grant *pg_find_grant(const char *grant_id)
{
    if (!grant_id || grant_id[0] == '\0') return NULL;
    for (size_t i = 0; i < PROPERTY_GRANT_MAX_GRANTS; i++) {
        if (!g_pg_store.grant_used[i]) continue;
        if (strcmp(g_pg_store.grants[i].grant_id, grant_id) == 0)
            return &g_pg_store.grants[i];
    }
    return NULL;
}

/* Collect a grant's ancestors, root-first, from its recorded lineage. Returns
 * false when ANY ancestor is missing from the store — the caller must treat
 * that as ANCESTOR_REVOKED, never as "no ancestors to check". */
bool pg_collect_ancestors(const struct metaverse_grant *g,
                          const struct metaverse_grant **out, size_t *out_count)
{
    *out_count = 0;
    for (size_t i = 0; i < g->lineage_count; i++) {
        struct metaverse_grant *a = pg_find_grant(g->lineage[i].grant_id);
        if (!a) return false;
        out[i] = a;
    }
    *out_count = g->lineage_count;
    return true;
}

bool pg_ensure_key(void)
{
    if (g_pg_store.key_set) return true;
    uint8_t seed[32];
    if (!zcl_random_secret_bytes(seed, sizeof(seed), "metaverse_receipt_key"))
        LOG_FAIL(PG_LOG, "receipt signing key: CSPRNG failed");
    uint8_t sk[32], pk[32];
    ed25519_keypair(pk, sk, seed);
    memcpy(g_pg_store.sk, sk, sizeof(sk));
    memcpy(g_pg_store.pk, pk, sizeof(pk));
    g_pg_store.key_set = true;
    memset(seed, 0, sizeof(seed));
    return true;
}

bool property_grant_service_set_signing_seed(const uint8_t seed[32])
{
    if (!seed)
        LOG_FAIL(PG_LOG, "set signing seed: NULL seed");
    pthread_mutex_lock(&g_pg_store.lock);
    uint8_t sk[32], pk[32];
    ed25519_keypair(pk, sk, seed);
    memcpy(g_pg_store.sk, sk, sizeof(sk));
    memcpy(g_pg_store.pk, pk, sizeof(pk));
    g_pg_store.key_set = true;
    pthread_mutex_unlock(&g_pg_store.lock);
    return true;
}

bool property_grant_service_signer_pubkey(uint8_t out[METAVERSE_PUBKEY_LEN])
{
    if (!out)
        LOG_FAIL(PG_LOG, "signer pubkey: NULL output");
    pthread_mutex_lock(&g_pg_store.lock);
    bool have = g_pg_store.key_set;
    if (have) memcpy(out, g_pg_store.pk, METAVERSE_PUBKEY_LEN);
    pthread_mutex_unlock(&g_pg_store.lock);
    if (!have)
        LOG_FAIL(PG_LOG, "signer pubkey: no receipt signing key established");
    return true;
}

/* ── Grant lifecycle ────────────────────────────────────────────────────── */

/* Insert a copy of `g`; caller holds the lock and has already validated. */
static enum property_grant_reason store_grant(const struct metaverse_grant *g)
{
    if (pg_find_grant(g->grant_id)) return PROPERTY_GRANT_GRANT_EXISTS;
    for (size_t i = 0; i < PROPERTY_GRANT_MAX_GRANTS; i++) {
        if (g_pg_store.grant_used[i]) continue;
        g_pg_store.grants[i] = *g;
        g_pg_store.grant_used[i] = true;
        return PROPERTY_GRANT_OK;
    }
    return PROPERTY_GRANT_STORE_FULL;
}

enum property_grant_reason property_grant_service_mint(struct metaverse_grant *g)
{
    if (!g) {
        LOG_ERROR(PG_LOG, "mint: NULL grant");
        return PROPERTY_GRANT_BAD_ARGS;
    }
    if (g->depth != 0 || g->lineage_count != 0) {
        LOG_ERROR(PG_LOG, "mint: depth %u is not a root grant — use delegate",
                     g->depth);
        return PROPERTY_GRANT_BAD_ARGS;
    }

    if (g->grant_id[0] == '\0' && !draw_id(g->grant_id, sizeof(g->grant_id))) {
        LOG_ERROR(PG_LOG, "mint: could not draw a grant id");
        return PROPERTY_GRANT_BAD_ARGS;
    }

    int64_t now = 0, height = 0;
    pg_now(&now, &height);
    if (g->created_unix == 0) g->created_unix = now;
    if (g->created_height == 0) g->created_height = height;
    if (g->rate_limit > 0 && g->window_start_unix == 0)
        g->window_start_unix = now;

    if (!metaverse_grant_well_formed(g)) {
        LOG_WARN(PG_LOG, "mint: grant %s is malformed", g->grant_id);
        return PROPERTY_GRANT_GRANT_MALFORMED;
    }

    pthread_mutex_lock(&g_pg_store.lock);
    enum property_grant_reason r = store_grant(g);
    pthread_mutex_unlock(&g_pg_store.lock);
    if (r != PROPERTY_GRANT_OK)
        LOG_WARN(PG_LOG, "mint: grant %s refused (%s)", g->grant_id,
                     property_grant_reason_token(r));
    return r;
}

enum property_grant_reason property_grant_service_delegate(
    const char *parent_grant_id, struct metaverse_grant *child)
{
    if (!parent_grant_id || !child) {
        LOG_ERROR(PG_LOG, "delegate: NULL parent id or child");
        return PROPERTY_GRANT_BAD_ARGS;
    }

    int64_t now = 0, height = 0;
    pg_now(&now, &height);

    pthread_mutex_lock(&g_pg_store.lock);
    struct metaverse_grant *parent = pg_find_grant(parent_grant_id);
    if (!parent) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_WARN(PG_LOG, "delegate: no grant %s", parent_grant_id);
        return PROPERTY_GRANT_GRANT_UNKNOWN;
    }

    /* Stamp lineage from the LIVE store: the parent chain plus the parent's
     * current generation. This capture is what makes a later revoke anywhere up
     * the chain invalidate this child without touching it. */
    child->depth = parent->depth + 1u;
    if (child->depth > METAVERSE_GRANT_MAX_DEPTH) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_WARN(PG_LOG, "delegate: child depth %u over ceiling %d",
                     child->depth, METAVERSE_GRANT_MAX_DEPTH);
        return PROPERTY_GRANT_DELEGATION_DEPTH_EXCEEDED;
    }
    memset(child->lineage, 0, sizeof(child->lineage));
    for (size_t i = 0; i < parent->lineage_count; i++)
        child->lineage[i] = parent->lineage[i];
    snprintf(child->lineage[parent->lineage_count].grant_id,
             sizeof(child->lineage[0].grant_id), "%s", parent->grant_id);
    child->lineage[parent->lineage_count].revocation_generation =
        parent->revocation_generation;
    child->lineage_count = parent->lineage_count + 1u;
    child->revoked = false;
    child->revocation_generation = 0;
    if (child->created_unix == 0) child->created_unix = now;
    if (child->created_height == 0) child->created_height = height;
    if (child->rate_limit > 0 && child->window_start_unix == 0)
        child->window_start_unix = now;
    if (child->grant_id[0] == '\0' &&
        !draw_id(child->grant_id, sizeof(child->grant_id))) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_ERROR(PG_LOG, "delegate: could not draw a grant id");
        return PROPERTY_GRANT_BAD_ARGS;
    }

    const struct metaverse_grant *anc[METAVERSE_GRANT_MAX_DEPTH];
    size_t anc_count = 0;
    if (!pg_collect_ancestors(parent, anc, &anc_count)) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_WARN(PG_LOG, "delegate: parent %s has a missing ancestor",
                     parent_grant_id);
        return PROPERTY_GRANT_ANCESTOR_REVOKED;
    }

    enum metaverse_grant_verdict v = metaverse_grant_check_delegation(
        parent, anc, anc_count, child, now, height);
    if (v != METAVERSE_GRANT_OK) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_WARN(PG_LOG, "delegate: parent %s refused child (%s)",
                     parent_grant_id, metaverse_grant_verdict_token(v));
        return property_grant_reason_from_verdict(v);
    }

    enum property_grant_reason r = store_grant(child);
    pthread_mutex_unlock(&g_pg_store.lock);
    if (r != PROPERTY_GRANT_OK)
        LOG_ERROR(PG_LOG, "delegate: store refused child %s (%s)",
                     child->grant_id, property_grant_reason_token(r));
    return r;
}

enum property_grant_reason property_grant_service_get(
    const char *grant_id, struct metaverse_grant *out)
{
    if (!grant_id || !out) return PROPERTY_GRANT_BAD_ARGS;
    pthread_mutex_lock(&g_pg_store.lock);
    const struct metaverse_grant *g = pg_find_grant(grant_id);
    if (g) *out = *g;
    pthread_mutex_unlock(&g_pg_store.lock);
    return g ? PROPERTY_GRANT_OK : PROPERTY_GRANT_GRANT_UNKNOWN;
}

enum property_grant_reason property_grant_service_revoke(const char *grant_id)
{
    if (!grant_id) {
        LOG_ERROR(PG_LOG, "revoke: NULL grant id");
        return PROPERTY_GRANT_BAD_ARGS;
    }
    pthread_mutex_lock(&g_pg_store.lock);
    struct metaverse_grant *g = pg_find_grant(grant_id);
    if (g) {
        g->revocation_generation++;
        g->revoked = true;
    }
    pthread_mutex_unlock(&g_pg_store.lock);
    if (!g) {
        LOG_WARN(PG_LOG, "revoke: no grant %s", grant_id);
        return PROPERTY_GRANT_GRANT_UNKNOWN;
    }
    return PROPERTY_GRANT_OK;
}

size_t property_grant_service_list(const char *holder,
                                   struct metaverse_grant *out, size_t max)
{
    if (!out || max == 0) return 0;
    bool all = (!holder || holder[0] == '\0');
    size_t n = 0;
    pthread_mutex_lock(&g_pg_store.lock);
    for (size_t i = 0; i < PROPERTY_GRANT_MAX_GRANTS && n < max; i++) {
        if (!g_pg_store.grant_used[i]) continue;
        if (!all && strcmp(g_pg_store.grants[i].holder, holder) != 0) continue;
        out[n++] = g_pg_store.grants[i];
    }
    pthread_mutex_unlock(&g_pg_store.lock);
    return n;
}

/* ── PLAN ───────────────────────────────────────────────────────────────── */

static struct property_grant_plan *find_plan(const char *plan_id)
{
    if (!plan_id || plan_id[0] == '\0') return NULL;
    for (size_t i = 0; i < PROPERTY_GRANT_MAX_PLANS; i++) {
        if (!g_pg_store.plan_used[i]) continue;
        if (strcmp(g_pg_store.plans[i].plan_id, plan_id) == 0)
            return &g_pg_store.plans[i];
    }
    return NULL;
}

/* Free an expired, uncommitted slot. Committed plans are kept as the audit
 * link between a plan id and its receipt, and are only dropped on reset. */
static struct property_grant_plan *claim_plan_slot(int64_t now)
{
    for (size_t i = 0; i < PROPERTY_GRANT_MAX_PLANS; i++) {
        if (!g_pg_store.plan_used[i]) return &g_pg_store.plans[i];
    }
    for (size_t i = 0; i < PROPERTY_GRANT_MAX_PLANS; i++) {
        struct property_grant_plan *p = &g_pg_store.plans[i];
        if (!p->committed && now >= p->expires_unix) return p;
    }
    return NULL;
}

enum property_grant_reason property_grant_service_plan(
    const char *grant_id, const struct metaverse_action_request *req,
    struct property_grant_plan *out)
{
    if (!grant_id || !req || !out) {
        LOG_ERROR(PG_LOG, "plan: NULL grant id, request, or output");
        return PROPERTY_GRANT_BAD_ARGS;
    }

    /* The caller does not get to choose the clock an expiry is measured
     * against, so whatever it put in req->now_unix/height is discarded. */
    struct metaverse_action_request r = *req;
    pg_now(&r.now_unix, &r.height);

    pthread_mutex_lock(&g_pg_store.lock);
    const struct metaverse_grant *g = pg_find_grant(grant_id);
    if (!g) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_WARN(PG_LOG, "plan: no grant %s", grant_id);
        return PROPERTY_GRANT_GRANT_UNKNOWN;
    }

    const struct metaverse_grant *anc[METAVERSE_GRANT_MAX_DEPTH];
    size_t anc_count = 0;
    const struct metaverse_grant *const *anc_arg = NULL;
    if (g->lineage_count > 0) {
        if (!pg_collect_ancestors(g, anc, &anc_count)) {
            pthread_mutex_unlock(&g_pg_store.lock);
            LOG_WARN(PG_LOG, "plan: grant %s has a missing ancestor",
                         grant_id);
            return PROPERTY_GRANT_ANCESTOR_REVOKED;
        }
        anc_arg = anc;
    }

    /* FAIL FAST. Action not in the set, property out of scope, counterparty not
     * allowed, budget exceeded, revoked, expired — all decided here, before any
     * plan artifact exists. */
    enum metaverse_grant_verdict v =
        metaverse_grant_check(g, anc_arg, anc_count, &r);
    if (v != METAVERSE_GRANT_OK) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_WARN(PG_LOG, "plan: grant %s refused %s on %s (%s)", grant_id,
                     metaverse_action_token(r.action),
                     metaverse_kind_name(r.property.kind),
                     metaverse_grant_verdict_token(v));
        return property_grant_reason_from_verdict(v);
    }

    /* Read the catalog if one is installed. A missing catalog is recorded, not
     * fatal: PLAN is a read-only quote, and COMMIT is where it becomes fatal. */
    struct property_grant_plan p;
    memset(&p, 0, sizeof(p));
    p.catalog_seen = false;
    p.property_revision = -1;
    if (g_pg_store.env.catalog_lookup) {
        struct metaverse_catalog_view view;
        memset(&view, 0, sizeof(view));
        if (g_pg_store.env.catalog_lookup(&r.property, &view,
                                          g_pg_store.env.catalog_ctx)) {
            p.catalog_seen = true;
            p.property_revision = view.revision;
            snprintf(p.controller_at_plan, sizeof(p.controller_at_plan), "%s",
                     view.controller);
        }
    }

    struct property_grant_plan *slot = claim_plan_slot(r.now_unix);
    if (!slot) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_ERROR(PG_LOG, "plan: no free plan slot (cap %d)",
                     PROPERTY_GRANT_MAX_PLANS);
        return PROPERTY_GRANT_STORE_FULL;
    }

    if (!draw_id(p.plan_id, sizeof(p.plan_id))) {
        pthread_mutex_unlock(&g_pg_store.lock);
        LOG_ERROR(PG_LOG, "plan: could not draw a plan id");
        return PROPERTY_GRANT_BAD_ARGS;
    }
    snprintf(p.grant_id, sizeof(p.grant_id), "%s", g->grant_id);
    p.request = r;
    p.created_unix = r.now_unix;
    p.expires_unix = r.now_unix + PROPERTY_GRANT_PLAN_TTL_SECONDS;
    p.committed = false;
    p.receipt_seq = 0;

    *slot = p;
    size_t idx = (size_t)(slot - g_pg_store.plans);
    g_pg_store.plan_used[idx] = true;
    pthread_mutex_unlock(&g_pg_store.lock);

    *out = p;
    return PROPERTY_GRANT_OK;
}

enum property_grant_reason property_grant_service_plan_get(
    const char *plan_id, struct property_grant_plan *out)
{
    if (!plan_id || !out) return PROPERTY_GRANT_BAD_ARGS;
    pthread_mutex_lock(&g_pg_store.lock);
    const struct property_grant_plan *p = find_plan(plan_id);
    if (p) *out = *p;
    pthread_mutex_unlock(&g_pg_store.lock);
    return p ? PROPERTY_GRANT_OK : PROPERTY_GRANT_PLAN_UNKNOWN;
}

struct property_grant_plan *pg_find_plan_locked(const char *plan_id)
{
    return find_plan(plan_id);
}
