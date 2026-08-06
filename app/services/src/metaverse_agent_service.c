/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: reconcile broker custody bindings with authoritative wallet reads.
 *
 * See services/metaverse_agent_service.h. Two readers over a broker directory,
 * plus the one validation both share.
 *
 * The absolute-path requirement is not cosmetic: a relative `dir` would be
 * resolved against whatever working directory the node happens to have, which
 * for a linger service is not the operator's shell. Refusing it makes the
 * subject of the read explicit at the call site.
 */

#include "services/metaverse_agent_service.h"

#include "base/result.h"
#include "base/hex.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "session/agent_broker.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MVS_DIR_MAX 384

/* A custody snapshot can contend with the reducer's authoritative wallet and
 * vault readers while the dev lane is catching up.  Use the RPC client's
 * normal bounded read deadline: a shorter front-door latency budget turned a
 * valid six-second snapshot into the false claim that its endpoint was
 * unreachable.  Freshness is still decided from the returned snapshot, never
 * from how quickly it arrived. */
#define MVS_MONEY_RPC_CONNECT_MS 500L
#define MVS_MONEY_RPC_TOTAL_MS 10000L

static metaverse_agent_rpc_fn g_money_rpc;

void metaverse_agent_service_set_rpc(metaverse_agent_rpc_fn fn)
{
    g_money_rpc = fn;
}

/* The one validation both readers share. Every refusal names which of the
 * three shape rules the caller broke, because "bad dir" alone does not tell an
 * operator whether to fix the path, the spelling, or the directory. */
static struct zcl_result dir_ok(const char *dir, char *out, size_t out_cap,
                                size_t *out_len)
{
    if (!out || out_cap == 0 || !out_len)
        return ZCL_ERR(MVS_ERR_BAD_ARGS, "out buffer is required");
    *out_len = 0;
    if (!dir || !dir[0])
        return ZCL_ERR(MVS_ERR_BAD_ARGS, "dir is required");
    if (dir[0] != '/')
        return ZCL_ERR(MVS_ERR_BAD_ARGS,
                       "dir must be an absolute path, got '%s'", dir);
    if (strnlen(dir, MVS_DIR_MAX + 1) > MVS_DIR_MAX)
        return ZCL_ERR(MVS_ERR_BAD_ARGS, "dir longer than %d bytes",
                       MVS_DIR_MAX);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return ZCL_ERR(MVS_ERR_NOT_A_DIR,
                       "'%s' is not an existing directory", dir);
    return ZCL_OK;
}

struct zcl_result metaverse_agent_service_status(const char *dir, char *out,
                                                 size_t out_cap,
                                                 size_t *out_len)
{
    struct zcl_result r = dir_ok(dir, out, out_cap, out_len);
    if (!r.ok)
        return r;

    size_t n = agent_broker_render_status_json(dir, out, out_cap);
    if (n == 0)
        return ZCL_ERR(MVS_ERR_RENDER_FAILED,
                       "status document for '%s' did not fit %zu bytes", dir,
                       out_cap);
    *out_len = n;
    return ZCL_OK;
}

struct zcl_result metaverse_agent_service_audit(const char *dir, size_t limit,
                                                char *out, size_t out_cap,
                                                size_t *out_len)
{
    struct zcl_result r = dir_ok(dir, out, out_cap, out_len);
    if (!r.ok)
        return r;

    size_t n = agent_audit_render_json(dir, limit, out, out_cap);
    if (n == 0)
        return ZCL_ERR(MVS_ERR_RENDER_FAILED,
                       "audit document for '%s' did not fit %zu bytes", dir,
                       out_cap);
    *out_len = n;
    return ZCL_OK;
}

struct money_binding_read {
    struct agent_money_binding binding;
    struct json_value snapshot;
    char observed_wallet_instance_id[33];
    bool configured;
    bool current;
};

static bool money_load_bindings(const char *dir,
                                struct money_binding_read rows[2])
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/money-bindings.json", dir);
    FILE *f = fopen(path, "re");
    if (!f)
        return true; /* explicit UNKNOWN rows are produced by the caller */
    char raw[4096];
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    bool overflow = !feof(f);
    (void)fclose(f);
    if (overflow || n == 0)
        return false;
    raw[n] = '\0';
    struct json_value doc;
    json_init(&doc);
    if (!json_read(&doc, raw, n)) {
        json_free(&doc);
        return false;
    }
    const struct json_value *wallets = json_get(&doc, "wallets");
    if (!wallets || wallets->type != JSON_ARR || json_size(wallets) > 2) {
        json_free(&doc);
        return false;
    }
    for (size_t i = 0; i < json_size(wallets); i++) {
        const struct json_value *w = json_at(wallets, i);
        const char *scope = w ? json_get_str(json_get(w, "scope")) : NULL;
        int slot = scope && strcmp(scope, "dev") == 0 ? 0
                 : scope && strcmp(scope, "prod") == 0 ? 1 : -1;
        const char *wid = w ? json_get_str(json_get(w, "wallet_instance_id")) : NULL;
        const char *gen = w ? json_get_str(json_get(w, "network_genesis")) : NULL;
        const char *dd = w ? json_get_str(json_get(w, "node_datadir")) : NULL;
        int64_t port = w ? json_get_int(json_get(w, "rpc_port")) : 0;
        if (slot < 0 || rows[slot].configured || !wid || strlen(wid) != 32 ||
            !gen || strlen(gen) != 64 || !dd || dd[0] != '/' ||
            strlen(dd) >= sizeof(rows[slot].binding.node_datadir) ||
            port <= 0 || port > 65535) {
            json_free(&doc);
            return false;
        }
        struct agent_money_binding *b = &rows[slot].binding;
        (void)snprintf(b->wallet_scope, sizeof(b->wallet_scope), "%s", scope);
        (void)snprintf(b->wallet_instance_id, sizeof(b->wallet_instance_id),
                       "%s", wid);
        (void)snprintf(b->network_genesis, sizeof(b->network_genesis), "%s",
                       gen);
        (void)snprintf(b->node_datadir, sizeof(b->node_datadir), "%s", dd);
        b->rpc_port = (int)port;
        rows[slot].configured = true;
    }
    json_free(&doc);
    return true;
}

static void money_unknown(struct json_value *out, const char *scope,
                          const char *status, const char *reason)
{
    json_set_object(out);
    (void)json_push_kv_str(out, "wallet_scope", scope);
    (void)json_push_kv_str(out, "wallet_instance_id", "");
    (void)json_push_kv_str(out, "status", status);
    (void)json_push_kv_bool(out, "complete", false);
    (void)json_push_kv_str(out, "reason", reason);
    (void)json_push_kv_str(out, "confirmed_zcl", "UNKNOWN");
    (void)json_push_kv_str(out, "pending_zcl", "UNKNOWN");
    (void)json_push_kv_str(out, "encumbered_zcl", "UNKNOWN");
    (void)json_push_kv_str(out, "intent_reserved_zcl", "UNKNOWN");
    (void)json_push_kv_str(out, "agent_available_zcl", "UNKNOWN");
    (void)json_push_kv_str(out, "snapshot_root", "");
}

static bool money_query(struct money_binding_read *row)
{
    if (!g_money_rpc)
        return false; /* raw-return-ok:no transport means explicit UNKNOWN */
    char params[96];
    (void)snprintf(params, sizeof(params),
                   "[\"custody\",{\"wallet_scope\":\"%s\"}]",
                   row->binding.wallet_scope);
    char *raw = g_money_rpc(row->binding.node_datadir, row->binding.rpc_port,
                            "agentsession", params,
                            MVS_MONEY_RPC_CONNECT_MS,
                            MVS_MONEY_RPC_TOTAL_MS);
    if (!raw)
        return false;
    struct json_value reply;
    json_init(&reply);
    bool parsed = json_read(&reply, raw, strlen(raw));
    free(raw);
    const struct json_value *snapshot = parsed ? json_get(&reply, "snapshot")
                                               : NULL;
    if (!snapshot || snapshot->type != JSON_OBJ) {
        json_free(&reply);
        return false;
    }
    const char *scope = json_get_str(json_get(snapshot, "wallet_scope"));
    const char *wid = json_get_str(json_get(snapshot, "wallet_instance_id"));
    const char *gen = json_get_str(json_get(snapshot, "network_genesis"));
    const char *status = json_get_str(json_get(snapshot, "status"));
    const bool reported_current = status && strcmp(status, "CURRENT") == 0;
    if (wid && strlen(wid) == 32)
        (void)snprintf(row->observed_wallet_instance_id,
                       sizeof(row->observed_wallet_instance_id), "%s", wid);
    int64_t observed = json_get_int(json_get(snapshot, "observed_at"));
    int64_t now = (int64_t)platform_time_wall_time_t();
    bool identity_ok = scope && wid && gen &&
        strcmp(scope, row->binding.wallet_scope) == 0 &&
        strcmp(wid, row->binding.wallet_instance_id) == 0 &&
        strcmp(gen, row->binding.network_genesis) == 0;
    json_copy(&row->snapshot, snapshot);
    json_free(&reply);
    if (!identity_ok) {
        json_free(&row->snapshot);
        json_init(&row->snapshot);
        money_unknown(&row->snapshot, row->binding.wallet_scope,
                      "CONFLICTED", "live wallet identity differs from grant");
        return true;
    }
    if (observed <= 0 || now < observed || now - observed > 30) {
        struct json_value *status_value =
            (struct json_value *)json_get(&row->snapshot, "status");
        struct json_value *complete_value =
            (struct json_value *)json_get(&row->snapshot, "complete");
        struct json_value *reason_value =
            (struct json_value *)json_get(&row->snapshot, "reason");
        if (status_value) json_set_str(status_value, "STALE");
        if (complete_value) json_set_bool(complete_value, false);
        if (reason_value)
            json_set_str(reason_value, "observation is older than 30 seconds");
        return true;
    }
    row->current = reported_current &&
        json_get_bool(json_get(&row->snapshot, "complete"));
    return true;
}

static void money_portfolio_root(const struct money_binding_read rows[2],
                                 char out[65])
{
    struct sha3_256_ctx c;
    sha3_256_init(&c);
    sha3_256_write(&c, (const uint8_t *)"zcl.agent_money.portfolio.v1", 28);
    for (int i = 0; i < 2; i++) {
        const char *status = json_get_str(json_get(&rows[i].snapshot, "status"));
        const char *root = json_get_str(json_get(&rows[i].snapshot,
                                                 "snapshot_root"));
        sha3_256_write(&c, (const uint8_t *)(status ? status : "UNKNOWN"),
                       strlen(status ? status : "UNKNOWN"));
        uint8_t decoded[32] = { 0 };
        if (root && root[0])
            (void)zcl_hex_decode(root, decoded, 32);
        sha3_256_write(&c, decoded, 32);
    }
    uint8_t root[32];
    sha3_256_finalize(&c, root);
    zcl_hex_encode(root, 32, out);
}

struct zcl_result metaverse_agent_service_money(const char *dir, char *out,
                                                size_t out_cap,
                                                size_t *out_len)
{
    struct zcl_result valid = dir_ok(dir, out, out_cap, out_len);
    if (!valid.ok)
        return valid;
    struct money_binding_read rows[2];
    memset(rows, 0, sizeof(rows));
    for (int i = 0; i < 2; i++)
        json_init(&rows[i].snapshot);
    bool binding_ok = money_load_bindings(dir, rows);
    for (int i = 0; i < 2; i++) {
        const char *scope = i == 0 ? "dev" : "prod";
        if (!binding_ok)
            money_unknown(&rows[i].snapshot, scope, "CONFLICTED",
                          "private custody binding file is malformed");
        else if (!rows[i].configured)
            money_unknown(&rows[i].snapshot, scope, "UNKNOWN",
                          "owner created no binding for this wallet scope");
        else if (!money_query(&rows[i]))
            money_unknown(&rows[i].snapshot, scope, "UNKNOWN",
                          "bound wallet endpoint is unreachable");
    }
    const bool configured_duplicate = rows[0].configured && rows[1].configured &&
        strcmp(rows[0].binding.wallet_instance_id,
               rows[1].binding.wallet_instance_id) == 0;
    const bool observed_duplicate =
        rows[0].observed_wallet_instance_id[0] &&
        rows[1].observed_wallet_instance_id[0] &&
        strcmp(rows[0].observed_wallet_instance_id,
               rows[1].observed_wallet_instance_id) == 0;
    if (configured_duplicate || observed_duplicate) {
        for (int i = 0; i < 2; i++) {
            json_free(&rows[i].snapshot); json_init(&rows[i].snapshot);
            money_unknown(&rows[i].snapshot, i == 0 ? "dev" : "prod",
                          "CONFLICTED",
                          "duplicate wallet_instance_id on active bindings");
            rows[i].current = false;
        }
    }

    struct json_value doc, wallets;
    json_init(&doc); json_set_object(&doc);
    json_init(&wallets); json_set_array(&wallets);
    (void)json_push_kv_str(&doc, "schema", "zcl.metaverse_agent_money.v1");
    for (int i = 0; i < 2; i++)
        (void)json_push_back(&wallets, &rows[i].snapshot);
    (void)json_push_kv(&doc, "wallets", &wallets);
    json_free(&wallets);
    bool known = rows[0].current && rows[1].current;
    (void)json_push_kv_bool(&doc, "portfolio_total_known", known);
    if (known) {
        int64_t total = json_get_int(json_get(&rows[0].snapshot,
                                              "confirmed_zat")) +
                        json_get_int(json_get(&rows[1].snapshot,
                                              "confirmed_zat"));
        char zcl[32];
        (void)snprintf(zcl, sizeof(zcl), "%lld.%08lld",
                       (long long)(total / 100000000LL),
                       (long long)(total % 100000000LL));
        (void)json_push_kv_str(&doc, "portfolio_confirmed_zcl", zcl);
        (void)json_push_kv_int(&doc, "portfolio_confirmed_zat", total);
    }
    char root[65];
    money_portfolio_root(rows, root);
    (void)json_push_kv_str(&doc, "snapshot_root", root);
    for (int i = 0; i < 2; i++)
        json_free(&rows[i].snapshot);
    size_t n = json_write(&doc, out, out_cap);
    json_free(&doc);
    if (n == 0)
        return ZCL_ERR(MVS_ERR_RENDER_FAILED,
                       "money document did not fit %zu bytes", out_cap);
    *out_len = n;
    return ZCL_OK;
}
