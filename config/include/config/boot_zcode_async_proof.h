/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Background requester-local dispatch over the existing work swarm. */

#ifndef ZCL_CONFIG_BOOT_ZCODE_ASYNC_PROOF_H
#define ZCL_CONFIG_BOOT_ZCODE_ASYNC_PROOF_H

#include <stdint.h>

struct boot_svc_ctx;
struct vcs_zcode_work_node;
struct node_db;
struct vcs_zcode_work_request_v1;
struct vcs_zcode_work_result_v1;

void boot_zcode_async_proof_tick(
    struct boot_svc_ctx *svc, struct vcs_zcode_work_node *work, int64_t now);
bool boot_zcode_async_proof_workspace(
    struct node_db *ndb, const struct vcs_zcode_work_request_v1 *request,
    char out[4096]);
bool boot_zcode_async_proof_observe_result(
    struct node_db *ndb, uint64_t peer,
    const struct vcs_zcode_work_request_v1 *request,
    const struct vcs_zcode_work_result_v1 *result,
    const char *receipt_root, int64_t now);

#endif /* ZCL_CONFIG_BOOT_ZCODE_ASYNC_PROOF_H */
