/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_bundle_fetch_seeds — the instant-on weld's file-service SEED SET.
 * Split out of config/src/boot_bundle_fetch.c: WHICH peers the weld is allowed
 * to contact is a policy decision with its own trust argument, separate from
 * the manifest/download machinery. See config/boot_bundle_fetch.h. */

#include "boot_bundle_fetch_seeds_internal.h"

#include "config/boot.h"                       /* struct app_context */
#include "config/bundle_fetch_seeds.h"         /* ZCL_BUNDLE_FETCH_CLEARNET_SEEDS */
#include "net/file_service.h"                  /* FS_PORT default */
#include "net/rom_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Append host[:port] to peers[] (default port FS_PORT). No-op when full or the
 * host does not fit rom_fetch_peer.addr. */
void bbf_add_peer(struct rom_fetch_peer *peers, size_t *np, size_t cap,
                  const char *host_port)
{
    if (!peers || !np || *np >= cap || !host_port || !host_port[0])
        return;

    char host[128];
    snprintf(host, sizeof(host), "%s", host_port);
    uint16_t port = FS_PORT;

    /* A trailing ":<port>" overrides FS_PORT (operator/test convenience). Split
     * on the LAST ':' only when the suffix is a pure decimal port. */
    char *colon = strrchr(host, ':');
    if (colon && colon[1]) {
        char *end = NULL;
        long p = strtol(colon + 1, &end, 10);
        if (end && *end == '\0' && p >= 1 && p <= 65535) {
            port = (uint16_t)p;
            *colon = '\0';
        }
    }
    if (!host[0] || strlen(host) >= sizeof(peers[0].addr))
        return;

    /* De-dup on (addr, port). */
    for (size_t i = 0; i < *np; i++)
        if (peers[i].port == port && strcmp(peers[i].addr, host) == 0)
            return;

    snprintf(peers[*np].addr, sizeof(peers[*np].addr), "%s", host);
    peers[*np].port = port;
    (*np)++;
}

/* Append host[:port] with the named port STRIPPED, so a `-connect=HOST:8033`
 * value is contacted on the file service's own FS_PORT rather than the P2P port
 * it names. Same split rule as bbf_add_peer (LAST ':' + pure decimal port), so
 * a bare IPv6 literal keeps its final group and the bracketed form splits. */
static void bbf_add_peer_host_only(struct rom_fetch_peer *peers, size_t *np,
                                   size_t cap, const char *host_port)
{
    if (!host_port || !host_port[0])
        return;
    char host[128];
    snprintf(host, sizeof(host), "%s", host_port);
    char *colon = strrchr(host, ':');
    if (colon && colon[1]) {
        char *end = NULL;
        long p = strtol(colon + 1, &end, 10);
        if (end && *end == '\0' && p >= 1 && p <= 65535)
            *colon = '\0';
    }
    if (host[0])
        bbf_add_peer(peers, np, cap, host);
}

size_t bbf_assemble_seeds(const struct app_context *ctx,
                          struct rom_fetch_peer *peers, size_t cap,
                          bool *out_explicit_first)
{
    size_t np = 0;
    if (out_explicit_first)
        *out_explicit_first = false;

    if (ctx && ctx->file_service_peer && ctx->file_service_peer[0]) {
        bbf_add_peer(peers, &np, cap, ctx->file_service_peer);
        if (out_explicit_first && np == 1)
            *out_explicit_first = true; /* the explicit peer took slot 0 */
    }

    if (ctx && ctx->connect_only) {
        for (int i = 0; i < ctx->n_connect_peers; i++)
            bbf_add_peer_host_only(peers, &np, cap, ctx->connect_peers[i]);
    } else {
        for (int i = 0; ZCL_BUNDLE_FETCH_CLEARNET_SEEDS[i]; i++)
            bbf_add_peer(peers, &np, cap, ZCL_BUNDLE_FETCH_CLEARNET_SEEDS[i]);
    }
    return np;
}

size_t boot_bundle_fetch_seed_count(const struct app_context *ctx)
{
    struct rom_fetch_peer peers[ROM_FETCH_MAX_WORKERS];
    memset(peers, 0, sizeof(peers));
    return bbf_assemble_seeds(ctx, peers, sizeof(peers) / sizeof(peers[0]),
                              NULL);
}
