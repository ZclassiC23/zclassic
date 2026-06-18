/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Clearnet frontend service lifecycle: the start/stop adapters for the
 * operator-facing surfaces — local file-transfer server, JSON-RPC HTTP
 * endpoint, block-explorer API cache, HTTPS explorer, the miner, the
 * embedded Tor onion service, and the store payment processor — plus the
 * onion request adapter and the spec-table registrar that wires them into
 * svc->frontend_kernel.
 *
 * Part of the boot composition root (paired with config/src/boot_services.c
 * via config/boot_internal.h). These services are OPTIONAL and gated on the
 * runtime profile; a start failure degrades the node rather than crashing it.
 * NONE of this file participates in the SIGTERM shutdown sequence — the
 * frontend kernel is torn down by zcl_service_kernel_stop_all() from
 * boot_services.c's shutdown path. The thread-spawning surfaces (HTTPS, Tor,
 * miner, file server) own their internal worker lifecycles inside the called
 * library functions; these adapters spawn no threads of their own.
 */

#include "config/boot_internal.h"
#include "config/boot_background_workers.h"
#include "config/boot_msg_callbacks.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_controller.h"
#include "net/file_service.h"
#include "net/https_server.h"
#include "net/onion_service.h"
#include "net/tor_integration.h"
#include "rpc/httpserver.h"
#include "chain/chainparams.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

extern _Atomic int g_deferred_proof_validation_below_height;

/* Tor → controller bridge: hands an onion HTTP request straight to the same
 * controller surface the clearnet endpoints use (no SOCKS, no port). */
static size_t onion_request_adapter(const char *method, const char *path,
                                    const uint8_t *body, size_t body_len,
                                    uint8_t *response, size_t response_max,
                                    void *user_data);

static bool boot_file_service_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->app_ctx || !boot_profile_has_file_service(svc->app_ctx))
        return true;
    if (svc->defer_offer_service) {
        printf("File service server deferred during fresh bootstrap receiver mode\n");
        return true;
    }
    fs_server_start(svc->datadir, (uint16_t)svc->app_ctx->fs_port);
    return true;
}

static void boot_file_service_stop(void *ctx)
{
    (void)ctx;
    fs_server_stop();
}

static bool boot_rpc_http_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->app_ctx)
        return false;
    set_rpc_warmup_finished();
    rpc_http_start(svc->rpc_table, (uint16_t)svc->app_ctx->rpc_port,
                   svc->app_ctx->rpc_user, svc->app_ctx->rpc_password,
                   svc->datadir);
    return true;
}

static void boot_rpc_http_stop(void *ctx)
{
    (void)ctx;
    rpc_http_stop();
}

/* Point the explorer + API cache backends at the local JSON-RPC endpoint,
 * preferring the .cookie credential and falling back to configured user/pass.
 * Also called once directly from app_init_services in boot_services.c. */
void boot_configure_frontend_rpc(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->app_ctx)
        return;

    char cookie_path[1024], cookie[256] = "";
    snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", svc->datadir);
    FILE *cf = fopen(cookie_path, "r");
    if (cf) {
        size_t n = fread(cookie, 1, sizeof(cookie) - 1, cf);
        fclose(cf);
        cookie[n] = '\0';
        char *nl = strchr(cookie, '\n');
        if (nl)
            *nl = '\0';
        char *colon = strchr(cookie, ':');
        if (colon) {
            *colon = '\0';
            api_set_rpc_backend(cookie, colon + 1, svc->app_ctx->rpc_port);
            explorer_set_rpc(cookie, colon + 1, svc->app_ctx->rpc_port);
            return;
        }
    }

    if (svc->app_ctx->rpc_user && svc->app_ctx->rpc_password) {
        api_set_rpc_backend(svc->app_ctx->rpc_user,
                            svc->app_ctx->rpc_password,
                            svc->app_ctx->rpc_port);
        explorer_set_rpc(svc->app_ctx->rpc_user,
                         svc->app_ctx->rpc_password,
                         svc->app_ctx->rpc_port);
        return;
    }

    api_set_rpc_backend(NULL, NULL, svc->app_ctx->rpc_port);
    explorer_set_rpc(NULL, NULL, svc->app_ctx->rpc_port);
}

static bool boot_api_cache_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->app_ctx || !boot_profile_has_explorer(svc->app_ctx))
        return true;

    int chain_tip_h = active_chain_height(&svc->state->chain_active);
    int best_header = svc->state->pindex_best_header ?
        svc->state->pindex_best_header->nHeight : chain_tip_h;
    if (best_header - chain_tip_h > 1000) {
        printf("API cache refresh deferred during IBD "
               "(chain=%d, headers=%d, behind=%d)\n",
               chain_tip_h, best_header, best_header - chain_tip_h);
        return true;
    }

    boot_configure_frontend_rpc(svc);
    api_start_cache();
    return true;
}

static void boot_api_cache_stop(void *ctx)
{
    (void)ctx;
    api_stop_cache();
}

static bool boot_https_explorer_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->app_ctx || !boot_profile_has_explorer(svc->app_ctx))
        return true;

    char cert_path[1024], key_path[1024];
    snprintf(cert_path, sizeof(cert_path), "%s/ssl/fullchain.pem",
             svc->datadir);
    snprintf(key_path, sizeof(key_path), "%s/ssl/privkey.pem",
             svc->datadir);
    if (access(cert_path, R_OK) != 0 || access(key_path, R_OK) != 0) {
        printf("HTTPS: no cert at %s - block explorer not on clearnet\n",
               cert_path);
        return true;
    }

    boot_configure_frontend_rpc(svc);

    int chain_tip_h = active_chain_height(&svc->state->chain_active);
    int best_header = svc->state->pindex_best_header ?
        svc->state->pindex_best_header->nHeight : chain_tip_h;
    bool near_tip = (best_header - chain_tip_h < 1000) &&
                    (chain_tip_h > g_deferred_proof_validation_below_height - 10000);
    /* Optional TLS servername (-httpsdomain=). NULL is fine: with a single
     * cert the server presents that cert regardless of SNI. */
    const char *https_domain = svc->app_ctx->https_domain;
    if (near_tip) {
        https_server_start_on_port(cert_path, key_path, https_domain,
                                   svc->app_ctx->https_port,
                                   svc->app_ctx->https_port - 363);
    } else {
        printf("HTTPS: deferred during IBD (chain=%d, headers=%d, "
               "behind=%d). Will start when near tip.\n",
               chain_tip_h, best_header, best_header - chain_tip_h);
        static char s_cert[1024], s_key[1024];
        strncpy(s_cert, cert_path, sizeof(s_cert) - 1);
        strncpy(s_key, key_path, sizeof(s_key) - 1);
        https_deferred_set(s_cert, s_key, https_domain);
    }
    return true;
}

static void boot_https_explorer_stop(void *ctx)
{
    (void)ctx;
    https_server_stop();
}

static bool boot_miner_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    const struct app_context *app = svc ? svc->app_ctx : NULL;
    if (!svc || !app || !app->gen)
        return true;

    svc->gen->ms = svc->state;
    svc->gen->coins_tip = svc->coins_tip;
    svc->gen->mempool = svc->mempool;
    svc->gen->params = svc->params;
    svc->gen->num_threads = app->gen_threads > 0 ? app->gen_threads : 1;
    svc->gen->block_found = boot_submit_mined_block;
    svc->gen->block_found_ctx = svc;
    svc->gen->coinbase_script.size = 0;

    if (app->miner_address) {
        size_t pk_pfx_len, sc_pfx_len;
        const unsigned char *pk_pfx = chain_params_base58_prefix(
            svc->params, B58_PUBKEY_ADDRESS, &pk_pfx_len);
        const unsigned char *sc_pfx = chain_params_base58_prefix(
            svc->params, B58_SCRIPT_ADDRESS, &sc_pfx_len);
        struct tx_destination dest;
        if (decode_destination(app->miner_address, pk_pfx, pk_pfx_len,
                               sc_pfx, sc_pfx_len, &dest))
            script_for_destination(&svc->gen->coinbase_script, &dest);
    }

    gen_start(svc->gen);
    return true;
}

static void boot_miner_stop(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (svc && svc->gen && svc->gen->running)
        gen_stop(svc->gen);
}

static bool boot_onion_tor_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->app_ctx)
        return false;

    char onion_dir[512];
    snprintf(onion_dir, sizeof(onion_dir), "%s/onion-keys", svc->datadir);
    struct stat onion_st;
    bool has_onion_keys = (stat(onion_dir, &onion_st) == 0);

    if (!boot_profile_has_onion(svc->app_ctx) && !has_onion_keys) {
        printf("Tor: skipped (use -tor or -profile=onion-node to enable)\n");
        return true;
    }

    onion_service_start(svc->datadir);
    tor_integration_set_handler(onion_request_adapter, NULL);
    printf("Starting embedded Tor...\n");
    if (!tor_integration_start(svc->datadir,
                               (uint16_t)svc->app_ctx->p2p_port)) {
        fprintf(stderr, "Warning: Tor failed to start\n");
    } else {
        const char *onion = tor_integration_get_onion_address();
        if (onion)
            printf("Tor .onion address: %s\n", onion);
        else
            printf("Tor: bootstrapping...\n");
    }
    return true;
}

static void boot_onion_tor_stop(void *ctx)
{
    (void)ctx;
    tor_integration_stop();
    onion_service_stop();
}

static bool boot_store_payment_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->app_ctx || !boot_profile_has_store(svc->app_ctx))
        return true;
    if (svc->defer_payment_service) {
        printf("Store payment processor deferred during bootstrap receiver mode\n");
        return true;
    }
    if (!boot_start_payment_service(svc)) {
        fprintf(stderr,
                "WARNING: failed to start tracked payment processor thread\n");
    }
    return true;
}

static void boot_store_payment_stop(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (svc)
        boot_join_payment_service(svc);
}

/* Register every clearnet frontend service into svc->frontend_kernel.
 * Called once from app_init_services in boot_services.c before the kernel
 * is started. Returns false on the first registration failure. */
bool boot_register_frontend_services(struct boot_svc_ctx *svc)
{
    const struct zcl_service_spec specs[] = {
        {
            .name = "file_service",
            .start = boot_file_service_start,
            .stop = boot_file_service_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "rpc_http",
            .start = boot_rpc_http_start,
            .stop = boot_rpc_http_stop,
            .ctx = svc,
        },
        {
            .name = "api_cache",
            .start = boot_api_cache_start,
            .stop = boot_api_cache_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "https_explorer",
            .start = boot_https_explorer_start,
            .stop = boot_https_explorer_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "miner",
            .start = boot_miner_start,
            .stop = boot_miner_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "onion_tor",
            .start = boot_onion_tor_start,
            .stop = boot_onion_tor_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "store_payment",
            .start = boot_store_payment_start,
            .stop = boot_store_payment_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
    };

    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        if (!zcl_service_kernel_register(&svc->frontend_kernel, &specs[i]))
            return false;
    }
    return true;
}

extern size_t onion_service_handle_request(const char *, const char *,
    const uint8_t *, size_t, uint8_t *, size_t);

static size_t onion_request_adapter(const char *method, const char *path,
    const uint8_t *req_data, size_t req_len,
    uint8_t *resp, size_t resp_max, void *ctx)
{
    (void)ctx;
    return onion_service_handle_request(method, path,
        req_data, req_len, resp, resp_max);
}
