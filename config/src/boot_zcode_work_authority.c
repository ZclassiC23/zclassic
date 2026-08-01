/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Import immutable ZCODE task/candidate authority for swarm work. */

#include "config/boot_zcode_work_authority.h"

#include "vcs/zcode_candidate_bundle.h"
#include "vcs/zcode_task_authority_bundle.h"
#include "vcs/zcode_work_context.h"

struct zcl_result boot_zcode_work_authority_import(
    const char *workspace, const struct vcs_zcode_work_context_v1 *context)
{
    if (!workspace || !context)
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
    return candidate == VCS_ZCODE_CANDIDATE_BUNDLE_OK
        ? ZCL_OK : ZCL_ERR(-1, "candidate authority: %s",
            vcs_zcode_candidate_bundle_result_string(candidate));
}
