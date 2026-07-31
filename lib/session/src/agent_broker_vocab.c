/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The wire <-> canonical translation. See session/agent_broker_vocab.h for the
 * contract. Everything below is a read of ONE table plus the compile-time
 * assertions that keep that table honest; there is no switch here that decides
 * anything about an action, because deciding is the metaverse's job.
 */

#include "session/agent_broker_vocab.h"

#include "base/log_macros.h"

#include <stdio.h>
#include <string.h>

#define VOCAB_TAG "agent.vocab"

/* ── the joined rows ────────────────────────────────────────────────────── */

static const struct mvap_verb_row k_rows[] = {
#define MVAP_VERB_ROW(id_, class_, action_, zero_ok_)                        \
    { .wire = MVAP_VERB_##id_,                                               \
      .verb_class = MVAP_VERB_CLASS_##class_,                                \
      .action = METAVERSE_ACTION_##action_,                                  \
      .allows_zero_property_id = (zero_ok_) },
    MVAP_VERB_CANON_TABLE(MVAP_VERB_ROW)
#undef MVAP_VERB_ROW
};

/* The two tables must describe the same verbs. Counting rows is enough to
 * catch the failure that matters — a verb appended to one table and forgotten
 * in the other — because the join table names each row by the wire id, so a
 * misspelled id does not compile at all. */
enum {
#define MVAP_VERB_COUNT_WIRE(id_, value_, since_) +1
    MVAP_VERB_TABLE_ROWS = 0 MVAP_VERB_TABLE(MVAP_VERB_COUNT_WIRE),
#undef MVAP_VERB_COUNT_WIRE
#define MVAP_VERB_COUNT_CANON(id_, class_, action_, zero_ok_) +1
    MVAP_VERB_CANON_TABLE_ROWS = 0 MVAP_VERB_CANON_TABLE(MVAP_VERB_COUNT_CANON),
#undef MVAP_VERB_COUNT_CANON
};

_Static_assert(MVAP_VERB_TABLE_ROWS == MVAP_VERB_CANON_TABLE_ROWS,
               "every wire verb needs exactly one canonical join row: append "
               "to MVAP_VERB_TABLE and MVAP_VERB_CANON_TABLE together");
_Static_assert(MVAP_VERB_TABLE_ROWS == (int)MVAP_VERB__COUNT - 1,
               "MVAP_VERB__COUNT must be one past the highest wire value");

/* The wire kind enum and METAVERSE_KIND_TABLE are numerically identical row
 * for row. Asserted here rather than assumed: a kind appended to one and not
 * the other must fail the build, because a silent mismatch would hand the
 * grant evaluator a property of the wrong kind. */
_Static_assert((int)MVAP_KIND_ANY      == (int)METAVERSE_KIND_UNKNOWN, "kind 0");
_Static_assert((int)MVAP_KIND_CONTENT  == (int)METAVERSE_KIND_CONTENT, "kind 1");
_Static_assert((int)MVAP_KIND_ZCODE    == (int)METAVERSE_KIND_ZCODE_PACKAGE, "kind 2");
_Static_assert((int)MVAP_KIND_NAME     == (int)METAVERSE_KIND_ZNAM_NAME, "kind 3");
_Static_assert((int)MVAP_KIND_ASSET    == (int)METAVERSE_KIND_ZSLP_ASSET, "kind 4");
_Static_assert((int)MVAP_KIND_SERVICE  == (int)METAVERSE_KIND_HOSTED_SERVICE, "kind 5");
_Static_assert((int)MVAP_KIND_ENDPOINT == (int)METAVERSE_KIND_ENDPOINT_ONION, "kind 6");
_Static_assert((int)MVAP_KIND_PRODUCT  == (int)METAVERSE_KIND_STOREFRONT_PRODUCT, "kind 7");
_Static_assert((int)MVAP_KIND_CONTRACT == (int)METAVERSE_KIND_CONTRACT_SWAP, "kind 8");
_Static_assert((int)MVAP_KIND__COUNT   == (int)METAVERSE_KIND_COUNT,
               "the wire kind vocabulary and METAVERSE_KIND_TABLE must have "
               "the same number of kinds");

/* ── row lookup ─────────────────────────────────────────────────────────── */

const struct mvap_verb_row *mvap_verb_row(uint32_t wire)
{
    for (size_t i = 0; i < sizeof(k_rows) / sizeof(k_rows[0]); i++)
        if (k_rows[i].wire == wire)
            return &k_rows[i];
    return NULL;
}

bool mvap_verb_is_query(uint32_t wire)
{
    const struct mvap_verb_row *r = mvap_verb_row(wire);
    return r && r->verb_class == MVAP_VERB_CLASS_QUERY;
}

bool mvap_verb_is_action(uint32_t wire)
{
    const struct mvap_verb_row *r = mvap_verb_row(wire);
    return r && r->verb_class == MVAP_VERB_CLASS_ACTION;
}

/* An ACTION runs PLAN -> COMMIT and is recorded; a QUERY does neither. There
 * is no third rule here that could disagree with the class column. */
bool mvap_verb_mints_receipt(uint32_t wire)
{
    return mvap_verb_is_action(wire);
}

/* Declared in agent_broker_proto.h, where callers look for verb facts; defined
 * HERE because the answer is the class column and nothing else. The switch that
 * used to answer this next to the codec — an independent list of mutating verbs
 * that could and did disagree with the metaverse — is gone. */
bool mvap_verb_is_mutation(uint32_t wire)
{
    return mvap_verb_is_action(wire);
}

bool mvap_verb_to_action(uint32_t wire, enum metaverse_action *out)
{
    const struct mvap_verb_row *r = mvap_verb_row(wire);
    if (!r || !out)
        return false;
    *out = r->action;
    return true;
}

uint32_t mvap_verb_from_action(enum metaverse_action action)
{
    for (size_t i = 0; i < sizeof(k_rows) / sizeof(k_rows[0]); i++)
        if (k_rows[i].verb_class == MVAP_VERB_CLASS_ACTION &&
            k_rows[i].action == action)
            return k_rows[i].wire;
    return MVAP_VERB_NONE;
}

/* ── kinds ──────────────────────────────────────────────────────────────── */

enum metaverse_kind mvap_kind_to_metaverse(uint16_t wire_kind)
{
    if (wire_kind >= MVAP_KIND__COUNT)
        return METAVERSE_KIND_UNKNOWN;
    return (enum metaverse_kind)wire_kind;
}

uint16_t mvap_kind_from_metaverse(enum metaverse_kind kind)
{
    if (!metaverse_kind_valid(kind))
        return MVAP_KIND_ANY;
    return (uint16_t)kind;
}

/* ── verdicts ───────────────────────────────────────────────────────────── */

int32_t mvap_status_from_grant_verdict(enum metaverse_grant_verdict verdict)
{
    switch (verdict) {
    case METAVERSE_GRANT_OK:                        return MVAP_OK;
    case METAVERSE_GRANT_BAD_ARGS:                  return MVAP_ERR_BAD_REQUEST;
    case METAVERSE_GRANT_MALFORMED:                 return MVAP_ERR_INTERNAL;
    case METAVERSE_GRANT_REVOKED:                   return MVAP_ERR_DENIED_REVOKED;
    case METAVERSE_GRANT_ANCESTOR_REVOKED:          return MVAP_ERR_DENIED_REVOKED;
    case METAVERSE_GRANT_EXPIRED_HEIGHT:            return MVAP_ERR_DENIED_EXPIRED;
    case METAVERSE_GRANT_EXPIRED_TIME:              return MVAP_ERR_DENIED_EXPIRED;
    case METAVERSE_GRANT_WRONG_HOLDER:              return MVAP_ERR_DENIED_NO_GRANT;
    case METAVERSE_GRANT_ACTION_NOT_GRANTED:        return MVAP_ERR_DENIED_ACTION;
    case METAVERSE_GRANT_PROPERTY_OUT_OF_SCOPE:     return MVAP_ERR_DENIED_PROPERTY;
    case METAVERSE_GRANT_KIND_OUT_OF_SCOPE:         return MVAP_ERR_DENIED_KIND;
    case METAVERSE_GRANT_COUNTERPARTY_NOT_ALLOWED:  return MVAP_ERR_DENIED_COUNTERPARTY;
    case METAVERSE_GRANT_VALUE_NEGATIVE:            return MVAP_ERR_BAD_REQUEST;
    case METAVERSE_GRANT_VALUE_ON_FREE_ACTION:      return MVAP_ERR_BAD_REQUEST;
    case METAVERSE_GRANT_BUDGET_EXCEEDED:           return MVAP_ERR_DENIED_BUDGET;
    case METAVERSE_GRANT_RATE_LIMITED:              return MVAP_ERR_DENIED_RATE;
    case METAVERSE_GRANT_DELEGATION_NOT_PERMITTED:  return MVAP_ERR_DENIED_DELEGATION;
    case METAVERSE_GRANT_DELEGATION_DEPTH_EXCEEDED: return MVAP_ERR_DENIED_DELEGATION;
    case METAVERSE_GRANT_VERDICT_COUNT:             break;
    }
    return MVAP_ERR_INTERNAL;
}

/* ── request translation ────────────────────────────────────────────────── */

/* The stand-in root for a request that names no specific property. A canonical
 * property id must be non-zero (metaverse/property_id.h: an all-zero root
 * would make "uninitialized" indistinguishable from "a real object"), and the
 * wire's all-zero id is a legitimate "catalog-wide" request. Domain-separated
 * so it cannot collide with any real content root. Which verbs may use it is
 * the `allows_zero_property_id` column, checked by the broker; this constant
 * only lets the canonical evaluator rule on everything that is not scope. */
static const uint8_t k_catalog_wide_root[METAVERSE_ROOT_BYTES] = {
    'z', 'c', 'l', '.', 'a', 'g', 'e', 'n', 't', '.', 'c', 'a', 't', 'a', 'l',
    'o', 'g', '_', 'w', 'i', 'd', 'e', '.', 'n', 'o', '_', 'p', 'r', 'o', 'p',
    '.', '1',
};

/* The kind a canonical id carries when the wire declared none. The broker
 * checks the request's DECLARED kind against the grant's kind mask itself, and
 * projects a wildcard kind scope, so this value never decides anything — it
 * exists because a canonical id has no "unspecified" kind. */
#define MVAP_PROJECTED_KIND_WHEN_UNSPECIFIED METAVERSE_KIND_CONTENT

bool mvap_request_to_query_request(const struct mvap_request *req,
                                   const char *actor, int64_t now_unix,
                                   int64_t height,
                                   struct metaverse_query_request *out)
{
    if (!req || !actor || !out)
        LOG_FAIL(VOCAB_TAG, "null argument req=%p actor=%p out=%p",
                 (const void *)req, (const void *)actor, (void *)out);

    const struct mvap_verb_row *row = mvap_verb_row(req->verb);
    if (!row)
        LOG_FAIL(VOCAB_TAG, "verb %u names no row in the canonical join",
                 req->verb);
    if (row->verb_class != MVAP_VERB_CLASS_QUERY)
        LOG_FAIL(VOCAB_TAG, "verb %u is not a query", req->verb);

    /* Which query this wire verb IS comes from the canonical query table, not
     * from a second mapping kept here. */
    enum metaverse_query q = metaverse_query_from_wire(req->verb);
    if (q == METAVERSE_QUERY_NONE)
        LOG_FAIL(VOCAB_TAG, "query verb %u has no canonical query", req->verb);

    memset(out, 0, sizeof(*out));
    snprintf(out->actor, sizeof(out->actor), "%s", actor);
    out->query = q;

    enum metaverse_kind kind = mvap_kind_to_metaverse(req->kind);
    out->property.kind =
        metaverse_kind_valid(kind) ? kind
                                   : MVAP_PROJECTED_KIND_WHEN_UNSPECIFIED;
    if (mvap_property_id_is_zero(req->property_id))
        memcpy(out->property.root, k_catalog_wide_root, METAVERSE_ROOT_BYTES);
    else
        memcpy(out->property.root, req->property_id, METAVERSE_ROOT_BYTES);

    out->now_unix = now_unix;
    out->height   = height;
    return true;
}

bool mvap_request_to_action_request(const struct mvap_request *req,
                                    const char *actor, int64_t now_unix,
                                    int64_t height,
                                    struct metaverse_action_request *out)
{
    if (!req || !actor || !out)
        LOG_FAIL(VOCAB_TAG, "null argument req=%p actor=%p out=%p",
                 (const void *)req, (const void *)actor, (void *)out);

    const struct mvap_verb_row *row = mvap_verb_row(req->verb);
    if (!row)
        LOG_FAIL(VOCAB_TAG, "verb %u names no row in the canonical join",
                 req->verb);
    if (req->value_zats > (uint64_t)INT64_MAX)
        LOG_FAIL(VOCAB_TAG, "value %llu cannot be a canonical zatoshi amount",
                 (unsigned long long)req->value_zats);

    memset(out, 0, sizeof(*out));
    snprintf(out->actor, sizeof(out->actor), "%s", actor);
    out->action = row->action;

    enum metaverse_kind kind = mvap_kind_to_metaverse(req->kind);
    out->property.kind =
        metaverse_kind_valid(kind) ? kind
                                   : MVAP_PROJECTED_KIND_WHEN_UNSPECIFIED;
    if (mvap_property_id_is_zero(req->property_id))
        memcpy(out->property.root, k_catalog_wide_root, METAVERSE_ROOT_BYTES);
    else
        memcpy(out->property.root, req->property_id, METAVERSE_ROOT_BYTES);

    /* Whether `param` IS a counterparty is a canonical fact about the action,
     * never a judgement this file makes about the verb. */
    if (metaverse_action_uses_counterparty(row->action))
        snprintf(out->counterparty, sizeof(out->counterparty), "%s",
                 req->param);

    out->value_zat = (int64_t)req->value_zats;
    out->now_unix  = now_unix;
    out->height    = height;
    return true;
}

/* The per-action "names a counterparty" fact is a column of the canonical
 * action row and is answered by metaverse_action_uses_counterparty(). The
 * broker states no second opinion about it. */
