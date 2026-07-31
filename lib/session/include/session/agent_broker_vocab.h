/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * agent_broker_vocab — the ONE join between the broker's WIRE vocabulary
 * (session/agent_broker_proto.h) and the CANONICAL metaverse vocabulary
 * (metaverse/property_grant.h).
 *
 * WHY THIS FILE EXISTS. The broker used to answer six questions on its own:
 * which verbs mutate, which carry value, which name a counterparty, which
 * kinds exist, what a verb's bit means, and which operations mint a receipt.
 * Answering them locally made the broker a SECOND authority over the same
 * facts the metaverse already owns, and the two answers had already drifted —
 * a TRANSFER debited the operator's cumulative budget on one side and was free
 * on the other. None of those questions is answered here either. This file
 * only TRANSLATES: wire value -> canonical action, wire kind -> canonical
 * kind, wire request -> struct metaverse_action_request. Every predicate over
 * the canonical action is then asked of the metaverse, and the verdict is
 * mapped back onto the wire's named refusals.
 *
 * THE TWO DISJOINT VOCABULARIES. A QUERY is read-only: it never mutates,
 * never carries value, never names a counterparty, never runs PLAN -> COMMIT
 * and never mints a receipt. An ACTION is one of the canonical mutating verbs
 * and does all of the above. `LIST` is the one identifier that used to mean
 * both — "enumerate what exists" on the wire and "list for sale" in the
 * metaverse — so it is never again a single identifier: wire value 2 is the
 * ENUMERATE query, and listing something for sale is the appended wire value
 * 14 carrying the canonical LIST action bit.
 *
 * QUERIES AND THE RESERVED INSPECT BIT. Queries left the canonical action
 * space, so bit 0x1 (canonical METAVERSE_ACTION_INSPECT) is RESERVED: it is
 * still decoded for compatibility and is never reissued to a new action. It is
 * what a projected grant carries when the broker asks the canonical evaluator
 * to rule on a query's liveness (revocation, expiry, holder), so a query is
 * judged by the same evaluator as everything else instead of by a second
 * private path.
 */

#ifndef ZCL_SESSION_AGENT_BROKER_VOCAB_H
#define ZCL_SESSION_AGENT_BROKER_VOCAB_H

#include "metaverse/property_grant.h"
#include "metaverse/property_id.h"
#include "session/agent_broker_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── the canonical join table ───────────────────────────────────────────────
 * One row per wire verb: (wire id, class, canonical action, whether the verb
 * accepts the all-zero "no specific property" id).
 *
 * The CLASS column is the query/action split. The ACTION column names the
 * canonical action the verb translates to; for a QUERY row it is the RESERVED
 * INSPECT bit (see the header note), never a fresh action.
 *
 * MVAP_VERB_TABLE in agent_broker_proto.h owns the wire VALUES and which
 * protocol version first carried them. This table owns nothing but the join,
 * and a _Static_assert in the .c requires the two to have the same rows. */
#define MVAP_VERB_CANON_TABLE(X)                                              \
    /*  wire id            class   canonical action   zero id ok */           \
    X(INSPECT,             QUERY,  INSPECT,           false)                  \
    X(LIST,                QUERY,  INSPECT,           true)                   \
    X(HOST,                ACTION, HOST,              false)                  \
    X(PUBLISH_REVISION,    ACTION, PUBLISH_REVISION,  false)                  \
    X(UPDATE_POINTER,      ACTION, UPDATE_POINTER,    false)                  \
    X(SELL,                ACTION, SELL,              false)                  \
    X(BUY,                 ACTION, BUY,               false)                  \
    X(DELIVER,             ACTION, DELIVER,           false)                  \
    X(LEASE,               ACTION, LEASE,             false)                  \
    X(TRANSFER,            ACTION, TRANSFER,          false)                  \
    X(ACCEPT_PAYMENT,      ACTION, ACCEPT_PAYMENT,    false)                  \
    X(DELEGATE,            ACTION, DELEGATE,          false)                  \
    X(REVOKE,              ACTION, REVOKE,            true)                   \
    X(LIST_FOR_SALE,       ACTION, LIST,              false)

enum mvap_verb_class {
    MVAP_VERB_CLASS_QUERY = 0,
    MVAP_VERB_CLASS_ACTION,
};

struct mvap_verb_row {
    uint32_t              wire;
    enum mvap_verb_class  verb_class;
    enum metaverse_action action;
    bool                  allows_zero_property_id;
};

/* The row for a wire verb, or NULL when the value names no verb. NULL is the
 * ONLY "unknown verb" answer — there is no fallthrough that treats an
 * unrecognized value as harmless. */
const struct mvap_verb_row *mvap_verb_row(uint32_t wire);

/* Class predicates. `mints_receipt` is not a third opinion: an ACTION runs
 * PLAN -> COMMIT and is recorded, a QUERY does neither, and that IS the
 * split. */
bool mvap_verb_is_query(uint32_t wire);
bool mvap_verb_is_action(uint32_t wire);
bool mvap_verb_mints_receipt(uint32_t wire);

/* Wire verb <-> canonical action. Both directions are the same table read two
 * ways; a query maps to the RESERVED INSPECT bit and
 * mvap_verb_from_action(METAVERSE_ACTION_INSPECT) is therefore
 * MVAP_VERB_NONE — the reserved bit names no action to encode. */
bool mvap_verb_to_action(uint32_t wire, enum metaverse_action *out);
uint32_t mvap_verb_from_action(enum metaverse_action action);

/* Wire kind <-> canonical kind. The two enums are numerically identical row
 * for row (asserted per row at compile time in the .c), so this is the
 * identity — stated as a function anyway so no caller is tempted to cast. */
enum metaverse_kind mvap_kind_to_metaverse(uint16_t wire_kind);
uint16_t mvap_kind_from_metaverse(enum metaverse_kind kind);

/* The canonical evaluator's verdict, rendered as the wire's named refusal.
 * Total: every verdict maps to exactly one status, and METAVERSE_GRANT_OK maps
 * to MVAP_OK. */
int32_t mvap_status_from_grant_verdict(enum metaverse_grant_verdict verdict);

/* Translate one decoded wire frame into the canonical request the metaverse
 * evaluator understands.
 *
 * `actor` is the grant holder the broker is acting for — it comes from the
 * broker's own grant, never from the wire, because the wire cannot name a
 * principal and must not be able to.
 *
 * Two shapes the wire has and the canonical type does not:
 *   - a request may carry no kind (MVAP_KIND_ANY). Canonical property ids are
 *     always (kind, root), so the projection substitutes a placeholder kind;
 *     the broker checks the request's declared kind against the grant's kind
 *     mask itself, because that check is an intersection the canonical
 *     either-ids-or-kinds scope form cannot express.
 *   - a request may carry the all-zero "no specific property" id, which is not
 *     a valid canonical id. The projection substitutes a fixed, domain-
 *     separated catalog-wide root so the canonical evaluator can still rule on
 *     everything that is not scope. Whether the verb may use the zero id at
 *     all is the `allows_zero_property_id` column.
 *
 * Returns false only on a NULL argument, an unknown verb, or a value that
 * cannot be represented as the canonical signed zatoshi amount. */
bool mvap_request_to_action_request(const struct mvap_request *req,
                                    const char *actor, int64_t now_unix,
                                    int64_t height,
                                    struct metaverse_action_request *out);

/* ═════════════════════════════════════════════════════════════════════════
 *  PENDING LANE A — the one canonical column that does not exist yet.
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Contract §3 declares "uses counterparty" as a column of the canonical action
 * row. lib/metaverse does not publish it yet: metaverse_grant_check() applies
 * the grant's counterparty allowlist to EVERY action, so on its own it refuses
 * a HOST with no counterparty under a grant that names one, which is not what
 * either side means.
 *
 * This declaration is the seam, NOT the design. When Lane A lands the
 * canonical row it defines METAVERSE_ACTION_ROW_CANONICAL and supplies this
 * function; the fallback in agent_broker_vocab.c then compiles out and the
 * broker keeps calling the same name. Nothing else in this lane encodes a
 * per-action counterparty opinion. */
#ifndef METAVERSE_ACTION_ROW_CANONICAL
bool metaverse_action_uses_counterparty(enum metaverse_action action);
#endif

#endif /* ZCL_SESSION_AGENT_BROKER_VOCAB_H */
