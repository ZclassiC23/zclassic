/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Cheap process-resource telemetry for first-call AI/operator status. */

#include "controllers/agent_resources.h"

#include "json/json.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define AGENT_RESOURCES_RSS_WARN_MB 4096
#define AGENT_RESOURCES_CGROUP_ROOT "/sys/fs/cgroup"

static int64_t agent_resources_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        LOG_RETURN(-1, "agent_resources",
                   "rss_kb: cannot open /proc/self/status");

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long kb = 0;
        if (sscanf(line, "VmRSS: %lld kB", &kb) == 1) {
            fclose(f);
            if (kb < 0)
                LOG_RETURN(-1, "agent_resources",
                           "rss_kb: negative VmRSS in /proc/self/status");
            return (int64_t)kb;
        }
    }

    fclose(f);
    LOG_RETURN(-1, "agent_resources",
               "rss_kb: VmRSS missing in /proc/self/status");
}

static int64_t agent_resources_uptime_seconds(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f)
        LOG_RETURN(-1, "agent_resources",
                   "uptime_seconds: cannot open /proc/uptime");

    double system_uptime = 0.0;
    if (fscanf(f, "%lf", &system_uptime) != 1) {
        fclose(f);
        LOG_RETURN(-1, "agent_resources",
                   "uptime_seconds: cannot parse /proc/uptime");
    }
    fclose(f);

    f = fopen("/proc/self/stat", "r");
    if (!f)
        LOG_RETURN(-1, "agent_resources",
                   "uptime_seconds: cannot open /proc/self/stat");

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        LOG_RETURN(-1, "agent_resources",
                   "uptime_seconds: empty /proc/self/stat");
    buf[n] = '\0';

    const char *p = strrchr(buf, ')');
    if (!p)
        LOG_RETURN(-1, "agent_resources",
                   "uptime_seconds: malformed /proc/self/stat comm");
    p++;
    for (int i = 0; i < 19; i++) {
        while (*p == ' ')
            p++;
        while (*p && *p != ' ')
            p++;
    }
    while (*p == ' ')
        p++;

    long long start_ticks = 0;
    if (sscanf(p, "%lld", &start_ticks) != 1)
        LOG_RETURN(-1, "agent_resources",
                   "uptime_seconds: cannot parse process start ticks");

    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    if (ticks_per_sec <= 0)
        ticks_per_sec = 100;

    double process_start = (double)start_ticks / (double)ticks_per_sec;
    double uptime = system_uptime - process_start;
    if (uptime < 0.0)
        LOG_RETURN(-1, "agent_resources",
                   "uptime_seconds: process start is in the future");
    return (int64_t)uptime;
}

static void agent_resources_trim_newline(char *s)
{
    if (!s)
        return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

static bool agent_resources_cgroup_dir(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return false; // raw-return-ok:optional-cgroup-unavailable

    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f)
        return false; // raw-return-ok:optional-cgroup-unavailable

    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::", 3) != 0)
            continue;
        char *rel = line + 3;
        agent_resources_trim_newline(rel);
        int n = 0;
        if (rel[0] == '\0' || strcmp(rel, "/") == 0) {
            n = snprintf(out, out_len, "%s", AGENT_RESOURCES_CGROUP_ROOT);
        } else if (rel[0] == '/') {
            n = snprintf(out, out_len, "%s%s",
                         AGENT_RESOURCES_CGROUP_ROOT, rel);
        } else {
            n = snprintf(out, out_len, "%s/%s",
                         AGENT_RESOURCES_CGROUP_ROOT, rel);
        }
        if (n < 0 || (size_t)n >= out_len)
            break;
        ok = true;
        break;
    }

    fclose(f);
    return ok;
}

static int64_t agent_resources_cgroup_bytes(const char *dir, const char *name)
{
    if (!dir || !name)
        return -1; // raw-return-ok:optional-cgroup-unavailable

    char path[768];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -1; // raw-return-ok:optional-cgroup-unavailable

    FILE *f = fopen(path, "r");
    if (!f)
        return -1; // raw-return-ok:optional-cgroup-unavailable

    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1; // raw-return-ok:optional-cgroup-unavailable
    }
    fclose(f);
    agent_resources_trim_newline(buf);
    if (strcmp(buf, "max") == 0)
        return -1; // raw-return-ok:unlimited-cgroup-value

    long long value = -1;
    if (sscanf(buf, "%lld", &value) != 1 || value < 0)
        return -1; // raw-return-ok:optional-cgroup-unavailable
    return (int64_t)value;
}

static int64_t agent_resources_bytes_to_mb(int64_t bytes)
{
    if (bytes < 0)
        return -1; // raw-return-ok:sentinel
    return bytes / (1024 * 1024);
}

static int64_t agent_resources_pct(int64_t current_mb, int64_t limit_mb)
{
    if (current_mb < 0 || limit_mb <= 0)
        return -1; // raw-return-ok:sentinel
    return (current_mb * 100) / limit_mb;
}

static void agent_resources_collect_cgroup(struct agent_resource_snapshot *s)
{
    char dir[768];
    if (!agent_resources_cgroup_dir(dir, sizeof(dir)))
        return;

    int64_t current_bytes =
        agent_resources_cgroup_bytes(dir, "memory.current");
    if (current_bytes < 0)
        return;

    int64_t high_bytes = agent_resources_cgroup_bytes(dir, "memory.high");
    int64_t max_bytes = agent_resources_cgroup_bytes(dir, "memory.max");
    s->cgroup_memory_available = true;
    s->cgroup_memory_current_mb =
        agent_resources_bytes_to_mb(current_bytes);
    s->cgroup_memory_high_mb = agent_resources_bytes_to_mb(high_bytes);
    s->cgroup_memory_max_mb = agent_resources_bytes_to_mb(max_bytes);
    s->cgroup_memory_high_pct =
        agent_resources_pct(s->cgroup_memory_current_mb,
                            s->cgroup_memory_high_mb);
    s->cgroup_memory_max_pct =
        agent_resources_pct(s->cgroup_memory_current_mb,
                            s->cgroup_memory_max_mb);

    bool high_warn = s->cgroup_memory_high_mb > 0 &&
                     s->cgroup_memory_current_mb >=
                     s->cgroup_memory_high_mb;
    bool max_warn = s->cgroup_memory_max_mb > 0 &&
                    s->cgroup_memory_max_pct >= 90;
    s->cgroup_memory_warning = high_warn || max_warn;
}

static const char *agent_resources_pressure(
    const struct agent_resource_snapshot *s)
{
    if (!s)
        return "unknown";
    if (s->cgroup_memory_available)
        return s->cgroup_memory_warning ? "warn" : "ok";
    if (s->rss_mb < 0)
        return "unknown";
    return s->rss_warning ? "warn" : "ok";
}

static const char *agent_resources_pressure_basis(
    const struct agent_resource_snapshot *s)
{
    if (!s)
        return "unknown";
    if (s->cgroup_memory_available) {
        if (s->cgroup_memory_high_mb > 0)
            return "cgroup_high";
        if (s->cgroup_memory_max_mb > 0)
            return "cgroup_max";
        return "cgroup";
    }
    if (s->rss_mb >= 0)
        return "rss";
    return "unknown";
}

void agent_resource_snapshot_collect(struct agent_resource_snapshot *snapshot)
{
    if (!snapshot)
        return;

    snapshot->rss_mb = -1;
    snapshot->rss_warn_threshold_mb = AGENT_RESOURCES_RSS_WARN_MB;
    snapshot->rss_warning = false;
    snapshot->cgroup_memory_available = false;
    snapshot->cgroup_memory_current_mb = -1;
    snapshot->cgroup_memory_high_mb = -1;
    snapshot->cgroup_memory_max_mb = -1;
    snapshot->cgroup_memory_high_pct = -1;
    snapshot->cgroup_memory_max_pct = -1;
    snapshot->cgroup_memory_warning = false;
    snapshot->uptime_seconds = -1;

    int64_t rss_kb = agent_resources_rss_kb();
    if (rss_kb >= 0)
        snapshot->rss_mb = rss_kb / 1024;
    snapshot->rss_warning =
        snapshot->rss_mb > snapshot->rss_warn_threshold_mb;
    agent_resources_collect_cgroup(snapshot);
    snapshot->uptime_seconds = agent_resources_uptime_seconds();
}

void agent_push_resources_json(struct json_value *out, const char *key,
                               const struct agent_resource_snapshot *snapshot)
{
    struct agent_resource_snapshot live;
    if (!snapshot) {
        agent_resource_snapshot_collect(&live);
        snapshot = &live;
    }

    struct json_value resources = {0};
    json_set_object(&resources);
    json_push_kv_str(&resources, "schema", "zcl.node_resources.v1");
    json_push_kv_int(&resources, "schema_version", 1);
    json_push_kv_int(&resources, "rss_mb", snapshot->rss_mb);
    json_push_kv_int(&resources, "rss_warn_threshold_mb",
                     snapshot->rss_warn_threshold_mb);
    json_push_kv_bool(&resources, "rss_warning", snapshot->rss_warning);
    json_push_kv_bool(&resources, "cgroup_memory_available",
                      snapshot->cgroup_memory_available);
    json_push_kv_int(&resources, "cgroup_memory_current_mb",
                     snapshot->cgroup_memory_current_mb);
    json_push_kv_int(&resources, "cgroup_memory_high_mb",
                     snapshot->cgroup_memory_high_mb);
    json_push_kv_int(&resources, "cgroup_memory_max_mb",
                     snapshot->cgroup_memory_max_mb);
    json_push_kv_int(&resources, "cgroup_memory_high_pct",
                     snapshot->cgroup_memory_high_pct);
    json_push_kv_int(&resources, "cgroup_memory_max_pct",
                     snapshot->cgroup_memory_max_pct);
    json_push_kv_bool(&resources, "cgroup_memory_warning",
                      snapshot->cgroup_memory_warning);
    json_push_kv_str(&resources, "memory_pressure",
                     agent_resources_pressure(snapshot));
    json_push_kv_str(&resources, "pressure_basis",
                     agent_resources_pressure_basis(snapshot));
    json_push_kv_int(&resources, "uptime_seconds",
                     snapshot->uptime_seconds);
    json_push_kv_str(&resources, "source",
                     snapshot->cgroup_memory_available
                     ? "proc_self+cgroup_v2" : "proc_self");
    json_push_kv(out, key ? key : "resources", &resources);
    json_free(&resources);
}
