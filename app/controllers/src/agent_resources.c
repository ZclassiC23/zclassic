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

void agent_resource_snapshot_collect(struct agent_resource_snapshot *snapshot)
{
    if (!snapshot)
        return;

    snapshot->rss_mb = -1;
    snapshot->rss_warn_threshold_mb = AGENT_RESOURCES_RSS_WARN_MB;
    snapshot->rss_warning = false;
    snapshot->uptime_seconds = -1;

    int64_t rss_kb = agent_resources_rss_kb();
    if (rss_kb >= 0)
        snapshot->rss_mb = rss_kb / 1024;
    snapshot->rss_warning =
        snapshot->rss_mb > snapshot->rss_warn_threshold_mb;
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
    json_push_kv_str(&resources, "memory_pressure",
                     snapshot->rss_mb < 0 ? "unknown" :
                     snapshot->rss_warning ? "warn" : "ok");
    json_push_kv_int(&resources, "uptime_seconds",
                     snapshot->uptime_seconds);
    json_push_kv_str(&resources, "source", "proc_self");
    json_push_kv(out, key ? key : "resources", &resources);
    json_free(&resources);
}
