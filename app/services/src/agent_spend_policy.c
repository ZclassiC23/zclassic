/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

// one-result-type-ok:policy-gate — the evaluator has no fallible surface to
// report: it returns a DECISION (struct agent_spend_policy_decision), and a
// refusal is that decision's own code/detail/evidence, not an error. Both
// dispatch choke points map those fields straight onto the reply's named
// error, so the reason already travels with the outcome; wrapping a decision
// in a zcl_result would give a refused spend two places to disagree about why.

/* See services/agent_spend_policy.h for the contract, the default-deny rule,
 * and the honest scope statement.
 *
 * The one table in this file (g_surface) is the whole policy vocabulary: the
 * leaves a bounded session is allowed to reach, and for each spend leaf, WHICH
 * input key carries the amount and which carries the recipient. Naming them
 * per leaf rather than sniffing for "amount"/"address" is deliberate — it is
 * why `core.wallet.shielded.send` is gated on `to` and not on a lucky guess,
 * and why adding a new spend leaf is a visible edit here rather than a silent
 * inheritance. A leaf absent from this table and touching the wallet is
 * refused; that is the point. */

#include "services/agent_spend_policy.h"

/* The grant store lives in the node's node.db and this policy runs in the CLI
 * process, which has none — so consulting it IS a call to the node. That call
 * belongs to the controller layer (it is the same loopback RPC client every
 * spend already uses); moving it down into models/ would put a socket under
 * the model layer, and a callback seam would hide the same edge behind
 * indirection while making the fail-closed path harder to read. The upward
 * include is the honest shape, and mirrors lib-layer-ok:agent-spend-policy-gate
 * in lib/kernel. */
#include "controllers/agent_session_client.h"  // shape-layer-ok:agent-grant-store-is-node-owned
#include "json/json.h"
#include "kernel/command_registry.h"
#include "models/agent_session.h"
#include "services/agent_session_service.h"
#include "util/log_macros.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ASP_TAG "agent_spend_policy"

/* ZCL per 1e8 zatoshi; caps are zatoshis in [0, AGENT_SESSION_MAX_ZAT]. */
#define ASP_ZAT_PER_ZCL 100000000.0

enum asp_class {
    ASP_SPEND,       /* moves funds — gated on amount + recipient */
    ASP_WALLET_READ, /* reads wallet state, reveals no key material */
    ASP_UNBOUNDABLE, /* a read whose REACH the policy cannot bound — listed so
                      * the refusal is deliberate rather than a default-deny
                      * accident, and so the operator keeps it unchanged. */
};

struct asp_surface {
    const char *path;
    enum asp_class klass;
    const char *amount_key;    /* ASP_SPEND only */
    const char *recipient_key; /* ASP_SPEND only; NULL = no recipient concept */
};

/* The understood surface. Every entry was checked against the command
 * definitions in config/commands for what it actually reaches.
 *
 * NOT here, on purpose, even though they carry the wallet capability:
 *   core.wallet.address.export-key  — hands over the raw spending key. One
 *                                     call and every cap below is moot.
 *   core.wallet.address.import      — installs a key the operator never saw.
 *   core.wallet.backup.now          — writes a wallet backup file the agent
 *                                     can read as the same user.
 *   core.wallet.rescan / .replay    — whole-chain work, not an agent's job.
 *   vault.swap.redeem / .refund     — custody settlement with no amount to
 *                                     bound; an unbounded action is not a
 *                                     bounded one.
 *   vault.session.*                 — the grant surface itself (refused by
 *                                     the grant rule before this table is
 *                                     even consulted). */
static const struct asp_surface g_surface[] = {
    /* spends */
    { "core.wallet.transaction.send", ASP_SPEND, "amount", "address" },
    { "core.wallet.shielded.send",    ASP_SPEND, "amount", "to" },
    { "vault.send",                   ASP_SPEND, "amount", "address" },
    { "vault.send-shielded",          ASP_SPEND, "amount", "to" },
    { "app.market.buy",               ASP_SPEND, "amount", NULL },
    { "app.swap.initiate",            ASP_SPEND, "amount", NULL },
    { "app.swap.participate",         ASP_SPEND, "amount", NULL },
    /* wallet reads */
    { "core.wallet.status",            ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.balance",           ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.audit",             ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.utxo.list",         ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.transaction.list",  ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.transaction.get",   ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.address.list",      ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.address.by-label",  ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.address.label",     ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.address.new",       ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.backup.status",     ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.shielded.address",  ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.shielded.balance",  ASP_WALLET_READ, NULL, NULL },
    { "core.wallet.shielded.notes",    ASP_WALLET_READ, NULL, NULL },
    { "app.swap.chains",               ASP_WALLET_READ, NULL, NULL },
    { "app.swap.list",                 ASP_WALLET_READ, NULL, NULL },
    { "vault.list",                    ASP_WALLET_READ, NULL, NULL },
    { "vault.show",                    ASP_WALLET_READ, NULL, NULL },
    { "vault.encumbered",              ASP_WALLET_READ, NULL, NULL },
    /* Arbitrary SQL. Declared READ + CAP_CHAIN_READ, so the default-deny
     * branch below classifies it as a plain read and lets it through — but
     * its REACH is every row in node.db, and node.db holds material whose
     * possession authorizes a spend: another grant's `agent_sessions`
     * bearer token, an HTLC preimage in `zswp_contracts`. A bounded session
     * that can read the widest grant's token is not bounded. The row denies
     * it explicitly so the reason is stated at the refusal rather than
     * inferred from an absence. The un-sessioned local operator is exempt
     * (this table is only consulted for a presented grant), and the same
     * material is denied one layer down at the SQL surface itself
     * (app/controllers/src/dbquery_controller.c, SECRET_TABLES). */
    { "core.storage.query",         ASP_UNBOUNDABLE, NULL, NULL },
    { "core.storage.query.offline", ASP_UNBOUNDABLE, NULL, NULL },
};

/* The grant surface: minting or revoking authority. A session presented on the
 * invocation may never reach it, or the bound is self-serve. Matched on the
 * branch prefix so a leaf added under vault.session tomorrow is covered the
 * day it is added, not the day someone remembers to list it. */
static bool asp_is_grant_surface(const char *path)
{
    return path && strncmp(path, "vault.session", 13) == 0;
}

static const struct asp_surface *asp_lookup(const char *path)
{
    if (!path)
        return NULL;
    for (size_t i = 0; i < sizeof(g_surface) / sizeof(g_surface[0]); i++)
        if (strcmp(g_surface[i].path, path) == 0)
            return &g_surface[i];
    return NULL;
}

static void asp_allow(struct agent_spend_policy_decision *out)
{
    out->allowed = true;
}

static void asp_refuse(struct agent_spend_policy_decision *out,
                       const char *code, const char *detail,
                       const char *leaf)
{
    out->allowed = false;
    (void)snprintf(out->code, sizeof(out->code), "%s", code);
    (void)snprintf(out->detail, sizeof(out->detail), "%s", detail);
    LOG_ERROR(ASP_TAG, "refusing %s for session %s: %s — %s",
              leaf ? leaf : "(unknown leaf)", out->evidence, code, detail);
}

/* Coerce an amount JSON value (int / real / decimal string) to zatoshis,
 * mirroring wnh_amount_real (app/controllers/src/wallet_native_handlers.c):
 * INT and REAL taken as-is, strings via strtod with full-consumption, and
 * negatives rejected. Sets *ok=false on any other shape. Values above the
 * int64 zatoshi range saturate at AGENT_SESSION_MAX_ZAT + 1, which is over
 * every possible cap, so saturation always refuses and can never launder a
 * spend past a limit. */
static int64_t asp_amount_zat(const struct json_value *amt, bool *ok)
{
    double v = 0.0;
    *ok = false;
    if (!amt)
        return 0;
    if (amt->type == JSON_REAL) {
        v = json_get_real(amt);
        *ok = v >= 0.0;
    } else if (amt->type == JSON_INT) {
        v = (double)json_get_int(amt);
        *ok = v >= 0.0;
    } else if (amt->type == JSON_STR) {
        const char *s = json_get_str(amt);
        if (s && s[0]) {
            char *end = NULL;
            v = strtod(s, &end);
            *ok = end && !*end && v >= 0.0;
        }
    }
    if (!*ok)
        return 0;
    if (!(v <= (double)AGENT_SESSION_MAX_ZAT / ASP_ZAT_PER_ZCL))
        return AGENT_SESSION_MAX_ZAT + 1; /* also catches NaN via !(<=) */
    return (int64_t)llround(v * ASP_ZAT_PER_ZCL);
}

void agent_spend_policy_evaluate(const char *session_id,
                                 const struct zcl_command_spec *spec,
                                 const struct json_value *input,
                                 bool committing,
                                 struct agent_spend_policy_decision *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    /* Explicit local-operator exemption: no session, no policy. */
    if (!session_id || !session_id[0]) {
        asp_allow(out);
        return;
    }
    agent_session_redact_id(session_id, out->evidence, sizeof(out->evidence));

    /* Cannot classify what was not named. A dispatch that reaches a gate with
     * no spec is a wiring bug, and the safe answer to a wiring bug on the
     * money path is no. */
    if (!spec || !spec->path) {
        asp_refuse(out, "POLICY_UNKNOWN_COMMAND",
                   "the dispatch named no command, so the policy cannot "
                   "classify it", NULL);
        return;
    }
    if (asp_is_grant_surface(spec->path)) {
        asp_refuse(out, "POLICY_NO_GRANT_MINT",
                   "a bounded session may not mint, widen or revoke a grant — "
                   "run this as the local operator, with ZCL_AGENT_SESSION "
                   "unset", spec->path);
        return;
    }

    const struct asp_surface *s = asp_lookup(spec->path);
    if (!s) {
        /* Default deny over the money surface and over every mutation. A
         * plain read the policy has no opinion about stays allowed, so a
         * bounded agent can still see the node. */
        bool touches_wallet =
            (spec->required_capabilities & ZCL_COMMAND_CAP_WALLET_REQUEST) != 0;
        bool mutates = spec->effect != ZCL_COMMAND_EFFECT_READ;
        bool risky = spec->risk >= ZCL_COMMAND_RISK_WALLET;
        if (touches_wallet || mutates || risky) {
            asp_refuse(out, "POLICY_NOT_UNDERSTOOD",
                       "this leaf touches wallet or node state and the agent "
                       "spend policy has no rule bounding it, so it is "
                       "refused for a bounded session", spec->path);
            return;
        }
        asp_allow(out);
        return;
    }
    if (s->klass == ASP_WALLET_READ) {
        asp_allow(out);
        return;
    }
    if (s->klass == ASP_UNBOUNDABLE) {
        asp_refuse(out, "POLICY_UNBOUNDABLE",
                   "this leaf reads arbitrary node state, including material "
                   "whose possession authorizes a spend, so a bounded session "
                   "may not run it — run it as the local operator, with "
                   "ZCL_AGENT_SESSION unset", spec->path);
        return;
    }

    /* ── an understood spend ─────────────────────────────────────────────── */
    const struct json_value *amt =
        (input && input->type == JSON_OBJ) ? json_get(input, s->amount_key)
                                           : NULL;
    if (!amt) {
        asp_refuse(out, "POLICY_AMOUNT",
                   "a spend leaf was dispatched without its amount, so the "
                   "policy cannot bound it", spec->path);
        return;
    }
    bool aok = false;
    int64_t amount_zat = asp_amount_zat(amt, &aok);
    if (!aok) {
        asp_refuse(out, "POLICY_AMOUNT",
                   "amount is not a non-negative ZCL decimal", spec->path);
        return;
    }
    /* An amount above the whole money supply cannot clear any cap, and the
     * saturating parse deliberately lands one zatoshi past it — refuse here
     * with the per-tx token rather than letting it reach the store as an
     * out-of-range argument, which would report a store error for what is
     * plainly a limit refusal. (A session may legitimately carry MAX_ZAT as
     * its "unbounded" cap, so saturating AT the maximum would be allowed.) */
    if (amount_zat > AGENT_SESSION_MAX_ZAT) {
        asp_refuse(out, "POLICY_TX_LIMIT",
                   "amount exceeds the entire money supply, so it exceeds "
                   "every possible per-tx cap", spec->path);
        return;
    }
    const char *recipient = NULL;
    if (s->recipient_key)
        recipient = json_get_str(json_get(input, s->recipient_key));

    /* One round trip that both checks and (when committing) debits — the node
     * owns the store and performs those as a single indivisible step, so there
     * is no window between them for a second invocation to slip through. */
    char why[64] = { 0 };
    if (!agent_session_client_authorize(session_id, amount_zat, recipient,
                                        committing, &out->window_remaining_zat,
                                        why, sizeof(why))) {
        asp_refuse(out, why[0] ? why : "POLICY_STORE",
                   committing ? "the session's spend policy refused this spend"
                              : "the session's spend policy refused this plan",
                   spec->path);
        return;
    }
    out->debited_zat = committing ? amount_zat : 0;
    asp_allow(out);
}

void agent_spend_policy_release(
    const char *session_id, const struct agent_spend_policy_decision *d)
{
    if (!session_id || !session_id[0] || !d || d->debited_zat <= 0)
        return;
    (void)agent_session_client_release(session_id, d->debited_zat);
}
