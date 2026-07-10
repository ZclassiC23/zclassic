/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE
#include "devloop.h"

#include "platform/time_compat.h"
#include "sim/social_app_sim.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool token_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) > 63)
        return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-')
            return false;
    }
    return true;
}

static bool resolve_root(const char *requested, char out[PATH_MAX])
{
    return requested && realpath(requested, out) != NULL;
}

static void emit_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : "");
         *p; p++) {
        if (*p == '"' || *p == '\\')
            putchar('\\');
        if (*p >= 0x20)
            putchar(*p);
    }
    putchar('"');
}

static bool macro_arg(const char *line, const char *macro,
                      char *out, size_t out_sz)
{
    size_t mlen = strlen(macro);
    if (strncmp(line, macro, mlen) != 0 || line[mlen] != '(')
        return false;
    const char *start = line + mlen + 1;
    if (*start == '"')
        start++;
    const char *end = start;
    while (*end && *end != ')' && *end != '"' && *end != ',')
        end++;
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= out_sz)
        return false;
    memcpy(out, start, len);
    out[len] = 0;
    return true;
}

int zcl_devloop_app_describe(const char *repo_root, const char *app_id)
{
    char root[PATH_MAX], path[PATH_MAX];
    if (!token_ok(app_id) || !resolve_root(repo_root, root)) {
        fprintf(stderr, "[devloop] app describe: invalid app or root\n");
        return 2;
    }
    int pn = snprintf(path, sizeof(path), "%s/apps/%s/app.def", root, app_id);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return 2;
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("{\"schema\":\"zcl.dev_app.v1\",\"status\":\"not_found\","
               "\"app_id\":");
        emit_string(app_id);
        printf(",\"agent_next_action\":\"dev app plan <app> <resource>\"}\n");
        return 1;
    }

    char caps[32][64], resources[32][64], sims[32][96];
    size_t cap_count = 0, resource_count = 0, sim_count = 0;
    char web[128] = "", znam[128] = "", line[1024], arg[128];
    bool onion = false;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (cap_count < 32 && macro_arg(p, "ZCL_APP_CAPABILITY", arg, sizeof(arg)))
            snprintf(caps[cap_count++], sizeof(caps[0]), "%s", arg);
        else if (resource_count < 32 &&
                 macro_arg(p, "ZCL_APP_RESOURCE", arg, sizeof(arg)))
            snprintf(resources[resource_count++], sizeof(resources[0]), "%s", arg);
        else if (sim_count < 32 && macro_arg(p, "ZCL_APP_SIM", arg, sizeof(arg)))
            snprintf(sims[sim_count++], sizeof(sims[0]), "%s", arg);
        else if (macro_arg(p, "ZCL_APP_WEB_MOUNT", arg, sizeof(arg)))
            snprintf(web, sizeof(web), "%s", arg);
        else if (macro_arg(p, "ZCL_APP_ZNAM", arg, sizeof(arg)))
            snprintf(znam, sizeof(znam), "%s", arg);
        else if (strstr(p, "ZCL_APP_ONION(true)"))
            onion = true;
    }
    fclose(f);

    printf("{\"schema\":\"zcl.dev_app.v1\",\"status\":\"ok\",\"app_id\":");
    emit_string(app_id);
    printf(",\"boundary\":\"core_owns_truth_apps_consume_capabilities\","
           "\"capabilities\":[");
    for (size_t i = 0; i < cap_count; i++) {
        if (i) putchar(',');
        emit_string(caps[i]);
    }
    printf("],\"resources\":[");
    for (size_t i = 0; i < resource_count; i++) {
        if (i) putchar(',');
        emit_string(resources[i]);
    }
    printf("],\"bindings\":{\"web\":");
    emit_string(web);
    printf(",\"onion\":%s,\"znam\":", onion ? "true" : "false");
    emit_string(znam);
    printf("},\"simulations\":[");
    for (size_t i = 0; i < sim_count; i++) {
        if (i) putchar(',');
        emit_string(sims[i]);
    }
    printf("],\"agent_next_action\":\"edit apps/%s; native watch owns proof and publish\"}\n",
           app_id);
    return 0;
}

int zcl_devloop_app_plan(const char *repo_root, const char *app_id,
                         const char *resource)
{
    char root[PATH_MAX];
    if (!resolve_root(repo_root, root) || !token_ok(app_id) ||
        !token_ok(resource)) {
        fprintf(stderr, "[devloop] app plan: invalid app, resource, or root\n");
        return 2;
    }
    (void)root;
    printf("{\"schema\":\"zcl.dev_app_plan.v1\",\"status\":\"planned\","
           "\"app_id\":");
    emit_string(app_id);
    printf(",\"resource\":");
    emit_string(resource);
    printf(",\"files\":[");
    const char *shapes[] = {
        "models", "controllers", "services", "events", "jobs",
        "projections", "views", "sim"
    };
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        if (i) putchar(',');
        char path[256];
        snprintf(path, sizeof(path), "apps/%s/%s/%s.c",
                 app_id, shapes[i], resource);
        emit_string(path);
    }
    printf("],\"bindings\":[\"web\",\"onion\",\"znam\"],"
           "\"required_proofs\":[\"same_seed_replay\","
           "\"partition_rejoin_convergence\",\"invalid_signature_rejection\"],"
           "\"forbidden\":[\"consensus_mutation\",\"wallet_keys\","
           "\"raw_storage\",\"raw_sockets\",\"boot_ownership\"],"
           "\"agent_next_action\":\"materialize this conventional slice through dev app scaffold\"}\n");
    return 0;
}

int zcl_devloop_app_simulate(const char *app_id, uint64_t seed)
{
    if (!app_id || strcmp(app_id, "social") != 0 || seed == 0) {
        fprintf(stderr, "[devloop] app simulate: unknown app or invalid seed\n");
        return 2;
    }
    int64_t started_us = platform_time_monotonic_us();
    struct zcl_social_sim_report first, replay;
    bool first_ok = zcl_social_app_sim_run(seed, &first);
    bool replay_ok = zcl_social_app_sim_run(seed, &replay);
    bool identical = replay_ok && first.transcript == replay.transcript &&
                     first.deliveries == replay.deliveries &&
                     first.rejected_invalid == replay.rejected_invalid;
    int64_t elapsed_us = platform_time_monotonic_us() - started_us;
    bool ok = first_ok && identical;
    printf("{\"schema\":\"zcl.dev_app_sim.v1\",\"app_id\":\"social\","
           "\"status\":\"%s\",\"seed\":\"0x%016llx\","
           "\"transcript\":\"0x%016llx\",\"two_run_wall_us\":%lld,"
           "\"deliveries\":%u,\"rejected_invalid\":%u,"
           "\"proofs\":{\"censorship_bypassed\":%s,"
           "\"partition_rejoin_converged\":%s,\"late_joiner_caught_up\":%s,"
           "\"invalid_signature_rejected\":%s,\"same_seed_replay\":%s}}\n",
           ok ? "passed" : "failed", (unsigned long long)seed,
           (unsigned long long)first.transcript, (long long)elapsed_us,
           first.deliveries, first.rejected_invalid,
           first.censorship_bypassed ? "true" : "false",
           first.partition_rejoin_converged ? "true" : "false",
           first.late_joiner_caught_up ? "true" : "false",
           first.invalid_signature_rejected ? "true" : "false",
           identical ? "true" : "false");
    return ok ? 0 : 1;
}
