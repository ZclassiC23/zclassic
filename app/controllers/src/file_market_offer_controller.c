/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Market seller offer RPC: the typed plan/commit publisher behind the
 * native `app market offer` leaf. Kept out of file_market_controller.c
 * (E1 file-size ceiling); the legacy contained zmarket_offer placeholder
 * stays there and points here. */

#include "platform/time_compat.h"
#include "base/hex.h"
#include "controllers/network_controller.h"
#include "controllers/wallet_helpers.h"
#include "controllers/file_market_controller.h"
#include "chain/chainparams.h"
#include "net/file_market.h"
#include "net/file_service.h"
#include "net/msgprocessor.h"
#include "net/netaddr.h"
#include "net/version.h"
#include "services/file_market_offer_service.h"
#include "util/log_macros.h"
#include "json/json.h"
#include "rpc/server.h"
#include "models/database.h"
#include "wallet/sapling_keys.h"
#include "wallet/wallet.h"
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>

static const char *market_offer_code(const struct zcl_result *r)
{
    if (!r) return "OFFER_REFUSED";
    switch (r->code) {
    case -2:  return "CONTENT_UNAVAILABLE";
    case -3:  return "CONTENT_INVALID";
    case -4:  return "CONTENT_TOO_LARGE";
    case -5:  return "CONTENT_UNSTABLE";
    case -6:  return "PRICE_INVALID";
    case -7:  return "ENDPOINT_UNKNOWN";
    case -8:  return "PAYEE_UNAVAILABLE";
    case -9:  return "SELLER_KEY_UNAVAILABLE";
    case -10: return "SEAL_FAILED";
    case -11: return "OFFER_SAVE_FAILED";
    case -12: return "CONTENT_BIND_FAILED";
    case -13: return "WIRE_FAILED";
    default:  return "OFFER_REFUSED";
    }
}

static bool market_offer_refuse(struct json_value *result,
                                struct zcl_result r)
{
    json_set_object(result);
    json_push_kv_bool(result, "ok", false);
    json_push_kv_str(result, "code", market_offer_code(&r));
    json_push_kv_str(result, "message", r.message);
    return true;
}

static void market_offer_amount(int64_t zat, char out[32])
{
    (void)snprintf(out, 32, "%lld.%08lld",
                   (long long)(zat / 100000000LL),
                   (long long)(zat >= 0 ? zat % 100000000LL
                                        : -(zat % 100000000LL)));
}

static struct zcl_result market_offer_endpoint(void *opaque,
                                               uint8_t peer_ip[16],
                                               uint16_t *peer_port)
{
    (void)opaque;
    /* The signed offer body commits the seller endpoint, so the node may
     * only name an address it actually knows is its own: the operator-set
     * -externalip, plus the live file-service port peers already learn via
     * the zfileaddr handshake. Anything else would sign a guess. */
    char ip[64];
    uint16_t ext_port = 0;
    if (!msg_version_get_external_ip(ip, sizeof(ip), &ext_port))
        return ZCL_ERR(-1, "set -externalip=<public-ip> so the signed offer "
                           "names a reachable seller endpoint");
    uint16_t fs_port = fs_server_is_running() ? fs_server_get_port() : 0;
    if (fs_port == 0)
        return ZCL_ERR(-2, "the file service is not listening");
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip, &addr4) != 1)
        return ZCL_ERR(-3, "configured external IP is not IPv4");
    memset(peer_ip, 0, 16);
    memcpy(peer_ip, pchIPv4Prefix, 12);
    memcpy(peer_ip + 12, &addr4, 4);
    *peer_port = fs_port;
    return ZCL_OK;
}

static struct zcl_result market_offer_payee(void *opaque,
                                            uint8_t z_addr_out[43])
{
    struct wallet_rpc_context *ctx = opaque;
    /* The payee must be a real wallet-derived Sapling address: a seller who
     * cannot decrypt the incoming notes never gets paid. Refuse an unseeded
     * keystore rather than minting an unrecoverable address (the
     * zslp_service merchant-address discipline). */
    if (!ctx || !ctx->wallet || !ctx->wallet->sapling_keys.has_seed)
        return ZCL_ERR(-1, "a seeded wallet Sapling keystore is required to "
                           "receive payment");
    uint8_t diversifier[ZC_DIVERSIFIER_SIZE], pk_d[32];
    if (!sapling_keystore_new_address(&ctx->wallet->sapling_keys,
                                      diversifier, pk_d))
        return ZCL_ERR(-2, "could not mint a seller payment address");
    memcpy(z_addr_out, diversifier, ZC_DIVERSIFIER_SIZE);
    memcpy(z_addr_out + ZC_DIVERSIFIER_SIZE, pk_d, 32);
    return ZCL_OK;
}

static bool market_offer_announce(void *opaque, const uint8_t *wire,
                                  size_t wire_len)
{
    struct msg_processor *mp = opaque;
    if (!mp || !wire || wire_len != FILE_MARKET_OFFER_WIRE_BYTES)
        return false;
    msg_processor_flood_message(mp, MSG_FILE_OFFER, wire, wire_len);
    return true;
}

static bool market_offer_render(const struct market_offer_view *view,
                                bool committed, struct json_value *result)
{
    if (!view || !result) return false;
    char root_hex[65];
    zcl_hex_encode(view->root_hash, 32, root_hex);
    json_set_object(result);
    json_push_kv_bool(result, "ok", true);
    json_push_kv_str(result, "schema", "zcl.app_market_offer_result.v1");
    json_push_kv_str(result, "root_hash", root_hex);
    json_push_kv_str(result, "filename", view->filename);
    json_push_kv_int(result, "size_bytes", (int64_t)view->size_bytes);
    json_push_kv_int(result, "num_chunks", view->num_chunks);
    json_push_kv_int(result, "price_per_mb_zat", view->price_per_mb);
    char amount[32];
    market_offer_amount(view->total_zat, amount);
    json_push_kv_str(result, "total_zcl", amount);
    json_push_kv_int(result, "total_zat", view->total_zat);
    json_push_kv_int(result, "expires_unix", view->expires_unix);
    if (committed) {
        char id_hex[65], seller_hex[65];
        zcl_hex_encode(view->offer_id, 32, id_hex);
        zcl_hex_encode(view->seller_pubkey, 32, seller_hex);
        json_push_kv_str(result, "offer_id", id_hex);
        json_push_kv_str(result, "seller_pubkey", seller_hex);
        json_push_kv_bool(result, "idempotent_replay",
                          view->idempotent_replay);
        json_push_kv_bool(result, "announced", view->announced);
        json_push_kv_str(result, "endpoint_source", "-externalip");
    }
    return true;
}

static bool rpc_zmarket_offer_publish(const struct json_value *params,
                                      bool help, struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "zmarket_offer_publish {filepath,price_per_mb_zat,confirm}\n"
            "Plan (confirm absent) or commit (confirm:true) one signed paid "
            "file offer from a private local file.\n");
        return true;
    }
    const struct json_value *in = json_at(params, 0);
    const char *filepath = in ? json_get_str(json_get(in, "filepath")) : NULL;
    const struct json_value *price = in
        ? json_get(in, "price_per_mb_zat") : NULL;
    bool confirm = in && json_get_bool(json_get(in, "confirm"));
    if (!filepath || !filepath[0] || !price || price->type != JSON_INT) {
        json_set_str(result, "filepath and integer price_per_mb_zat are required");
        return false;
    }

    struct wallet_rpc_context *ctx = wallet_rpc_context_current();
    const struct chain_params *params_chain = chain_params_get();
    if (!ctx || !ctx->node_db || !ctx->node_db->open || !params_chain)
        return market_offer_refuse(result,
            ZCL_ERR(-1, "wallet context, market database, and chain parameters are required"));

    struct market_offer_runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.node_db = ctx->node_db;
    runtime.endpoint = market_offer_endpoint;
    runtime.payee = market_offer_payee;
    runtime.payee_ctx = ctx;
    runtime.announce = market_offer_announce;
    runtime.announce_ctx = rpc_net_get_msg_processor();
    memcpy(runtime.network_genesis,
           params_chain->consensus.hashGenesisBlock.data, 32);
    runtime.now_unix = (int64_t)platform_time_wall_time_t();

    struct market_offer_request request;
    memset(&request, 0, sizeof(request));
    request.filepath = filepath;
    request.price_per_mb_zat = json_get_int(price);

    struct market_offer_view view;
    struct zcl_result r = confirm
        ? file_market_offer_commit(&runtime, &request, &view)
        : file_market_offer_plan(&runtime, &request, &view);
    if (!r.ok)
        return market_offer_refuse(result, r);
    return market_offer_render(&view, confirm, result);
}

void register_market_offer_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "market", "zmarket_offer_publish",
          rpc_zmarket_offer_publish, false },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
