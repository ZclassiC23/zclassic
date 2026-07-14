/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Read-only App-manifest inspection for the dev tree (describe / plan /
 * simulate). Each operation has a bounded JSON *buffer producer* — the single
 * source of truth — plus a thin stdout print wrapper used by the checkout-local
 * devloop dispatcher. The producers are release-safe (no process spawn, no
 * mutation), so the Wave 2.2 registry handlers in
 * tools/command/native_dev_command.c bind straight to them. */

#define _GNU_SOURCE
#include "devloop.h"

#include "platform/time_compat.h"
#include "sim/social_app_sim.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── bounded JSON appender ─────────────────────────────────────────────── */
struct devbuf {
    char *out;
    size_t cap;
    size_t len;
    bool ok;
};

static void devbuf_init(struct devbuf *b, char *out, size_t cap)
{
    b->out = out;
    b->cap = cap;
    b->len = 0;
    b->ok = out != NULL && cap != 0;
    if (b->ok)
        out[0] = 0;
}

static void devbuf_addf(struct devbuf *b, const char *fmt, ...)
{
    if (!b->ok || b->len >= b->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->out + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->ok = false;
        return;
    }
    b->len += (size_t)n;
}

static void devbuf_addstr(struct devbuf *b, const char *s)
{
    devbuf_addf(b, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            devbuf_addf(b, "\\%c", *p);
        else if (*p < 0x20)
            devbuf_addf(b, "\\u%04x", *p);
        else
            devbuf_addf(b, "%c", *p);
    }
    devbuf_addf(b, "\"");
}

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

/* ── describe ──────────────────────────────────────────────────────────── */
size_t zcl_devloop_app_describe_json(const char *repo_root, const char *app_id,
                                     char *out, size_t out_sz)
{
    char root[PATH_MAX], path[PATH_MAX];
    if (!token_ok(app_id) || !resolve_root(repo_root, root))
        return 0;
    int pn = snprintf(path, sizeof(path), "%s/apps/%s/app.def", root, app_id);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return 0;

    struct devbuf b;
    devbuf_init(&b, out, out_sz);
    FILE *f = fopen(path, "r");
    if (!f) {
        devbuf_addf(&b, "{\"schema\":\"zcl.dev_app.v1\",\"status\":"
                        "\"not_found\",\"app_id\":");
        devbuf_addstr(&b, app_id);
        devbuf_addf(&b, ",\"agent_next_action\":"
                        "\"dev app plan <app> <resource>\"}");
        return b.ok ? b.len : 0;
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

    devbuf_addf(&b, "{\"schema\":\"zcl.dev_app.v1\",\"status\":\"ok\","
                    "\"app_id\":");
    devbuf_addstr(&b, app_id);
    devbuf_addf(&b, ",\"boundary\":"
                    "\"core_owns_truth_apps_consume_capabilities\","
                    "\"capabilities\":[");
    for (size_t i = 0; i < cap_count; i++) {
        if (i) devbuf_addf(&b, ",");
        devbuf_addstr(&b, caps[i]);
    }
    devbuf_addf(&b, "],\"resources\":[");
    for (size_t i = 0; i < resource_count; i++) {
        if (i) devbuf_addf(&b, ",");
        devbuf_addstr(&b, resources[i]);
    }
    devbuf_addf(&b, "],\"bindings\":{\"web\":");
    devbuf_addstr(&b, web);
    devbuf_addf(&b, ",\"onion\":%s,\"znam\":", onion ? "true" : "false");
    devbuf_addstr(&b, znam);
    devbuf_addf(&b, "},\"simulations\":[");
    for (size_t i = 0; i < sim_count; i++) {
        if (i) devbuf_addf(&b, ",");
        devbuf_addstr(&b, sims[i]);
    }
    devbuf_addf(&b, "],\"agent_next_action\":"
                    "\"edit apps/%s; native watch owns proof and publish\"}",
                app_id);
    return b.ok ? b.len : 0;
}

int zcl_devloop_app_describe(const char *repo_root, const char *app_id)
{
    char body[8192];
    size_t n = zcl_devloop_app_describe_json(repo_root, app_id, body,
                                             sizeof(body));
    if (n == 0) {
        fprintf(stderr, "[devloop] app describe: invalid app or root\n");
        return 2;
    }
    printf("%s\n", body);
    return strstr(body, "\"status\":\"ok\"") ? 0 : 1;
}

/* ── plan ──────────────────────────────────────────────────────────────── */
size_t zcl_devloop_app_plan_json(const char *repo_root, const char *app_id,
                                 const char *resource, char *out, size_t out_sz)
{
    char root[PATH_MAX];
    if (!resolve_root(repo_root, root) || !token_ok(app_id) ||
        !token_ok(resource))
        return 0;
    (void)root;

    struct devbuf b;
    devbuf_init(&b, out, out_sz);
    devbuf_addf(&b, "{\"schema\":\"zcl.dev_app_plan.v1\",\"status\":"
                    "\"planned\",\"app_id\":");
    devbuf_addstr(&b, app_id);
    devbuf_addf(&b, ",\"resource\":");
    devbuf_addstr(&b, resource);
    devbuf_addf(&b, ",\"files\":[");
    const char *shapes[] = {
        "models", "controllers", "services", "events", "jobs",
        "projections", "views", "sim"
    };
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        if (i) devbuf_addf(&b, ",");
        char path[256];
        snprintf(path, sizeof(path), "apps/%s/%s/%s.c",
                 app_id, shapes[i], resource);
        devbuf_addstr(&b, path);
    }
    devbuf_addf(&b, "],\"bindings\":[\"web\",\"onion\",\"znam\"],"
                    "\"required_proofs\":[\"same_seed_replay\","
                    "\"partition_rejoin_convergence\","
                    "\"invalid_signature_rejection\"],"
                    "\"forbidden\":[\"consensus_mutation\",\"wallet_keys\","
                    "\"raw_storage\",\"raw_sockets\",\"boot_ownership\"],"
                    "\"agent_next_action\":\"materialize this conventional "
                    "slice through dev app scaffold\"}");
    return b.ok ? b.len : 0;
}

int zcl_devloop_app_plan(const char *repo_root, const char *app_id,
                         const char *resource)
{
    char body[4096];
    size_t n = zcl_devloop_app_plan_json(repo_root, app_id, resource, body,
                                         sizeof(body));
    if (n == 0) {
        fprintf(stderr, "[devloop] app plan: invalid app, resource, or root\n");
        return 2;
    }
    printf("%s\n", body);
    return 0;
}

/* ── simulate ──────────────────────────────────────────────────────────── */
size_t zcl_devloop_app_simulate_json(const char *app_id, uint64_t seed,
                                     char *out, size_t out_sz)
{
    if (!app_id || strcmp(app_id, "social") != 0 || seed == 0)
        return 0;
    int64_t started_us = platform_time_monotonic_us();
    struct zcl_social_sim_report first, replay;
    bool first_ok = zcl_social_app_sim_run(seed, &first);
    bool replay_ok = zcl_social_app_sim_run(seed, &replay);
    bool identical = replay_ok && first.transcript == replay.transcript &&
                     first.deliveries == replay.deliveries &&
                     first.rejected_invalid == replay.rejected_invalid &&
                     first.real_secp256k1_verified ==
                         replay.real_secp256k1_verified &&
                     first.tampered_payload_rejected ==
                         replay.tampered_payload_rejected &&
                     first.wrong_author_rejected ==
                         replay.wrong_author_rejected &&
                     first.forged_event_id_distinct ==
                         replay.forged_event_id_distinct;
    int64_t elapsed_us = platform_time_monotonic_us() - started_us;
    bool ok = first_ok && identical;

    struct devbuf b;
    devbuf_init(&b, out, out_sz);
    devbuf_addf(&b, "{\"schema\":\"zcl.dev_app_sim.v1\",\"app_id\":\"social\","
                    "\"status\":\"%s\",\"seed\":\"0x%016llx\","
                    "\"transcript\":\"0x%016llx\",\"two_run_wall_us\":%lld,"
                    "\"deliveries\":%u,\"rejected_invalid\":%u,"
                    "\"proofs\":{\"censorship_bypassed\":%s,"
                    "\"partition_rejoin_converged\":%s,"
                    "\"late_joiner_caught_up\":%s,"
                    "\"invalid_signature_rejected\":%s,"
                    "\"real_secp256k1_verified\":%s,"
                    "\"tampered_payload_rejected\":%s,"
                    "\"wrong_author_rejected\":%s,"
                    "\"forged_event_id_distinct\":%s,"
                    "\"same_seed_replay\":%s},"
                    "\"contained\":[\"wallet_signing_broker\","
                    "\"event_store\",\"replay_head_store\","
                    "\"p2p_transport\",\"chat_encryption\"]}",
                ok ? "passed" : "failed", (unsigned long long)seed,
                (unsigned long long)first.transcript, (long long)elapsed_us,
                first.deliveries, first.rejected_invalid,
                first.censorship_bypassed ? "true" : "false",
                first.partition_rejoin_converged ? "true" : "false",
                first.late_joiner_caught_up ? "true" : "false",
                first.invalid_signature_rejected ? "true" : "false",
                first.real_secp256k1_verified ? "true" : "false",
                first.tampered_payload_rejected ? "true" : "false",
                first.wrong_author_rejected ? "true" : "false",
                first.forged_event_id_distinct ? "true" : "false",
                identical ? "true" : "false");
    return b.ok ? b.len : 0;
}

int zcl_devloop_app_simulate(const char *app_id, uint64_t seed)
{
    char body[4096];
    size_t n = zcl_devloop_app_simulate_json(app_id, seed, body, sizeof(body));
    if (n == 0) {
        fprintf(stderr, "[devloop] app simulate: unknown app or invalid seed\n");
        return 2;
    }
    printf("%s\n", body);
    return strstr(body, "\"status\":\"passed\"") ? 0 : 1;
}
