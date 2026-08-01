/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Import immutable ZCODE task/candidate authority for swarm work. */

#include "config/boot_zcode_work_authority.h"

#include "vcs/zcode_candidate_bundle.h"
#include "vcs/zcode_candidate_tree.h"
#include "vcs/zcode_task_authority_bundle.h"
#include "vcs/zcode_work_context.h"

struct zcl_result boot_zcode_work_authority_import(
    struct vcs_package_store *store, const uint8_t context_root[32],
    const char *workspace, const struct vcs_zcode_work_context_v1 *context)
{
    if (!store || !context_root || !workspace || !context)
        return ZCL_ERR(-1, "authority import requires workspace and context");
    enum vcs_zcode_task_authority_result task =
        context->task_authority_len > 0
            ? vcs_zcode_task_authority_bundle_import(
                  workspace, &context->task, context->task_authority,
                  context->task_authority_len)
            : VCS_ZCODE_TASK_AUTHORITY_CAS;
    if (task != VCS_ZCODE_TASK_AUTHORITY_OK)
        return ZCL_ERR(-1, "task authority: %s",
            vcs_zcode_task_authority_result_string(task));
    enum vcs_zcode_candidate_bundle_result candidate =
        context->candidate_authority_len > 0
            ? vcs_zcode_candidate_bundle_import(
                  workspace, &context->task, &context->candidate,
                  context->candidate_authority,
                  context->candidate_authority_len)
            : VCS_ZCODE_CANDIDATE_BUNDLE_SHAPE;
    if (candidate != VCS_ZCODE_CANDIDATE_BUNDLE_OK)
        return ZCL_ERR(-1, "candidate authority: %s",
            vcs_zcode_candidate_bundle_result_string(candidate));
    enum vcs_zcode_candidate_tree_result tree =
        vcs_zcode_candidate_tree_import(
            store, context_root, workspace, &context->task,
            &context->candidate);
    return tree == VCS_ZCODE_CANDIDATE_TREE_OK
        ? ZCL_OK : ZCL_ERR(-1, "candidate tree: %s",
            vcs_zcode_candidate_tree_result_string(tree));
}
