/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP app controller: names, messaging, tokens, file market, atomic
 * swaps — everything built on top of the base chain. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/path_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── ZSLP tokens ────────────────────────────────────────────── */

DEFINE_PT(h_zcl_tokens, "zslp_listtokens", "mcp.app")

/* ── Names (ZNAM) ───────────────────────────────────────────── */

DEFINE_PT(h_zcl_name_list, "name_list", "mcp.app")

DEFINE_PT_STR(h_zcl_name_resolve, "name", "name_resolve", "mcp.app")

static int h_zcl_name_register(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *t = json_get_str(json_get(req->args, "type"));
    const char *v = json_get_str(json_get(req->args, "value"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, t);
    mcp_params_push_str(&p, v);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_register", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "name_register", "mcp.app",
                                   "name=%s", n ? n : "(null)");
}

static int h_zcl_name_update(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *t = json_get_str(json_get(req->args, "type"));
    const char *v = json_get_str(json_get(req->args, "value"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, t);
    mcp_params_push_str(&p, v);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_update", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "name_update", "mcp.app",
                                   "name=%s", n ? n : "(null)");
}

static int h_zcl_name_transfer(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *o = json_get_str(json_get(req->args, "new_owner"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, o);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_transfer", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "name_transfer", "mcp.app",
                                   "name=%s", n ? n : "(null)");
}

static int h_zcl_name_renew(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_renew", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "name_renew", "mcp.app",
                                   "name=%s", n ? n : "(null)");
}

static int h_zcl_name_set_record(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *t = json_get_str(json_get(req->args, "type"));
    const char *v = json_get_str(json_get(req->args, "value"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, t);
    mcp_params_push_str(&p, v);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_set_record", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "name_set_record", "mcp.app",
                                   "name=%s", n ? n : "(null)");
}

static int h_zcl_name_set_text(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *k = json_get_str(json_get(req->args, "key"));
    const char *v = json_get_str(json_get(req->args, "value"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, k);
    mcp_params_push_str(&p, v);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_set_text", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "name_set_text", "mcp.app",
                                   "name=%s", n ? n : "(null)");
}

/* ── Messaging (ZMSG) ───────────────────────────────────────── */

static int h_zcl_msg_send_named(const struct mcp_request *req, struct mcp_response *res)
{
    const char *n = json_get_str(json_get(req->args, "name"));
    const char *m = json_get_str(json_get(req->args, "message"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    mcp_params_push_str(&p, m);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("msg_send_named", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "msg_send_named", "mcp.app",
                                   "name=%s", n ? n : "(null)");
}

static int h_zcl_msg_send(const struct mcp_request *req, struct mcp_response *res)
{
    const char *ch = json_get_str(json_get(req->args, "channel"));
    const char *m  = json_get_str(json_get(req->args, "message"));
    bool onchain = ch && strcmp(ch, "onchain") == 0;
    struct mcp_params p;
    mcp_params_init(&p);
    if (onchain) {
        /* onchain: recipient z-address, message, channel, funding address,
         * optional reply-to. Positional order matches the msg_send RPC. */
        const char *to    = json_get_str(json_get(req->args, "to"));
        const char *from  = json_get_str(json_get(req->args, "from_address"));
        const char *reply = json_get_str(json_get(req->args, "reply_to"));
        mcp_params_push_str(&p, to ? to : "");
        mcp_params_push_str(&p, m);
        mcp_params_push_str(&p, "onchain");
        mcp_params_push_str(&p, from ? from : "");
        if (reply && reply[0])
            mcp_params_push_str(&p, reply);
    } else {
        int64_t pid = json_get_int(json_get(req->args, "peer_id"));
        mcp_params_push_int(&p, pid);
        mcp_params_push_str(&p, m);
    }
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("msg_send", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "msg_send", "mcp.app",
                                   "channel=%s", onchain ? "onchain" : "p2p");
}

static int h_zcl_msg_inbox(const struct mcp_request *req, struct mcp_response *res)
{
    const struct json_value *uo = json_get(req->args, "unread_only");
    char *out = (uo && json_get_bool(uo))
                 ? mcp_node_rpc("msg_inbox", "[true]")
                 : mcp_node_rpc("msg_inbox", NULL);
    return mcp_return_rpc_body(res, out, "msg_inbox", "mcp.app");
}

DEFINE_PT_STR(h_zcl_msg_read, "msg_id", "msg_read", "mcp.app")

/* ── File market ────────────────────────────────────────────── */

DEFINE_PT(h_zcl_market_list,   "zmarket_list",   "mcp.app")
DEFINE_PT(h_zcl_market_status, "zmarket_status", "mcp.app")

static int h_zcl_market_offer(const struct mcp_request *req, struct mcp_response *res)
{
    const char *fp = json_get_str(json_get(req->args, "filepath"));
    if (!path_check_fs_arg(fp, 1024)) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "filepath: missing, empty, oversized (>1024), "
                 "or contains control characters");
        LOG_WARN("mcp.app", "market_offer: %s", res->error_message);
        return 0;
    }
    int64_t price  = json_get_int(json_get(req->args, "price_per_mb_zat"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, fp);
    mcp_params_push_int(&p, price);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("zmarket_offer", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "zmarket_offer", "mcp.app",
                                   "filepath=%s", fp ? fp : "(null)");
}

DEFINE_PT_STR(h_zcl_market_buy, "root_hash", "zmarket_buy", "mcp.app")

/* ── Atomic swaps (ZSWP) ────────────────────────────────────── */

DEFINE_PT(h_zcl_swap_chains, "swap_chains", "mcp.app")

static int h_zcl_swap_initiate(const struct mcp_request *req, struct mcp_response *res)
{
    const char *ma = json_get_str(json_get(req->args, "my_address"));
    const char *ca = json_get_str(json_get(req->args, "counter_address"));
    int64_t amount   = json_get_int(json_get(req->args, "amount"));
    int64_t locktime = json_get_int(json_get(req->args, "locktime_blocks"));
    const char *chain = json_get_str_or(req->args, "chain", NULL);
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, ma);
    mcp_params_push_str(&p, ca);
    mcp_params_push_int(&p, amount);
    mcp_params_push_int(&p, locktime);
    if (chain) mcp_params_push_str(&p, chain);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("swap_initiate", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "swap_initiate", "mcp.app",
                                   "amount=%lld", (long long)amount);
}

static int h_zcl_swap_participate(const struct mcp_request *req, struct mcp_response *res)
{
    const char *ma = json_get_str(json_get(req->args, "my_address"));
    const char *ca = json_get_str(json_get(req->args, "counter_address"));
    int64_t amount   = json_get_int(json_get(req->args, "amount"));
    int64_t locktime = json_get_int(json_get(req->args, "locktime_blocks"));
    const char *sh = json_get_str(json_get(req->args, "secret_hash"));
    const char *chain = json_get_str_or(req->args, "chain", NULL);
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, ma);
    mcp_params_push_str(&p, ca);
    mcp_params_push_int(&p, amount);
    mcp_params_push_int(&p, locktime);
    mcp_params_push_str(&p, sh);
    if (chain) mcp_params_push_str(&p, chain);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("swap_participate", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "swap_participate", "mcp.app",
                                   "amount=%lld", (long long)amount);
}

static int h_zcl_swap_list(const struct mcp_request *req, struct mcp_response *res)
{
    const struct json_value *st = json_get(req->args, "state");
    char *out;
    if (st) {
        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str(&p, json_get_str(st));
        char *params = mcp_params_to_json(&p);
        out = params ? mcp_node_rpc("swap_list", params) : NULL;
        free(params);
    } else {
        out = mcp_node_rpc("swap_list", NULL);
    }
    return mcp_return_rpc_body(res, out, "swap_list", "mcp.app");
}

/* ── Parameter specs ────────────────────────────────────────── */

static const struct mcp_param_spec p_name_resolve[] = {
    { "name", MCP_PARAM_STR, true, "Name to resolve",
      0, 0, 1, 63, NULL, NULL },
};
static const struct mcp_param_spec p_name_register[] = {
    { "name",  MCP_PARAM_STR, true, "Name (1-63 chars)",
      0, 0, 1, 63, NULL, NULL },
    { "type",  MCP_PARAM_STR, true, "Target type",
      0, 0, 0, 0, "onion,zaddr,taddr", NULL },
    { "value", MCP_PARAM_STR, true, "Target value",
      0, 0, 1, 256, NULL, NULL },
};
static const struct mcp_param_spec p_name_update[] = {
    { "name",  MCP_PARAM_STR, true, "Name (1-63 chars, must already be registered)",
      0, 0, 1, 63, NULL, NULL },
    { "type",  MCP_PARAM_STR, true, "New target type",
      0, 0, 0, 0, "onion,zaddr,taddr,btc,ltc,doge,content", NULL },
    { "value", MCP_PARAM_STR, true, "New target value",
      0, 0, 1, 256, NULL, NULL },
};
static const struct mcp_param_spec p_name_transfer[] = {
    { "name",      MCP_PARAM_STR, true, "Name (1-63 chars, must already be registered)",
      0, 0, 1, 63, NULL, NULL },
    { "new_owner", MCP_PARAM_STR, true, "New owner address (1-63 chars)",
      0, 0, 1, 63, NULL, NULL },
};
static const struct mcp_param_spec p_name_renew[] = {
    { "name", MCP_PARAM_STR, true, "Name (1-63 chars, must already be registered)",
      0, 0, 1, 63, NULL, NULL },
};
static const struct mcp_param_spec p_name_set_record[] = {
    { "name",  MCP_PARAM_STR, true, "Name (1-63 chars, must already be registered)",
      0, 0, 1, 63, NULL, NULL },
    { "type",  MCP_PARAM_STR, true, "Coin type for this record",
      0, 0, 0, 0, "onion,zaddr,taddr,btc,ltc,doge,content", NULL },
    { "value", MCP_PARAM_STR, true, "Address/value for that coin type",
      0, 0, 1, 256, NULL, NULL },
};
static const struct mcp_param_spec p_name_set_text[] = {
    { "name",  MCP_PARAM_STR, true, "Name (1-63 chars, must already be registered)",
      0, 0, 1, 63, NULL, NULL },
    { "key",   MCP_PARAM_STR, true, "Text record key (1-32 chars)",
      0, 0, 1, 32, NULL, NULL },
    { "value", MCP_PARAM_STR, false, "Text record value (0-128 chars); omit to clear",
      0, 0, 0, 128, NULL, NULL },
};
static const struct mcp_param_spec p_msg_send_named[] = {
    { "name",    MCP_PARAM_STR, true, "ZCL Name (e.g. alice)",
      0, 0, 1, 63, NULL, NULL },
    { "message", MCP_PARAM_STR, true, "Message text",
      0, 0, 1, 4000, NULL, NULL },
};
static const struct mcp_param_spec p_msg_send[] = {
    { "message", MCP_PARAM_STR, true, "Message text (onchain max 474 bytes)",
      0, 0, 1, 4000, NULL, NULL },
    { "channel", MCP_PARAM_STR, false, "Delivery channel",
      0, 0, 0, 0, "p2p,onchain", "\"p2p\"" },
    { "peer_id", MCP_PARAM_INT, false, "p2p: connected peer ID",
      0, 1000000, 0, 0, NULL, NULL },
    { "to", MCP_PARAM_STR, false, "onchain: recipient shielded (zs1...) address",
      0, 0, 0, 128, NULL, NULL },
    { "from_address", MCP_PARAM_STR, false,
      "onchain: funding z/t address (required for onchain)",
      0, 0, 0, 128, NULL, NULL },
    { "reply_to", MCP_PARAM_STR, false,
      "onchain: 64-hex parent msg_id this message replies to",
      0, 0, 0, 64, NULL, NULL },
};
static const struct mcp_param_spec p_msg_inbox[] = {
    { "unread_only", MCP_PARAM_BOOL, false, "Only unread",
      0, 0, 0, 0, NULL, "false" },
};
static const struct mcp_param_spec p_msg_read[] = {
    { "msg_id", MCP_PARAM_STR, true, "64-char hex message ID",
      0, 0, 64, 64, NULL, NULL },
};
static const struct mcp_param_spec p_market_offer[] = {
    { "filepath",         MCP_PARAM_STR, true, "Path to file to share",
      0, 0, 1, 1024, NULL, NULL },
    { "price_per_mb_zat", MCP_PARAM_INT, true, "Price per MB in zatoshis",
      0, 1000000000LL, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_market_buy[] = {
    { "root_hash", MCP_PARAM_STR, true, "64-char hex SHA3 of offer",
      0, 0, 64, 64, NULL, NULL },
};
static const struct mcp_param_spec p_swap_initiate[] = {
    { "my_address",      MCP_PARAM_STR, true,  "Your address (refund path)",
      0, 0, 1, 128, NULL, NULL },
    { "counter_address", MCP_PARAM_STR, true,  "Counterparty address",
      0, 0, 1, 128, NULL, NULL },
    { "amount",          MCP_PARAM_INT, true,  "Amount in coins",
      1, 21000000LL, 0, 0, NULL, NULL },
    { "locktime_blocks", MCP_PARAM_INT, true,  "Lock duration in blocks",
      1, 1000000, 0, 0, NULL, NULL },
    { "chain",           MCP_PARAM_STR, false, "Chain",
      0, 0, 0, 0, "zcl,btc,ltc,doge", "\"zcl\"" },
};
static const struct mcp_param_spec p_swap_participate[] = {
    { "my_address",      MCP_PARAM_STR, true,  "Your address",
      0, 0, 1, 128, NULL, NULL },
    { "counter_address", MCP_PARAM_STR, true,  "Initiator address",
      0, 0, 1, 128, NULL, NULL },
    { "amount",          MCP_PARAM_INT, true,  "Amount",
      1, 21000000LL, 0, 0, NULL, NULL },
    { "locktime_blocks", MCP_PARAM_INT, true,  "Lock blocks (shorter than initiator)",
      1, 1000000, 0, 0, NULL, NULL },
    { "secret_hash",     MCP_PARAM_STR, true,  "64-char hex secret hash",
      0, 0, 64, 64, NULL, NULL },
    { "chain",           MCP_PARAM_STR, false, "Chain",
      0, 0, 0, 0, "zcl,btc,ltc,doge", "\"zcl\"" },
};
static const struct mcp_param_spec p_swap_list[] = {
    { "state", MCP_PARAM_STR, false, "Filter by state",
      0, 0, 0, 0, "pending,funded,redeemed,refunded", NULL },
};

static const struct mcp_tool_route k_routes[] = {
    /* Tokens */
    { "zcl_tokens", "app",
      "List all ZSLP tokens on the network.",
      NULL, 0, h_zcl_tokens, 0, NULL },

    /* Names */
    { "zcl_name_resolve", "app",
      "Resolve a ZCL Name to its target (.onion, z-addr, or t-addr).",
      p_name_resolve, PARAM_COUNT(p_name_resolve),
      h_zcl_name_resolve,
      /* Canonical self_test args: pick a sentinel safe to probe on
       * every node (returns "not found"). */
      .self_test_args = "{\"name\":\"__self_test_probe__\"}" },
    { "zcl_name_register", "app",
      "Build an OP_RETURN script to register a ZCL Name on-chain.",
      p_name_register, PARAM_COUNT(p_name_register),
      h_zcl_name_register, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_name_update", "app",
      "Replace a registered ZCL Name's primary target. Owner-only: the "
      "wallet must hold the current owner's private key.",
      p_name_update, PARAM_COUNT(p_name_update),
      h_zcl_name_update, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_name_transfer", "app",
      "Transfer ownership of a registered ZCL Name to a new owner "
      "address. Owner-only.",
      p_name_transfer, PARAM_COUNT(p_name_transfer),
      h_zcl_name_transfer, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_name_renew", "app",
      "Extend a registered ZCL Name's registration term by one term. "
      "Permissionless — anyone may pay to renew.",
      p_name_renew, PARAM_COUNT(p_name_renew),
      h_zcl_name_renew, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_name_set_record", "app",
      "Set an additional multi-coin address record (BTC/LTC/DOGE/...) "
      "for a registered ZCL Name. Owner-only.",
      p_name_set_record, PARAM_COUNT(p_name_set_record),
      h_zcl_name_set_record, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_name_set_text", "app",
      "Set an arbitrary key/value text record (email, url, avatar, ...) "
      "for a registered ZCL Name. Owner-only.",
      p_name_set_text, PARAM_COUNT(p_name_set_text),
      h_zcl_name_set_text, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_name_list", "app",
      "List all registered ZCL Names on the network.",
      NULL, 0, h_zcl_name_list, 0, NULL },

    /* Messaging */
    { "zcl_msg_send_named", "app",
      "Send a message to a ZCL Name. Resolves the name first.",
      p_msg_send_named,
      PARAM_COUNT(p_msg_send_named),
      h_zcl_msg_send_named, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_msg_send", "app",
      "Send a message. channel=p2p (default) to a connected peer, or "
      "channel=onchain as a shielded Sapling-memo transaction.",
      p_msg_send, PARAM_COUNT(p_msg_send), h_zcl_msg_send,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_msg_inbox", "app",
      "List messages in the inbox. Newest first.",
      p_msg_inbox, PARAM_COUNT(p_msg_inbox),
      h_zcl_msg_inbox, 0, NULL },
    { "zcl_msg_read", "app",
      "Mark a message as read and return its content.",
      p_msg_read, PARAM_COUNT(p_msg_read), h_zcl_msg_read,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE /* mutates read-state */ },

    /* File market */
    { "zcl_market_list", "app",
      "List files available on the ZCL Market P2P file sharing network.",
      NULL, 0, h_zcl_market_list, 0, NULL },
    { "zcl_market_offer", "app",
      "Announce a file for sale on the ZCL Market.",
      p_market_offer, PARAM_COUNT(p_market_offer),
      h_zcl_market_offer, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_market_buy", "app",
      "Initiate purchase and download of a file from the ZCL Market.",
      p_market_buy, PARAM_COUNT(p_market_buy),
      h_zcl_market_buy, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_market_status", "app",
      "ZCL Market status: cached offers, persisted offers, active downloads.",
      NULL, 0, h_zcl_market_status, 0, NULL },

    /* Atomic swaps */
    { "zcl_swap_chains", "app",
      "List supported chains for atomic swaps: ZCL, BTC, LTC, DOGE.",
      NULL, 0, h_zcl_swap_chains, 0, NULL },
    { "zcl_swap_initiate", "app",
      "Initiate an atomic swap. Generates secret, builds HTLC, returns P2SH.",
      p_swap_initiate, PARAM_COUNT(p_swap_initiate),
      h_zcl_swap_initiate, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_swap_participate", "app",
      "Participate in an atomic swap (counter-HTLC with shorter locktime).",
      p_swap_participate,
      PARAM_COUNT(p_swap_participate),
      h_zcl_swap_participate, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_swap_list", "app",
      "List atomic swap contracts.",
      p_swap_list, PARAM_COUNT(p_swap_list),
      h_zcl_swap_list, 0, NULL },
};

void mcp_register_app(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register_required(&k_routes[i]);
}
