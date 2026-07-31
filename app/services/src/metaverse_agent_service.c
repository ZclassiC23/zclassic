/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * See services/metaverse_agent_service.h. Two readers over a broker directory,
 * plus the one validation both share.
 *
 * The absolute-path requirement is not cosmetic: a relative `dir` would be
 * resolved against whatever working directory the node happens to have, which
 * for a linger service is not the operator's shell. Refusing it makes the
 * subject of the read explicit at the call site.
 */

#include "services/metaverse_agent_service.h"

#include "base/result.h"
#include "session/agent_broker.h"

#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#define MVS_DIR_MAX 384

/* The one validation both readers share. Every refusal names which of the
 * three shape rules the caller broke, because "bad dir" alone does not tell an
 * operator whether to fix the path, the spelling, or the directory. */
static struct zcl_result dir_ok(const char *dir, char *out, size_t out_cap,
                                size_t *out_len)
{
    if (!out || out_cap == 0 || !out_len)
        return ZCL_ERR(MVS_ERR_BAD_ARGS, "out buffer is required");
    *out_len = 0;
    if (!dir || !dir[0])
        return ZCL_ERR(MVS_ERR_BAD_ARGS, "dir is required");
    if (dir[0] != '/')
        return ZCL_ERR(MVS_ERR_BAD_ARGS,
                       "dir must be an absolute path, got '%s'", dir);
    if (strnlen(dir, MVS_DIR_MAX + 1) > MVS_DIR_MAX)
        return ZCL_ERR(MVS_ERR_BAD_ARGS, "dir longer than %d bytes",
                       MVS_DIR_MAX);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return ZCL_ERR(MVS_ERR_NOT_A_DIR,
                       "'%s' is not an existing directory", dir);
    return ZCL_OK;
}

struct zcl_result metaverse_agent_service_status(const char *dir, char *out,
                                                 size_t out_cap,
                                                 size_t *out_len)
{
    struct zcl_result r = dir_ok(dir, out, out_cap, out_len);
    if (!r.ok)
        return r;

    size_t n = agent_broker_render_status_json(dir, out, out_cap);
    if (n == 0)
        return ZCL_ERR(MVS_ERR_RENDER_FAILED,
                       "status document for '%s' did not fit %zu bytes", dir,
                       out_cap);
    *out_len = n;
    return ZCL_OK;
}

struct zcl_result metaverse_agent_service_audit(const char *dir, size_t limit,
                                                char *out, size_t out_cap,
                                                size_t *out_len)
{
    struct zcl_result r = dir_ok(dir, out, out_cap, out_len);
    if (!r.ok)
        return r;

    size_t n = agent_audit_render_json(dir, limit, out, out_cap);
    if (n == 0)
        return ZCL_ERR(MVS_ERR_RENDER_FAILED,
                       "audit document for '%s' did not fit %zu bytes", dir,
                       out_cap);
    *out_len = n;
    return ZCL_OK;
}
