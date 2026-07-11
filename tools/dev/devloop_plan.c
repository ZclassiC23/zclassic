/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "devloop.h"

#include "controllers/agent_impact_rules.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const g_hotswap_eligible[] = {
#define HOTSWAP_ELIGIBLE(path) path,
#include "../../config/hotswap_eligible.def"
#undef HOTSWAP_ELIGIBLE
};

static bool path_is_safe(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || strstr(path, ".."))
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 0x20 || *p == '\\')
            return false;
    }
    return true;
}

static bool path_is_hotswap_eligible(const char *path)
{
    for (size_t i = 0; i < sizeof(g_hotswap_eligible) /
                            sizeof(g_hotswap_eligible[0]); i++) {
        if (strcmp(path, g_hotswap_eligible[i]) == 0)
            return true;
    }
    return false;
}

static bool has_suffix(const char *path, const char *suffix)
{
    size_t plen = path ? strlen(path) : 0;
    size_t slen = suffix ? strlen(suffix) : 0;
    return plen >= slen && memcmp(path + plen - slen, suffix, slen) == 0;
}

static bool path_is_docs(const char *path)
{
    return path &&
        (strncmp(path, "docs/", 5) == 0 ||
         strcmp(path, "README.md") == 0 ||
         strcmp(path, "AGENTS.md") == 0 ||
         has_suffix(path, ".md"));
}

static bool path_is_consensus_risk(const char *path)
{
    static const char *const prefixes[] = {
        "domain/consensus/", "lib/consensus/", "lib/validation/",
        "lib/chain/", "lib/primitives/", "lib/crypto/",
        "lib/sapling/", "app/jobs/",
    };
    if (!path)
        return false;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    }
    return false;
}

bool zcl_devloop_plan_files(const char *const *files, size_t file_count,
                            struct zcl_devloop_plan *out)
{
    if (!out || (file_count > 0 && !files) ||
        file_count > ZCL_DEVLOOP_MAX_FILES)
        return false;

    memset(out, 0, sizeof(*out));
    out->action = ZCL_DEVLOOP_CHECK;
    out->action_name = "check";
    out->reason = "no_changes";
    out->proof_group = "";
    out->probe_tool = "";
    out->file_count = file_count;
    if (file_count == 0)
        return true;

    bool all_docs = true;
    bool all_hotswap = true;
    struct agent_impact_acc impact = {0};
    for (size_t i = 0; i < file_count; i++) {
        if (!path_is_safe(files[i]))
            return false;
        all_docs = all_docs && path_is_docs(files[i]);
        all_hotswap = all_hotswap && path_is_hotswap_eligible(files[i]);
        out->consensus_risk = out->consensus_risk ||
                              path_is_consensus_risk(files[i]);
        (void)agent_impact_apply_shared_rules(files[i], &impact);
    }
    out->docs_only = all_docs;

    if (all_docs) {
        out->reason = "documentation_only";
        return true;
    }
    if (file_count == 1 && all_hotswap) {
        out->action = ZCL_DEVLOOP_HOTSWAP;
        out->action_name = "hotswap";
        out->reason = "single_stateless_provider";
        out->proof_group = "hotswap_simnet";
        out->probe_tool = "zcl_name_list";
        return true;
    }

    out->action = ZCL_DEVLOOP_RELOAD;
    out->action_name = "reload";
    snprintf(out->proof_group_storage, sizeof(out->proof_group_storage), "%s",
             out->consensus_risk
                ? "consensus_parity"
                : (impact.groups_len > 0
                    ? impact.groups[0]
                    : "make_lint_gates"));
    out->proof_group = out->proof_group_storage;
    out->reason = out->consensus_risk
        ? "consensus_or_chain_state_is_never_swappable"
        : (all_hotswap
            ? "multi_provider_generation_not_yet_admitted"
            : "state_or_abi_contract_requires_process_reload");
    return true;
}

static bool appendf(char *out, size_t out_sz, size_t *pos,
                    const char *fmt, ...)
{
    if (!out || !pos || *pos >= out_sz)
        return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, out_sz - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= out_sz - *pos)
        return false;
    *pos += (size_t)n;
    return true;
}

static bool append_json_string(char *out, size_t out_sz, size_t *pos,
                               const char *value)
{
    if (!appendf(out, out_sz, pos, "\""))
        return false;
    for (const unsigned char *p = (const unsigned char *)(value ? value : "");
         *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (!appendf(out, out_sz, pos, "\\%c", *p))
                return false;
        } else if (*p < 0x20) {
            if (!appendf(out, out_sz, pos, "\\u%04x", *p))
                return false;
        } else if (!appendf(out, out_sz, pos, "%c", *p)) {
            return false;
        }
    }
    return appendf(out, out_sz, pos, "\"");
}

size_t zcl_devloop_plan_json(const char *const *files, size_t file_count,
                             char *out, size_t out_sz)
{
    struct zcl_devloop_plan plan;
    size_t pos = 0;
    if (!out || out_sz == 0 ||
        !zcl_devloop_plan_files(files, file_count, &plan))
        return 0;

    if (!appendf(out, out_sz, &pos,
                 "{\"schema\":\"zcl.dev_plan.v1\",\"action\":") ||
        !append_json_string(out, out_sz, &pos, plan.action_name) ||
        !appendf(out, out_sz, &pos, ",\"reason\":") ||
        !append_json_string(out, out_sz, &pos, plan.reason) ||
        !appendf(out, out_sz, &pos,
                 ",\"consensus_risk\":%s,\"docs_only\":%s,\"files\":[",
                 plan.consensus_risk ? "true" : "false",
                 plan.docs_only ? "true" : "false"))
        return 0;

    for (size_t i = 0; i < file_count; i++) {
        if ((i && !appendf(out, out_sz, &pos, ",")) ||
            !append_json_string(out, out_sz, &pos, files[i]))
            return 0;
    }
    if (!appendf(out, out_sz, &pos, "],\"foreground_proof\":") ||
        !append_json_string(out, out_sz, &pos, plan.proof_group) ||
        !appendf(out, out_sz, &pos, ",\"probe\":") ||
        !append_json_string(out, out_sz, &pos, plan.probe_tool) ||
        !appendf(out, out_sz, &pos,
                 ",\"agent_next_action\":\"edit code; the native loop owns execution\"}"))
        return 0;
    return pos;
}
