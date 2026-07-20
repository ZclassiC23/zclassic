/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * core.status.brief body — split out of status_native_handlers.c (E1 file
 * size ceiling) so it can grow its own field list without pushing the other
 * read compositions over the 800-line cap.
 *
 * A compact FLAT projection of the sync/serving fields an operator or AI
 * checks on every call. The running node already maintains those facts in
 * its bounded cached `agent` document, so this projection performs one RPC
 * and validates that document strictly. Unsupported methods, RPC errors,
 * wrong schemas, and missing required fields fail closed; they never become
 * a passing result full of "unknown" values.
 */

#include "controllers/status_native_handlers.h"
#include "controllers/native_handler_body.h"
#include "controllers/status_native_helpers.h"

#include "json/json.h"
#include "controllers/rpc_client.h"
#include "util/log_macros.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool status_schema_is(const struct json_value *obj,
                             const char *expected)
{
    const char *schema = obj && obj->type == JSON_OBJ
        ? json_get_str(json_get(obj, "schema")) : NULL;
    return schema && expected && strcmp(schema, expected) == 0;
}

/* zcl.public_status.v2 carries a numeric sentinel beside an explicit
 * `<field>_known` bit.  The bit is authoritative: an unavailable height is a
 * valid status fact and projects as null, while `known=true` with a negative
 * sentinel is malformed source/runtime skew. */
static bool status_read_known_height(const struct json_value *obj,
                                     const char *value_key,
                                     const char *known_key,
                                     int64_t *value_out, bool *known_out)
{
    const struct json_value *value = obj ? json_get(obj, value_key) : NULL;
    bool known = false;
    if (!value || value->type != JSON_INT || value->val.i < -1 ||
        value->val.i > INT_MAX ||
        !status_read_bool(obj, known_key, &known) ||
        (known && value->val.i < 0))
        return false;
    if (value_out)
        *value_out = value->val.i;
    if (known_out)
        *known_out = known;
    return true;
}

static bool status_read_optional_nonnegative(const struct json_value *obj,
                                             const char *key,
                                             int64_t *value_out,
                                             bool *known_out)
{
    const struct json_value *value = obj ? json_get(obj, key) : NULL;
    if (!value || value->type != JSON_INT || value->val.i < -1)
        return false;
    if (value_out)
        *value_out = value->val.i;
    if (known_out)
        *known_out = value->val.i >= 0;
    return true;
}

/* Status prose is a one-line machine surface.  Reject runtime-skew strings
 * that could inject whitespace/control text into key=value output. */
static bool status_machine_token(const char *value)
{
    if (!value || !value[0])
        return false;
    size_t n = strlen(value);
    if (n > 127)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == ':'))
            return false;
    }
    return true;
}

/* ── Named-predicate validation ────────────────────────────────────
 *
 * zcl.public_status.v2 is validated by a long ordered list of
 * predicates. Historically these lived in one giant `&&` chain, so
 * whichever predicate failed, the operator/AI only ever saw one
 * opaque message ("RPC agent returned an error or invalid
 * zcl.public_status.v2") with no clue which field was the problem —
 * including the common case of an older node binary whose `agent` RPC
 * predates a field the CLI now requires.
 *
 * `status_validate` tracks the FIRST predicate that fails: its field
 * name (for the error message) and whether the underlying JSON key
 * was entirely ABSENT (as opposed to present-but-malformed). Absent +
 * schema-matches is schema/version skew — the node binary predates
 * the CLI contract; present-but-wrong is a malformed/self-contradictory
 * document. The SCHECK() macro below turns each conjunct of the
 * former `&&` chain into a named, order-preserving, short-circuiting
 * check: once one predicate has failed, every later SCHECK() is a
 * no-op that does not evaluate its condition (so it stays safe to
 * dereference sub-objects the earlier checks proved exist). */
struct status_validate {
    bool failed;
    bool version_skew; /* first failing key was absent, not malformed */
    char field[96];
};

static bool status_key_missing(const struct json_value *obj, const char *key)
{
    return !obj || obj->type != JSON_OBJ || json_get(obj, key) == NULL;
}

static bool status_note_fail(struct status_validate *v, const char *field,
                             bool missing)
{
    if (!v->failed) {
        v->failed = true;
        v->version_skew = missing;
        (void)snprintf(v->field, sizeof(v->field), "%s", field);
    }
    return false;
}

/* `missing`/`cond` are only evaluated once v->failed is false, so this
 * is safe to chain even when later conds read through an object an
 * earlier failed check would have left NULL. */
#define SCHECK(v, field, missing, cond) \
    ((v)->failed ? false : ((cond) ? true : status_note_fail((v), (field), (missing))))

char *zcl_native_status_brief_body(const struct json_value *args,
                                   struct zcl_native_body_err *err)
{
    (void)args;
    if (!err)
        LOG_NULL("native.status", "status brief missing error sink");

    char *raw = node_rpc_call("agent", NULL);
    struct json_value agent;
    json_init(&agent);
    bool parsed = status_parse_rpc_json(&agent, raw, JSON_OBJ);
    const struct json_value *peers = parsed ? json_get(&agent, "peers") : NULL;
    const struct json_value *conditions =
        parsed ? json_get(&agent, "conditions") : NULL;
    const struct json_value *resources =
        parsed ? json_get(&agent, "resources") : NULL;
    const struct json_value *reducer =
        parsed ? json_get(&agent, "reducer") : NULL;
    const struct json_value *security =
        parsed ? json_get(&agent, "security_posture") : NULL;
    const struct json_value *first_call =
        parsed ? json_get(&agent, "first_call") : NULL;

    int64_t served_height = -1, header_height = -1, gap = 0;
    int64_t target_height = -1;
    int64_t peer_best = 0, peer_count = 0, active_conditions = 0;
    int64_t rss_mb = 0, tip_advance_age_seconds = 0;
    int64_t first_call_budget_ms = 0;
    bool served_known = false, header_known = false, peer_best_known = false;
    bool target_known = false, gap_known = false, rss_known = false;
    bool tip_age_known = false, chain_evidence_consistent = false;
    bool partial_result = false, first_call_partial = false;
    bool serving = false, healthy = false, anchor_gap = false;
    bool nullifier_gap = false, budget_exceeded = true;
    const char *sync_state = parsed
        ? json_get_str(json_get(&agent, "sync_state")) : NULL;
    const char *reported_blocker = parsed
        ? json_get_str(json_get(&agent, "primary_blocker")) : NULL;

    bool resources_shape_ok = resources && resources->type == JSON_OBJ &&
        status_schema_is(resources, "zcl.node_resources.v1") &&
        status_read_optional_nonnegative(resources, "rss_mb", &rss_mb,
                                         &rss_known);

    struct status_validate v = {0};
    bool valid = parsed &&
        SCHECK(&v, "schema", status_key_missing(&agent, "schema"),
              status_schema_is(&agent, "zcl.public_status.v2")) &&
        SCHECK(&v, "served_height_known",
              status_key_missing(&agent, "served_height") ||
                  status_key_missing(&agent, "served_height_known"),
              status_read_known_height(&agent, "served_height",
                                       "served_height_known", &served_height,
                                       &served_known)) &&
        SCHECK(&v, "header_height_known",
              status_key_missing(&agent, "header_height") ||
                  status_key_missing(&agent, "header_height_known"),
              status_read_known_height(&agent, "header_height",
                                       "header_height_known", &header_height,
                                       &header_known)) &&
        SCHECK(&v, "gap", status_key_missing(&agent, "gap"),
              status_read_nonnegative_int(&agent, "gap", &gap)) &&
        SCHECK(&v, "peer_best_height_known",
              status_key_missing(&agent, "peer_best_height") ||
                  status_key_missing(&agent, "peer_best_height_known"),
              status_read_known_height(&agent, "peer_best_height",
                                       "peer_best_height_known", &peer_best,
                                       &peer_best_known)) &&
        SCHECK(&v, "target_height_known",
              status_key_missing(&agent, "target_height") ||
                  status_key_missing(&agent, "target_height_known"),
              status_read_known_height(&agent, "target_height",
                                       "target_height_known", &target_height,
                                       &target_known)) &&
        SCHECK(&v, "chain_evidence_consistent",
              status_key_missing(&agent, "chain_evidence_consistent"),
              status_read_bool(&agent, "chain_evidence_consistent",
                               &chain_evidence_consistent)) &&
        SCHECK(&v, "partial_result",
              status_key_missing(&agent, "partial_result"),
              status_read_bool(&agent, "partial_result", &partial_result)) &&
        SCHECK(&v, "serving", status_key_missing(&agent, "serving"),
              status_read_bool(&agent, "serving", &serving)) &&
        SCHECK(&v, "healthy", status_key_missing(&agent, "healthy"),
              status_read_bool(&agent, "healthy", &healthy)) &&
        SCHECK(&v, "sync_state", status_key_missing(&agent, "sync_state"),
              status_machine_token(sync_state)) &&
        SCHECK(&v, "primary_blocker",
              status_key_missing(&agent, "primary_blocker"),
              status_machine_token(reported_blocker)) &&
        SCHECK(&v, "peers", status_key_missing(&agent, "peers"),
              peers && peers->type == JSON_OBJ) &&
        SCHECK(&v, "conditions", status_key_missing(&agent, "conditions"),
              conditions && conditions->type == JSON_OBJ) &&
        SCHECK(&v, "conditions.schema",
              status_key_missing(conditions, "schema"),
              status_schema_is(conditions,
                               "zcl.condition_engine_summary.v2")) &&
        SCHECK(&v, "reducer", status_key_missing(&agent, "reducer"),
              reducer && reducer->type == JSON_OBJ) &&
        SCHECK(&v, "security_posture",
              status_key_missing(&agent, "security_posture"),
              security && security->type == JSON_OBJ) &&
        SCHECK(&v, "security_posture.schema",
              status_key_missing(security, "schema"),
              status_schema_is(security, "zcl.security_posture.v1")) &&
        SCHECK(&v, "first_call", status_key_missing(&agent, "first_call"),
              first_call && first_call->type == JSON_OBJ) &&
        SCHECK(&v, "first_call.schema",
              status_key_missing(first_call, "schema"),
              status_schema_is(first_call, "zcl.first_call_contract.v1")) &&
        SCHECK(&v, "peers.total", status_key_missing(peers, "total"),
              status_read_nonnegative_int(peers, "total", &peer_count)) &&
        SCHECK(&v, "conditions.active_count",
              status_key_missing(conditions, "active_count"),
              status_read_nonnegative_int(conditions, "active_count",
                                          &active_conditions)) &&
        SCHECK(&v, "reducer.tip_advance_age_seconds",
              status_key_missing(reducer, "tip_advance_age_seconds"),
              status_read_optional_nonnegative(
                  reducer, "tip_advance_age_seconds",
                  &tip_advance_age_seconds, &tip_age_known)) &&
        SCHECK(&v, "security_posture.anchor_backfill_gap",
              status_key_missing(security, "anchor_backfill_gap"),
              status_read_bool(security, "anchor_backfill_gap",
                               &anchor_gap)) &&
        SCHECK(&v, "security_posture.nullifier_backfill_gap",
              status_key_missing(security, "nullifier_backfill_gap"),
              status_read_bool(security, "nullifier_backfill_gap",
                               &nullifier_gap)) &&
        SCHECK(&v, "first_call.budget_ms",
              status_key_missing(first_call, "budget_ms"),
              status_read_nonnegative_int(first_call, "budget_ms",
                                          &first_call_budget_ms)) &&
        SCHECK(&v, "first_call.budget_ms", false, first_call_budget_ms > 0) &&
        SCHECK(&v, "first_call.partial_result",
              status_key_missing(first_call, "partial_result"),
              status_read_bool(first_call, "partial_result",
                               &first_call_partial)) &&
        SCHECK(&v, "first_call.budget_exceeded",
              status_key_missing(first_call, "budget_exceeded"),
              status_read_bool(first_call, "budget_exceeded",
                               &budget_exceeded)) &&
        /* budget_exceeded is a pure timing fact (elapsed > budget_ms) and is
         * ORTHOGONAL to completeness: a busy node whose cached-field collect
         * contends cs_main can overrun 250ms while still gathering every
         * field, so budget_exceeded=true with partial_result=false is a
         * truthful slow-but-complete result, not self-contradiction. (This
         * false coupling made the flagless status of a folding node fail with
         * a cryptic "missing/invalid field first_call.budget_exceeded" after
         * several seconds instead of returning the real, complete brief.)
         * Data completeness is enforced independently by the resources check
         * below (`resources` must be present unless partial_result), so no
         * budget-vs-partial cross-check is needed or correct here. */
        SCHECK(&v, "first_call.partial_result", false,
              first_call_partial == partial_result) &&
        SCHECK(&v, "resources", status_key_missing(&agent, "resources"),
              resources_shape_ok || (partial_result && !resources)) &&
        SCHECK(&v, "partial_reason",
              status_key_missing(&agent, "partial_reason"),
              !partial_result ||
                  (json_get_str(json_get(&agent, "partial_reason")) != NULL &&
                   json_get_str(json_get(&agent, "partial_reason"))[0])) &&
        SCHECK(&v, "served_height_known", false, !serving || served_known) &&
        SCHECK(&v, "healthy", false, !healthy || serving) &&
        SCHECK(&v, "security_posture.anchor_backfill_gap", false,
              (!anchor_gap && !nullifier_gap) || !healthy);

    if (valid) {
        gap_known = chain_evidence_consistent;
        /* A consistent local chain has target==header and an exact arithmetic
         * gap.  An inconsistent/unknown chain is emitted as null and the
         * producer's sentinel gap must remain zero. */
        valid = SCHECK(&v, "gap", false,
                       gap_known
                           ? served_known && header_known && target_known &&
                             target_height == header_height &&
                             header_height >= served_height &&
                             gap == header_height - served_height
                           : gap == 0);
    }

    if (!valid) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        if (!parsed) {
            /* `parsed` is false for two very different situations that must
             * not both read as a schema fault: an unparsable byte stream, or
             * a well-formed transport/RPC error object (node not running,
             * connect refused/timed out, response deadline exceeded, cookie
             * unreadable). Surface the transport reason verbatim when we have
             * one -- that is the honest "no live node / node busy" answer the
             * operator needs, not "invalid zcl.public_status.v2". */
            const char *rpc_err = NULL;
            if (agent.type == JSON_OBJ) {
                const struct json_value *e = json_get(&agent, "error");
                rpc_err = json_get_str(json_get(
                    e && e->type == JSON_OBJ ? e : &agent, "message"));
            }
            if (rpc_err && rpc_err[0])
                (void)snprintf(err->message, sizeof(err->message),
                              "node status unavailable: %s", rpc_err);
            else
                (void)snprintf(err->message, sizeof(err->message),
                              "node status unavailable: RPC agent returned an "
                              "unparsable response");
        } else if (v.version_skew) {
            (void)snprintf(err->message, sizeof(err->message),
                          "invalid zcl.public_status.v2: node binary "
                          "predates the CLI contract (missing field %s)",
                          v.field);
        } else {
            (void)snprintf(err->message, sizeof(err->message),
                          "invalid zcl.public_status.v2: missing/invalid "
                          "field %s", v.field);
        }
        /* err->message now owns a copy of any rpc_err text, so the source
         * document is safe to release before LOG_NULL returns NULL. */
        json_free(&agent);
        free(raw);
        LOG_NULL("native.status", "%s", err->message);
    }

    /* Permanent independently-diagnosed shielded-history incompleteness is
     * causal. Rank it above downstream queue/download symptoms from the
     * general operator latch. */
    const char *primary_blocker = anchor_gap
        ? "utxo_apply.anchor_backfill_gap"
        : nullifier_gap ? "utxo_apply.nullifier_backfill_gap"
        : reported_blocker;

    /* Typed-blocker-registry head + count, from the same authority as
     * `dumpstate blocker`. Read OPTIONALLY (an older node predating this
     * contract simply omits the sub-object) so the two operator surfaces
     * always agree on the registry head instead of naming disjoint truths.
     * `active_blockers`/`blocker_head` are emitted only when the node exports
     * them; `blocker_head` still points into `agent`, which stays alive until
     * after `root` is serialized below (json_push_kv_str copies the string). */
    int64_t active_blockers = 0;
    bool blocker_registry_known = false;
    const char *blocker_head = NULL;
    /* POINT 3.4: overdue_transient_count surfaces the same registry truth
     * for TRANSIENT/DEPENDENCY blockers that are stuck past their deadline
     * or TTL (see agent_blocker_is_overdue_transient in
     * event_agent_summary.c). A TRANSIENT never becomes `primary_blocker`
     * (PERMANENT/RESOURCE-only headline, unchanged here) so without this an
     * overdue transient could sit invisible behind a "healthy" brief. Read
     * OPTIONALLY, same as active_blockers/blocker_head above, so an older
     * node predating this field simply omits it. */
    int64_t overdue_transient_count = 0;
    bool overdue_transient_known = false;
    const char *overdue_transient_id = NULL;
    const struct json_value *blocker_registry =
        json_get(&agent, "blocker_registry");
    if (blocker_registry && blocker_registry->type == JSON_OBJ) {
        blocker_registry_known = status_read_nonnegative_int(
            blocker_registry, "active_count", &active_blockers);
        const char *head = json_get_str(
            json_get(blocker_registry, "dominant_id"));
        if (head && head[0])
            blocker_head = head;
        overdue_transient_known = status_read_nonnegative_int(
            blocker_registry, "overdue_transient_count",
            &overdue_transient_count);
        const char *odom = json_get_str(
            json_get(blocker_registry, "overdue_transient_dominant_id"));
        if (odom && odom[0] && strcmp(odom, "none") != 0)
            overdue_transient_id = odom;
    }

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    status_push_int_if_known(&root, "hstar", served_known, served_height);
    status_push_int_if_known(&root, "header_height", header_known,
                             header_height);
    status_push_int_if_known(&root, "gap", gap_known, gap);
    status_push_int_if_known(&root, "peer_best", peer_best_known, peer_best);
    json_push_kv_str(&root, "sync_state", sync_state);
    json_push_kv_bool(&root, "serving", serving);
    json_push_kv_bool(&root, "healthy", healthy);
    json_push_kv_int(&root, "peer_count", peer_count);
    json_push_kv_str(&root, "primary_blocker", primary_blocker);
    /* The full agent contract does not yet export the dominant blocker's
     * capture age. Tip-stall age is a different fact and must not be passed
     * off as blocker age. */
    status_push_int_if_known(&root, "blocker_age_s", false, 0);
    /* Registry head shown beside primary_blocker so the brief and
     * `dumpstate blocker` never present disjoint truths. Both fields are
     * OMITTED (never null) when the node predates the registry export —
     * the documented optional-sub-object contract. */
    if (blocker_registry_known)
        json_push_kv_int(&root, "active_blockers", active_blockers);
    if (blocker_head)
        json_push_kv_str(&root, "blocker_head", blocker_head);
    /* POINT 3.4: overdue TRANSIENT/DEPENDENCY count, so the compact brief
     * alone shows the registry truth even though such a blocker never
     * becomes `primary_blocker`. Omitted (never null/zero-fabricated) when
     * the node predates the field, matching active_blockers/blocker_head.
     * The note is emitted only when the count is actually positive so a
     * healthy node never grows an empty/zero note field. */
    if (overdue_transient_known)
        json_push_kv_int(&root, "overdue_transient_count",
                         overdue_transient_count);
    if (overdue_transient_count > 0) {
        char note[128];
        (void)snprintf(note, sizeof(note),
                      "%lld transient blockers overdue: %s",
                      (long long)overdue_transient_count,
                      overdue_transient_id ? overdue_transient_id
                                           : "unknown");
        json_push_kv_str(&root, "overdue_transient_note", note);
    }
    json_push_kv_int(&root, "active_conditions", active_conditions);
    status_push_int_if_known(&root, "rss_mb", rss_known, rss_mb);
    status_push_int_if_known(&root, "tip_advance_age_seconds", tip_age_known,
                             tip_advance_age_seconds);

    char *out = zcl_json_value_to_body(&root, "status_brief_body");
    json_free(&root);
    json_free(&agent);
    free(raw);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_INTERNAL;
        snprintf(err->message, sizeof(err->message),
                 "malloc failed for %s", "status brief response");
        LOG_NULL("native.ops", "malloc failed for %s", "status brief response");
    }
    return out;
}
