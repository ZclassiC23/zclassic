/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: read-only native views of the ZC23 Living Commons projection. */

#include "command/native_command.h"

#include "base/checked.h"
#include "base/hex.h"
#include "json/json.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_commons_projection.h"
#include "vcs/zcode_creation_attribution.h"
#include "vcs/zcode_score_receipt.h"

#include <stdlib.h>
#include <string.h>

struct zcc_shadow_package {
    const char *name;
    const char *content_root_hex;
    const char *release_root_hex;
};

#define ZCODE_PACKAGE(name, dir, sequence, content, release, recipe, lock, capsule, dependency, signature) \
    {name, content, release},
static const struct zcc_shadow_package zcc_shadow_packages[] = {
#include "../../config/zcode_package_registry.def"
};
#undef ZCODE_PACKAGE

static const char *zcc_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zcc_keys(const struct json_value *input,
                     const char *const *allowed, size_t count)
{
    if (!input || input->type != JSON_OBJ) return false;
    for (size_t i = 0; i < input->num_children; i++) {
        bool known = false;
        for (size_t j = 0; j < count; j++)
            known = known || strcmp(input->keys[i], allowed[j]) == 0;
        if (!known) return false;
    }
    return true;
}

static void zcc_fail(struct zcl_command_reply *reply, const char *code,
                     const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.commons");
}

static void zcc_hex(struct json_value *data, const char *key,
                    const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static bool zcc_root(const struct json_value *input, const char *key,
                     uint8_t root[32])
{
    const char *hex = zcc_str(input, key);
    return hex && strlen(hex) == 64 && zcl_hex_decode_lower(hex, root, 32);
}

static const char *zcc_status_name(
    enum vcs_zcode_commons_verification_status status)
{
    switch (status) {
    case VCS_ZCODE_COMMONS_PARTIAL: return "partial";
    case VCS_ZCODE_COMMONS_COMPLETE: return "complete";
    default: return "unknown";
    }
}

static const char *zcc_category_name(uint16_t category)
{
    switch (category) {
    case VCS_ZCODE_CREATION_PUBLIC_SOURCE: return "public_source";
    case VCS_ZCODE_CREATION_BORN_RED_FIX: return "born_red_fix";
    case VCS_ZCODE_CREATION_SECURITY_FIX: return "security_fix";
    case VCS_ZCODE_CREATION_INDEPENDENT_REPRODUCTION:
        return "independent_reproduction";
    case VCS_ZCODE_CREATION_COMPATIBILITY: return "compatibility";
    case VCS_ZCODE_CREATION_PRESERVATION: return "preservation";
    default: return "invalid";
    }
}

static size_t zcc_unique_packages(
    const struct vcs_zcode_commons_projection *projection, bool releases)
{
    size_t unique = 0;
    size_t count = vcs_zcode_commons_projection_creation_count(projection);
    for (size_t i = 0; i < count; i++) {
        const struct vcs_zcode_commons_creation_entry *entry =
            vcs_zcode_commons_projection_creation_at(projection, i);
        const uint8_t *root = releases ? entry->release_root
                                       : entry->package_root;
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            const struct vcs_zcode_commons_creation_entry *prior =
                vcs_zcode_commons_projection_creation_at(projection, j);
            const uint8_t *prior_root = releases ? prior->release_root
                                                 : prior->package_root;
            seen = seen || memcmp(root, prior_root, 32) == 0;
        }
        if (!seen) unique++;
    }
    return unique;
}

static void zcc_render_summary(
    struct json_value *data,
    const struct vcs_zcode_commons_projection *projection)
{
    enum vcs_zcode_commons_verification_status status =
        vcs_zcode_commons_projection_status(projection);
    uint64_t attributed =
        vcs_zcode_commons_projection_attributed_atoms(projection);
    uint64_t minted = vcs_zcode_commons_projection_minted_atoms(projection);
    uint64_t unattributed = minted >= attributed ? minted - attributed : 0;
    (void)json_push_kv_str(data, "verification_status",
                           zcc_status_name(status));
    (void)json_push_kv_bool(data, "policy_valid_minted_supply_known",
                            status == VCS_ZCODE_COMMONS_COMPLETE);
    (void)json_push_kv_int(data, "policy_valid_minted_supply_atoms",
                           status == VCS_ZCODE_COMMONS_COMPLETE
                               ? (int64_t)minted : 0);
    (void)json_push_kv_int(data, "parsed_mint_atoms", (int64_t)minted);
    (void)json_push_kv_int(data, "attributed_atoms", (int64_t)attributed);
    (void)json_push_kv_int(data, "unattributed_atoms",
                           (int64_t)unattributed);
    (void)json_push_kv_bool(data, "attributed_exceeds_mint",
                            attributed > minted);
    (void)json_push_kv_int(data, "unissued_atoms",
        (int64_t)vcs_zcode_commons_projection_unissued_atoms(projection));
    size_t creations =
        vcs_zcode_commons_projection_creation_count(projection);
    (void)json_push_kv_int(data, "creation_objects", (int64_t)creations);
    (void)json_push_kv_int(data, "epoch_objects",
        (int64_t)vcs_zcode_commons_projection_epoch_count(projection));
    (void)json_push_kv_int(data, "package_count",
                           (int64_t)zcc_unique_packages(projection, false));
    (void)json_push_kv_int(data, "release_count",
                           (int64_t)zcc_unique_packages(projection, true));
    uint64_t categories[7] = {0};
    for (size_t i = 0; i < creations; i++) {
        uint16_t category =
            vcs_zcode_commons_projection_creation_at(projection, i)->category;
        if (category < 7) categories[category]++;
    }
    (void)json_push_kv_int(data, "born_red_defect_tests",
                           (int64_t)categories[VCS_ZCODE_CREATION_BORN_RED_FIX]);
    (void)json_push_kv_int(data, "independent_reproductions",
        (int64_t)categories[VCS_ZCODE_CREATION_INDEPENDENT_REPRODUCTION]);
    (void)json_push_kv_int(data, "security_fixes",
        (int64_t)categories[VCS_ZCODE_CREATION_SECURITY_FIX]);
    (void)json_push_kv_int(data, "compatibility_events",
        (int64_t)categories[VCS_ZCODE_CREATION_COMPATIBILITY]);
    uint8_t failed[32]; const char *reason = NULL;
    bool has_failure = vcs_zcode_commons_projection_first_failure(
        projection, failed, &reason);
    (void)json_push_kv_bool(data, "structural_integrity", !has_failure);
    if (has_failure) {
        zcc_hex(data, "first_failure_root", failed);
        (void)json_push_kv_str(data, "first_failure", reason);
        (void)json_push_kv_str(data, "next_safe_diagnostic_action",
                               "inspect first_failure_root in workspace CAS");
    } else if (creations == 0) {
        (void)json_push_kv_str(data, "next_safe_diagnostic_action",
                               "publish canonical creation evidence to CAS");
    } else {
        (void)json_push_kv_str(data, "next_safe_diagnostic_action",
            "supply immutable policy and active-chain anchor context");
    }
}

static struct vcs_zcode_commons_projection *zcc_build(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    const char *const *keys, size_t key_count)
{
    const char *workspace = request ? zcc_str(request->input, "workspace")
                                    : NULL;
    if (!request || !reply || !workspace ||
        !zcc_keys(request->input, keys, key_count)) {
        zcc_fail(reply, "BAD_COMMONS_INPUT",
                 "closed input requires an explicit workspace");
        return NULL;
    }
    struct vcs_zcode_commons_projection *projection =
        vcs_zcode_commons_projection_build(workspace);
    if (!projection)
        zcc_fail(reply, "COMMONS_REBUILD_FAILED",
                 "read-only CAS projection rebuild failed");
    return projection;
}

void zcl_native_handle_zcode_commons_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace"};
    struct vcs_zcode_commons_projection *projection =
        zcc_build(request, reply, keys, 1);
    if (!projection) return;
    zcc_render_summary(&reply->data, projection);
    vcs_zcode_commons_projection_free(projection);
}

void zcl_native_handle_zcode_commons_rebuild(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace"};
    struct vcs_zcode_commons_projection *projection =
        zcc_build(request, reply, keys, 1);
    if (!projection) return;
    uint8_t root[32];
    zcc_render_summary(&reply->data, projection);
    if (vcs_zcode_commons_projection_root(projection, root))
        zcc_hex(&reply->data, "projection_root", root);
    (void)json_push_kv_bool(&reply->data, "persisted", false);
    (void)json_push_kv_str(&reply->data, "authority", "canonical_workspace_cas");
    vcs_zcode_commons_projection_free(projection);
}

static const struct zcc_shadow_package *zcc_shadow_package_lookup(
    const uint8_t package_root[32], const uint8_t release_root[32])
{
    for (size_t i = 0;
         i < sizeof(zcc_shadow_packages) / sizeof(zcc_shadow_packages[0]);
         i++) {
        uint8_t package[32], release[32];
        if (zcl_hex_decode_lower(zcc_shadow_packages[i].content_root_hex,
                                 package, sizeof(package)) &&
            zcl_hex_decode_lower(zcc_shadow_packages[i].release_root_hex,
                                 release, sizeof(release)) &&
            memcmp(package, package_root, 32) == 0 &&
            memcmp(release, release_root, 32) == 0)
            return &zcc_shadow_packages[i];
    }
    return NULL;
}

void zcl_native_handle_zcode_commons_shadow_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {
        "workspace", "score_receipt_root"
    };
    const char *workspace = request ? zcc_str(request->input, "workspace")
                                    : NULL;
    uint8_t score_root[32], derived[32], *wire = NULL;
    size_t wire_len = 0;
    if (!request || !reply || !workspace ||
        !zcc_keys(request->input, keys, 2) ||
        !zcc_root(request->input, "score_receipt_root", score_root) ||
        vcs_object_load_raw_bounded(
            workspace, score_root, VCS_ZCODE_SCORE_WIRE_BYTES,
            &wire, &wire_len) != 0) {
        free(wire);
        zcc_fail(reply, "SHADOW_SCORE_NOT_FOUND",
                 "exact Score receipt root absent or input malformed");
        return;
    }
    struct vcs_zcode_score_receipt_v1 score;
    bool parsed = vcs_zcode_score_receipt_parse(
            wire, wire_len, &score) == VCS_ZCODE_SCORE_OK &&
        vcs_zcode_score_receipt_id(&score, derived) == VCS_ZCODE_SCORE_OK &&
        memcmp(derived, score_root, 32) == 0;
    free(wire);
    if (!parsed ||
        vcs_zcode_score_receipt_verify_cas(workspace, &score) !=
            VCS_ZCODE_SCORE_OK) {
        zcc_fail(reply, "SHADOW_VERTICAL_INVALID",
                 "Score task/candidate/proof/PROVEN vertical did not rederive");
        return;
    }
    const struct zcc_shadow_package *package = zcc_shadow_package_lookup(
        score.package_root, score.release_root);
    if (!package) {
        zcc_fail(reply, "SHADOW_PACKAGE_UNREGISTERED",
                 "Score package/release pair is absent from the generated registry");
        return;
    }

    bool offhost = (score.awarded_mask &
        (UINT8_C(1) << VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION)) != 0;
    (void)json_push_kv_str(&reply->data, "mode", "shadow_pre_genesis");
    (void)json_push_kv_str(&reply->data, "package_name", package->name);
    zcc_hex(&reply->data, "score_receipt_root", score_root);
    zcc_hex(&reply->data, "package_root", score.package_root);
    zcc_hex(&reply->data, "release_root", score.release_root);
    zcc_hex(&reply->data, "task_root", score.task_root);
    zcc_hex(&reply->data, "candidate_root", score.candidate_root);
    zcc_hex(&reply->data, "proof_policy_root", score.proof_policy_root);
    zcc_hex(&reply->data, "proof_set_root", score.proof_set_root);
    zcc_hex(&reply->data, "proven_lane_root", score.proven_lane_root);
    zcc_hex(&reply->data, "accepted_extraction_evidence_root",
            score.evidence_roots[VCS_ZCODE_SCORE_ACCEPTED_EXTRACTION]);
    zcc_hex(&reply->data, "independent_reproduction_evidence_root",
            score.evidence_roots[VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION]);
    (void)json_push_kv_bool(&reply->data, "vertical_reverified", true);
    (void)json_push_kv_bool(&reply->data,
                            "approved_off_host_reproduction", offhost);
    (void)json_push_kv_str(&reply->data, "shadow_status",
        offhost ? "ready_for_creation_attribution_plan"
                : "blocked_off_host_reproduction");
    if (!offhost)
        (void)json_push_kv_str(&reply->data, "blocker",
            "no_owner_approved_off_host_reproducer_is_registered");
    (void)json_push_kv_str(&reply->data, "why_shadow_units_would_exist",
        "challenge_matured_public_c23_creation_with_approved_off_host_reproduction");
    (void)json_push_kv_int(&reply->data, "shadow_award_atoms", 0);
    (void)json_push_kv_bool(&reply->data,
                            "creation_attribution_created", false);
    (void)json_push_kv_bool(&reply->data,
                            "epoch_creation_set_created", false);
    (void)json_push_kv_bool(&reply->data, "moves_live_funds", false);
    (void)json_push_kv_bool(&reply->data, "creates_ownership_right", false);
    (void)json_push_kv_bool(&reply->data, "token_required_for_access", false);
    (void)json_push_kv_bool(&reply->data, "money_establishes_truth", false);
    (void)json_push_kv_bool(&reply->data,
                            "permissive_license_validation_required", true);
    (void)json_push_kv_str(&reply->data, "next_safe_action",
        offhost ? "prepare_unsigned_shadow_attribution_for_owner_review"
                : "register_and_verify_an_owner_approved_off_host_reproducer");
}

void zcl_native_handle_zcode_commons_verify(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace"};
    struct vcs_zcode_commons_projection *projection =
        zcc_build(request, reply, keys, 1);
    if (!projection) return;
    zcc_render_summary(&reply->data, projection);
    uint8_t failed[32]; const char *reason = NULL;
    bool structural = !vcs_zcode_commons_projection_first_failure(
        projection, failed, &reason);
    (void)json_push_kv_bool(&reply->data, "exact_epoch_accounting",
                            structural);
    (void)json_push_kv_str(&reply->data, "creation_attribution_invariant",
        structural ? "unknown_without_active_chain_policy_context"
                   : "invalid_structural_accounting");
    (void)json_push_kv_bool(&reply->data, "balance_used_for_truth", false);
    vcs_zcode_commons_projection_free(projection);
}

void zcl_native_handle_zcode_commons_epoch(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace", "epoch"};
    const struct json_value *value = request ? json_get(request->input, "epoch")
                                             : NULL;
    struct vcs_zcode_commons_projection *projection =
        zcc_build(request, reply, keys, 2);
    if (!projection) return;
    if (!value || value->type != JSON_INT || json_get_int(value) < 0) {
        vcs_zcode_commons_projection_free(projection);
        zcc_fail(reply, "BAD_EPOCH", "epoch must be a nonnegative integer");
        return;
    }
    uint64_t wanted = (uint64_t)json_get_int(value);
    const struct vcs_zcode_commons_epoch_entry *found = NULL;
    for (size_t i = 0; i < vcs_zcode_commons_projection_epoch_count(projection);
         i++) {
        const struct vcs_zcode_commons_epoch_entry *entry =
            vcs_zcode_commons_projection_epoch_at(projection, i);
        if (entry->epoch == wanted) { found = entry; break; }
    }
    if (!found) {
        vcs_zcode_commons_projection_free(projection);
        zcc_fail(reply, "EPOCH_NOT_FOUND", "epoch absent from workspace CAS");
        return;
    }
    zcc_hex(&reply->data, "epoch_creation_root", found->root);
    zcc_hex(&reply->data, "previous_epoch_creation_root", found->previous_root);
    (void)json_push_kv_int(&reply->data, "epoch", (int64_t)found->epoch);
    (void)json_push_kv_int(&reply->data, "cap_atoms", (int64_t)found->cap_atoms);
    (void)json_push_kv_int(&reply->data, "minted_atoms",
                           (int64_t)found->minted_atoms);
    (void)json_push_kv_int(&reply->data, "unissued_atoms",
                           (int64_t)found->unissued_atoms);
    (void)json_push_kv_int(&reply->data, "attribution_count",
                           found->attribution_count);
    vcs_zcode_commons_projection_free(projection);
}

static void zcc_render_creation(
    struct json_value *data,
    const struct vcs_zcode_creation_attribution_v1 *attribution,
    const uint8_t root[32])
{
    zcc_hex(data, "creation_attribution_root", root);
    (void)json_push_kv_int(data, "epoch", (int64_t)attribution->epoch);
    (void)json_push_kv_int(data, "award_atoms",
                           (int64_t)attribution->award_atoms);
    (void)json_push_kv_str(data, "category",
                           zcc_category_name(attribution->category));
    (void)json_push_kv_int(data, "challenge_opening_height",
                           (int64_t)attribution->challenge_opening_height);
    (void)json_push_kv_int(data, "challenge_maturity_height",
                           (int64_t)attribution->challenge_maturity_height);
    (void)json_push_kv_int(data, "challenge_maturity_mtp",
                           attribution->challenge_maturity_mtp);
    zcc_hex(data, "contributor_binding_root",
            attribution->contributor_binding_root);
    zcc_hex(data, "task_root", attribution->task_root);
    zcc_hex(data, "candidate_root", attribution->candidate_root);
    zcc_hex(data, "proof_policy_root", attribution->proof_policy_root);
    zcc_hex(data, "proof_set_root", attribution->proof_set_root);
    zcc_hex(data, "proven_lane_root", attribution->proven_lane_root);
    zcc_hex(data, "score_receipt_root", attribution->score_receipt_root);
    zcc_hex(data, "package_root", attribution->package_root);
    zcc_hex(data, "release_root", attribution->release_root);
    zcc_hex(data, "license_evidence_root", attribution->license_evidence_root);
    (void)json_push_kv_bool(data, "patronage_receipt_is_ownership", false);
}

void zcl_native_handle_zcode_commons_creation_show(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace", "root"};
    uint8_t root[32], derived[32], *wire = NULL; size_t wire_len = 0;
    const char *workspace = request ? zcc_str(request->input, "workspace")
                                    : NULL;
    if (!request || !reply || !workspace ||
        !zcc_keys(request->input, keys, 2) ||
        !zcc_root(request->input, "root", root) ||
        vcs_object_load_raw_bounded(workspace, root,
            VCS_ZCODE_CREATION_ATTRIBUTION_WIRE_BYTES, &wire, &wire_len) != 0) {
        free(wire);
        zcc_fail(reply, "CREATION_NOT_FOUND",
                 "exact creation root absent or input malformed");
        return;
    }
    struct vcs_zcode_creation_attribution_v1 attribution;
    bool ok = vcs_zcode_creation_attribution_parse(wire, wire_len,
            &attribution) == VCS_ZCODE_CREATION_OK &&
        vcs_zcode_creation_attribution_root(&attribution, derived) ==
            VCS_ZCODE_CREATION_OK && memcmp(root, derived, 32) == 0;
    free(wire);
    if (!ok) {
        zcc_fail(reply, "CREATION_CORRUPT",
                 "stored creation wire did not rederive its address");
        return;
    }
    zcc_render_creation(&reply->data, &attribution, root);
}

void zcl_native_handle_zcode_commons_lineage(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace", "package_root"};
    uint8_t package[32];
    struct vcs_zcode_commons_projection *projection =
        zcc_build(request, reply, keys, 2);
    if (!projection) return;
    if (!zcc_root(request->input, "package_root", package)) {
        vcs_zcode_commons_projection_free(projection);
        zcc_fail(reply, "BAD_PACKAGE_ROOT", "full lowercase package root required");
        return;
    }
    struct json_value rows; json_init(&rows); json_set_array(&rows);
    size_t matches = 0;
    for (size_t i = 0;
         i < vcs_zcode_commons_projection_creation_count(projection); i++) {
        const struct vcs_zcode_commons_creation_entry *entry =
            vcs_zcode_commons_projection_creation_at(projection, i);
        if (memcmp(entry->package_root, package, 32) != 0) continue;
        struct json_value row; json_init(&row); json_set_object(&row);
        zcc_hex(&row, "creation_attribution_root", entry->root);
        zcc_hex(&row, "release_root", entry->release_root);
        (void)json_push_kv_int(&row, "epoch", (int64_t)entry->epoch);
        (void)json_push_kv_int(&row, "award_atoms",
                               (int64_t)entry->award_atoms);
        (void)json_push_kv_str(&row, "category",
                               zcc_category_name(entry->category));
        (void)json_push_back(&rows, &row); json_free(&row); matches++;
    }
    zcc_hex(&reply->data, "package_root", package);
    (void)json_push_kv_int(&reply->data, "count", (int64_t)matches);
    (void)json_push_kv(&reply->data, "creations", &rows); json_free(&rows);
    (void)json_push_kv_bool(&reply->data, "implies_package_ownership", false);
    vcs_zcode_commons_projection_free(projection);
}
