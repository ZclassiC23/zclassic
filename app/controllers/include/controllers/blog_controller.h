/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Blog controller — serves static HTML blog over Tor onion service.
 * Files served from {datadir}/blog/ directory.
 * Also handles ZSLP node registry for peer discovery. */

#ifndef ZCL_CONTROLLERS_BLOG_H
#define ZCL_CONTROLLERS_BLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net/onion_discovery.h"

/* Serve an HTTP request for the blog.
 * path: URL path (e.g., "/", "/about", "/post/hello")
 * out: output buffer for HTTP response (headers + body)
 * out_len: size of output buffer
 * Returns bytes written, or 0 if path not found. */
size_t blog_serve(const char *datadir, const char *path,
                  char *out, size_t out_len);

/* ZSLP Node Registry — store/retrieve .onion addresses on-chain */

/* Build a ZSLP GENESIS tx for the ZCL23NODES token registry.
 * Returns the OP_RETURN script bytes. */
size_t blog_build_node_registry_genesis(uint8_t *out, size_t out_len);

/* Build a ZSLP SEND tx that announces an .onion address.
 * The onion hostname is stored in the token's metadata. */
size_t blog_build_node_announce(uint8_t *out, size_t out_len,
                                 const uint8_t token_id[32],
                                 const char *onion_hostname);

/* Scan the chain for ZCL23NODES token SEND txs.
 * Extracts .onion addresses for peer discovery.
 * Returns count of discovered addresses. */
int blog_discover_onion_peers(const char *datadir,
                               struct onion_peer *out, size_t max);

/* Auto-announce .onion address on-chain via ZSLP SEND.
 * Returns true if a new announcement was created.
 * Returns false if already published or on error. */
bool blog_auto_announce_onion(const char *datadir, const char *onion_address);

#endif
