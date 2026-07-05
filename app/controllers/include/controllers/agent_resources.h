/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_AGENT_RESOURCES_H
#define ZCL_CONTROLLERS_AGENT_RESOURCES_H

#include <stdbool.h>
#include <stdint.h>

struct json_value;

struct agent_resource_snapshot {
    int64_t rss_mb;
    int64_t rss_warn_threshold_mb;
    bool rss_warning;
    bool cgroup_memory_available;
    int64_t cgroup_memory_current_mb;
    int64_t cgroup_memory_high_mb;
    int64_t cgroup_memory_max_mb;
    int64_t cgroup_memory_high_pct;
    int64_t cgroup_memory_max_pct;
    bool cgroup_memory_warning;
    int64_t uptime_seconds;
};

void agent_resource_snapshot_collect(struct agent_resource_snapshot *snapshot);
void agent_push_resources_json(struct json_value *out, const char *key,
                               const struct agent_resource_snapshot *snapshot);

#endif
