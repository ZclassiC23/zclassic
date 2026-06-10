/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Internal cross-translation-unit glue for the REST API controller.
 * Private to app/controllers/src/api_controller*.c. */

#ifndef ZCL_APP_CONTROLLERS_SRC_API_CONTROLLER_INTERNAL_H
#define ZCL_APP_CONTROLLERS_SRC_API_CONTROLLER_INTERNAL_H

#include "controllers/api_controller.h"
#include "models/database.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Shared context (defined in api_controller.c) ── */

struct api_context {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
    struct node_db *node_db;
    const char *datadir;
};

struct api_rpc_backend {
    char user[128];
    char pass[128];
    int port;
};

extern struct api_context g_api_ctx;
extern struct api_rpc_backend g_api_rpc;

/* ── HTTP response headers (kept here so siblings can emit errors) ── */

#define SECURITY_HEADERS \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-Frame-Options: DENY\r\n" \
    "Strict-Transport-Security: max-age=31536000\r\n"

#define JSON_HEADERS \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Content-Type\r\n" \
    "Cache-Control: public, max-age=10\r\n" \
    SECURITY_HEADERS \
    "Connection: close\r\n\r\n"

#define JSON_404_HEADERS \
    "HTTP/1.1 404 Not Found\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Connection: close\r\n\r\n"

#define JSON_500_HEADERS \
    "HTTP/1.1 500 Internal Server Error\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Connection: close\r\n\r\n"

#define JSON_503_HEADERS \
    "HTTP/1.1 503 Service Unavailable\r\n" \
    "Content-Type: application/json; charset=utf-8\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Retry-After: 10\r\n" \
    "Connection: close\r\n\r\n"

/* ── Helpers defined in api_controller.c ── */

size_t api_json_error(uint8_t *r, size_t max, const char *headers,
                  const char *message);
int api_rpc_call(const char *method, const char *params_json,
             char *out, size_t outmax);
bool api_is_json_safe_param(const char *s, size_t maxlen);
struct node_db *api_node_db(void);
bool api_is_printable_ascii(const char *s);
bool api_parse_zslp_limit(const char *path, size_t *limit_out);
bool api_parse_collection_limit(const char *path, const char *query_key,
                                size_t default_limit, size_t max_limit,
                                size_t *limit_out);
bool api_start_detached_thread(pthread_t *thread_out,
                               void *(*entry)(void *),
                               void *arg);

/* ── Compute handlers (defined in api_controller_compute.c) ── */

size_t compute_blocks(uint8_t *r, size_t max);
size_t compute_stats(uint8_t *r, size_t max);
size_t compute_supply(uint8_t *r, size_t max);
size_t compute_hodl(uint8_t *r, size_t max);
size_t compute_deep_stats(uint8_t *r, size_t max);

/* ── Lookup handlers (defined in api_controller_lookup.c) ── */

enum lookup_type {
    LOOKUP_NONE = 0,
    LOOKUP_BLOCK,
    LOOKUP_TX,
    LOOKUP_ADDRESS,
};

size_t compute_block(const char *param, uint8_t *r, size_t max);
size_t compute_tx(const char *param, uint8_t *r, size_t max);
size_t compute_address(const char *param, uint8_t *r, size_t max);

size_t do_lookup(enum lookup_type type, const char *param,
                 uint8_t *response, size_t response_max);

/* ── Resource collection handlers (defined in api_controller_resources.c) ── */

size_t api_serve_zslp_tokens(const char *path, uint8_t *response,
                             size_t response_max);
size_t api_serve_zslp_token(const char *token_id, uint8_t *response,
                            size_t response_max);
size_t api_serve_zslp_token_transfers(const char *path, const char *token_id,
                                      uint8_t *response, size_t response_max);
size_t api_serve_onion_announcements(const char *path, uint8_t *response,
                                     size_t response_max);
size_t api_serve_file_services(const char *path, uint8_t *response,
                               size_t response_max);
size_t api_serve_peers(const char *path, uint8_t *response,
                       size_t response_max);

/* ── Node / diagnostics route handlers (defined in api_controller_node.c) ──
 * None touch the background cache buffers in api_controller.c — they read
 * g_api_ctx + lock-free atomics and write directly into the caller's
 * response buffer, so the router's cache semantics are unchanged. */

size_t api_serve_events(const char *path, uint8_t *response,
                        size_t response_max);
size_t api_serve_syncstate(uint8_t *response, size_t response_max);
size_t api_serve_downloadstats(uint8_t *response, size_t response_max);
size_t api_serve_health(uint8_t *response, size_t response_max);
size_t api_serve_node_snapshot(uint8_t *response, size_t response_max);
size_t api_serve_node_mmb(uint8_t *response, size_t response_max);
size_t api_serve_node_status(uint8_t *response, size_t response_max);
size_t api_serve_wallet(uint8_t *response, size_t response_max);
size_t api_serve_files_manifest(uint8_t *response, size_t response_max);
size_t api_serve_file_chunk(const char *hex, uint8_t *response,
                            size_t response_max);

#endif
