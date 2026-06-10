/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Deterministic chaos scenario runner.
 *
 * Runs declarative scenarios against simulated peers, virtual time, network
 * partitions, allocation faults, and crash/restart artifacts. The harness is
 * intentionally process-local so failures collapse into reproducible seeds
 * before they reach the production node paths.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "net/net_fault.h"
#include "platform/time_compat.h"
#include "sim/sim_peer.h"
#include "util/parse_num.h"
#include "util/safe_alloc.h"

#define CHAOS_MAX_LINE 512
#define CHAOS_MAX_ARGS 16
#define CHAOS_MAX_EXPECTS 64

struct chaos_ctx {
    const char *scenario_path;
    const char *artifact_dir;
    uint64_t seed;
    uint64_t rng_state;
    bool seed_set;
    char boot_phase[32];
    unsigned peer_count;
    struct sim_peer_set peers;
    struct platform_clock_source clock_src;
    int64_t sim_wall_unix;
    int64_t sim_monotonic_us;
    uint64_t clock_advance_count;
    uint64_t scheduled_event_count;
    int64_t last_event_height;
    bool crashed;
    int64_t tip_height;
    int64_t reorg_count;
    int64_t consensus_rejects;
    int64_t block_bytes;
    int64_t clock_advance_seconds;
    int64_t mempool_prune_runs;
    int64_t graceful_shutdowns;
    size_t expect_count;
    bool verbose;
    char alloc_fault_site[64];
    uint64_t alloc_fault_count;
    bool alloc_fault_triggered;
    int64_t net_partition_seconds;
    int64_t net_partition_until;
    bool net_partition_triggered;
    int64_t net_partition_drops;
};

typedef int (*chaos_handler_fn)(struct chaos_ctx *ctx, int argc, char **argv,
                                int line_no);

struct chaos_command {
    const char *name;
    chaos_handler_fn handler;
};

static const struct chaos_command *find_command(const char *name);

static int64_t chaos_clock_monotonic_us(void *user)
{
    const struct chaos_ctx *ctx = (const struct chaos_ctx *)user;
    return ctx ? ctx->sim_monotonic_us : 0;
}

static int64_t chaos_clock_wall_unix(void *user)
{
    const struct chaos_ctx *ctx = (const struct chaos_ctx *)user;
    return ctx ? ctx->sim_wall_unix : 0;
}

static void chaos_ctx_init(struct chaos_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->boot_phase, sizeof(ctx->boot_phase), "idb_complete");
    ctx->rng_state = 0x9e3779b97f4a7c15ULL;
    sim_peer_set_init(&ctx->peers);
    ctx->sim_wall_unix = platform_time_wall_unix();
    ctx->clock_src.monotonic_us = chaos_clock_monotonic_us;
    ctx->clock_src.wall_unix = chaos_clock_wall_unix;
    ctx->clock_src.user = ctx;
}

static const char *arg_value(int argc, char **argv, const char *key)
{
    size_t key_len = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], key, key_len) == 0 && argv[i][key_len] == '=')
            return argv[i] + key_len + 1;
    }
    return NULL;
}

static char *trim_ascii(char *s)
{
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static int split_args(char *line, char **argv, int argv_cap)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        if (argc >= argv_cap) return -E2BIG;
        argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static bool parse_u64_auto(const char *s, uint64_t *out)
{
    if (!s || !*s || !out) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_duration_seconds(const char *s, int64_t *out)
{
    if (!s || !*s || !out) return false;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || v <= 0) return false;

    int64_t mult = 1;
    if (*end == '\0' || strcmp(end, "s") == 0) {
        mult = 1;
    } else if (strcmp(end, "m") == 0) {
        mult = 60;
    } else if (strcmp(end, "h") == 0) {
        mult = 60 * 60;
    } else if (strcmp(end, "d") == 0) {
        mult = 24 * 60 * 60;
    } else {
        return false;
    }

    if (v > INT64_MAX / mult) return false;
    *out = (int64_t)v * mult;
    return true;
}

static int fail_line(int line_no, const char *msg)
{
    fprintf(stderr, "chaos:%d: %s\n", line_no, msg);
    return -EINVAL;
}

static uint64_t chaos_rng_next(struct chaos_ctx *ctx)
{
    uint64_t x = ctx->rng_state;
    if (x == 0)
        x = 0x9e3779b97f4a7c15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    ctx->rng_state = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static const char *path_basename(const char *path)
{
    const char *base = path;
    if (!path) return "scenario";
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return *base ? base : "scenario";
}

static void sanitize_artifact_stem(const char *path, char *out,
                                   size_t out_len)
{
    const char *base = path_basename(path);
    size_t n = 0;
    if (out_len == 0) return;
    for (const unsigned char *p = (const unsigned char *)base;
         *p && n + 1 < out_len; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.')
            out[n++] = (char)*p;
        else
            out[n++] = '_';
    }
    if (n == 0 && out_len > 1)
        out[n++] = 'x';
    out[n] = '\0';
}

static int copy_file_bytes(const char *src_path, const char *dst_path)
{
    FILE *src = fopen(src_path, "rb");
    if (!src) return -errno;
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) {
        int rc = -errno;
        fclose(src);
        return rc;
    }

    char buf[4096];
    int rc = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n > 0 && fwrite(buf, 1, n, dst) != n) {
            rc = -EIO;
            break;
        }
        if (n < sizeof(buf)) {
            if (ferror(src))
                rc = -EIO;
            break;
        }
    }

    if (fclose(src) != 0 && rc == 0)
        rc = -EIO;
    if (fclose(dst) != 0 && rc == 0)
        rc = -EIO;
    return rc;
}

static int write_failure_artifacts(const struct chaos_ctx *ctx)
{
    const char *dir = ctx && ctx->artifact_dir ? ctx->artifact_dir
                                               : "chaos-output";
    if (!ctx || !ctx->scenario_path || !*ctx->scenario_path)
        return -EINVAL;
    if (mkdir(dir, 0777) != 0 && errno != EEXIST)
        return -errno;

    char stem[128];
    sanitize_artifact_stem(ctx->scenario_path, stem, sizeof(stem));

    char summary_path[512];
    char scenario_path[512];
    int n = snprintf(summary_path, sizeof(summary_path),
                     "%s/%s.failure.txt", dir, stem);
    if (n < 0 || (size_t)n >= sizeof(summary_path))
        return -ENAMETOOLONG;
    n = snprintf(scenario_path, sizeof(scenario_path),
                 "%s/%s.scenario", dir, stem);
    if (n < 0 || (size_t)n >= sizeof(scenario_path))
        return -ENAMETOOLONG;

    FILE *summary = fopen(summary_path, "wb");
    if (!summary)
        return -errno;
    fprintf(summary, "scenario=%s\n", ctx->scenario_path);
    fprintf(summary, "seed=%s0x%016" PRIx64 "\n",
            ctx->seed_set ? "" : "(default)", ctx->seed);
    fprintf(summary, "boot_phase=%s\n", ctx->boot_phase);
    fprintf(summary, "peer_count=%u\n", ctx->peer_count);
    fprintf(summary, "expects=%zu\n", ctx->expect_count);
    fprintf(summary, "tip_height=%" PRId64 "\n", ctx->tip_height);
    fprintf(summary, "reorg_count=%" PRId64 "\n", ctx->reorg_count);
    fprintf(summary, "consensus_rejects=%" PRId64 "\n",
            ctx->consensus_rejects);
    fprintf(summary, "blocks_sent=%u\n", ctx->peers.blocks_sent);
    fprintf(summary, "block_bytes=%" PRId64 "\n", ctx->block_bytes);
    fprintf(summary, "malformed_blocks=%u\n",
            ctx->peers.malformed_blocks_sent);
    fprintf(summary, "active_peers=%u\n", ctx->peers.active_count);
    fprintf(summary, "killed_peers=%u\n", ctx->peers.killed_count);
    fprintf(summary, "clock_advance_count=%" PRIu64 "\n",
            ctx->clock_advance_count);
    fprintf(summary, "clock_advance_seconds=%" PRId64 "\n",
            ctx->clock_advance_seconds);
    fprintf(summary, "scheduled_events=%" PRIu64 "\n",
            ctx->scheduled_event_count);
    fprintf(summary, "alloc_faults=%" PRIu64 "\n", ctx->alloc_fault_count);
    fprintf(summary, "graceful_shutdowns=%" PRId64 "\n",
            ctx->graceful_shutdowns);
    fprintf(summary, "partition_seconds=%" PRId64 "\n",
            ctx->net_partition_seconds);
    fprintf(summary, "partition_until=%" PRId64 "\n",
            ctx->net_partition_until);
    fprintf(summary, "partition_drops=%" PRId64 "\n",
            ctx->net_partition_drops);
    fprintf(summary, "artifact_scenario=%s\n", scenario_path);
    fprintf(summary, "replay_command=build/bin/zclassic23-chaos --scenario=%s --verbose\n",
            scenario_path);
    int close_rc = fclose(summary);
    if (close_rc != 0)
        return -EIO;

    return copy_file_bytes(ctx->scenario_path, scenario_path);
}

static int handle_seed(struct chaos_ctx *ctx, int argc, char **argv,
                       int line_no)
{
    if (argc != 2) return fail_line(line_no, "seed requires one value");
    uint64_t seed = 0;
    if (!parse_u64_auto(argv[1], &seed))
        return fail_line(line_no, "seed must be an integer or hex value");
    ctx->seed = seed;
    ctx->rng_state = seed ^ 0x9e3779b97f4a7c15ULL;
    ctx->seed_set = true;
    return 0;
}

static int handle_boot_phase(struct chaos_ctx *ctx, int argc, char **argv,
                             int line_no)
{
    if (argc != 2) return fail_line(line_no, "boot_phase requires one value");
    if (strcmp(argv[1], "idb_complete") != 0 &&
        strcmp(argv[1], "listening") != 0 &&
        strcmp(argv[1], "mempool_open") != 0) {
        return fail_line(line_no, "unknown boot_phase");
    }
    snprintf(ctx->boot_phase, sizeof(ctx->boot_phase), "%s", argv[1]);
    return 0;
}

static int handle_peer_count(struct chaos_ctx *ctx, int argc, char **argv,
                             int line_no)
{
    if (argc != 2) return fail_line(line_no, "peer_count requires one value");
    uint64_t n = 0;
    if (!parse_u64_auto(argv[1], &n) || n > 1024)
        return fail_line(line_no, "peer_count must be 0..1024");
    if (sim_peer_set_resize(&ctx->peers, (unsigned)n) != 0)
        return fail_line(line_no, "failed to create simulated peers");
    ctx->peer_count = (unsigned)n;
    return 0;
}

static int compare_metric(int64_t actual, const char *op, int64_t expected)
{
    if (strcmp(op, "==") == 0) return actual == expected ? 0 : -1;
    if (strcmp(op, "!=") == 0) return actual != expected ? 0 : -1;
    if (strcmp(op, ">=") == 0) return actual >= expected ? 0 : -1;
    if (strcmp(op, "<=") == 0) return actual <= expected ? 0 : -1;
    if (strcmp(op, ">") == 0) return actual > expected ? 0 : -1;
    if (strcmp(op, "<") == 0) return actual < expected ? 0 : -1;
    return -2;
}

static bool metric_value(const struct chaos_ctx *ctx, const char *name,
                         int64_t *out)
{
    if (strcmp(name, "tip_height") == 0) {
        *out = ctx->tip_height;
        return true;
    }
    if (strcmp(name, "reorg_count") == 0) {
        *out = ctx->reorg_count;
        return true;
    }
    if (strcmp(name, "consensus_rejects") == 0) {
        *out = ctx->consensus_rejects;
        return true;
    }
    if (strcmp(name, "clock_advance_seconds") == 0) {
        *out = ctx->clock_advance_seconds;
        return true;
    }
    if (strcmp(name, "graceful_shutdowns") == 0) {
        *out = ctx->graceful_shutdowns;
        return true;
    }
    if (strcmp(name, "mempool_prune_runs") == 0 ||
        strcmp(name, "mempool_prunes") == 0) {
        *out = ctx->mempool_prune_runs;
        return true;
    }
    if (strcmp(name, "active_peers") == 0) {
        *out = (int64_t)ctx->peers.active_count;
        return true;
    }
    if (strcmp(name, "killed_peers") == 0) {
        *out = (int64_t)ctx->peers.killed_count;
        return true;
    }
    if (strcmp(name, "blocks_sent") == 0) {
        *out = (int64_t)ctx->peers.blocks_sent;
        return true;
    }
    if (strcmp(name, "block_bytes") == 0) {
        *out = ctx->block_bytes;
        return true;
    }
    if (strcmp(name, "malformed_blocks") == 0) {
        *out = (int64_t)ctx->peers.malformed_blocks_sent;
        return true;
    }
    if (strcmp(name, "clock_advance_count") == 0) {
        *out = (int64_t)ctx->clock_advance_count;
        return true;
    }
    if (strcmp(name, "scheduled_events") == 0) {
        *out = (int64_t)ctx->scheduled_event_count;
        return true;
    }
    if (strcmp(name, "alloc_faults") == 0) {
        *out = (int64_t)ctx->alloc_fault_count;
        return true;
    }
    if (strcmp(name, "partition_drops") == 0) {
        *out = ctx->net_partition_drops;
        return true;
    }
    if (strcmp(name, "sim_time") == 0) {
        *out = ctx->sim_wall_unix;
        return true;
    }
    return false;
}

static int handle_expect(struct chaos_ctx *ctx, int argc, char **argv,
                         int line_no)
{
    if (argc < 2) return fail_line(line_no, "expect requires an assertion");
    if (ctx->expect_count >= CHAOS_MAX_EXPECTS)
        return fail_line(line_no, "too many expect assertions");
    ctx->expect_count++;

    if (argc == 2 && strcmp(argv[1], "no_crash") == 0) {
        if (ctx->crashed) {
            fprintf(stderr, "chaos:%d: expect no_crash failed\n", line_no);
            return -1;
        }
        return 0;
    }

    if (argc == 4) {
        int64_t actual = 0;
        int64_t expected = 0;
        if (!metric_value(ctx, argv[1], &actual))
            return fail_line(line_no, "unknown expect metric");
        if (!zcl_parse_i64(argv[3], &expected))
            return fail_line(line_no, "expect value must be an integer");
        int cmp = compare_metric(actual, argv[2], expected);
        if (cmp == -2) return fail_line(line_no, "unknown expect operator");
        if (cmp != 0) {
            fprintf(stderr,
                    "chaos:%d: expect failed: %s %s %" PRId64
                    " (actual=%" PRId64 ")\n",
                    line_no, argv[1], argv[2], expected, actual);
            return -1;
        }
        return 0;
    }

    return fail_line(line_no, "unsupported expect assertion");
}

static int handle_trigger_oom_at(struct chaos_ctx *ctx, int argc, char **argv,
                                 int line_no)
{
    if (argc != 2) return fail_line(line_no, "trigger_oom_at requires one label");
    if (strlen(argv[1]) >= sizeof(ctx->alloc_fault_site))
        return fail_line(line_no, "trigger_oom_at label too long");

    snprintf(ctx->alloc_fault_site, sizeof(ctx->alloc_fault_site), "%s",
             argv[1]);
    zcl_alloc_fault_fail_next(ctx->alloc_fault_site);
    void *p = zcl_malloc(1, ctx->alloc_fault_site);
    if (p) {
        free(p);
        return fail_line(line_no, "allocation fault did not fire");
    }
    if (zcl_alloc_fault_armed_label() != NULL)
        return fail_line(line_no, "allocation fault did not clear");
    ctx->alloc_fault_count++;
    ctx->alloc_fault_triggered = true;
    ctx->graceful_shutdowns++;
    return 0;
}

static int handle_kill_peer(struct chaos_ctx *ctx, int argc, char **argv,
                            int line_no)
{
    if (argc != 2) return fail_line(line_no, "kill_peer requires one peer id");
    uint64_t id = 0;
    if (!parse_u64_auto(argv[1], &id) || id > UINT32_MAX)
        return fail_line(line_no, "kill_peer id must be an integer");
    int rc = sim_peer_kill(&ctx->peers, (unsigned)id);
    if (rc == -ENOENT)
        return fail_line(line_no, "kill_peer id is not configured");
    if (rc == -EALREADY)
        return fail_line(line_no, "kill_peer id is already disconnected");
    if (rc != 0)
        return fail_line(line_no, "kill_peer failed");
    return 0;
}

static int handle_random_kill_peers(struct chaos_ctx *ctx, int argc,
                                    char **argv, int line_no)
{
    if (argc != 2)
        return fail_line(line_no, "random_kill_peers requires count=N");

    const char *count_arg = arg_value(argc, argv, "count");
    uint64_t count = 0;
    if (!count_arg || !parse_u64_auto(count_arg, &count) ||
        count > UINT32_MAX) {
        return fail_line(line_no,
                         "random_kill_peers count must be an integer");
    }
    if (count > ctx->peers.active_count)
        return fail_line(line_no,
                         "random_kill_peers count exceeds active peers");

    for (uint64_t killed = 0; killed < count; killed++) {
        unsigned active_index =
            (unsigned)(chaos_rng_next(ctx) % ctx->peers.active_count);
        unsigned selected = UINT32_MAX;
        for (unsigned i = 0, seen = 0; i < ctx->peers.count; i++) {
            if (!ctx->peers.peers[i].connected)
                continue;
            if (seen == active_index) {
                selected = i;
                break;
            }
            seen++;
        }
        if (selected == UINT32_MAX)
            return fail_line(line_no, "random_kill_peers selection failed");
        int rc = sim_peer_kill(&ctx->peers, selected);
        if (rc != 0)
            return fail_line(line_no, "random_kill_peers kill failed");
    }
    return 0;
}

static int handle_send_block(struct chaos_ctx *ctx, int argc, char **argv,
                             int line_no)
{
    if (argc != 3 && argc != 4)
        return fail_line(line_no,
                         "send_block requires peer=I file=PATH [height=N]");

    const char *peer_arg = arg_value(argc, argv, "peer");
    const char *path = arg_value(argc, argv, "file");
    const char *height_arg = arg_value(argc, argv, "height");
    uint64_t peer_id = 0;
    if (!peer_arg || !parse_u64_auto(peer_arg, &peer_id) ||
        peer_id > UINT32_MAX) {
        return fail_line(line_no, "send_block peer must be integer");
    }
    if (!path || !*path)
        return fail_line(line_no, "send_block file is required");
    int64_t height = 0;
    if (height_arg && (!zcl_parse_i64(height_arg, &height) || height <= 0))
        return fail_line(line_no, "send_block height must be a positive integer");

    const struct sim_peer *peer = sim_peer_get(&ctx->peers, (unsigned)peer_id);
    if (!peer)
        return fail_line(line_no, "send_block peer is not configured");
    if (!peer->connected)
        return fail_line(line_no, "send_block peer is disconnected");
    if (net_partition_active_at(platform_time_wall_unix())) {
        ctx->net_partition_drops++;
        return 0;
    }

    size_t bytes_read = 0;
    int rc = sim_peer_send_block(&ctx->peers, (unsigned)peer_id, path,
                                 &bytes_read);
    if (rc == -ENOENT)
        return fail_line(line_no, "send_block peer is not configured");
    if (rc == -ENOTCONN)
        return fail_line(line_no, "send_block peer is disconnected");
    if (rc == -ENODATA)
        return fail_line(line_no, "send_block fixture is empty");
    if (rc != 0)
        return fail_line(line_no, "send_block fixture could not be read");

    if (bytes_read > (size_t)(INT64_MAX - ctx->block_bytes))
        return fail_line(line_no, "send_block byte counter overflows");
    ctx->block_bytes += (int64_t)bytes_read;
    if (height_arg) {
        if (height > ctx->tip_height)
            ctx->tip_height = height;
    } else {
        ctx->tip_height++;
    }
    return 0;
}

static int handle_send_malformed_block(struct chaos_ctx *ctx, int argc,
                                       char **argv, int line_no)
{
    if (argc != 3)
        return fail_line(line_no,
                         "send_malformed_block requires peer=I type=ENUM");

    const char *peer_arg = arg_value(argc, argv, "peer");
    const char *type = arg_value(argc, argv, "type");
    uint64_t peer_id = 0;
    if (!peer_arg || !parse_u64_auto(peer_arg, &peer_id) ||
        peer_id > UINT32_MAX) {
        return fail_line(line_no, "send_malformed_block peer must be integer");
    }
    if (!type || !sim_peer_malformed_type_known(type))
        return fail_line(line_no, "send_malformed_block unknown type");

    const struct sim_peer *peer = sim_peer_get(&ctx->peers, (unsigned)peer_id);
    if (!peer)
        return fail_line(line_no, "send_malformed_block peer is not configured");
    if (!peer->connected)
        return fail_line(line_no, "send_malformed_block peer is disconnected");
    if (net_partition_active_at(platform_time_wall_unix())) {
        ctx->net_partition_drops++;
        return 0;
    }

    int rc = sim_peer_send_malformed_block(&ctx->peers, (unsigned)peer_id,
                                           type);
    if (rc == -ENOENT)
        return fail_line(line_no, "send_malformed_block peer is not configured");
    if (rc == -ENOTCONN)
        return fail_line(line_no, "send_malformed_block peer is disconnected");
    if (rc != 0)
        return fail_line(line_no, "send_malformed_block failed");

    ctx->consensus_rejects++;
    return 0;
}

static int handle_at_event(struct chaos_ctx *ctx, int argc, char **argv,
                           int line_no)
{
    if (argc < 3)
        return fail_line(line_no, "at_event requires HEIGHT COMMAND [ARGS]");
    int64_t event_height = 0;
    if (!zcl_parse_i64(argv[1], &event_height) || event_height < 0)
        return fail_line(line_no, "at_event height must be a non-negative integer");

    const struct chaos_command *cmd = find_command(argv[2]);
    if (!cmd)
        return fail_line(line_no, "at_event nested command is unknown");
    if (strcmp(argv[2], "at_event") == 0)
        return fail_line(line_no, "at_event cannot nest at_event");

    ctx->scheduled_event_count++;
    ctx->last_event_height = event_height;
    return cmd->handler(ctx, argc - 2, argv + 2, line_no);
}

static int handle_advance_clock(struct chaos_ctx *ctx, int argc, char **argv,
                                int line_no)
{
    if (argc != 2)
        return fail_line(line_no, "advance_clock requires one duration");

    const char *duration = argv[1];
    if (*duration == '+')
        duration++;

    int64_t seconds = 0;
    if (!parse_duration_seconds(duration, &seconds))
        return fail_line(line_no,
                         "advance_clock duration must be +Ns/+Nm/+Nh/+Nd");
    if (ctx->sim_wall_unix > INT64_MAX - seconds)
        return fail_line(line_no, "advance_clock wall time overflows");
    if (seconds > (INT64_MAX - ctx->sim_monotonic_us) / 1000000LL)
        return fail_line(line_no, "advance_clock monotonic time overflows");
    if (ctx->clock_advance_seconds > INT64_MAX - seconds)
        return fail_line(line_no, "advance_clock duration overflows");

    ctx->sim_wall_unix += seconds;
    ctx->sim_monotonic_us += seconds * 1000000LL;
    ctx->clock_advance_count++;
    ctx->clock_advance_seconds += seconds;
    if (ctx->peers.active_count > 0)
        ctx->tip_height += seconds / 60;
    if (strcmp(ctx->boot_phase, "mempool_open") == 0 && seconds >= 3600)
        ctx->mempool_prune_runs += seconds / 3600;
    return 0;
}

static int handle_partition_network(struct chaos_ctx *ctx, int argc,
                                    char **argv, int line_no)
{
    if (argc != 2)
        return fail_line(line_no, "partition_network requires for=DURATION");

    const char *duration = argv[1];
    if (strncmp(duration, "for=", 4) == 0)
        duration += 4;

    int64_t seconds = 0;
    if (!parse_duration_seconds(duration, &seconds))
        return fail_line(line_no,
                         "partition_network duration must be Ns/Nm/Nh/Nd");

    int64_t now = platform_time_wall_unix();
    if (now > INT64_MAX - seconds)
        return fail_line(line_no, "partition_network duration overflows");

    ctx->net_partition_seconds = seconds;
    ctx->net_partition_until = now + seconds;
    net_partition_until_unix(ctx->net_partition_until);
    if (!net_partition_active_at(now))
        return fail_line(line_no, "network partition did not arm");
    ctx->net_partition_triggered = true;
    return 0;
}

static const struct chaos_command COMMANDS[] = {
    { "seed", handle_seed },
    { "boot_phase", handle_boot_phase },
    { "peer_count", handle_peer_count },
    { "expect", handle_expect },
    { "at_event", handle_at_event },
    { "kill_peer", handle_kill_peer },
    { "random_kill_peers", handle_random_kill_peers },
    { "send_block", handle_send_block },
    { "send_malformed_block", handle_send_malformed_block },
    { "advance_clock", handle_advance_clock },
    { "trigger_oom_at", handle_trigger_oom_at },
    { "partition_network", handle_partition_network },
};

static const struct chaos_command *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(COMMANDS[i].name, name) == 0)
            return &COMMANDS[i];
    }
    return NULL;
}

static int run_scenario(struct chaos_ctx *ctx)
{
    net_partition_clear();
    platform_clock_set_source(&ctx->clock_src);
    FILE *fp = fopen(ctx->scenario_path, "rb");
    if (!fp) {
        fprintf(stderr, "chaos: failed to open %s: %s\n",
                ctx->scenario_path, strerror(errno));
        platform_clock_clear_source();
        return 1;
    }

    char line[CHAOS_MAX_LINE];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        } else if (!feof(fp)) {
            fprintf(stderr, "chaos:%d: line too long\n", line_no);
            fclose(fp);
            platform_clock_clear_source();
            return 1;
        }

        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *body = trim_ascii(line);
        if (*body == '\0') continue;

        char *argv[CHAOS_MAX_ARGS];
        int argc = split_args(body, argv, CHAOS_MAX_ARGS);
        if (argc < 0) {
            fprintf(stderr, "chaos:%d: too many arguments\n", line_no);
            fclose(fp);
            platform_clock_clear_source();
            return 1;
        }

        const struct chaos_command *cmd = find_command(argv[0]);
        if (!cmd) {
            fprintf(stderr, "chaos:%d: unknown command '%s'\n",
                    line_no, argv[0]);
            fclose(fp);
            platform_clock_clear_source();
            return 1;
        }
        int rc = cmd->handler(ctx, argc, argv, line_no);
        if (rc != 0) {
            fclose(fp);
            platform_clock_clear_source();
            return 1;
        }
        if (ctx->verbose)
            printf("chaos:%d: %s OK\n", line_no, argv[0]);
    }

    fclose(fp);
    if (ctx->expect_count == 0) {
        fprintf(stderr, "chaos: scenario has no expect assertions\n");
        platform_clock_clear_source();
        return 1;
    }
    platform_clock_clear_source();
    return 0;
}

#ifndef CHAOS_NO_MAIN
static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --scenario=PATH [--verbose] [--artifact-dir=PATH]\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct chaos_ctx ctx;
    chaos_ctx_init(&ctx);

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--scenario=", 11) == 0) {
            ctx.scenario_path = argv[i] + 11;
        } else if (strncmp(argv[i], "--artifact-dir=", 15) == 0) {
            ctx.artifact_dir = argv[i] + 15;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            ctx.verbose = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!ctx.scenario_path || !*ctx.scenario_path) {
        usage(argv[0]);
        return 2;
    }

    int rc = run_scenario(&ctx);
    if (rc == 0) {
        printf("PASS %s seed=%s0x%016" PRIx64
               " boot_phase=%s peers=%u expects=%zu\n",
               ctx.scenario_path,
               ctx.seed_set ? "" : "(default)",
               ctx.seed,
               ctx.boot_phase,
               ctx.peer_count,
               ctx.expect_count);
    } else {
        printf("FAIL %s\n", ctx.scenario_path);
        int artifact_rc = write_failure_artifacts(&ctx);
        if (artifact_rc == 0)
            printf("ARTIFACTS %s\n",
                   ctx.artifact_dir ? ctx.artifact_dir : "chaos-output");
        else
            fprintf(stderr, "chaos: failed to write artifacts: %s\n",
                    strerror(-artifact_rc));
    }
    return rc;
}
#endif
