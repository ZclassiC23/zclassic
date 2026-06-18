/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Public HTTPS server for the block explorer.
 * Serves on 0.0.0.0:443 (TLS) + 0.0.0.0:80 (redirect).
 * No authentication — public read-only explorer. */

#ifndef ZCL_NET_HTTPS_SERVER_H
#define ZCL_NET_HTTPS_SERVER_H

#include <stdbool.h>

bool https_server_start(const char *cert_path, const char *key_path,
                         const char *hostname);
bool https_server_start_on_port(const char *cert_path, const char *key_path,
                                const char *hostname, int https_port, int http_port);
void https_server_stop(void);

void https_deferred_set(const char *cert, const char *key, const char *hostname);
void https_deferred_check(void);

#endif
