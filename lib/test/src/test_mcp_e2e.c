/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * End-to-end MCP integration test.  Forks the real `build/bin/zclassic23 -mcp`
 * binary with a throwaway datadir, speaks JSON-RPC over its stdin/stdout,
 * and asserts on the wire-level envelope shapes that an MCP client
 * (Claude Code, a TypeScript type generator, an auto-test harness)
 * would see.
 *
 * Coverage:
 *   - `initialize`   → JSON-RPC result with protocolVersion + serverInfo
 *   - `tools/list`   → tool count matches the in-process router exactly
 *   - `tools/call zcl_tools_list`  → self-describing tool body
 *   - `tools/call zcl_openapi`     → OpenAPI-ish body with paths
 *   - `tools/call zcl_metrics`     → Prometheus text in body
 *   - bad method name              → -32601 Method not found
 *   - tools/call with no name      → -32602 missing tool name
 *
 * The test does NOT need a running node — every tool we call is a
 * meta tool that reads in-process state (router / metrics).  Other
 * tools would try to reach the HTTP RPC server and fail with curl
 * errors, which is fine but not what this suite verifies.
 *
 * Robustness:
 *   - Tool counts are read live from `mcp_router_count()` in the
 *     test_zcl process, not hard-coded.  Adding a new MCP tool does
 *     not require touching this file.
 *   - The harness compares `build/bin/zclassic23` mtime against the MCP
 *     source files.  If the binary is stale (older than any router
 *     or controller source) the test reports `SKIP (stale binary —
 *     run \`make test-e2e\` to rebuild)` instead of failing with a
 *     confusing tool-count mismatch.
 *   - If `build/bin/zclassic23` is not present at all (fresh checkout), the
 *     test reports `SKIP` and `build/bin/test_zcl` stays green.
 *
 * To always run the e2e suite against a fresh binary, use:
 *   $ make test-e2e
 */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "json/json.h"
#include "mcp/router.h"
#include "mcp/controllers.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Returns mtime in seconds, or -1 if path is missing. */
static long file_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_mtime;
}

/* The MCP source files that, when newer than build/bin/zclassic23, mean the
 * binary's tool surface has drifted from what test_zcl knows about.
 * If any of these is newer than the binary the e2e test will SKIP
 * with a clear message rather than fail with a confusing tool count
 * mismatch.  Glob expansion is intentionally avoided — every file
 * here is checked individually so a missing file (e.g. on a slim
 * checkout) is harmless. */
static const char *const stale_witnesses[] = {
    "tools/mcp/router.c",
    "tools/mcp/controllers/meta_controller.c",
    "tools/mcp/controllers/chain_controller.c",
    "tools/mcp/controllers/wallet_controller.c",
    "tools/mcp/controllers/net_controller.c",
    "tools/mcp/controllers/ops_controller.c",
    "tools/mcp/controllers/diagnostics_controller.c",
    "tools/mcp/controllers/app_controller.c",
    "lib/metrics/src/prometheus_metrics.c",
    "tools/mcp/middleware.c",
    "tools/mcp_server.c",
    "src/main.c",
    NULL,
};

/* Returns true when build/bin/zclassic23 looks newer than every witness
 * source file we know about. */
static bool zclassic23_is_fresh(const char *bin_path,
                                 const char **stale_path_out)
{
    long bin_mt = file_mtime(bin_path);
    if (bin_mt < 0) return false;
    for (size_t i = 0; stale_witnesses[i]; i++) {
        long src_mt = file_mtime(stale_witnesses[i]);
        if (src_mt < 0) continue;             /* missing → ignore */
        if (src_mt > bin_mt) {
            if (stale_path_out) *stale_path_out = stale_witnesses[i];
            return false;
        }
    }
    return true;
}

/* Count occurrences of `needle` in `hay` (no overlap). */
static size_t count_substring(const char *hay, const char *needle)
{
    size_t n = 0;
    size_t nlen = strlen(needle);
    if (!nlen) return 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

/* ── Child process harness ─────────────────────────────────── */

struct mcp_child {
    pid_t pid;
    int   in_fd;    /* we write here */
    int   out_fd;   /* we read here  */
    char  datadir[256];
};

static bool child_start(struct mcp_child *ch)
{
    memset(ch, 0, sizeof(*ch));

    /* Throwaway datadir so the child doesn't touch the real node. */
    snprintf(ch->datadir, sizeof(ch->datadir),
             "/tmp/zcl_mcp_e2e_%d", (int)getpid());
    mkdir(ch->datadir, 0700);

    int to_child[2];
    int from_child[2];
    if (pipe(to_child) < 0 || pipe(from_child) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return false;
    }

    if (pid == 0) {
        /* Child */
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        /* Keep stderr pointing at our parent terminal so errors surface */
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);

        char datadir_arg[320];
        snprintf(datadir_arg, sizeof(datadir_arg),
                 "-datadir=%s", ch->datadir);
        execl("build/bin/zclassic23", "zclassic23", "-mcp", datadir_arg,
              "-rpcport=19999", NULL);
        _exit(127);
    }

    /* Parent */
    close(to_child[0]);
    close(from_child[1]);
    ch->pid = pid;
    ch->in_fd = to_child[1];
    ch->out_fd = from_child[0];
    return true;
}

static void child_stop(struct mcp_child *ch)
{
    if (ch->pid > 0) {
        kill(ch->pid, SIGTERM);
        int status;
        for (int i = 0; i < 10; i++) {
            if (waitpid(ch->pid, &status, WNOHANG) > 0) break;
            struct timespec ts = {0, 50 * 1000000L};
            nanosleep(&ts, NULL);
        }
        kill(ch->pid, SIGKILL);
        waitpid(ch->pid, &status, 0);
    }
    if (ch->in_fd >= 0)  close(ch->in_fd);
    if (ch->out_fd >= 0) close(ch->out_fd);
    if (ch->datadir[0]) {
        /* Best-effort cleanup; ignore errors. */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", ch->datadir);
        (void)system(cmd);
    }
    ch->pid = 0;
    ch->in_fd = ch->out_fd = -1;
}

/* Send one JSON-RPC line to the child.  Returns true on success. */
static bool child_send(struct mcp_child *ch, const char *line)
{
    size_t len = strlen(line);
    if (write(ch->in_fd, line, len) != (ssize_t)len) return false;
    if (write(ch->in_fd, "\n", 1) != 1) return false;
    return true;
}

/* Read one newline-terminated line from the child with a deadline.
 * Writes at most cap-1 bytes into out.  Returns length or -1 on
 * timeout / error.  The child buffers stdout line-by-line, so this
 * assumes each response is on a single line. */
static ssize_t child_recv_line(struct mcp_child *ch, char *out, size_t cap,
                                int timeout_ms)
{
    size_t pos = 0;
    int64_t deadline_ms = (int64_t)timeout_ms;
    int64_t elapsed = 0;
    struct timespec t0;
    platform_time_monotonic_timespec(&t0);

    while (pos + 1 < cap) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ch->out_fd, &rs);
        int64_t left = deadline_ms - elapsed;
        if (left <= 0) return -1;
        struct timeval tv = { .tv_sec = left / 1000,
                              .tv_usec = (left % 1000) * 1000 };
        int rc = select(ch->out_fd + 1, &rs, NULL, NULL, &tv);
        if (rc <= 0) return -1;

        char c;
        ssize_t r = read(ch->out_fd, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') break;
        out[pos++] = c;

        struct timespec t1;
        platform_time_monotonic_timespec(&t1);
        elapsed = (int64_t)((t1.tv_sec - t0.tv_sec) * 1000 +
                            (t1.tv_nsec - t0.tv_nsec) / 1000000);
    }
    out[pos] = 0;
    return (ssize_t)pos;
}

/* One-shot request/response helper. */
static bool child_rpc(struct mcp_child *ch, const char *req,
                      char *out, size_t cap)
{
    if (!child_send(ch, req)) return false;
    return child_recv_line(ch, out, cap, 5000) >= 0;
}

static bool contains(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/* ── Tests ─────────────────────────────────────────────────── */

static int test_initialize(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: initialize returns protocolVersion + serverInfo") {
        char resp[8192];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "\"protocolVersion\""));
        ASSERT(contains(resp, "\"serverInfo\""));
        ASSERT(contains(resp, "\"name\":\"zcl23\""));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tools_list(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: tools/list reports the same tool count as the in-process router") {
        char resp[262144];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "\"tools\":["));

        /* Compare the wire-level tool count to mcp_router_count() in
         * THIS process.  Both are built from the same source, so any
         * mismatch means the binary is stale (or the router was
         * mutated by something else).  We've already early-returned
         * via SKIP in the entry point on stale binaries, so reaching
         * here with a mismatch is a real bug. */
        size_t expected = mcp_router_count();
        size_t observed = count_substring(resp, "\"name\":\"zcl_");
        if (expected != observed) {
            printf("e2e tool count: expected=%zu observed=%zu\n",
                   expected, observed);
        }
        ASSERT(expected == observed);

        /* Spot-check a handful of key tools that have been around
         * since wave 1 — these names are part of the surface contract. */
        ASSERT(contains(resp, "\"zcl_status\""));
        ASSERT(contains(resp, "\"zcl_kpi\""));
        ASSERT(contains(resp, "\"zcl_getblock\""));
        ASSERT(contains(resp, "\"zcl_tools_list\""));
        ASSERT(contains(resp, "\"zcl_self_test\""));
        ASSERT(contains(resp, "\"zcl_openapi\""));
        ASSERT(contains(resp, "\"zcl_metrics\""));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tools_call_tools_list(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: tools/call zcl_tools_list returns the router table") {
        char resp[262144];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"zcl_tools_list\",\"arguments\":{}}}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "\"content\""));
        ASSERT(contains(resp, "count"));
        /* The inner body emits "count":N — match it dynamically against
         * the in-process router so adding tools doesn't break this test. */
        char count_needle[64];
        snprintf(count_needle, sizeof(count_needle),
                 "count\\\":%zu", mcp_router_count());
        if (!contains(resp, count_needle)) {
            printf("e2e: did not find %s in tools/list body\n", count_needle);
        }
        ASSERT(contains(resp, count_needle));
        ASSERT(contains(resp, "zcl_getblock"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tools_call_openapi(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: tools/call zcl_openapi returns an OpenAPI-ish doc") {
        char resp[262144];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"zcl_openapi\",\"arguments\":{}}}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "openapi"));
        ASSERT(contains(resp, "paths"));
        ASSERT(contains(resp, "/tools/zcl_status"));
        ASSERT(contains(resp, "operationId"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tools_call_metrics(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: tools/call zcl_metrics returns prometheus text") {
        char resp[65536];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"zcl_metrics\",\"arguments\":{}}}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "prometheus"));
        ASSERT(contains(resp, "zcl_mcp_requests"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_bad_method(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: unknown method returns -32601 Method not found") {
        char resp[4096];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"nonsense/method\"}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "-32601"));
        ASSERT(contains(resp, "Method not found"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_missing_tool_name(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: tools/call without a tool name returns -32602") {
        char resp[4096];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
            "\"params\":{}}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "-32602"));
        ASSERT(contains(resp, "missing tool name"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_unknown_tool(struct mcp_child *ch)
{
    int failures = 0;
    TEST("e2e: unknown tool returns UNKNOWN_TOOL envelope inside content") {
        char resp[8192];
        ASSERT(child_rpc(ch,
            "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"zcl_nope_never\",\"arguments\":{}}}",
            resp, sizeof(resp)));
        ASSERT(contains(resp, "UNKNOWN_TOOL"));
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ───────────────────────────────────────────── */

int test_mcp_e2e(void);

int test_mcp_e2e(void)
{
    int failures = 0;

    if (!file_exists("build/bin/zclassic23")) {
        printf("e2e: build/bin/zclassic23 not built — SKIP "
               "(run `make test-e2e` to rebuild and run)\n");
        return 0;
    }

    /* Refuse to run against a stale binary.  Without this guard the
     * tool-count assertion would fail with a message that looks like
     * a real regression but is in fact just an old build, which has
     * burned multiple sessions of debugging time. */
    const char *stale_path = NULL;
    if (!zclassic23_is_fresh("build/bin/zclassic23", &stale_path)) {
        printf("e2e: build/bin/zclassic23 is stale — newer source: %s — SKIP "
               "(run `make test-e2e` to rebuild and run)\n",
               stale_path ? stale_path : "(unknown)");
        return 0;
    }

    /* Populate this process's router with the same tool surface the
     * forked binary will register, so the test can compare counts
     * dynamically (instead of hard-coding 69 / 70 / …).  We reset
     * before AND after so we don't pollute later tests. */
    mcp_router_reset();
    mcp_register_ops();
    mcp_register_diagnostics();
    mcp_register_chain();
    mcp_register_net();
    mcp_register_wallet();
    mcp_register_app();
    mcp_register_meta();

    struct mcp_child ch;
    if (!child_start(&ch)) {
        printf("e2e: failed to fork build/bin/zclassic23 -mcp — SKIP\n");
        return 0;
    }

    failures += test_initialize(&ch);
    failures += test_tools_list(&ch);
    failures += test_tools_call_tools_list(&ch);
    failures += test_tools_call_openapi(&ch);
    failures += test_tools_call_metrics(&ch);
    failures += test_bad_method(&ch);
    failures += test_missing_tool_name(&ch);
    failures += test_unknown_tool(&ch);

    child_stop(&ch);
    mcp_router_reset();
    return failures;
}
