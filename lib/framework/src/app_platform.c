/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/app_platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool reject(char *why, size_t why_sz, const char *fmt, ...)
{
    if (why && why_sz > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, why_sz, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool valid_token(const char *value, size_t max_len,
                        bool allow_slash)
{
    if (!value || !value[0])
        return false;
    size_t len = strlen(value);
    if (len > max_len)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (isalnum(c) || c == '_' || c == '-' || c == '.' ||
            (allow_slash && c == '/'))
            continue;
        return false;
    }
    return true;
}

static bool valid_sha256(const char *hex)
{
    if (!hex || strlen(hex) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)hex[i]))
            return false;
    }
    return true;
}

const char *zcl_app_capability_name(uint64_t cap)
{
    switch (cap) {
    case ZCL_APP_CAP_CHAIN_READ: return "chain_read";
    case ZCL_APP_CAP_SIGNED_EVENTS: return "signed_events";
    case ZCL_APP_CAP_RESIDENT_STATE: return "resident_state";
    case ZCL_APP_CAP_WEB_ROUTES: return "web_routes";
    case ZCL_APP_CAP_ONION_BINDING: return "onion_binding";
    case ZCL_APP_CAP_ZNAM_BINDING: return "znam_binding";
    case ZCL_APP_CAP_P2P_TOPICS: return "p2p_topics";
    case ZCL_APP_CAP_WALLET_REQUESTS: return "wallet_requests";
    case ZCL_APP_CAP_SCHEDULED_JOBS: return "scheduled_jobs";
    case ZCL_APP_CAP_CLOCK: return "clock";
    case ZCL_APP_CAP_RANDOM: return "random";
    default: return NULL;
    }
}

bool zcl_app_manifest_v1_validate(const struct zcl_app_manifest_v1 *m,
                                  uint64_t host_caps,
                                  const char *expected_build,
                                  char *why,
                                  size_t why_sz)
{
    if (why && why_sz > 0)
        why[0] = 0;
    if (!m)
        return reject(why, why_sz, "manifest is null");
    if (m->struct_size < sizeof(*m) ||
        m->manifest_version != ZCL_APP_MANIFEST_V1 ||
        m->required_host_abi != ZCL_APP_HOST_ABI_V1)
        return reject(why, why_sz, "app or host ABI mismatch");
    if (!valid_token(m->app_id, ZCL_APP_ID_MAX, false))
        return reject(why, why_sz, "invalid app_id");
    if (!m->display_name || !m->display_name[0] ||
        !valid_token(m->app_version, 31, false))
        return reject(why, why_sz, "invalid display name or version");
    if (!m->build_identity || !expected_build ||
        strcmp(m->build_identity, expected_build) != 0)
        return reject(why, why_sz, "build identity mismatch");
    if (!valid_sha256(m->content_sha256))
        return reject(why, why_sz, "invalid content sha256");
    if ((m->required_capabilities & ~host_caps) != 0)
        return reject(why, why_sz, "required host capability is unavailable");
    if (m->route_count > 0 && !m->routes)
        return reject(why, why_sz, "route table missing");
    if (m->topic_count > 0 && !m->topics)
        return reject(why, why_sz, "topic table missing");
    if (!m->self_test || !m->quiesce)
        return reject(why, why_sz, "self-test or quiescence hook missing");

    for (size_t i = 0; i < m->route_count; i++) {
        const struct zcl_app_route_v1 *route = &m->routes[i];
        if (route->struct_size < sizeof(*route) || !route->handler ||
            !valid_token(route->method, 16, false) ||
            !valid_token(route->path, ZCL_APP_ROUTE_MAX, true) ||
            route->path[0] != '/')
            return reject(why, why_sz, "invalid app route at index %zu", i);
        for (size_t j = 0; j < i; j++) {
            if (strcmp(route->method, m->routes[j].method) == 0 &&
                strcmp(route->path, m->routes[j].path) == 0)
                return reject(why, why_sz, "duplicate app route at index %zu", i);
        }
    }
    for (size_t i = 0; i < m->topic_count; i++) {
        const struct zcl_app_topic_v1 *topic = &m->topics[i];
        if (topic->struct_size < sizeof(*topic) ||
            !valid_token(topic->name, ZCL_APP_TOPIC_MAX, false) ||
            topic->wire_version == 0 || topic->max_event_bytes == 0)
            return reject(why, why_sz, "invalid app topic at index %zu", i);
    }
    if (m->state_schema_version > 0) {
        if (!(m->required_capabilities & ZCL_APP_CAP_RESIDENT_STATE))
            return reject(why, why_sz, "state schema lacks resident_state capability");
        if (!m->migration || m->migration->struct_size < sizeof(*m->migration) ||
            !m->migration->prepare || !m->migration->commit ||
            !m->migration->abort)
            return reject(why, why_sz, "stateful app lacks transactional migration hooks");
    }
    return true;
}
