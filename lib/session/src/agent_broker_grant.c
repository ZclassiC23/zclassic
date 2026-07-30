/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The grant evaluator. Every refusal is a NAMED status, never a bare "denied":
 * an operator reading an audit row must be able to tell "this agent asked for a
 * property outside its scope" apart from "this agent ran out of budget".
 *
 * agent_grant_authorize() is PURE — it debits nothing and mutates nothing — so
 * the broker can call it twice (once before PLAN on the agent's claimed kind,
 * once after PLAN on the catalog's authoritative kind) and get the same answer
 * for the same inputs. Only agent_grant_commit_debit() moves the counters, and
 * it runs only after a COMMIT succeeded.
 */

#include "session/agent_broker.h"

#include "base/log_macros.h"
#include "crypto/sha3.h"

#include <string.h>

#define GRANT_TAG "agent.grant"

void agent_grant_allow_action(struct agent_grant *g, uint32_t verb)
{
    if (!g || verb == MVAP_VERB_NONE || verb >= MVAP_VERB__COUNT)
        return;
    g->actions_mask |= (uint32_t)1u << verb;
}

void agent_grant_allow_kind(struct agent_grant *g, uint16_t kind)
{
    if (!g || kind >= MVAP_KIND__COUNT)
        return;
    g->kinds_mask |= (uint32_t)1u << kind;
}

bool agent_grant_add_property(struct agent_grant *g,
                              const uint8_t id[MVAP_PROPERTY_ID_LEN])
{
    if (!g || !id)
        LOG_FAIL(GRANT_TAG, "null argument g=%p id=%p", (void *)g,
                 (const void *)id);
    if (g->n_properties >= AGENT_GRANT_MAX_PROPS)
        LOG_FAIL(GRANT_TAG, "grant %s already holds the maximum %d properties",
                 g->grant_id, AGENT_GRANT_MAX_PROPS);
    memcpy(g->properties[g->n_properties], id, MVAP_PROPERTY_ID_LEN);
    g->n_properties++;
    return true;
}

/* Canonical scope digest. Deliberately EXCLUDES the mutable counters
 * (spent_zats, window_used, window_start_ms) so the fingerprint identifies the
 * grant's AUTHORITY, not its usage — an operator comparing two status dumps
 * wants "same grant" to survive normal spending. */
void agent_grant_fingerprint(const struct agent_grant *g, uint8_t out[32])
{
    if (!out)
        return;
    memset(out, 0, 32);
    if (!g)
        return;

    struct sha3_256_ctx c;
    sha3_256_init(&c);
    sha3_256_write(&c, (const unsigned char *)"zcl.agent_grant.v1", 18);
    sha3_256_write(&c, (const unsigned char *)g->grant_id,
                   strnlen(g->grant_id, AGENT_GRANT_ID_MAX));
    sha3_256_write(&c, (const unsigned char *)g->principal,
                   strnlen(g->principal, AGENT_PRINCIPAL_MAX));
    for (size_t i = 0; i < g->n_properties; i++)
        sha3_256_write(&c, g->properties[i], MVAP_PROPERTY_ID_LEN);

    uint8_t nums[8][8];
    uint64_t vals[8] = {
        (uint64_t)g->kinds_mask, (uint64_t)g->actions_mask,
        g->max_value_zats, g->budget_zats,
        (uint64_t)g->expires_unix_ms, (uint64_t)g->rate_limit,
        (uint64_t)g->window_seconds, g->revocation_generation,
    };
    for (size_t i = 0; i < 8; i++)
        for (size_t b = 0; b < 8; b++)
            nums[i][b] = (uint8_t)((vals[i] >> (8 * b)) & 0xFFu);
    sha3_256_write(&c, (const unsigned char *)nums, sizeof(nums));

    sha3_256_write(&c, (const unsigned char *)g->counterparty_allowlist,
                   strnlen(g->counterparty_allowlist, AGENT_ALLOWLIST_MAX));
    unsigned char flags[3] = { (unsigned char)g->may_delegate,
                               (unsigned char)g->max_delegation_depth,
                               (unsigned char)g->revoked };
    sha3_256_write(&c, flags, sizeof(flags));
    sha3_256_finalize(&c, out);
}

/* True iff `tok` appears as a whole space-separated entry of `list`. */
static bool allowlist_contains(const char *list, const char *tok)
{
    if (!list || !list[0])
        return true;                     /* empty list == any counterparty */
    if (!tok || !tok[0])
        return false;
    size_t tlen = strnlen(tok, MVAP_PARAM_MAX);
    const char *p = list;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        size_t elen = (size_t)(p - start);
        if (elen == tlen && memcmp(start, tok, tlen) == 0)
            return true;
    }
    return false;
}

/* Which verbs name a counterparty in `param`. */
static bool verb_has_counterparty(uint32_t verb)
{
    switch (verb) {
    case MVAP_VERB_SELL:
    case MVAP_VERB_BUY:
    case MVAP_VERB_DELIVER:
    case MVAP_VERB_LEASE:
    case MVAP_VERB_TRANSFER:
    case MVAP_VERB_ACCEPT_PAYMENT:
    case MVAP_VERB_DELEGATE:
        return true;
    default:
        return false;
    }
}

/* Which verbs may carry a non-zero value. */
static bool verb_carries_value(uint32_t verb)
{
    switch (verb) {
    case MVAP_VERB_SELL:
    case MVAP_VERB_BUY:
    case MVAP_VERB_LEASE:
    case MVAP_VERB_ACCEPT_PAYMENT:
        return true;
    default:
        return false;
    }
}

int32_t agent_grant_authorize(const struct agent_grant *g,
                              const struct mvap_request *req, int64_t now_ms)
{
    if (!g || !req)
        return MVAP_ERR_INTERNAL;
    if (!g->grant_id[0])
        return MVAP_ERR_DENIED_NO_GRANT;
    if (g->revoked)
        return MVAP_ERR_DENIED_REVOKED;
    if (g->expires_unix_ms != 0 && now_ms >= g->expires_unix_ms)
        return MVAP_ERR_DENIED_EXPIRED;

    if (req->verb == MVAP_VERB_NONE || req->verb >= MVAP_VERB__COUNT)
        return MVAP_ERR_UNKNOWN_VERB;
    if ((g->actions_mask & ((uint32_t)1u << req->verb)) == 0)
        return MVAP_ERR_DENIED_ACTION;

    /* Property scope. A zero id is only meaningful for the two verbs that do
     * not name one; every other verb must land inside the grant's scope. */
    bool zero_id = mvap_property_id_is_zero(req->property_id);
    if (zero_id) {
        if (req->verb != MVAP_VERB_LIST && req->verb != MVAP_VERB_REVOKE)
            return MVAP_ERR_DENIED_PROPERTY;
    } else if (g->n_properties > 0) {
        bool hit = false;
        for (size_t i = 0; i < g->n_properties && !hit; i++)
            hit = memcmp(g->properties[i], req->property_id,
                         MVAP_PROPERTY_ID_LEN) == 0;
        if (!hit)
            return MVAP_ERR_DENIED_PROPERTY;
    }

    /* Kind scope. Checked whenever the request names a kind; the broker calls
     * this a second time with the catalog's authoritative kind so a request
     * that understated its kind is caught before COMMIT. */
    if (req->kind != MVAP_KIND_ANY &&
        (g->kinds_mask & ((uint32_t)1u << req->kind)) == 0)
        return MVAP_ERR_DENIED_KIND;

    if (req->value_zats > 0) {
        if (!verb_carries_value(req->verb))
            return MVAP_ERR_BAD_REQUEST;
        if (req->value_zats > g->max_value_zats)
            return MVAP_ERR_DENIED_VALUE;
        if (g->spent_zats > g->budget_zats ||
            req->value_zats > g->budget_zats - g->spent_zats)
            return MVAP_ERR_DENIED_BUDGET;
    }

    if (verb_has_counterparty(req->verb) &&
        !allowlist_contains(g->counterparty_allowlist, req->param))
        return MVAP_ERR_DENIED_COUNTERPARTY;

    if (req->verb == MVAP_VERB_DELEGATE &&
        (!g->may_delegate || g->max_delegation_depth == 0))
        return MVAP_ERR_DENIED_DELEGATION;

    /* Rate limit over a rolling window. A window whose start is older than the
     * width is treated as fresh, so the check is stateless-safe to repeat. */
    if (g->rate_limit > 0 && g->window_seconds > 0 &&
        mvap_verb_is_mutation(req->verb)) {
        int64_t width_ms = (int64_t)g->window_seconds * 1000;
        bool fresh = (now_ms - g->window_start_ms) >= width_ms;
        uint32_t used = fresh ? 0u : g->window_used;
        if (used >= g->rate_limit)
            return MVAP_ERR_DENIED_RATE;
    }

    return MVAP_OK;
}

void agent_grant_commit_debit(struct agent_grant *g,
                              const struct mvap_request *req, int64_t now_ms)
{
    if (!g || !req)
        return;
    if (req->value_zats > 0) {
        /* authorize() already proved this cannot overflow the budget. */
        g->spent_zats += req->value_zats;
    }
    if (g->rate_limit > 0 && g->window_seconds > 0 &&
        mvap_verb_is_mutation(req->verb)) {
        int64_t width_ms = (int64_t)g->window_seconds * 1000;
        if ((now_ms - g->window_start_ms) >= width_ms) {
            g->window_start_ms = now_ms;
            g->window_used = 0;
        }
        g->window_used++;
    }
}
