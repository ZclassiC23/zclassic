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

#include "base/log_macros.h"
#include "session/agent_broker.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MVS_TAG "metaverse.agent.service"
#define MVS_DIR_MAX 384

static bool dir_ok(const char *dir, char *why, size_t why_cap)
{
    if (!dir || !dir[0]) {
        snprintf(why, why_cap, "BAD_ARGS");
        LOG_FAIL(MVS_TAG, "dir is required");
    }
    if (dir[0] != '/') {
        snprintf(why, why_cap, "BAD_ARGS");
        LOG_FAIL(MVS_TAG, "dir must be an absolute path, got '%s'", dir);
    }
    if (strnlen(dir, MVS_DIR_MAX + 1) > MVS_DIR_MAX) {
        snprintf(why, why_cap, "BAD_ARGS");
        LOG_FAIL(MVS_TAG, "dir longer than %d bytes", MVS_DIR_MAX);
    }
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(why, why_cap, "NOT_A_DIR");
        LOG_FAIL(MVS_TAG, "'%s' is not an existing directory", dir);
    }
    return true;
}

size_t metaverse_agent_service_status(const char *dir, char *out,
                                      size_t out_cap, char *why,
                                      size_t why_cap)
{
    if (!out || out_cap == 0 || !why || why_cap == 0)
        return 0;
    why[0] = '\0';
    if (!dir_ok(dir, why, why_cap))
        return 0;

    size_t n = agent_broker_render_status_json(dir, out, out_cap);
    if (n == 0) {
        snprintf(why, why_cap, "RENDER_FAILED");
        LOG_RETURN(0, MVS_TAG, "status document for '%s' did not fit %zu bytes",
                   dir, out_cap);
    }
    return n;
}

size_t metaverse_agent_service_audit(const char *dir, size_t limit, char *out,
                                     size_t out_cap, char *why, size_t why_cap)
{
    if (!out || out_cap == 0 || !why || why_cap == 0)
        return 0;
    why[0] = '\0';
    if (!dir_ok(dir, why, why_cap))
        return 0;

    size_t n = agent_audit_render_json(dir, limit, out, out_cap);
    if (n == 0) {
        snprintf(why, why_cap, "RENDER_FAILED");
        LOG_RETURN(0, MVS_TAG, "audit document for '%s' did not fit %zu bytes",
                   dir, out_cap);
    }
    return n;
}
