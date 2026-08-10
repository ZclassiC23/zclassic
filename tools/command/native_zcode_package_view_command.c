/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Static host contract and no-state guide for pure package views. */

#include "command/native_command.h"

#include "hotswap/hotswap_service.h"
#include "json/json.h"
#include "services/zcode_package_view_service.h"

#include <stdio.h>
#include <string.h>

static bool package_view_frozen_kat(const void *opaque, char *why,
                                    size_t why_sz)
{
    const struct zcode_package_view_service_v1 *service = opaque;
    struct vcs_package_index_entry entry = {0};
    struct zcode_package_view_entry_v1 rendered;
    struct zcode_package_guide_result_v1 guide;
    (void)snprintf(entry.release_id_hex, sizeof(entry.release_id_hex), "%064x",
                   1);
    (void)snprintf(entry.package_root_hex, sizeof(entry.package_root_hex),
                   "%064x", 2);
    (void)snprintf(entry.name, sizeof(entry.name), "%s", "alice/ring");
    (void)snprintf(entry.semver, sizeof(entry.semver), "%s", "1.2.3");
    (void)snprintf(entry.license, sizeof(entry.license), "%s", "Apache-2.0");
    (void)snprintf(entry.publisher_hex, sizeof(entry.publisher_hex), "%066x",
                   3);
    (void)snprintf(entry.chain_id, sizeof(entry.chain_id), "%s", "main");
    entry.publisher_sequence = 7;
    entry.manifest_present = true;
    entry.file_count = 12;
    entry.total_bytes = 3456;
    entry.chunk_total = 4;
    entry.license_present = true;
    entry.executable_count = 1;
    if (!service || !service->render_entry || !service->render_guide ||
        !service->render_entry(&entry, &rendered) || !rendered.valid ||
        strcmp(rendered.name, "alice/ring") != 0 ||
        strcmp(rendered.semver, "1.2.3") != 0 ||
        strcmp(rendered.license, "Apache-2.0") != 0 ||
        rendered.publisher_sequence != 7 || !rendered.manifest_present ||
        rendered.file_count != 12 || rendered.total_bytes != 3456 ||
        rendered.chunk_total != 4 || !rendered.license_present ||
        rendered.executable_count != 1 ||
        !service->render_guide(&guide) || !guide.cas_authority_static ||
        !guide.index_reads_static || !guide.publication_static ||
        !guide.execution_static) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen package entry/authority vector failed");
        return false;
    }
    entry.release_id_hex[0] = '\0';
    if (!service->render_entry(&entry, &rendered) || rendered.valid) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen incomplete package-entry vector failed");
        return false;
    }
    return true;
}

static const struct zcl_hotswap_service_contract k_package_view_contract = {
    .service_id = ZCODE_PACKAGE_VIEW_SERVICE_ID,
    .source_tu = "app/services/src/zcode_package_view_service.c",
    .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
    .vtable_size = sizeof(struct zcode_package_view_service_v1),
    .abi_fingerprint = ZCODE_PACKAGE_VIEW_ABI_FINGERPRINT,
    .schema_fingerprint = ZCODE_PACKAGE_VIEW_SCHEMA_FINGERPRINT,
    .wire_fingerprint = ZCODE_PACKAGE_VIEW_WIRE_FINGERPRINT,
    .kat_fingerprint = ZCODE_PACKAGE_VIEW_KAT_FINGERPRINT,
    .frozen_kat = package_view_frozen_kat,
};

const struct zcl_hotswap_service_contract *
zcl_native_zcode_package_view_service_contract(void)
{
    return &k_package_view_contract;
}

void zcl_native_handle_zcode_package_guide(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !request->input || !reply ||
        request->input->type != JSON_OBJ || request->input->num_children != 0) {
        if (reply) zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_PACKAGE_GUIDE_INPUT", "validate", false, false,
            "zcode package guide accepts no input keys",
            "zcode.package.guide");
        return;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_package_view_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_PACKAGE_VIEW_SERVICE_ID, &lease);
    if (!service) service = zcode_package_view_service_builtin();
    struct zcode_package_guide_result_v1 guide;
    if (!service->render_guide(&guide)) {
        zcl_hotswap_service_release(&lease);
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
            "PACKAGE_VIEW_FAILED", "render", false, false,
            "the pure package view service refused guide rendering",
            "zcode.package.guide");
        return;
    }
    (void)json_push_kv_str(&reply->data, "service_id",
                           ZCODE_PACKAGE_VIEW_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_bool(&reply->data, "cas_authority_static",
                            guide.cas_authority_static);
    (void)json_push_kv_bool(&reply->data, "index_reads_static",
                            guide.index_reads_static);
    (void)json_push_kv_bool(&reply->data, "publication_static",
                            guide.publication_static);
    (void)json_push_kv_bool(&reply->data, "execution_static",
                            guide.execution_static);
    (void)json_push_kv_str(&reply->data, "live_surface", guide.live_surface);
    (void)json_push_kv_str(&reply->data, "static_boundary",
                           guide.static_boundary);
    (void)json_push_kv_str(&reply->data, "next_command", guide.next_command);
    zcl_hotswap_service_release(&lease);
}
