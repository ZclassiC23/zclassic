/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Fixed REST resource routes. The main router handles dynamic member paths;
 * this table owns exact resource/controller/action dispatch and /api/v1
 * canonicalization. */

#include "api_controller_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const struct api_resource_route k_api_resource_routes[] = {
    { "GET", "/api",               "api",       "index",    api_serve_api_index },
    { "GET", "/api/agent",         "agent",     "show",     api_serve_node_summary },
    { "GET", "/api/status",        "agent",     "show",     api_serve_node_summary },
    { "GET", "/api/milestone",     "milestone", "show",     api_serve_milestone },
    { "GET", "/api/refold",        "refold",    "show",     api_serve_refold_status },
    { "GET", "/api/node",          "node",      "show",     api_serve_node_summary },
    { "GET", "/api/node/summary",  "node",      "summary",  api_serve_node_summary },
    { "GET", "/api/node/status",   "node",      "status",   api_serve_node_status },
    { "GET", "/api/node/snapshot", "node",      "snapshot", api_serve_node_snapshot },
    { "GET", "/api/node/mmb",      "node",      "mmb",      api_serve_node_mmb },
    { "GET", "/api/health",        "health",    "show",     api_serve_health },
    { "GET", "/api/syncstate",     "sync",      "show",     api_serve_syncstate },
    { "GET", "/api/downloadstats", "downloads", "show",     api_serve_downloadstats },
    { "GET", "/api/blocks",        "blocks",    "index",    api_route_blocks },
    { "GET", "/api/stats",         "stats",     "index",    api_route_stats },
    { "GET", "/api/stats/deep",    "stats",     "deep",     api_route_deep_stats },
    { "GET", "/api/supply",        "supply",    "show",     api_route_supply },
    { "GET", "/api/hodl",          "hodl",      "show",     api_route_hodl },
    { "GET", "/api/factoids",      "factoids",  "show",     api_route_factoids },
    { "GET", "/api/wallet",        "wallet",    "show",     api_serve_wallet },
    { "GET", "/api/files/manifest", "files",    "manifest", api_serve_files_manifest },
};

const char *api_canonical_route_path(const char *path, char *buf,
                                     size_t buf_len)
{
    if (!path)
        return NULL;
    if (strcmp(path, ZCL_REST_API_BASE_PATH) == 0)
        return ZCL_REST_API_COMPAT_BASE_PATH;

    const char *prefix = ZCL_REST_API_BASE_PATH "/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) == 0) {
        if (!buf || buf_len == 0)
            return path;
        int n = snprintf(buf, buf_len, "%s/%s",
                         ZCL_REST_API_COMPAT_BASE_PATH,
                         path + prefix_len);
        if (n < 0 || (size_t)n >= buf_len)
            return path;
        return buf;
    }
    return path;
}

bool api_path_has_unsupported_version(const char *path, char *version_out,
                                      size_t version_out_len)
{
    if (!path || strncmp(path, "/api/v", 6) != 0 ||
        !isdigit((unsigned char)path[6]))
        return false;

    size_t len = 1; /* include the leading 'v' */
    const char *p = path + 6;
    while (isdigit((unsigned char)*p)) {
        len++;
        p++;
    }

    if (*p != '\0' && *p != '/' && *p != '?')
        return false;

    char version[32];
    if (len >= sizeof(version))
        len = sizeof(version) - 1;
    memcpy(version, path + 5, len);
    version[len] = '\0';

    if (strcmp(version, ZCL_REST_API_VERSION) == 0)
        return false;

    if (version_out && version_out_len > 0) {
        snprintf(version_out, version_out_len, "%s", version);
    }
    return true;
}

const struct api_resource_route *
api_resource_route_find(const char *method, const char *path)
{
    if (!method || !path)
        return NULL;
    size_t n = sizeof(k_api_resource_routes) /
               sizeof(k_api_resource_routes[0]);
    for (size_t i = 0; i < n; i++) {
        const struct api_resource_route *r = &k_api_resource_routes[i];
        if (strcmp(method, r->method) == 0 && strcmp(path, r->path) == 0)
            return r;
    }
    return NULL;
}

#ifdef ZCL_TESTING
size_t api_resource_route_count(void)
{
    return sizeof(k_api_resource_routes) / sizeof(k_api_resource_routes[0]);
}

const char *api_resource_route_resource_at(size_t i)
{
    if (i >= api_resource_route_count())
        return NULL;
    return k_api_resource_routes[i].resource;
}

const char *api_resource_route_action_at(size_t i)
{
    if (i >= api_resource_route_count())
        return NULL;
    return k_api_resource_routes[i].action;
}
#endif
