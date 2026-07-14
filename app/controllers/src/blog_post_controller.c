/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: expose the Blog MVC resource and its read-only public HTTP routes. */

#include "controllers/blog_post_controller.h"

#include "config/runtime.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "views/blog_post_view.h"
#include "znam/znam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct zcl_result blog_post_controller_create(
    struct node_db *ndb, struct wallet *wallet,
    const struct zcl_app_event_signing_binding_v1 *binding,
    const struct blog_publish_request *request,
    struct blog_publish_result *out)
{
    if (!request || !request->blog_name || !request->slug ||
        !request->title || !request->body)
        return ZCL_ERR(-1, "BlogPost#create requires name, slug, title, and body");
    return blog_publication_create(ndb, wallet, binding, request, out);
}

struct zcl_result blog_post_controller_import(
    struct node_db *ndb, const struct zcl_app_signed_event_v1 *event,
    struct db_blog_post *out)
{
    return blog_publication_import_event(ndb, event, out);
}

struct zcl_result blog_post_controller_show(
    struct node_db *ndb, const char *blog_name, const char *slug,
    struct blog_post_page *out)
{
    if (!ndb || !ndb->open || !blog_name || !slug || !out)
        return ZCL_ERR(-1, "BlogPost#show requires db, name, slug, and output");
    memset(out, 0, sizeof(*out));
    if (!db_blog_post_find_by_slug(ndb, blog_name, slug, &out->post))
        return ZCL_ERR(-2, "BlogPost#show did not find the resource");
    uint8_t payload[BLOG_BODY_MAX + BLOG_TITLE_MAX + BLOG_NAME_MAX +
                    BLOG_SLUG_MAX + 32];
    struct zcl_app_signed_event_v1 verified_event;
    struct zcl_result verified = blog_publication_export_event(
        &out->post, payload, sizeof(payload), &verified_event);
    if (!verified.ok)
        return verified;
    out->content_available = true;
    /* Public GET/HEAD is strictly read-only.  Chain observation persists a
     * projection receipt and therefore belongs to the owned publication/job
     * lane, never an unauthenticated request path. */
    if (db_blog_publication_receipt_find_by_event(
            ndb, out->post.event_id, &out->receipt)) {
        out->has_receipt = true;
    }
    /* Receipts in v28 are node.db projection evidence only. The future live
     * verifier sets this after H* + active-slot/body proof. */
    out->served_frontier_proven = false;
    return ZCL_OK;
}

struct zcl_result blog_post_controller_index(
    struct node_db *ndb, const char *blog_name_or_null,
    struct blog_post_index_page *out)
{
    if (!ndb || !ndb->open || !out)
        return ZCL_ERR(-1, "BlogPost#index requires an open db and output");
    if (blog_name_or_null && blog_name_or_null[0] &&
        !znam_validate_name(blog_name_or_null))
        return ZCL_ERR(-2, "BlogPost#index name is not canonical");
    memset(out, 0, sizeof(*out));
    if (blog_name_or_null && blog_name_or_null[0])
        (void)snprintf(out->blog_name, sizeof(out->blog_name), "%s",
                       blog_name_or_null);
    out->count = blog_publication_recent_verified_summaries(
        ndb, blog_name_or_null, out->posts, BLOG_POST_INDEX_MAX);
    return ZCL_OK;
}

static size_t blog_http_response(const char *status,
                                 const uint8_t *body, size_t body_len,
                                 bool head_only,
                                 uint8_t *response, size_t response_max)
{
    if (!status || !body || !response)
        return 0;
    int header_len = snprintf((char *)response, response_max,
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
        "base-uri 'none'; frame-ancestors 'none'; form-action 'none'\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, body_len);
    if (header_len < 0 || (size_t)header_len >= response_max)
        return 0;
    if (head_only)
        return (size_t)header_len;
    if (body_len > response_max - (size_t)header_len)
        return 0;
    memcpy(response + header_len, body, body_len);
    return (size_t)header_len + body_len;
}

static bool blog_path_segments(const char *path,
                               char name[BLOG_NAME_MAX + 1],
                               char slug[BLOG_SLUG_MAX + 1],
                               bool *has_name, bool *has_slug)
{
    *has_name = false;
    *has_slug = false;
    name[0] = 0;
    slug[0] = 0;
    if (!path || strncmp(path, "/blog", 5) != 0 ||
        (path[5] != 0 && path[5] != '/'))
        return false;
    const char *p = path + 5;
    if (*p == '/')
        p++;
    if (*p == 0)
        return true;
    const char *slash = strchr(p, '/');
    size_t name_len = slash ? (size_t)(slash - p) : strlen(p);
    if (name_len == 0 || name_len > BLOG_NAME_MAX)
        return false;
    memcpy(name, p, name_len);
    name[name_len] = 0;
    *has_name = true;
    if (!slash)
        return true;
    p = slash + 1;
    if (!*p || strchr(p, '/'))
        return false;
    size_t slug_len = strlen(p);
    if (slug_len == 0 || slug_len > BLOG_SLUG_MAX)
        return false;
    memcpy(slug, p, slug_len + 1);
    *has_slug = true;
    return true;
}

size_t blog_site_handle_request(const char *method, const char *path,
                                const uint8_t *body, size_t body_len,
                                uint8_t *response, size_t response_max)
{
    (void)body;
    (void)body_len;
    if (!method || !path || !response || response_max < 512)
        return 0;
    bool head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        static const uint8_t denied[] =
            "<!doctype html><h1>405 Method Not Allowed</h1>";
        return blog_http_response("405 Method Not Allowed", denied,
                                  sizeof(denied) - 1, false,
                                  response, response_max);
    }
    const char *query = strchr(path, '?');
    size_t path_len = query ? (size_t)(query - path) : strlen(path);
    char clean_path[256];
    if (path_len == 0 || path_len >= sizeof(clean_path)) {
        static const uint8_t bad[] = "<!doctype html><h1>400 Bad Request</h1>";
        return blog_http_response("400 Bad Request", bad, sizeof(bad) - 1,
                                  head_only, response, response_max);
    }
    memcpy(clean_path, path, path_len);
    clean_path[path_len] = 0;
    char name[BLOG_NAME_MAX + 1], slug[BLOG_SLUG_MAX + 1];
    bool has_name = false, has_slug = false;
    if (!blog_path_segments(clean_path, name, slug, &has_name, &has_slug) ||
        (has_name && !znam_validate_name(name))) {
        static const uint8_t missing[] = "<!doctype html><h1>404 Not Found</h1>";
        return blog_http_response("404 Not Found", missing,
                                  sizeof(missing) - 1, head_only,
                                  response, response_max);
    }
    struct node_db *ndb = app_runtime_node_db();
    if (!ndb || !ndb->open) {
        LOG_WARN("blog", "public Blog requested while node.db is unavailable");
        return 0; /* caller may use the legacy static fallback */
    }
    size_t body_capacity = response_max < 262144 ? response_max : 262144;
    uint8_t *rendered = zcl_malloc(body_capacity, "blog site response body");
    if (!rendered) {
        LOG_WARN("blog", "public Blog response allocation failed");
        return 0;
    }
    size_t rendered_len = 0;
    struct zcl_result result;
    if (has_slug) {
        struct blog_post_page page;
        result = blog_post_controller_show(ndb, name, slug, &page);
        if (result.ok)
            rendered_len = blog_post_view_render(
                &page, rendered, body_capacity);
    } else {
        struct blog_post_index_page page;
        result = blog_post_controller_index(
            ndb, has_name ? name : NULL, &page);
        if (result.ok)
            rendered_len = blog_post_index_view_render(
                &page, rendered, body_capacity);
    }
    size_t response_len = 0;
    if (result.ok && rendered_len > 0) {
        response_len = blog_http_response("200 OK", rendered, rendered_len,
                                          head_only, response, response_max);
    } else {
        static const uint8_t missing[] = "<!doctype html><h1>404 Not Found</h1>";
        response_len = blog_http_response("404 Not Found", missing,
                                          sizeof(missing) - 1, head_only,
                                          response, response_max);
    }
    free(rendered);
    return response_len;
}
