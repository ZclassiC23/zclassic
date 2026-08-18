/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: read-only native preparation and sealing for development packages. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "util/safe_alloc.h"
#include "vcs/package_accept.h"
#include "vcs/package_prepare.h"
#include "vcs/package_release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *zpd_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zpd_int(const struct json_value *input, const char *key,
                    int64_t *out)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    if (!value || value->type != JSON_INT)
        return false;
    *out = json_get_int(value);
    return true;
}

static void zpd_fail(struct zcl_command_reply *reply, const char *code,
                     const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.package.dev");
}

static bool zpd_push_hex(struct json_value *out, const char *key,
                         const uint8_t *bytes, size_t len)
{
    if (len > (SIZE_MAX - 1u) / 2u)
        return false;
    char *hex = zcl_malloc(len * 2u + 1u, "zcode.package.dev.hex");
    if (!hex)
        return false;
    zcl_hex_encode(bytes, len, hex);
    bool ok = json_push_kv_str(out, key, hex);
    free(hex);
    return ok;
}

static bool zpd_decode_hex(const char *hex, size_t max_bytes,
                           uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    size_t hex_len = hex ? strlen(hex) : 0;
    if ((hex_len & 1u) != 0 || hex_len == 0 || hex_len > max_bytes * 2u)
        return false;
    size_t len = hex_len / 2u;
    uint8_t *bytes = zcl_malloc(len, "zcode.package.dev.input");
    if (!bytes || !zcl_hex_decode_lower(hex, bytes, len)) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_len = len;
    return true;
}

void zcl_native_handle_zcode_package_dev_prepare(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *dir = zpd_str(request->input, "dir");
    const char *pubkey = zpd_str(request->input, "publisher_pubkey");
    int64_t sequence = 0;
    /* An unstated chain is THIS node's chain, not a literal. vcs_package_-
     * prepare defaults to "zclassic-main" because it is deliberately free of
     * chainparams — several standalone tools link it without them — so the
     * default belongs here, in the leaf that does know which chain it is on.
     * vcs_package_accept, the very next thing to touch this release, compares
     * chain_id against exactly this value, so without it every prepare on a
     * testnet or regtest node produced a release that could only ever be
     * refused as wrong-chain-id, on the publishing node as much as on any
     * peer that fetched the carrier. An explicit chain_id still wins. */
    const char *chain_id = zpd_str(request->input, "chain_id");
    char active_chain[VCS_PACKAGE_RELEASE_CHAIN_ID_MAX + 1u];
    if ((!chain_id || !chain_id[0]) &&
        vcs_package_accept_chain_id(active_chain, sizeof(active_chain)))
        chain_id = active_chain;
    struct vcs_package_prepare_options options = {
        .dir = dir,
        .reward_address = zpd_str(request->input, "reward_address"),
        .chain_id = chain_id,
    };
    if (!dir || !pubkey ||
        !zcl_hex_decode_lower(pubkey, options.publisher_pubkey,
                              sizeof(options.publisher_pubkey)) ||
        !zpd_int(request->input, "publisher_sequence", &sequence) ||
        sequence <= 0) {
        zpd_fail(reply, "BAD_PREPARE_INPUT",
                 "dir, a 66-lowercase-hex publisher_pubkey, and a positive publisher_sequence are required");
        return;
    }
    options.publisher_sequence = (uint64_t)sequence;
    struct vcs_package_prepared prepared;
    char detail[256] = {0};
    enum vcs_package_prepare_error err = vcs_package_prepare(
        &options, &prepared, detail, sizeof(detail));
    if (err != VCS_PACKAGE_PREPARE_OK) {
        char message[384];
        (void)snprintf(message, sizeof(message), "%s: %s",
                       vcs_package_prepare_error_string(err), detail);
        zpd_fail(reply, "PACKAGE_PREPARE_FAILED", message);
        return;
    }
    bool ok =
        zpd_push_hex(&reply->data, "package_root", prepared.package_root, 32) &&
        zpd_push_hex(&reply->data, "release_signing_digest",
                     prepared.signing_digest, 32) &&
        zpd_push_hex(&reply->data, "recipe_root", prepared.recipe_root, 32) &&
        zpd_push_hex(&reply->data, "dependency_lock_root",
                     prepared.lock_root, 32) &&
        zpd_push_hex(&reply->data, "api_capsule_root",
                     prepared.capsule_root, 32) &&
        zpd_push_hex(&reply->data, "release_body_hex", prepared.release_body,
                     prepared.release_body_len) &&
        zpd_push_hex(&reply->data, "manifest_hex", prepared.manifest_wire,
                     prepared.manifest_wire_len) &&
        zpd_push_hex(&reply->data, "recipe_hex", prepared.recipe_wire,
                     prepared.recipe_wire_len) &&
        zpd_push_hex(&reply->data, "dependency_lock_hex", prepared.lock_wire,
                     prepared.lock_wire_len) &&
        zpd_push_hex(&reply->data, "api_capsule_hex", prepared.capsule_wire,
                     prepared.capsule_wire_len) &&
        json_push_kv_str(&reply->data, "name", prepared.release.name) &&
        json_push_kv_str(&reply->data, "semver", prepared.release.semver) &&
        json_push_kv_str(&reply->data, "license", prepared.release.license) &&
        /* The chain this release binds to. It decides whether any peer will
         * ever accept the carrier, and it used to be invisible here. */
        json_push_kv_str(&reply->data, "chain_id", prepared.release.chain_id) &&
        json_push_kv_int(&reply->data, "publisher_sequence", sequence) &&
        json_push_kv_int(&reply->data, "file_count",
                         (int64_t)prepared.manifest.count) &&
        json_push_kv_int(&reply->data, "dependency_count",
                         (int64_t)(prepared.lock.count - 1u)) &&
        json_push_kv_int(&reply->data, "public_header_count",
                         (int64_t)prepared.capsule.count) &&
        json_push_kv_bool(&reply->data, "read_only", true) &&
        json_push_kv_str(&reply->data, "signature_status", "unsigned");
    vcs_package_prepared_free(&prepared);
    if (!ok)
        zpd_fail(reply, "PACKAGE_PREPARE_OUTPUT",
                 "bounded canonical output could not be rendered");
}

void zcl_native_handle_zcode_package_dev_seal(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *body_hex = zpd_str(request->input, "release_body_hex");
    const char *signature_hex = zpd_str(request->input, "signature_hex");
    uint8_t *body = NULL;
    size_t body_len = 0;
    uint8_t signature[VCS_PACKAGE_RELEASE_SIGNATURE_BYTES];
    if (!zpd_decode_hex(body_hex,
                        VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES -
                            VCS_PACKAGE_RELEASE_SIGNATURE_BYTES,
                        &body, &body_len) ||
        !signature_hex ||
        !zcl_hex_decode_lower(signature_hex, signature,
                              sizeof(signature))) {
        free(body);
        zpd_fail(reply, "BAD_SEAL_INPUT",
                 "release_body_hex must be bounded lowercase hex and signature_hex must be 128 lowercase hex");
        return;
    }
    size_t wire_len = body_len + sizeof(signature);
    uint8_t *wire = zcl_malloc(wire_len, "zcode.package.dev.release");
    if (!wire) {
        free(body);
        zpd_fail(reply, "PACKAGE_SEAL_ALLOC", "release wire allocation failed");
        return;
    }
    memcpy(wire, body, body_len);
    memcpy(wire + body_len, signature, sizeof(signature));
    free(body);
    struct vcs_package_release release;
    enum vcs_package_release_error err =
        vcs_package_release_parse(wire, wire_len, &release);
    if (err == VCS_PACKAGE_RELEASE_OK)
        err = vcs_package_release_verify(&release);
    uint8_t release_id[32];
    if (err == VCS_PACKAGE_RELEASE_OK)
        err = vcs_package_release_id(&release, release_id);
    uint8_t *canonical = NULL;
    size_t canonical_len = 0;
    if (err == VCS_PACKAGE_RELEASE_OK)
        err = vcs_package_release_serialize(&release, &canonical,
                                            &canonical_len);
    if (err != VCS_PACKAGE_RELEASE_OK || canonical_len != wire_len ||
        memcmp(canonical, wire, wire_len) != 0) {
        char message[192];
        (void)snprintf(message, sizeof(message), "release verification: %s",
                       vcs_package_release_error_string(err));
        free(canonical); free(wire);
        zpd_fail(reply, "PACKAGE_SEAL_FAILED", message);
        return;
    }
    bool ok = zpd_push_hex(&reply->data, "release_hex", wire, wire_len) &&
              zpd_push_hex(&reply->data, "release_id", release_id, 32) &&
              json_push_kv_str(&reply->data, "schema",
                               "zcl.zcode_release.v1") &&
              json_push_kv_str(&reply->data, "signature_status", "verified") &&
              json_push_kv_bool(&reply->data, "read_only", true);
    free(canonical); free(wire);
    if (!ok)
        zpd_fail(reply, "PACKAGE_SEAL_OUTPUT",
                 "verified release could not be rendered");
}
