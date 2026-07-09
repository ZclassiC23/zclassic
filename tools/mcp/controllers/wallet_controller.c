/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP wallet controller: balance, addresses, sending, wallet diagnostics. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Simple passthroughs (no params) ─────────────────────────── */

DEFINE_PT(h_zcl_balance,               "z_gettotalbalance", "mcp.wallet")
DEFINE_PT(h_zcl_getnewaddress,         "getnewaddress",     "mcp.wallet")
DEFINE_PT(h_zcl_z_getnewaddress,       "z_getnewaddress",   "mcp.wallet")
DEFINE_PT(h_zcl_getwalletinfo,         "getwalletinfo",     "mcp.wallet")
DEFINE_PT(h_zcl_z_listaddresses,       "z_listaddresses",   "mcp.wallet")
DEFINE_PT(h_zcl_walletaudit,           "walletaudit",       "mcp.wallet")
DEFINE_PT(h_zcl_wallet_backup_status,  "walletbackupstatus","mcp.wallet")
DEFINE_PT(h_zcl_wallet_backup_now,     "walletbackupnow",   "mcp.wallet")

/* ── Parameterized handlers ──────────────────────────────────── */

static bool parse_rpc_string_result(const char *raw, char *out, size_t out_cap)
{
    if (!raw || !out || out_cap == 0)
        return false;

    struct json_value v;
    json_init(&v);
    bool ok = json_read(&v, raw, strlen(raw)) && v.type == JSON_STR;
    if (ok)
        snprintf(out, out_cap, "%s", json_get_str(&v));
    json_free(&v);
    return ok && out[0] != '\0';
}

static bool parse_backup_ok(const char *raw, struct json_value *backup_out)
{
    if (!backup_out)
        return false;
    json_init(backup_out);
    if (!raw)
        return false;
    if (!json_read(backup_out, raw, strlen(raw)))
        return false;
    const struct json_value *ok = json_get(backup_out, "ok");
    return ok && ok->type == JSON_BOOL && json_get_bool(ok);
}

static int h_zcl_wallet_receive_intent(const struct mcp_request *req,
                                       struct mcp_response *res)
{
    const char *purpose = json_get_str_or(req->args, "purpose", "agent-lab");

    char *t_raw = mcp_node_rpc("getnewaddress", NULL);
    char taddr[160] = "";
    if (!parse_rpc_string_result(t_raw, taddr, sizeof(taddr))) {
        free(t_raw);
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "getnewaddress did not return a transparent address");
        LOG_ERR("mcp.wallet", "wallet_receive_intent: getnewaddress failed");
        return 0;
    }
    free(t_raw);

    char *z_raw = mcp_node_rpc("z_getnewaddress", NULL);
    char zaddr[256] = "";
    if (!parse_rpc_string_result(z_raw, zaddr, sizeof(zaddr))) {
        free(z_raw);
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "z_getnewaddress did not return a Sapling address; "
                 "do not fund the transparent address until backup is checked");
        LOG_ERR("mcp.wallet", "wallet_receive_intent: z_getnewaddress failed "
                "after taddr=%s", taddr);
        return 0;
    }
    free(z_raw);

    char *backup_raw = mcp_node_rpc("walletbackupnow", NULL);
    struct json_value backup;
    if (!parse_backup_ok(backup_raw, &backup)) {
        free(backup_raw);
        json_free(&backup);
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "walletbackupnow failed after address creation; do not fund");
        snprintf(res->error_param, sizeof(res->error_param), "walletbackupnow");
        LOG_ERR("mcp.wallet", "wallet_receive_intent: walletbackupnow failed "
                "after taddr=%s zaddr=%s", taddr, zaddr);
        return 0;
    }
    free(backup_raw);

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_str(&root, "purpose", purpose ? purpose : "agent-lab");
    json_push_kv_str(&root, "funding_address", taddr);
    json_push_kv_str(&root, "transparent_address", taddr);
    json_push_kv_str(&root, "sapling_address", zaddr);
    json_push_kv_bool(&root, "safe_to_fund", true);
    json_push_kv_str(&root, "qr_payload", taddr);
    json_push_kv(&root, "backup", &backup);

    size_t need = json_write(&root, NULL, 0) + 1;
    char *body = zcl_malloc(need, "wallet_receive_intent_body");
    if (!body) {
        json_free(&backup);
        json_free(&root);
        return mcp_res_set_oom(res, need, "mcp.wallet",
                               "wallet receive intent response");
    }
    json_write(&root, body, need);
    json_free(&backup);
    json_free(&root);
    res->body = body;
    return 0;
}

static int h_zcl_send(const struct mcp_request *req, struct mcp_response *res)
{
    const char *from = json_get_str(json_get(req->args, "from"));
    const char *to   = json_get_str(json_get(req->args, "to"));
    const struct json_value *amt = json_get(req->args, "amount");
    double amount = (amt && amt->type == JSON_REAL) ? json_get_real(amt)
                                                    : (double)json_get_int(amt);

    /* Build [from, [{address: to, amount}]] via the JSON encoder — a
     * quote in `from` or `to` would otherwise rewrite the params array. */
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, from);

    struct json_value recip, recip_arr;
    json_init(&recip);     json_set_object(&recip);
    json_push_kv_str (&recip, "address", to ? to : "");
    json_push_kv_real(&recip, "amount",  amount);
    json_init(&recip_arr); json_set_array(&recip_arr);
    json_push_back(&recip_arr, &recip);
    mcp_params_push_value(&p, &recip_arr);
    json_free(&recip);
    json_free(&recip_arr);

    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("z_sendmany", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "z_sendmany", "mcp.wallet",
                                   "from=%s to=%s",
                                   from ? from : "(null)",
                                   to ? to : "(null)");
}

static int h_zcl_sendtoaddress(const struct mcp_request *req,
                                struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "address"));
    const struct json_value *amt = json_get(req->args, "amount");
    double amount = (amt && amt->type == JSON_REAL) ? json_get_real(amt)
                                                    : (double)json_get_int(amt);

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str (&p, addr);
    mcp_params_push_real(&p, amount);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("sendtoaddress", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "sendtoaddress", "mcp.wallet",
                                   "addr=%s", addr ? addr : "(null)");
}

static int h_zcl_listunspent(const struct mcp_request *req,
                              struct mcp_response *res)
{
    char params[128];
    snprintf(params, sizeof(params), "[%lld,%lld]",
             (long long)json_get_int_or(req->args, "minconf", 1),
             (long long)json_get_int_or(req->args, "maxconf", 9999999));
    return mcp_return_rpc_body(res, mcp_node_rpc("listunspent", params),
                                "listunspent", "mcp.wallet");
}

static int h_zcl_listtransactions(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    char params[128];
    snprintf(params, sizeof(params), "[\"\",%lld,%lld]",
             (long long)json_get_int_or(req->args, "count", 10),
             (long long)json_get_int_or(req->args, "skip",   0));
    return mcp_return_rpc_body(res, mcp_node_rpc("listtransactions", params),
                                "listtransactions", "mcp.wallet");
}

DEFINE_PT_STR(h_zcl_gettransaction, "txid", "gettransaction", "mcp.wallet")

static int h_zcl_listaddresses(const struct mcp_request *req,
                                struct mcp_response *res)
{
    (void)req;
    /* The node RPC `listwalletkeys` returns {transparent_keys:[{address,...}],
     * sapling_keys:[...]}.  Call it without private keys and project just
     * the addresses so the caller gets a clean list. */
    char *raw = mcp_node_rpc("listwalletkeys", "[false]");
    if (!raw)
        return mcp_return_rpc_body(res, raw, "listwalletkeys", "mcp.wallet");

    struct json_value root;
    if (!json_read(&root, raw, strlen(raw))) { res->body = raw; return 0; }
    free(raw);

    size_t cap = 65536;
    char *out = zcl_malloc(cap, "listaddresses_body");
    if (!out) {
        json_free(&root);
        return mcp_res_set_oom(res, cap, "mcp.wallet", "listaddresses response");
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "{\"t_addresses\":[");

    const struct json_value *tk = json_get(&root, "transparent_keys");
    bool first = true;
    if (tk && tk->type == JSON_ARR) {
        for (size_t i = 0; i < tk->num_children; i++) {
            const struct json_value *k = &tk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    pos += (size_t)snprintf(out + pos, cap - pos, "],\"z_addresses\":[");

    const struct json_value *sk = json_get(&root, "sapling_keys");
    first = true;
    if (sk && sk->type == JSON_ARR) {
        for (size_t i = 0; i < sk->num_children; i++) {
            const struct json_value *k = &sk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    if (pos + 2 < cap) { out[pos++] = ']'; out[pos++] = '}'; out[pos] = 0; }

    json_free(&root);
    res->body = out;
    return 0;
}

DEFINE_PT_STR(h_zcl_dumpprivkey, "address", "dumpprivkey", "mcp.wallet")

static int h_zcl_importprivkey(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const char *wif   = json_get_str(json_get(req->args, "privkey"));
    const char *label = json_get_str(json_get(req->args, "label"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str (&p, wif);
    mcp_params_push_str (&p, label);
    mcp_params_push_bool(&p, json_get_bool_or(req->args, "rescan", false));
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("importprivkey", params) : NULL;
    free(params);
    return mcp_return_rpc_body(res, out, "importprivkey", "mcp.wallet");
}

DEFINE_PT_STR(h_zcl_importaddress, "address", "importaddress", "mcp.wallet")

static int h_zcl_z_listunspent(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    char params[64];
    snprintf(params, sizeof(params), "[%lld]",
             (long long)json_get_int_or(req->args, "minconf", 1));
    return mcp_return_rpc_body(res, mcp_node_rpc("z_listunspent", params),
                                "z_listunspent", "mcp.wallet");
}

static int h_zcl_z_getbalance(const struct mcp_request *req,
                                struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "address"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, addr);
    mcp_params_push_int(&p, json_get_int_or(req->args, "minconf", 1));
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("z_getbalance", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "z_getbalance", "mcp.wallet",
                                   "address=%s", addr ? addr : "(null)");
}

static int h_zcl_rescanblockchain(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    const struct json_value *s = json_get(req->args, "start_height");
    const struct json_value *e = json_get(req->args, "stop_height");
    char params[64];
    if (s && e)
        snprintf(params, sizeof(params), "[%lld,%lld]",
                 (long long)json_get_int(s), (long long)json_get_int(e));
    else if (s)
        snprintf(params, sizeof(params), "[%lld]",
                 (long long)json_get_int(s));
    else
        snprintf(params, sizeof(params), "[]");
    return mcp_return_rpc_body(res, mcp_node_rpc("rescanblockchain", params),
                                "rescanblockchain", "mcp.wallet");
}

static int h_zcl_listwalletkeys(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    if (json_get_bool_or(req->args, "include_privkeys", false)) {
        res->error = MCP_ERR_OUT_OF_RANGE;
        snprintf(res->error_message, sizeof(res->error_message),
                 "zcl_listwalletkeys never returns private keys over MCP; "
                 "use explicit destructive key-export tools instead");
        snprintf(res->error_param, sizeof(res->error_param),
                 "include_privkeys");
        LOG_ERR("mcp.wallet",
                "zcl_listwalletkeys rejected include_privkeys=true");
        return 0;
    }
    char params[32];
    snprintf(params, sizeof(params), "[false]");
    return mcp_return_rpc_body(res, mcp_node_rpc("listwalletkeys", params),
                                "listwalletkeys", "mcp.wallet");
}

static int h_zcl_replaywalletfromchain(const struct mcp_request *req,
                                         struct mcp_response *res)
{
    /* RPC requires "confirm" literal. Wrap the user's boolean for safety. */
    if (!json_get_bool_or(req->args, "confirm", false)) {
        res->error = MCP_ERR_OUT_OF_RANGE;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replaywalletfromchain requires confirm=true "
                 "(destructive: wipes derived wallet state)");
        snprintf(res->error_param, sizeof(res->error_param), "confirm");
        LOG_ERR("mcp.wallet", "replaywalletfromchain called without confirm=true");
        return 0;
    }
    return mcp_return_rpc_body(res,
                                mcp_node_rpc("replaywalletfromchain", "[\"confirm\"]"),
                                "replaywalletfromchain", "mcp.wallet");
}

/* ── Parameter specs ─────────────────────────────────────────── */

static const struct mcp_param_spec p_send[] = {
    { "from",   MCP_PARAM_STR,  true, "Source address",
      0, 0, 1, 128, NULL, NULL },
    { "to",     MCP_PARAM_STR,  true, "Destination address",
      0, 0, 1, 128, NULL, NULL },
    { "amount", MCP_PARAM_REAL, true, "Amount in ZCL",
      0, 0, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_sendtoaddr[] = {
    { "address", MCP_PARAM_STR,  true, "Destination t-address",
      0, 0, 1, 128, NULL, NULL },
    { "amount",  MCP_PARAM_REAL, true, "Amount in ZCL",
      0, 0, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_listunspent[] = {
    { "minconf", MCP_PARAM_INT, false, "Minimum confirmations",
      0, 9999999, 0, 0, NULL, "1" },
    { "maxconf", MCP_PARAM_INT, false, "Maximum confirmations",
      0, 9999999, 0, 0, NULL, "9999999" },
};

static const struct mcp_param_spec p_listtx[] = {
    { "count", MCP_PARAM_INT, false, "Number of transactions to return",
      1, 10000, 0, 0, NULL, "10" },
    { "skip",  MCP_PARAM_INT, false, "Number of most recent to skip",
      0, 10000000, 0, 0, NULL, "0" },
};

static const struct mcp_param_spec p_gettx[] = {
    { "txid", MCP_PARAM_STR, true, "Transaction id (hex)",
      0, 0, 1, 128, NULL, NULL },
};

static const struct mcp_param_spec p_addr[] = {
    { "address", MCP_PARAM_STR, true, "Address",
      0, 0, 1, 128, NULL, NULL },
};

static const struct mcp_param_spec p_importaddr[] = {
    { "address", MCP_PARAM_STR, true, "Transparent address to watch",
      0, 0, 1, 128, NULL, NULL },
};

static const struct mcp_param_spec p_importkey[] = {
    { "privkey", MCP_PARAM_STR,  true,  "WIF-encoded private key",
      0, 0, 1, 128, NULL, NULL },
    { "label",   MCP_PARAM_STR,  false, "Optional label",
      0, 0, 0, 128, NULL, "\"\"" },
    { "rescan",  MCP_PARAM_BOOL, false, "Rescan chain after import",
      0, 0, 0, 0, NULL, "false" },
};

static const struct mcp_param_spec p_zunspent[] = {
    { "minconf", MCP_PARAM_INT, false, "Minimum confirmations",
      0, 9999999, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_zbalance[] = {
    { "address", MCP_PARAM_STR, true,  "Shielded z-address or t-address",
      0, 0, 1, 128, NULL, NULL },
    { "minconf", MCP_PARAM_INT, false, "Minimum confirmations",
      0, 9999999, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_rescan[] = {
    { "start_height", MCP_PARAM_INT, false, "Start block height",
      0, 100000000, 0, 0, NULL, "0" },
    { "stop_height",  MCP_PARAM_INT, false, "Stop block height",
      0, 100000000, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_listkeys[] = {
    { "include_privkeys", MCP_PARAM_BOOL, false,
      "Include WIF private keys in the response",
      0, 0, 0, 0, NULL, "false" },
};

static const struct mcp_param_spec p_confirm[] = {
    { "confirm", MCP_PARAM_BOOL, true,
      "Must be true — destructive: wipes derived wallet state",
      0, 0, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_receive_intent[] = {
    { "purpose", MCP_PARAM_STR, false,
      "Short label for the receive intent",
      0, 0, 0, 96, NULL, "\"agent-lab\"" },
};

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_tool_route k_routes[] = {
    { "zcl_balance", "wallet",
      "Total wallet balance: transparent + shielded.",
      NULL, 0, h_zcl_balance, 0, NULL },
    { "zcl_getnewaddress", "wallet",
      "Generate new transparent (t-addr) receiving address.",
      NULL, 0, h_zcl_getnewaddress,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_z_getnewaddress", "wallet",
      "Generate new shielded Sapling (z-addr) receiving address.",
      NULL, 0, h_zcl_z_getnewaddress,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_wallet_receive_intent", "wallet",
      "Create t/z lab receive addresses and force a verified wallet backup "
      "before returning public funding data.",
      p_receive_intent, PARAM_COUNT(p_receive_intent),
      h_zcl_wallet_receive_intent,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_send", "wallet",
      "Send ZCL (transparent or shielded).",
      p_send, PARAM_COUNT(p_send), h_zcl_send,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },

    { "zcl_getwalletinfo", "wallet",
      "One-shot wallet health snapshot: balance, tx count, keys, status.",
      NULL, 0, h_zcl_getwalletinfo, 0, NULL },
    { "zcl_listunspent", "wallet",
      "List transparent UTXOs available to spend.",
      p_listunspent, PARAM_COUNT(p_listunspent),
      h_zcl_listunspent, 0, NULL },
    { "zcl_listtransactions", "wallet",
      "Recent wallet transaction history.",
      p_listtx, PARAM_COUNT(p_listtx),
      h_zcl_listtransactions, 0, NULL },
    { "zcl_gettransaction", "wallet",
      "Fetch a single wallet transaction by id.",
      p_gettx, PARAM_COUNT(p_gettx), h_zcl_gettransaction, 0, NULL },
    { "zcl_sendtoaddress", "wallet",
      "Simple send to a single transparent address.",
      p_sendtoaddr, PARAM_COUNT(p_sendtoaddr),
      h_zcl_sendtoaddress, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_listaddresses", "wallet",
      "All transparent (t-addr) addresses in the wallet.",
      NULL, 0, h_zcl_listaddresses, 0, NULL },
    { "zcl_dumpprivkey", "wallet",
      "Export the WIF private key for a transparent address.",
      p_addr, PARAM_COUNT(p_addr), h_zcl_dumpprivkey,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE /* exposes secrets */ },
    { "zcl_importprivkey", "wallet",
      "Import a WIF private key into the wallet.",
      p_importkey, PARAM_COUNT(p_importkey),
      h_zcl_importprivkey, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_importaddress", "wallet",
      "Watch a transparent address without private key. Tracks balance and "
      "transactions but cannot spend.",
      p_importaddr, PARAM_COUNT(p_importaddr),
      h_zcl_importaddress, 0, NULL },
    { "zcl_z_listaddresses", "wallet",
      "All shielded Sapling (z-addr) addresses in the wallet.",
      NULL, 0, h_zcl_z_listaddresses, 0, NULL },
    { "zcl_z_listunspent", "wallet",
      "List shielded notes available to spend.",
      p_zunspent, PARAM_COUNT(p_zunspent),
      h_zcl_z_listunspent, 0, NULL },
    { "zcl_z_getbalance", "wallet",
      "Balance for a single t-address or z-address.",
      p_zbalance, PARAM_COUNT(p_zbalance),
      h_zcl_z_getbalance, 0, NULL },
    { "zcl_rescanblockchain", "wallet",
      "Manually trigger a wallet rescan over a height range.",
      p_rescan, PARAM_COUNT(p_rescan),
      h_zcl_rescanblockchain, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_walletaudit", "wallet",
      "Reconcile the wallet against the on-chain UTXO set.",
      NULL, 0, h_zcl_walletaudit, 0, NULL },
    { "zcl_wallet_backup_status", "wallet",
      "Wallet backup service status and last verified backup.",
      NULL, 0, h_zcl_wallet_backup_status, 0, NULL },
    { "zcl_wallet_backup_now", "wallet",
      "Run one synchronous verified wallet backup.",
      NULL, 0, h_zcl_wallet_backup_now,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_listwalletkeys", "wallet",
      "List all wallet key metadata without private key material.",
      p_listkeys, PARAM_COUNT(p_listkeys),
      h_zcl_listwalletkeys, 0, NULL },
    { "zcl_replaywalletfromchain", "wallet",
      "Rebuild the derived wallet state by replaying the chain. "
      "Destructive — requires confirm=true.",
      p_confirm, PARAM_COUNT(p_confirm),
      h_zcl_replaywalletfromchain, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
};

void mcp_register_wallet(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register_required(&k_routes[i]);
}
