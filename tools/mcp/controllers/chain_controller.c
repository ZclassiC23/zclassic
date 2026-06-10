/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP chain controller: block, chain, UTXO commitment, sync, MMB.
 */

#include "platform/time_compat.h"
#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "chain/chain.h"
#include "controllers/chain_projection.h"
#include "json/json.h"
#include "services/replay_verify_service.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* ── Handlers ───────────────────────────────────────────────── */

static int h_zcl_getblockcount(const struct mcp_request *req,
                               struct mcp_response *res)
{
    (void)req;

    int64_t height = chain_projection_best_block_height();
    if (height >= 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)height);
        res->body = strdup(buf);
        if (!res->body) {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "getblockcount projection response alloc failed");
            LOG_ERR("mcp.chain", "getblockcount projection response alloc failed");
        }
        return 0;
    }

    fprintf(stderr,  // obs-ok:mcp-chain-projection-fallback
            "[mcp.chain] projection miss: getblockcount rpc fallback\n");
    return mcp_return_rpc_body(res, mcp_node_rpc("getblockcount", NULL),
                               "getblockcount", "mcp.chain");
}

DEFINE_PT(h_zcl_chain_tip,         "getchaintip",       "mcp.chain")
DEFINE_PT(h_zcl_getblockchaininfo, "getblockchaininfo", "mcp.chain")
DEFINE_PT(h_zcl_syncstate,         "syncstate",         "mcp.chain")
DEFINE_PT(h_zcl_validationstatus,  "validationstatus",  "mcp.chain")
DEFINE_PT(h_zcl_dataintegrity,     "getdataintegrity",  "mcp.chain")
DEFINE_PT(h_zcl_mmb,               "getmmrroot",        "mcp.chain")
DEFINE_PT(h_zcl_utxocommitment,    "getutxocommitment", "mcp.chain")
DEFINE_PT(h_zcl_hodlwave,          "gethodlwave",       "mcp.chain")

static int h_zcl_reorg_history(const struct mcp_request *req,
                                struct mcp_response *res)
{
    char params[32];
    snprintf(params, sizeof(params), "[%lld]",
             (long long)json_get_int_or(req->args, "count", 50));
    return mcp_return_rpc_body(res, mcp_node_rpc("getreorghistory", params),
                                "getreorghistory", "mcp.chain");
}

/* ── zcl_replay_verify ─────────────────────────────────────────────
 *
 * Offline integrity / PoW verification sweep over the legacy on-disk
 * block log. For each block in a
 * bounded height window it re-derives four cheap consensus invariants:
 * (1) equihash solution, (2) difficulty target, (3) prev-block linkage,
 * (4) merkle root. Read-only — does not touch the live node, services,
 * or wallet. See services/replay_verify_service.{c,h}. */
static int h_zcl_replay_verify(const struct mcp_request *req,
                               struct mcp_response *res)
{
    int64_t start_h    = json_get_int_or(req->args, "start_height", 0);
    int64_t max_blocks = json_get_int_or(req->args, "max_blocks", 1000);
    const char *legacy_dir =
        json_get_str_or(req->args, "legacy_datadir", NULL);

    if (!legacy_dir || !legacy_dir[0]) {
        static char def_legacy[1024];
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(def_legacy, sizeof def_legacy, "%s/.zclassic", home);
            legacy_dir = def_legacy;
        } else {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "legacy_datadir not provided and $HOME is unset");
            LOG_ERR("mcp.chain", "replay_verify: no legacy_datadir");
            return 0;
        }
    }

    if (start_h < 0)               start_h = 0;
    if (start_h > (int64_t)UINT32_MAX) start_h = UINT32_MAX;
    if (max_blocks < 0)            max_blocks = 0;   /* 0 == to tip */

    struct replay_verify_report rep;
    struct zcl_result rr = replay_verify_run(legacy_dir,
                                             (uint32_t)start_h,
                                             (uint64_t)max_blocks,
                                             &rep);
    if (!rr.ok) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay_verify failed: code=%d %s", rr.code, rr.message);
        LOG_ERR("mcp.chain", "replay_verify: %s", res->error_message);
        return 0;
    }

    char buf[1024];
    int n = snprintf(buf, sizeof buf,
            "{\"blocks_checked\":%llu,"
            "\"pow_failures\":%llu,"
            "\"linkage_failures\":%llu,"
            "\"merkle_failures\":%llu,"
            "\"first_fail_height\":%lld,"
            "\"first_fail_reason\":\"%s\","
            "\"start_height\":%u,"
            "\"end_height\":%u,"
            "\"tip_height\":%u}",
            (unsigned long long)rep.blocks_checked,
            (unsigned long long)rep.pow_failures,
            (unsigned long long)rep.linkage_failures,
            (unsigned long long)rep.merkle_failures,
            (long long)rep.first_fail_height,
            rep.first_fail_reason ? rep.first_fail_reason : "none",
            rep.start_height, rep.end_height, rep.tip_height);

    char *body = (n > 0 && n < (int)sizeof buf) ? strdup(buf) : NULL;
    if (!body) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay_verify: body alloc failed");
        LOG_ERR("mcp.chain", "replay_verify: body alloc failed");
        return 0;
    }
    res->body = body;
    return 0;
}

static int h_zcl_utxo_audit(const struct mcp_request *req,
                            struct mcp_response *res)
{
    const char *remote = json_get_str_or(req->args, "remote_sha3", NULL);
    const char *source = json_get_str_or(req->args, "source",      NULL);

    struct mcp_params p;
    mcp_params_init(&p);
    if (remote && remote[0]) {
        mcp_params_push_str(&p, remote);
        mcp_params_push_int(&p, json_get_int_or(req->args, "remote_height", 0));
        mcp_params_push_str(&p, source && source[0] ? source : "trusted-peer");
    }
    char *params = mcp_params_to_json(&p);
    char *out = mcp_node_rpc("getutxoaudit", params);
    free(params);
    return mcp_return_rpc_body(res, out, "getutxoaudit", "mcp.chain");
}

static int h_zcl_getrawtransaction(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    const char *txid = json_get_str(json_get(req->args, "txid"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, txid);
    mcp_params_push_int(&p, json_get_int_or(req->args, "verbose", 1));
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("getrawtransaction", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "getrawtransaction", "mcp.chain",
                                   "txid=%s", txid ? txid : "(null)");
}

static int h_zcl_getblock(const struct mcp_request *req, struct mcp_response *res)
{
    const char *id_str = json_get_str(json_get(req->args, "block_id"));
    int verbosity = (int)json_get_int_or(req->args, "verbosity", 1);

    bool is_num = id_str && id_str[0];
    for (const char *c = id_str; is_num && *c; c++)
        if (*c < '0' || *c > '9') is_num = false;

    char clean[128] = {0};
    const char *hash_str = id_str;
    if (is_num) {
        struct mcp_params ph;
        mcp_params_init(&ph);
        mcp_params_push_int(&ph, id_str ? atoll(id_str) : 0);
        char *php = mcp_params_to_json(&ph);
        char *hash = php ? mcp_node_rpc("getblockhash", php) : NULL;
        free(php);
        if (!hash)
            return mcp_return_rpc_body_ctx(res, NULL, "getblockhash", "mcp.chain",
                                           "height=%s", id_str ? id_str : "(null)");
        size_t ci = 0;
        for (size_t i = 0; hash[i] && ci < 127; i++)
            if (hash[i] != '"' && hash[i] != '\n') clean[ci++] = hash[i];
        clean[ci] = 0;
        free(hash);
        hash_str = clean;
    }

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, hash_str);
    mcp_params_push_int(&p, verbosity);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("getblock", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "getblock", "mcp.chain",
                                   "id=%s", id_str ? id_str : "(null)");
}

/* invalidateblock — the operator recovery lever. Drop a stale fork
 * (mark BLOCK_FAILED_VALID + disconnect-and-reorg). Destructive. */
DEFINE_PT_STR(h_zcl_invalidateblock, "hash", "invalidateblock", "mcp.chain")

/* reconsiderblock — the inverse of invalidateblock. Destructive. */
DEFINE_PT_STR(h_zcl_reconsiderblock, "hash", "reconsiderblock", "mcp.chain")

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_recovery_block[] = {
    { "hash", MCP_PARAM_STR, true, "Block hash (hex) to invalidate/reconsider",
      0, 0, 64, 64, NULL, NULL },
};

static const struct mcp_param_spec p_getblock[] = {
    { "block_id",  MCP_PARAM_STR, true,  "Height or hash",
      0, 0, 1, 128, NULL, NULL },
    { "verbosity", MCP_PARAM_INT, false, "0=hex, 1=JSON, 2=JSON+tx",
      0, 2, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_getrawtx[] = {
    { "txid",    MCP_PARAM_STR, true,  "Transaction id (hex)",
      0, 0, 1, 128, NULL, NULL },
    { "verbose", MCP_PARAM_INT, false, "0=hex, 1=JSON",
      0, 1, 0, 0, NULL, "1" },
};

static const struct mcp_param_spec p_reorg_history[] = {
    { "count", MCP_PARAM_INT, false,
      "Max reorg events to return (1..1024)",
      1, 1024, 0, 0, NULL, "50" },
};

static const struct mcp_param_spec p_replay_verify[] = {
    { "start_height", MCP_PARAM_INT, false,
      "First height to verify (inclusive).",
      0, 100000000, 0, 0, NULL, "0" },
    { "max_blocks", MCP_PARAM_INT, false,
      "Maximum blocks to verify this call; 0 = to tip.",
      0, 100000000, 0, 0, NULL, "1000" },
    { "legacy_datadir", MCP_PARAM_STR, false,
      "Legacy zclassicd data directory. Defaults to $HOME/.zclassic.",
      0, 0, 0, 1023, NULL, NULL },
};

static const struct mcp_param_spec p_utxo_audit[] = {
    { "remote_sha3", MCP_PARAM_STR, false,
      "Trusted peer SHA3 commitment to compare against.",
      0, 0, 64, 64, NULL, NULL },
    { "remote_height", MCP_PARAM_INT, false,
      "Trusted peer height for the commitment.",
      0, 100000000, 0, 0, NULL, "0" },
    { "source", MCP_PARAM_STR, false,
      "Trusted peer or operator label.",
      0, 0, 0, 63, NULL, "\"trusted-peer\"" },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_getblockcount", "chain",
      "Current block height.", NULL, 0, h_zcl_getblockcount, 0, NULL },
    { "zcl_chain_tip", "chain",
      "Active chain tip in one call: hash, height, time, age_seconds, "
      "work, bits, difficulty. Power-user shortcut that bundles "
      "getbestblockhash + getblockheader + chainwork.",
      NULL, 0, h_zcl_chain_tip, 0, NULL },
    { "zcl_getblock", "chain",
      "Get block by height or hash.",
      p_getblock, PARAM_COUNT(p_getblock), h_zcl_getblock,
      /* required block_id, but height "1" exists on every synced node. */
      .self_test_args = "{\"block_id\":\"1\"}" },
    { "zcl_getrawtransaction", "chain",
      "Transaction by id. verbose=1 decodes, verbose=0 returns hex.",
      p_getrawtx, PARAM_COUNT(p_getrawtx),
      h_zcl_getrawtransaction, 0, NULL },
    { "zcl_getblockchaininfo", "chain",
      "Chain state: height, best block, difficulty, chain work, value pools.",
      NULL, 0, h_zcl_getblockchaininfo, 0, NULL },
    { "zcl_syncstate", "chain",
      "Sync state machine: phase, progress, header/block/UTXO status.",
      NULL, 0, h_zcl_syncstate, 0, NULL },
    { "zcl_validationstatus", "chain",
      "Background validation: verified height, sigs, proofs, blocks/sec.",
      NULL, 0, h_zcl_validationstatus, 0, NULL },
    { "zcl_dataintegrity", "chain",
      "SHA3-256 hashes over all consensus tables.",
      NULL, 0, h_zcl_dataintegrity, 0, NULL },
    { "zcl_mmb", "chain",
      "Merkle Mountain Belt root. FlyClient chain verification.",
      NULL, 0, h_zcl_mmb, 0, NULL },
    { "zcl_utxocommitment", "chain",
      "SHA3-256 over entire UTXO set in canonical order.",
      NULL, 0, h_zcl_utxocommitment, 0, NULL },
    { "zcl_utxo_audit", "chain",
      "Post-IBD UTXO drift audit. Computes local commitment and optionally compares a trusted peer SHA3.",
      p_utxo_audit, PARAM_COUNT(p_utxo_audit),
      h_zcl_utxo_audit, 0, NULL },
    { "zcl_hodlwave", "chain",
      "UTXO age distribution: 10 buckets from 24h to 5y+.",
      NULL, 0, h_zcl_hodlwave, 0, NULL },
    { "zcl_reorg_history", "chain",
      "Recent chain.reorg_* events (start, disconnect_failed, "
      "recovery_complete). Power-user lens on chain stability.",
      p_reorg_history, PARAM_COUNT(p_reorg_history),
      h_zcl_reorg_history, 0, NULL },
    { "zcl_replay_verify", "chain",
      "Offline integrity/PoW sweep over the legacy block log. "
      "For each block in a bounded window verifies "
      "equihash solution, difficulty target (nBits), prev-block linkage, "
      "and merkle root — reusing the canonical consensus check_block. "
      "Returns {blocks_checked, pow_failures, linkage_failures, "
      "merkle_failures, first_fail_height, first_fail_reason, "
      "start_height, end_height, tip_height}. Read-only; defaults to "
      "$HOME/.zclassic and a 1000-block bounded run.",
      p_replay_verify, PARAM_COUNT(p_replay_verify),
      h_zcl_replay_verify, 0,
      .self_test_args = "{\"start_height\":0,\"max_blocks\":4}" },
    { "zcl_invalidateblock", "chain",
      "Recovery lever: permanently mark a block invalid by hash. The "
      "active chain disconnects back below it (if on the active chain) "
      "and reorgs to the next-best fully-valid chain; every reconnected "
      "block is fully re-validated. Mirrors Bitcoin Core invalidateblock. "
      "Use zcl_reconsiderblock to undo. Destructive — rate-gated.",
      p_recovery_block, PARAM_COUNT(p_recovery_block),
      h_zcl_invalidateblock, MCP_TOOL_FLAG_DESTRUCTIVE, NULL },
    { "zcl_reconsiderblock", "chain",
      "Recovery lever: clear invalidity from a block and its descendants "
      "by hash, re-adding them to chain selection. If the reconsidered "
      "chain has the most work it is re-validated and reconnected. The "
      "inverse of zcl_invalidateblock. Destructive — rate-gated.",
      p_recovery_block, PARAM_COUNT(p_recovery_block),
      h_zcl_reconsiderblock, MCP_TOOL_FLAG_DESTRUCTIVE, NULL },
};

void mcp_register_chain(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
}
