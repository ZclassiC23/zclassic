/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Property grants — the RULES half of PRINCIPAL → PROPERTY → RIGHTS → ACTION
 * → RECEIPT. Pure decision functions over an explicit grant record and an
 * explicit request. No clock, no filesystem, no database, no allocation:
 * every fact a decision needs (`now`, `height`, the property's current owner
 * and revision) is passed in by the caller, so the same evaluation can be run
 * at PLAN time and re-run at COMMIT time against different facts and give
 * different, defensible answers.
 *
 * The state machine that owns the store, the clock, and the receipt chain is
 * services/property_grant_service.h. This file decides; it never records.
 *
 * ── WHY THE BUDGET IS CUMULATIVE ──────────────────────────────────────────
 * `max_value_zat` bounds the TOTAL value the grant may ever move, not the
 * value of one action. A per-action ceiling is not a budget: an agent holding
 * a 1-ZCL-per-action grant can move 1000 ZCL in 1000 actions and never
 * violate it, so the operator's exposure is unbounded and the number they
 * wrote down was decorative. A cumulative ceiling is the only form where the
 * number the operator authorized is the number at risk. Per-action and
 * per-window ZATOSHI caps already exist one layer down
 * (services/agent_spend_policy.h); this layer supplies the ceiling that layer
 * cannot express. The rate limit here bounds action COUNT per window, which is
 * a liveness/abuse bound, not a value bound — the two are independent and both
 * are enforced.
 *
 * ── REVOCATION IS A GENERATION, NOT A FLAG ────────────────────────────────
 * Every grant carries the generation of each of its ANCESTORS as observed at
 * mint time (`lineage[]`), plus its own current `revocation_generation`.
 * Revoking a grant bumps that grant's generation. A delegated sub-grant is
 * therefore invalidated by the mere fact that its recorded ancestor generation
 * no longer matches — nothing has to walk down and mark children, and a child
 * that was never written to disk (or was written by a compromised path) cannot
 * survive its parent's revocation. This is why revocation is instant and total
 * across a delegation subtree. */

#ifndef ZCL_METAVERSE_PROPERTY_GRANT_H
#define ZCL_METAVERSE_PROPERTY_GRANT_H

#include "metaverse/property_id.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 128-bit id rendered as 32 lowercase hex chars — same shape and the same
 * bearer-token hygiene as an agent session id (services/agent_session_*). */
#define METAVERSE_GRANT_ID_LEN 32

/* A principal is a wallet address, a ZID, or an agent name. Sized for the
 * longest of those (a shielded address) with room to spare. */
#define METAVERSE_PRINCIPAL_MAX 128

/* Capacities. Fixed, so a grant is a plain value type that can be copied,
 * hashed, and compared without owning heap. Exceeding one is a named refusal,
 * never a silent truncation. */
#define METAVERSE_GRANT_IDS_MAX 8
#define METAVERSE_GRANT_COUNTERPARTIES_MAX 8

/* Delegation depth ceiling for the whole system. A root grant is depth 0, so
 * this bounds the lineage array and therefore the cost of a full ancestor
 * generation check. Four levels is already more indirection than any operator
 * can reason about; the bound exists so the check is O(1). */
#define METAVERSE_GRANT_MAX_DEPTH 4

/* ── Actions ─────────────────────────────────────────────────────────────── */

/* Ordered; the value IS the bit position, so never reorder — a grant's action
 * set is a persisted bitmask and reordering would silently re-map rights. */
enum metaverse_action {
    METAVERSE_ACTION_INSPECT = 0,
    METAVERSE_ACTION_HOST,
    METAVERSE_ACTION_PUBLISH_REVISION,
    METAVERSE_ACTION_UPDATE_POINTER,
    METAVERSE_ACTION_LIST,
    METAVERSE_ACTION_BUY,
    METAVERSE_ACTION_SELL,
    METAVERSE_ACTION_DELIVER,
    METAVERSE_ACTION_LEASE,
    METAVERSE_ACTION_TRANSFER,
    METAVERSE_ACTION_ACCEPT_PAYMENT,
    METAVERSE_ACTION_DELEGATE,
    METAVERSE_ACTION_REVOKE,
    METAVERSE_ACTION_COUNT
};

typedef uint32_t metaverse_action_set;

static inline metaverse_action_set metaverse_action_bit(enum metaverse_action a)
{
    if (a < 0 || a >= METAVERSE_ACTION_COUNT) return 0u;
    return (metaverse_action_set)1u << (unsigned)a;
}

/* Stable lowercase-with-hyphens token per action ("inspect", "host",
 * "publish-revision", …). Contract: tests and the CLI assert these exactly.
 * Out-of-range renders "unknown". */
const char *metaverse_action_token(enum metaverse_action a);

/* Parse an action token. Returns false for NULL/""/unrecognized. Accepts both
 * '-' and '_' as the word separator so `publish_revision` from a JSON key and
 * `publish-revision` from a CLI flag name the same right. */
bool metaverse_action_parse(const char *token, enum metaverse_action *out);

/* Parse a comma-separated action list ("inspect,host,list") into a set.
 * An empty/NULL list yields the empty set and returns true — a grant with no
 * actions is well-formed and useless, which is the correct default. Any
 * unrecognized element fails the whole parse (never a partial set: a
 * silently-dropped element would WIDEN nothing but a silently-kept typo
 * narrows a grant the operator believed they had written). */
bool metaverse_action_set_parse(const char *csv, metaverse_action_set *out);

/* Render a set as a comma-separated token list in enum order. Writes "" for
 * the empty set. Needs 256 bytes to hold every token. */
bool metaverse_action_set_render(metaverse_action_set set,
                                char *out, size_t out_cap);

/* True when the action MOVES VALUE and therefore must be charged against the
 * grant's cumulative budget: BUY, SELL, LEASE, TRANSFER, ACCEPT_PAYMENT.
 * A non-value action carrying a non-zero value is a malformed request, not a
 * free pass — metaverse_grant_check rejects it (VALUE_ON_FREE_ACTION). */
bool metaverse_action_moves_value(enum metaverse_action a);

/* ── Scope ───────────────────────────────────────────────────────────────── */

/* A grant is scoped EITHER to an exact set of property ids OR to a set of
 * kinds — never both, and never neither. "Both" is the dangerous shape: an
 * operator who lists three ids and also ticks a kind has written a
 * kind-wildcard and will believe they wrote a three-item allowlist. */
enum metaverse_grant_scope_form {
    METAVERSE_SCOPE_IDS = 0,
    METAVERSE_SCOPE_KINDS,
};

/* One ancestor, as observed when this grant was minted. */
struct metaverse_grant_lineage_entry {
    char grant_id[METAVERSE_GRANT_ID_LEN + 1];
    uint32_t revocation_generation;
};

struct metaverse_grant {
    char grant_id[METAVERSE_GRANT_ID_LEN + 1];
    char holder[METAVERSE_PRINCIPAL_MAX + 1];   /* the principal that may act */
    char issuer[METAVERSE_PRINCIPAL_MAX + 1];   /* who minted it */

    /* Delegation position. depth 0 = root grant, lineage_count == depth. */
    uint32_t depth;
    struct metaverse_grant_lineage_entry lineage[METAVERSE_GRANT_MAX_DEPTH];
    size_t lineage_count;

    bool delegation_allowed;
    uint32_t max_delegation_depth;  /* absolute ceiling on a child's depth */

    enum metaverse_grant_scope_form scope_form;
    struct metaverse_property_id ids[METAVERSE_GRANT_IDS_MAX];
    size_t id_count;                /* meaningful only for SCOPE_IDS */
    metaverse_kind_set kinds;       /* meaningful only for SCOPE_KINDS */

    metaverse_action_set actions;

    int64_t max_value_zat;          /* CUMULATIVE ceiling; 0 = no value may move */
    int64_t spent_zat;              /* running total, monotonic */

    char counterparties[METAVERSE_GRANT_COUNTERPARTIES_MAX]
                       [METAVERSE_PRINCIPAL_MAX + 1];
    size_t counterparty_count;      /* 0 = any counterparty (wildcard) */

    uint32_t rate_limit;            /* actions per window; 0 = unlimited */
    int64_t rate_window_seconds;    /* > 0 when rate_limit > 0 */
    int64_t window_start_unix;      /* start of the current window */
    uint32_t window_used;           /* actions committed in the current window */

    /* Expiry. Height-based is preferred (the project's consensus doctrine
     * prefers height invariants: a height cannot be moved by an operator's
     * clock), but a grant over off-chain property has no height to hang on,
     * so both are supported and BOTH are enforced when set. 0 = not set. */
    int64_t expires_height;
    int64_t expires_unix;

    uint32_t revocation_generation; /* bumped by revoke; kills the subtree */
    bool revoked;

    int64_t created_unix;
    int64_t created_height;
};

/* ── Verdicts ────────────────────────────────────────────────────────────── */

/* One taxonomy, no generic "denied". Every value has a stable token; the
 * service maps these straight into its own reason taxonomy, and tests assert
 * the tokens exactly. */
enum metaverse_grant_verdict {
    METAVERSE_GRANT_OK = 0,
    METAVERSE_GRANT_BAD_ARGS,
    METAVERSE_GRANT_MALFORMED,               /* the grant record itself is invalid */
    METAVERSE_GRANT_REVOKED,
    METAVERSE_GRANT_ANCESTOR_REVOKED,
    METAVERSE_GRANT_EXPIRED_HEIGHT,
    METAVERSE_GRANT_EXPIRED_TIME,
    METAVERSE_GRANT_WRONG_HOLDER,
    METAVERSE_GRANT_ACTION_NOT_GRANTED,
    METAVERSE_GRANT_PROPERTY_OUT_OF_SCOPE,
    METAVERSE_GRANT_KIND_OUT_OF_SCOPE,
    METAVERSE_GRANT_COUNTERPARTY_NOT_ALLOWED,
    METAVERSE_GRANT_VALUE_NEGATIVE,
    METAVERSE_GRANT_VALUE_ON_FREE_ACTION,
    METAVERSE_GRANT_BUDGET_EXCEEDED,
    METAVERSE_GRANT_RATE_LIMITED,
    METAVERSE_GRANT_DELEGATION_NOT_PERMITTED,
    METAVERSE_GRANT_DELEGATION_DEPTH_EXCEEDED,
    METAVERSE_GRANT_VERDICT_COUNT
};

const char *metaverse_grant_verdict_token(enum metaverse_grant_verdict v);

/* ── Requests ────────────────────────────────────────────────────────────── */

/* One proposed action, plus the facts the rules need. `now_unix` and `height`
 * are supplied by the caller (0 for "unknown" — an unknown height cannot
 * satisfy a height expiry and the check fails closed with EXPIRED_HEIGHT only
 * when expires_height is actually set). */
struct metaverse_action_request {
    char actor[METAVERSE_PRINCIPAL_MAX + 1];       /* must equal grant->holder */
    struct metaverse_property_id property;
    enum metaverse_action action;
    char counterparty[METAVERSE_PRINCIPAL_MAX + 1]; /* "" when none */
    int64_t value_zat;                              /* 0 for a free action */
    int64_t now_unix;
    int64_t height;
};

/* True when the grant record's own invariants hold: id/holder non-empty,
 * scope form consistent with the populated scope field, lineage_count == depth
 * and <= MAX_DEPTH, non-negative budget, rate window present iff a rate limit
 * is set. A malformed grant NEVER authorizes anything. */
bool metaverse_grant_well_formed(const struct metaverse_grant *g);

/* The full static-scope decision for one request against one grant.
 *
 * `ancestors` / `ancestor_count` are the CURRENT records of this grant's
 * ancestors, in the same root-first order as `g->lineage`, as the store holds
 * them right now. Pass NULL/0 only for a root grant (depth 0); a depth>0 grant
 * evaluated without its ancestors returns ANCESTOR_REVOKED — fail closed,
 * because "I could not check the parent" and "the parent said no" must have
 * the same effect.
 *
 * Order matters and is deliberate: revocation and expiry are checked BEFORE
 * scope, so a revoked grant reports REVOKED rather than leaking whether the
 * property was in scope.
 *
 * This does NOT consume anything — no budget debit, no window increment. It is
 * the same call at PLAN time and at COMMIT time; the service is what records
 * the effect afterwards. */
enum metaverse_grant_verdict metaverse_grant_check(
    const struct metaverse_grant *g,
    const struct metaverse_grant *const *ancestors, size_t ancestor_count,
    const struct metaverse_action_request *req);

/* Decide whether `g` may mint a child grant at `child_depth` carrying
 * `child_actions` over `child_scope`. Enforces: DELEGATE is in g->actions,
 * g->delegation_allowed, child_depth == g->depth + 1, child_depth <=
 * g->max_delegation_depth and <= METAVERSE_GRANT_MAX_DEPTH, and ATTENUATION —
 * a child may never hold an action its parent lacks, nor a budget above the
 * parent's remaining budget, nor a scope wider than the parent's. */
enum metaverse_grant_verdict metaverse_grant_check_delegation(
    const struct metaverse_grant *g,
    const struct metaverse_grant *const *ancestors, size_t ancestor_count,
    const struct metaverse_grant *child,
    int64_t now_unix, int64_t height);

/* Remaining cumulative budget (never negative). */
int64_t metaverse_grant_budget_remaining(const struct metaverse_grant *g);

/* Actions still available in the current window at `now_unix`, accounting for
 * a window that has already rolled over. UINT32_MAX when unlimited. */
uint32_t metaverse_grant_rate_remaining(const struct metaverse_grant *g,
                                       int64_t now_unix);

/* Record the effect of a COMMITTED action on the grant: roll the rate window
 * if `now_unix` is past it, increment the window counter, and add `value_zat`
 * to `spent_zat`. Returns false (and mutates nothing) when the effect would
 * break an invariant — a caller that ignores the return value would be
 * over-spending a grant, so it is the one place in this file that must not be
 * treated as advisory. Call this ONLY after metaverse_grant_check returned OK
 * for the same request. */
bool metaverse_grant_record_commit(struct metaverse_grant *g,
                                  const struct metaverse_action_request *req);

/* True when `id` is inside the grant's scope, ignoring every other rule.
 * Exposed because the catalog projection wants to render "which of these
 * properties can this grant touch" without building a full request. */
bool metaverse_grant_in_scope(const struct metaverse_grant *g,
                             const struct metaverse_property_id *id);

#endif /* ZCL_METAVERSE_PROPERTY_GRANT_H */
