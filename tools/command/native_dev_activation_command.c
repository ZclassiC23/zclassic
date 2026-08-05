/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Owner-gated, typed plan -> commit adapter for the isolated dev generation
 * activation engine.  The release binary never contains this implementation.
 */

#define _GNU_SOURCE
#include "command/native_command.h"

#ifdef ZCL_DEV_BUILD

#include "base/hex.h"
#include "crypto/sha3.h"
#include "dev_activation.h"
#include "devloop.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/authority_receipt.h"
#include "util/log_macros.h"

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DAC_TAG "native.dev.activation"
#define DAC_SCHEMA "zcl.dev_generation_activate.v1"
#define DAC_AUTHORITY_SCHEMA "zcl.dev_generation_activate.authority.v1"

static void dac_fail(struct zcl_command_reply *reply,
                     enum zcl_command_status status,
                     enum zcl_command_exit exit_code, const char *code,
                     const char *phase, const char *message,
                     const char *evidence)
{
    LOG_ERROR(DAC_TAG, "%s: %s (%s)", code, message,
              evidence && evidence[0] ? evidence : "-");
    zcl_command_reply_fail(reply, status, exit_code, code, phase, false, false,
                           message, evidence ? evidence : "");
}

static const char *dac_source_root(const struct zcl_command_request *request)
{
    if (request && request->context && request->context->source_root &&
        request->context->source_root[0])
        return request->context->source_root;
    const char *root = getenv("ZCL_DEV_SOURCE_ROOT");
    return root && root[0] ? root : ".";
}

static bool dac_hex64(const char *s)
{
    return s && strlen(s) == 64 &&
           strspn(s, "0123456789abcdef") == 64;
}

static bool dac_generation(const char *s)
{
    return s && strncmp(s, "gen-", 4) == 0 && dac_hex64(s + 4);
}

static bool dac_key(const char *s)
{
    if (!s || !s[0] || strlen(s) > 64)
        return false;
    return strspn(s,
                  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                  "0123456789._:-") == strlen(s);
}

static bool dac_unhex32(const char hex[65], uint8_t out[32])
{
    return zcl_hex_decode_lower(hex, out, 32);
}

static void dac_hash_parts(const char *domain, const char *const *parts,
                           size_t count, char out[65])
{
    struct sha3_256_ctx sha;
    uint8_t digest[32];
    static const uint8_t zero = 0;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, strlen(domain));
    sha3_256_write(&sha, &zero, 1);
    for (size_t i = 0; i < count; i++) {
        sha3_256_write(&sha, (const uint8_t *)parts[i], strlen(parts[i]));
        sha3_256_write(&sha, &zero, 1);
    }
    sha3_256_finalize(&sha, digest);
    zcl_hex_encode(digest, sizeof(digest), out);
}

static bool dac_authority_fields(const char *candidate,
                                 const char *expected_current,
                                 const char *effect,
                                 uint8_t artifact[32], uint8_t anchor[32],
                                 uint8_t detail[32])
{
    char anchor_hex[65];
    const char *parts[] = { expected_current };
    dac_hash_parts("zcl.dev_generation_activate.resident.v1", parts, 1,
                   anchor_hex);
    return dac_unhex32(candidate, artifact) &&
           dac_unhex32(anchor_hex, anchor) && dac_unhex32(effect, detail);
}

static bool dac_authority_name(const char *intent, char out[96])
{
    if (!dac_hex64(intent))
        return false;
    int n = snprintf(out, 96, "activation-authority-%s.bin", intent);
    return n > 0 && n < 96;
}

static bool dac_read_current(const char *gen_root, char out[80])
{
    char link[PATH_MAX];
    int n = snprintf(link, sizeof(link), "%s/current", gen_root);
    if (n <= 0 || (size_t)n >= sizeof(link))
        return false;
    ssize_t got = readlink(link, out, 79);
    if (got < 0) {
        out[0] = 0;
        return true;
    }
    if (got == 0 || got >= 79)
        return false;
    out[got] = 0;
    return dac_generation(out);
}

static bool dac_capture_source(const char *root,
                               struct dev_source_record *record,
                               char why[192])
{
    memset(record, 0, sizeof(*record));
    if (!zcl_dev_source_identity_capture(root, record, why, 192))
        return false;
    return dac_hex64(record->source_id) &&
           dac_hex64(record->mutation_id) && record->cas_present &&
           dac_hex64(record->cas_root_sha3);
}

static void dac_effect(const char *key, const char *candidate,
                       const struct dev_source_record *source,
                       const char *expected_current, int64_t expires,
                       char intent_id[65], char effect_digest[65])
{
    const char *intent_parts[] = { key };
    dac_hash_parts("zcl.dev_generation_activate.intent.v1", intent_parts, 1,
                   intent_id);
    char expires_text[32];
    (void)snprintf(expires_text, sizeof(expires_text), "%lld",
                   (long long)expires);
    const char *effect_parts[] = {
        intent_id, candidate, source->source_id, source->mutation_id,
        source->cas_root_sha3, expected_current, expires_text
    };
    dac_hash_parts("zcl.dev_generation_activate.effect.v1", effect_parts,
                   sizeof(effect_parts) / sizeof(effect_parts[0]),
                   effect_digest);
}

static void dac_result_fields(struct zcl_command_reply *reply,
                              const struct dev_activation_result *result)
{
    (void)json_push_kv_str(&reply->data, "activation_status",
                           result->activation_status);
    (void)json_push_kv_str(&reply->data, "verify_status",
                           result->verify_status);
    (void)json_push_kv_str(&reply->data, "rollback_status",
                           result->rollback_status);
    (void)json_push_kv_str(&reply->data, "candidate_generation",
                           result->candidate_generation);
    (void)json_push_kv_str(&reply->data, "running_generation",
                           result->running_generation);
}

static bool dac_request(const char *root, const char *source_id,
                        const char *mutation, const char *expected_current,
                        enum dev_activation_mode mode,
                        struct dev_activation_cycle_request *cycle)
{
    if (!dev_activation_request_from_cycle(root, "", cycle))
        return false;
    cycle->req.source_identity = source_id;
    cycle->req.source_mutation = mutation;
    cycle->req.expected_current_generation = expected_current;
    cycle->req.mode = mode;
    return true;
}

static void dac_plan(const struct zcl_command_request *request,
                     struct zcl_command_reply *reply, const char *key)
{
    int64_t ttl = 900;
    const struct json_value *ttl_value =
        json_get(request->input, "expires_in_seconds");
    if (ttl_value && !json_is_null(ttl_value))
        ttl = json_get_int(ttl_value);
    if (ttl < 60 || ttl > 3600) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "INVALID_EXPIRY", "normalize",
                 "expires_in_seconds must be between 60 and 3600", "");
        return;
    }

    char root[PATH_MAX];
    if (!realpath(dac_source_root(request), root)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "ROOT_RESOLVE_FAILED", "normalize",
                 "could not resolve the checkout root", "");
        return;
    }
    struct dev_source_record source;
    char why[192] = {0};
    if (!dac_capture_source(root, &source, why)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "SOURCE_EPOCH_UNKNOWN", "plan",
                 "could not capture a complete source epoch",
                 why[0] ? why : "source_capture_failed");
        return;
    }
    struct dev_activation_cycle_request cycle;
    if (!dac_request(root, source.source_id, source.mutation_id, NULL,
                     DEV_ACTIVATION_MODE_STAGE_ONLY, &cycle)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "ACTIVATION_REQUEST_FAILED", "plan",
                 "could not construct the confined dev-lane request", "");
        return;
    }
    char current[80];
    if (!dac_read_current(cycle.gen_root, current)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "RESIDENT_EPOCH_UNKNOWN", "plan",
                 "current dev generation is malformed or unreachable", "");
        return;
    }
    cycle.req.expected_current_generation = current;
    struct dev_activation_ops ops;
    dev_activation_default_ops(&cycle.req, &ops);
    struct dev_activation_result result = {0};
    int rc = dev_activation_run(&cycle.req, &ops, &result);
    if (rc != DEV_ACTIVATION_OK) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "ACTIVATION_PREFLIGHT_FAILED", "plan",
                 "candidate staging or preflight failed closed",
                 result.failure_capsule);
        return;
    }

    int64_t expires = platform_time_wall_unix() + ttl;
    const char *expected = current[0] ? current : "-";
    char intent_id[65], effect_digest[65];
    dac_effect(key, result.candidate_sha256, &source, expected, expires,
               intent_id, effect_digest);

    struct authority_receipt_header authority = {0};
    (void)snprintf(authority.schema, sizeof(authority.schema), "%s",
                   DAC_AUTHORITY_SCHEMA);
    if (!dac_authority_fields(result.candidate_sha256, expected,
                              effect_digest, authority.artifact_digest,
                              authority.context_anchor,
                              authority.detail_digest)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "AUTHORITY_FIELDS_INVALID", "authorize",
                 "could not bind the activation authority receipt", "");
        return;
    }
    char authority_name[96];
    if (!dac_authority_name(intent_id, authority_name) ||
        !authority_receipt_header_seal_and_write(
            &authority, cycle.datadir, authority_name, NULL, 0)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "AUTHORITY_RECEIPT_FAILED", "authorize",
                 "could not persist the activation authority receipt", "");
        return;
    }

    struct json_value commit;
    json_init(&commit);
    json_set_object(&commit);
    (void)json_push_kv_str(&commit, "idempotency_key", key);
    (void)json_push_kv_str(&commit, "intent_id", intent_id);
    (void)json_push_kv_str(&commit, "effect_digest", effect_digest);
    (void)json_push_kv_str(&commit, "candidate_sha256",
                           result.candidate_sha256);
    (void)json_push_kv_str(&commit, "source_id_sha256", source.source_id);
    (void)json_push_kv_str(&commit, "source_mutation_sha256",
                           source.mutation_id);
    (void)json_push_kv_str(&commit, "source_cas_sha3",
                           source.cas_root_sha3);
    (void)json_push_kv_str(&commit, "expected_current_generation", expected);
    (void)json_push_kv_int(&commit, "expires_unix", expires);
    (void)json_push_kv_bool(&commit, "confirm", true);
    char commit_input[1024];
    size_t written = json_write(&commit, commit_input, sizeof(commit_input));
    json_free(&commit);
    if (written == 0 || written >= sizeof(commit_input)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "COMMIT_INPUT_OVERFLOW", "serialize",
                 "could not serialize the exact activation commit", "");
        return;
    }

    (void)json_push_kv_str(&reply->data, "schema", DAC_SCHEMA);
    (void)json_push_kv_str(&reply->data, "stage", "plan");
    (void)json_push_kv_str(&reply->data, "intent_id", intent_id);
    (void)json_push_kv_str(&reply->data, "effect_digest", effect_digest);
    (void)json_push_kv_int(&reply->data, "expires_unix", expires);
    (void)json_push_kv_str(&reply->data, "source_id_sha256",
                           source.source_id);
    (void)json_push_kv_str(&reply->data, "source_cas_sha3",
                           source.cas_root_sha3);
    dac_result_fields(reply, &result);
    (void)json_push_kv_str(&reply->data, "commit_input", commit_input);
    (void)json_push_kv_str(&reply->data, "confirm_hint",
                           "re-run this command with commit_input to activate");
    reply->error.mutated = true; /* immutable staging + durable deploy state */
}

static void dac_commit(const struct zcl_command_request *request,
                       struct zcl_command_reply *reply, const char *key)
{
    const char *intent = json_get_str(json_get(request->input, "intent_id"));
    const char *effect =
        json_get_str(json_get(request->input, "effect_digest"));
    const char *candidate =
        json_get_str(json_get(request->input, "candidate_sha256"));
    const char *source_id =
        json_get_str(json_get(request->input, "source_id_sha256"));
    const char *mutation =
        json_get_str(json_get(request->input, "source_mutation_sha256"));
    const char *cas = json_get_str(json_get(request->input, "source_cas_sha3"));
    const char *expected = json_get_str(
        json_get(request->input, "expected_current_generation"));
    const struct json_value *expires_value =
        json_get(request->input, "expires_unix");
    int64_t expires = expires_value ? json_get_int(expires_value) : 0;
    if (!dac_hex64(intent) || !dac_hex64(effect) || !dac_hex64(candidate) ||
        !dac_hex64(source_id) || !dac_hex64(mutation) || !dac_hex64(cas) ||
        !expected || (strcmp(expected, "-") != 0 && !dac_generation(expected)) ||
        expires <= 0) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "INVALID_COMMIT_INPUT", "normalize",
                 "commit requires the complete unmodified plan input", "");
        return;
    }
    struct dev_source_record planned = {0};
    (void)snprintf(planned.source_id, sizeof(planned.source_id), "%s",
                   source_id);
    (void)snprintf(planned.mutation_id, sizeof(planned.mutation_id), "%s",
                   mutation);
    (void)snprintf(planned.cas_root_sha3, sizeof(planned.cas_root_sha3), "%s",
                   cas);
    planned.cas_present = true;
    char want_intent[65], want_effect[65];
    dac_effect(key, candidate, &planned, expected, expires, want_intent,
               want_effect);
    if (strcmp(intent, want_intent) != 0 || strcmp(effect, want_effect) != 0) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_DENIED,
                 "EFFECT_DIGEST_MISMATCH", "authorize",
                 "activation plan fields changed after preflight", "");
        return;
    }
    if (platform_time_wall_unix() > expires) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "ACTIVATION_PLAN_EXPIRED", "authorize",
                 "activation plan expired; create a fresh plan", "");
        return;
    }

    char root[PATH_MAX];
    if (!realpath(dac_source_root(request), root)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "ROOT_RESOLVE_FAILED", "normalize",
                 "could not resolve the checkout root", "");
        return;
    }
    struct dev_source_record current_source;
    char why[192] = {0};
    if (!dac_capture_source(root, &current_source, why) ||
        strcmp(current_source.source_id, source_id) != 0 ||
        strcmp(current_source.mutation_id, mutation) != 0 ||
        strcmp(current_source.cas_root_sha3, cas) != 0) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "SOURCE_EPOCH_SUPERSEDED", "authorize",
                 "source changed after activation planning",
                 why[0] ? why : "source_epoch_mismatch");
        return;
    }
    struct dev_activation_cycle_request cycle;
    const char *expected_engine = strcmp(expected, "-") == 0 ? "" : expected;
    if (!dac_request(root, source_id, mutation, expected_engine,
                     DEV_ACTIVATION_MODE_ACTIVATE, &cycle)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "ACTIVATION_REQUEST_FAILED", "commit",
                 "could not construct the confined dev-lane request", "");
        return;
    }
    uint8_t artifact_digest[32], context_anchor[32], detail_digest[32];
    char authority_name[96];
    int datadir_fd = -1;
    if (!dac_authority_fields(candidate, expected, effect, artifact_digest,
                              context_anchor, detail_digest) ||
        !dac_authority_name(intent, authority_name) ||
        (datadir_fd = open(cycle.datadir,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0 ||
        !authority_receipt_header_authority_available(
            datadir_fd, authority_name, DAC_AUTHORITY_SCHEMA, artifact_digest,
            context_anchor, detail_digest)) {
        if (datadir_fd >= 0)
            (void)close(datadir_fd);
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_DENIED,
                 "AUTHORITY_RECEIPT_UNAVAILABLE", "authorize",
                 "activation requires the exact independently sealed plan receipt",
                 "create a fresh plan with this exact dev binary");
        return;
    }
    (void)close(datadir_fd);
    char current[80];
    if (!dac_read_current(cycle.gen_root, current)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "RESIDENT_EPOCH_UNKNOWN", "authorize",
                 "current dev generation is malformed or unreachable", "");
        return;
    }
    char candidate_generation[80];
    (void)snprintf(candidate_generation, sizeof(candidate_generation),
                   "gen-%s", candidate);
    bool retry_same_effect = strcmp(current, candidate_generation) == 0;
    if (!retry_same_effect && strcmp(current, expected_engine) != 0) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "RESIDENT_EPOCH_SUPERSEDED", "authorize",
                 "resident generation changed after activation planning", "");
        return;
    }
    cycle.req.expected_current_generation =
        retry_same_effect ? candidate_generation : expected_engine;
    uint8_t candidate_bytes[32];
    if (!dac_unhex32(candidate, candidate_bytes)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "INVALID_CANDIDATE", "normalize",
                 "candidate hash is malformed", "");
        return;
    }
    struct dev_activation_ops ops;
    dev_activation_default_ops(&cycle.req, &ops);
    struct dev_activation_result result = {0};
    int rc = dev_activation_activate_generation(candidate_bytes, &cycle.req,
                                                &ops, &result);
    if (rc != DEV_ACTIVATION_OK) {
        dac_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                 "ACTIVATION_COMMIT_FAILED", "commit",
                 "dev activation failed closed and rollback was attempted",
                 result.failure_capsule);
        return;
    }
    (void)json_push_kv_str(&reply->data, "schema", DAC_SCHEMA);
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_str(&reply->data, "intent_id", intent);
    (void)json_push_kv_str(&reply->data, "effect_digest", effect);
    (void)json_push_kv_bool(&reply->data, "idempotent_retry",
                            retry_same_effect);
    dac_result_fields(reply, &result);
    reply->error.mutated = true;
}

void zcl_native_handle_dev_generation_activate(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !request->input || !reply) {
        if (reply)
            dac_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                     ZCL_COMMAND_EXIT_INTERNAL, "BAD_REQUEST", "normalize",
                     "missing activation request input", "");
        return;
    }
    const char *key =
        json_get_str(json_get(request->input, "idempotency_key"));
    if (!dac_key(key)) {
        dac_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "INVALID_IDEMPOTENCY_KEY", "normalize",
                 "idempotency_key must be 1..64 letters, digits, '.', '_', ':', or '-'",
                 "idempotency_key");
        return;
    }
    if (json_get_bool(json_get(request->input, "confirm")))
        dac_commit(request, reply, key);
    else
        dac_plan(request, reply, key);
}

#endif /* ZCL_DEV_BUILD */
