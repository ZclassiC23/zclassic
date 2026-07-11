/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE
#include "devloop.h"

#include "config/command_catalog.h"
#include "kernel/command_registry.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool zcl_devloop_is_method(const char *method)
{
    return method && strcmp(method, "dev") == 0;
}

static const char *source_root(void)
{
    const char *root = getenv("ZCL_DEV_SOURCE_ROOT");
    return root && root[0] ? root : ".";
}

static int print_menu(const char *path)
{
    char body[32768];
    size_t n = zcl_devloop_menu_json(path, body, sizeof(body));
    if (n == 0) {
        fprintf(stderr, "[devloop] menu response exceeded its bound\n");
        return 1;
    }
    printf("%s\n", body);
    return strstr(body, "\"error\"") ? 1 : 0;
}

static bool menu_path(char out[512], const char **args, int start, int nargs)
{
    size_t pos = 0;
    int n = snprintf(out, 512, "dev");
    if (n <= 0)
        return false;
    pos = (size_t)n;
    for (int i = start; i < nargs; i++) {
        const char *part = args[i];
        if (!part || !part[0])
            continue;
        if (strncmp(part, "dev.", 4) == 0 && i == start) {
            n = snprintf(out, 512, "%s", part);
            if (n <= 0 || n >= 512)
                return false;
            pos = (size_t)n;
            continue;
        }
        if (strchr(part, '/') || strstr(part, ".."))
            return false;
        n = snprintf(out + pos, 512 - pos, ".%s", part);
        if (n <= 0 || (size_t)n >= 512 - pos)
            return false;
        pos += (size_t)n;
    }
    return true;
}

static int run_focused(const char *group)
{
#ifndef ZCL_DEV_BUILD
    (void)group;
    fprintf(stderr, "[devloop] focused execution requires a dev build\n");
    return 2;
#else
    if (!group || !group[0] ||
        strspn(group, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != strlen(group)) {
        fprintf(stderr, "[devloop] focused: invalid exact group\n");
        return 2;
    }
    char root[PATH_MAX], bin[PATH_MAX], selector[256];
    if (!realpath(source_root(), root)) {
        fprintf(stderr, "[devloop] focused: source root unavailable\n");
        return 2;
    }
    snprintf(bin, sizeof(bin), "%s/build/bin/test_parallel_fast", root);
    snprintf(selector, sizeof(selector), "--only=%s", group);
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "[devloop] focused: prebuilt runner unavailable\n");
        return 2;
    }
    const char *argv[] = { bin, selector, NULL };
    struct zcl_devloop_process_result result;
    if (!zcl_devloop_process_run(root, argv, 300000, &result))
        return 1;
    bool ok = result.exit_code == 0 && !result.timed_out &&
              result.term_signal == 0;
    printf("{\"schema\":\"zcl.dev_focused_test.v1\",\"status\":\"%s\","
           "\"group\":\"%s\",\"elapsed_ms\":%lld,\"exit_code\":%d}\n",
           ok ? "passed" : "failed", group,
           (long long)result.elapsed_ms, result.exit_code);
    if (!ok && result.output_len)
        fprintf(stderr, "%s\n", result.output);
    return ok ? 0 : 1;
#endif
}

static int plan_files(const char **args, int start, int nargs)
{
    char body[16384];
    const char *const *files = args + start;
    size_t count = nargs > start ? (size_t)(nargs - start) : 0;
    size_t n = zcl_devloop_plan_json(files, count, body, sizeof(body));
    if (n == 0) {
        fprintf(stderr, "[devloop] plan: invalid or oversized file set\n");
        return 2;
    }
    printf("%s\n", body);
    return 0;
}

int zcl_devloop_cli_main(const char **args, int nargs)
{
    if (nargs <= 0)
        return print_menu("dev");

    if (strcmp(args[0], "status") == 0 && nargs == 1)
        return zcl_devloop_print_status();
    if (strcmp(args[0], "app") == 0 && nargs == 3 &&
        strcmp(args[1], "describe") == 0)
        return zcl_devloop_app_describe(source_root(), args[2]);
    if (strcmp(args[0], "app") == 0 && nargs == 4 &&
        strcmp(args[1], "plan") == 0)
        return zcl_devloop_app_plan(source_root(), args[2], args[3]);
    if (strcmp(args[0], "app") == 0 && (nargs == 3 || nargs == 4) &&
        strcmp(args[1], "simulate") == 0) {
        uint64_t seed = UINT64_C(0x534f4349414c0001);
        if (nargs == 4) {
            char *end = NULL;
            seed = strtoull(args[3], &end, 0);
            if (!end || *end)
                return 2;
        }
        return zcl_devloop_app_simulate(args[2], seed);
    }
    if (strcmp(args[0], "core") == 0 && nargs == 2 &&
        strcmp(args[1], "boundary") == 0) {
        printf("{\"schema\":\"zcl.core_app_boundary.v1\","
               "\"rule\":\"core_owns_truth_apps_consume_capabilities\","
               "\"core\":[\"consensus\",\"validation\",\"chain_mutation\","
               "\"wallet_keys\",\"raw_storage\",\"sockets\",\"boot\"],"
               "\"apps\":[\"resources\",\"signed_events\",\"services\","
               "\"projections\",\"web\",\"onion\",\"znam\",\"p2p_topics\"],"
               "\"core_change\":\"guarded_reload\","
               "\"app_change\":\"simulate_then_atomic_publish\"}\n");
        return 0;
    }
    if (strcmp(args[0], "change") == 0 && nargs >= 2 &&
        strcmp(args[1], "plan") == 0)
        return plan_files(args, 2, nargs);
    if (strcmp(args[0], "change") == 0 && nargs >= 2 &&
        strcmp(args[1], "cycle") == 0) {
        const char *const *files = args + 2;
        size_t count = nargs > 2 ? (size_t)(nargs - 2) : 0;
        return zcl_devloop_run_cycle(source_root(), files, count);
    }
    if (strcmp(args[0], "loop") == 0 && nargs >= 2 &&
        strcmp(args[1], "watch") == 0)
        return zcl_devloop_watch(nargs >= 3 ? args[2] : source_root());
    if (strcmp(args[0], "loop") == 0 && nargs == 2 &&
        strcmp(args[1], "heartbeat") == 0)
        return zcl_devloop_print_status();
    if (strcmp(args[0], "test") == 0 && nargs == 2 &&
        strcmp(args[1], "sim") == 0)
        return zcl_devloop_run_sim(source_root());
    if (strcmp(args[0], "test") == 0 && nargs == 3 &&
        strcmp(args[1], "focused") == 0)
        return run_focused(args[2]);
    if (strcmp(args[0], "diagnose") == 0 && nargs == 2 &&
        strcmp(args[1], "latest") == 0)
        return zcl_devloop_print_status();
    if ((strcmp(args[0], "search") == 0 && nargs >= 2) ||
        (strcmp(args[0], "diagnose") == 0 && nargs >= 3 &&
         strcmp(args[1], "search") == 0)) {
        int qi = strcmp(args[0], "search") == 0 ? 1 : 2;
        char query[512] = {0};
        size_t pos = 0;
        for (int i = qi; i < nargs; i++) {
            int n = snprintf(query + pos, sizeof(query) - pos, "%s%s",
                             pos ? " " : "", args[i]);
            if (n <= 0 || (size_t)n >= sizeof(query) - pos)
                return 2;
            pos += (size_t)n;
        }
        char body[16384];
        size_t n = zcl_devloop_menu_search_json(query, body, sizeof(body));
        if (n == 0)
            return 2;
        printf("%s\n", body);
        return 0;
    }

    int start = strcmp(args[0], "help") == 0 ? 1 : 0;

    /* Registry-driven fail-closed: a leaf the registry marks PLANNED has no
     * executable handler here. Resolve the longest registered dev path and, if
     * it is a planned leaf, block with exit 3 instead of printing a menu that
     * would imply the command works. */
    const char *words[64];
    size_t wc = 0;
    words[wc++] = "dev";
    for (int i = start; i < nargs && wc < 64; i++)
        words[wc++] = args[i];
    size_t consumed = 0;
    bool was_alias = false;
    char invoked[ZCL_COMMAND_MAX_PATH];
    const struct zcl_command_spec *spec = zcl_command_registry_resolve_words(
        zcl_command_catalog(), words, wc, &consumed, &was_alias, invoked,
        sizeof(invoked));
    if (spec && spec->mode != ZCL_COMMAND_MODE_BRANCH &&
        spec->availability == ZCL_COMMAND_PLANNED) {
        printf("{\"schema\":\"zcl.result.v1\",\"command\":\"%s\",\"ok\":false,"
               "\"status\":\"blocked\",\"exit_code\":3,"
               "\"error\":{\"code\":\"COMMAND_PLANNED\",\"message\":"
               "\"command is declared but not implemented\",\"evidence\":\"%s\"},"
               "\"next\":[{\"command\":\"discover.describe\",\"input\":"
               "{\"path\":\"%s\"},\"reason\":"
               "\"inspect availability and replacement\"}]}\n",
               spec->path,
               spec->availability_reason ? spec->availability_reason : "",
               spec->path);
        return 3;
    }

    char path[512];
    if (!menu_path(path, args, start, nargs)) {
        fprintf(stderr, "[devloop] help: invalid tree path\n");
        return 2;
    }
    return print_menu(path);
}
