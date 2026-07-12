/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "devloop.h"

#include "controllers/agent_impact_rules.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct hotswap_eligible_entry {
    const char *path;
    const char *probe;
};

static const struct hotswap_eligible_entry g_hotswap_eligible[] = {
#define HOTSWAP_ELIGIBLE(path_) { .path = path_, .probe =
#define HOTSWAP_PROBE(probe_) probe_ },
#include "../../config/hotswap_eligible.def"
#undef HOTSWAP_PROBE
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

static const struct hotswap_eligible_entry *hotswap_entry(const char *path)
{
    for (size_t i = 0; i < sizeof(g_hotswap_eligible) /
                            sizeof(g_hotswap_eligible[0]); i++) {
        if (strcmp(path, g_hotswap_eligible[i].path) == 0)
            return &g_hotswap_eligible[i];
    }
    return NULL;
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

/* The SEALED consensus core: the exact surface `core/MANIFEST.sha3` pins
 * (`git ls-files core/`). This is intentionally the whole `core/` tree —
 * broader than path_is_consensus_risk()'s prefix list, which predates the
 * core-split absorption and does not name core/math. Keeping this a single
 * "core/" prefix keeps the fast-loop refusal aligned with the seal manifest
 * by construction (there is no second list to drift). */
bool zcl_devloop_path_is_sealed_core(const char *path)
{
    return path && strncmp(path, "core/", 5) == 0;
}

bool zcl_devloop_path_is_relevant(const char *path)
{
    if (!path || !path[0])
        return false;
    size_t len = strlen(path);
    if (path[len - 1] == '~' || strstr(path, ".swp") ||
        strstr(path, ".tmp") || strstr(path, "/build/") ||
        strncmp(path, "build/", 6) == 0 ||
        strncmp(path, ".git/", 5) == 0)
        return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    /* Transient lint/shape-gate fixtures: test_make_lint_gates.c writes
     * `_*fixture*` .c files under app/, lib/, and domain/ to exercise the
     * path gates, then deletes them. A leading-underscore basename that
     * mentions "fixture" is never a real edit; reacting to it fires a phantom
     * reload cycle on every test-suite run (the file is already gone by the
     * time the cycle rebuilds). No tracked source matches this shape — the
     * real fixture sources under lib/test/fixtures/ have no leading '_'. */
    if (base[0] == '_' && strstr(base, "fixture"))
        return false;
    if (strcmp(base, "Makefile") == 0)
        return true;
    const char *dot = strrchr(base, '.');
    return dot &&
        (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
         strcmp(dot, ".def") == 0 || strcmp(dot, ".md") == 0 ||
         strcmp(dot, ".mk") == 0 || strcmp(dot, ".service") == 0);
}

static bool path_is_consensus_risk(const char *path)
{
    static const char *const prefixes[] = {
        "core/consensus/", "core/params/", "core/chainparams/",
        "lib/validation/", "lib/chain/", "lib/primitives/", "lib/crypto/",
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
    const struct hotswap_eligible_entry *single_hotswap = NULL;
    struct agent_impact_acc impact = {0};
    for (size_t i = 0; i < file_count; i++) {
        if (!path_is_safe(files[i]))
            return false;
        all_docs = all_docs && path_is_docs(files[i]);
        const struct hotswap_eligible_entry *entry = hotswap_entry(files[i]);
        all_hotswap = all_hotswap && entry != NULL;
        if (file_count == 1)
            single_hotswap = entry;
        bool sealed = zcl_devloop_path_is_sealed_core(files[i]);
        out->sealed_core = out->sealed_core || sealed;
        /* A sealed-core file is always heaviest-proof: even core/math (not in
         * the legacy consensus_risk prefix list) decides block/tx validity. */
        out->consensus_risk = out->consensus_risk || sealed ||
                              path_is_consensus_risk(files[i]);
        (void)agent_impact_apply_shared_rules(files[i], &impact);
    }
    out->docs_only = all_docs;

    if (all_docs) {
        out->reason = "documentation_only";
        return true;
    }
    if (file_count == 1 && all_hotswap && single_hotswap) {
        out->action = ZCL_DEVLOOP_HOTSWAP;
        out->action_name = "hotswap";
        out->reason = "single_stateless_provider";
        out->proof_group = "hotswap_simnet";
        out->probe_tool = single_hotswap->probe;
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
                 ",\"consensus_risk\":%s,\"sealed_core\":%s,\"docs_only\":%s,"
                 "\"files\":[",
                 plan.consensus_risk ? "true" : "false",
                 plan.sealed_core ? "true" : "false",
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

bool zcl_devloop_unseal_token_present(const char *repo_root)
{
    /* READ-ONLY presence check of <repo_root>/.core-unseal-token — the
     * one-shot token `make core-unseal REASON=…` mints. We deliberately do
     * NOT open, mint, or unlink it: `make core-seal` is the sole consumer, so
     * one unseal authorizes exactly one landed commit (which may span several
     * iterative dev-cycles while the author converges the sealed edit), never
     * one dev-cycle. Path traversal is a non-issue: repo_root is the native
     * source root, not attacker input. */
    const char *root = (repo_root && repo_root[0]) ? repo_root : ".";
    char path[ZCL_DEVLOOP_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.core-unseal-token", root);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    return access(path, F_OK) == 0;
}

size_t zcl_devloop_refusal_json(const char *const *files, size_t file_count,
                                char *out, size_t out_sz)
{
    size_t pos = 0;
    if (!out || out_sz == 0 || (file_count > 0 && !files))
        return 0;

    /* A zcl.dev_cycle.v1 verdict whose status is the new "refused" — the
     * structured envelope the fast loop emits (stdout + persisted verdict)
     * when a changed-file set touches the sealed consensus core with no valid
     * unseal token. "Sealed != frozen": the envelope always names the
     * elevated procedure, so it never dead-ends. */
    if (!appendf(out, out_sz, &pos,
                 "{\"schema\":\"zcl.dev_cycle.v1\",\"producer\":\"native\","
                 "\"status\":\"refused\",\"reason\":\"sealed_consensus_core\","
                 "\"paths\":["))
        return 0;

    /* Only the sealed members that actually triggered the refusal. */
    bool first = true;
    for (size_t i = 0; i < file_count; i++) {
        if (!zcl_devloop_path_is_sealed_core(files[i]))
            continue;
        if ((!first && !appendf(out, out_sz, &pos, ",")) ||
            !append_json_string(out, out_sz, &pos, files[i]))
            return 0;
        first = false;
    }

    if (!appendf(out, out_sz, &pos,
                 "],\"manifest\":\"core/MANIFEST.sha3\","
                 "\"law\":\"docs/CONSENSUS_PARITY_DOCTRINE.md\","
                 "\"unseal\":\"make core-unseal REASON=... "
                 "(owner-gated; see core/UNSEAL.md)\","
                 "\"elevated_procedure\":\"full make ci + copy-prove + "
                 "owner-gated deploy\","
                 "\"agent_next_action\":\"edit outside core/, or run the "
                 "owner-gated unseal ritual (make core-unseal) for a "
                 "consensus-parity fix\"}"))
        return 0;
    return pos;
}
