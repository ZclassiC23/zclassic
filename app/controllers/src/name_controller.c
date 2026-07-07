/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Names RPC controller.
 *
 * Commands:
 *   name_register  — register a name on-chain via OP_RETURN
 *   name_resolve   — look up a name's target
 *   name_list      — list all registered names */

#include "models/znam.h"
#include "json/json.h"
#include "rpc/server.h"
#include "models/database.h"
#include "wallet/wallet.h"
#include "validation/txmempool.h"
#include "services/zslp_command_service.h"
#include "encoding/utilstrencodings.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "util/log_macros.h"

/* ── Context ────────────────────────────────────────────────────── */

static struct node_db *g_name_ndb = NULL;
static struct wallet *g_name_wallet = NULL;
static struct tx_mempool *g_name_mempool = NULL;

void rpc_name_set_state(struct node_db *ndb)
{
    g_name_ndb = ndb;
}

void rpc_name_set_wallet(struct wallet *w, struct tx_mempool *mp)
{
    g_name_wallet = w;
    g_name_mempool = mp;
}

/* ── Helper ─────────────────────────────────────────────────────── */

static const char *type_name(uint8_t t)
{
    switch (t) {
    case ZNAM_TYPE_ONION: return "onion";
    case ZNAM_TYPE_ZADDR: return "z-address";
    case ZNAM_TYPE_TADDR: return "t-address";
    case ZNAM_TYPE_BTC: return "bitcoin";
    case ZNAM_TYPE_LTC: return "litecoin";
    case ZNAM_TYPE_DOGE: return "dogecoin";
    case ZNAM_TYPE_CONTENT: return "content";
    default: return "unknown";
    }
}

static uint8_t parse_type(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "onion") == 0) return ZNAM_TYPE_ONION;
    if (strcmp(s, "zaddr") == 0 || strcmp(s, "z-address") == 0)
        return ZNAM_TYPE_ZADDR;
    if (strcmp(s, "taddr") == 0 || strcmp(s, "t-address") == 0)
        return ZNAM_TYPE_TADDR;
    if (strcmp(s, "btc") == 0 || strcmp(s, "bitcoin") == 0)
        return ZNAM_TYPE_BTC;
    if (strcmp(s, "ltc") == 0 || strcmp(s, "litecoin") == 0)
        return ZNAM_TYPE_LTC;
    if (strcmp(s, "doge") == 0 || strcmp(s, "dogecoin") == 0)
        return ZNAM_TYPE_DOGE;
    if (strcmp(s, "content") == 0 || strcmp(s, "content-hash") == 0)
        return ZNAM_TYPE_CONTENT;
    return 0;
}

static void entry_to_json(const struct znam_entry *e, struct json_value *obj)
{
    json_set_object(obj);
    json_push_kv_str(obj, "name", e->name);
    json_push_kv_str(obj, "owner", e->owner_address);
    json_push_kv_int(obj, "target_type", e->target_type);
    json_push_kv_str(obj, "type", type_name(e->target_type));
    json_push_kv_str(obj, "value", e->target_value);
    json_push_kv_int(obj, "reg_height", e->reg_height);
    char hex[65];
    HexStr(e->reg_txid, 32, false, hex, sizeof(hex));
    json_push_kv_str(obj, "reg_txid", hex);
}

#define ZNAM_API_RECORD_LIMIT 64
#define ZNAM_API_LIST_LIMIT 100

static void append_name_verification(struct json_value *obj)
{
    struct json_value verification = {0};

    json_set_object(&verification);
    json_push_kv_str(&verification, "schema", "zcl.names.verification.v1");
    json_push_kv_str(&verification, "base_layer", "zclassic_l1");
    json_push_kv_str(&verification, "application_layer",
                     "zclassic23_application_layer");
    json_push_kv_str(&verification, "anchor", "confirmed_znam_op_return");
    json_push_kv_str(&verification, "projection", "znam_projection");
    json_push_kv_str(&verification, "mutation_authority",
                     "confirmed_chain_history");
    json_push_kv_str(&verification, "consensus_boundary",
                     "legacy_zclassic_consensus_unchanged");
    json_push_kv(obj, "zcl_verification", &verification);
    json_free(&verification);
}

static void append_name_crud_links(struct json_value *obj, const char *name)
{
    struct json_value links = {0};
    char self[256];

    json_set_object(&links);
    json_push_kv_str(&links, "collection", "/api/v1/names");
    json_push_kv_str(&links, "protocol", "/api/v1/protocols/znam");
    json_push_kv_str(&links, "create", "name_register");
    json_push_kv_str(&links, "read", "/api/v1/names/{name}");
    json_push_kv_str(&links, "update", "name_register/update OP_RETURN");
    json_push_kv_str(&links, "delete", "not_supported_by_znam_v1");
    if (name && name[0]) {
        snprintf(self, sizeof(self), "/api/v1/names/%s", name);
        json_push_kv_str(&links, "self", self);
    }
    json_push_kv(obj, "_links", &links);
    json_free(&links);
}

struct service_record_classification {
    const char *service_name;
    const char *service_contract_name;
    const char *recommended_operation_id;
    const char *next_action;
    const char *transport;
    const char *endpoint_kind;
    bool is_endpoint_hint;
    bool supports_onion;
    bool supports_direct_p2p;
    bool supports_bootstrap;
};

static bool str_eq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static bool value_mentions_onion(const char *value)
{
    return value && strstr(value, ".onion") != NULL;
}

static bool has_prefix(const char *s, const char *prefix)
{
    size_t n;

    if (!s || !prefix)
        return false;
    n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static const char *service_key_suffix(const char *key)
{
    if (has_prefix(key, "service."))
        return key + strlen("service.");
    if (has_prefix(key, "svc."))
        return key + strlen("svc.");
    return NULL;
}

static struct service_record_classification
classify_service_record(const char *key, const char *value)
{
    const char *suffix = service_key_suffix(key);
    struct service_record_classification c = {
        .service_name = "service_hint",
        .service_contract_name = "znam_names",
        .recommended_operation_id = "znam_names.resolve_name",
        .next_action = "inspect_service_record_metadata",
        .transport = "unspecified",
        .endpoint_kind = "service_metadata",
    };

    if (str_eq(key, "onion") || str_eq(suffix, "onion") ||
        value_mentions_onion(value)) {
        c.service_name = "onion_directory";
        c.service_contract_name = "onion_directory";
        c.recommended_operation_id =
            "onion_directory.list_onion_announcements";
        c.next_action =
            "probe_onion_endpoint_then_prefer_direct_p2p_when_available";
        c.transport = "onion";
        c.endpoint_kind = "tor_hidden_service";
        c.is_endpoint_hint = true;
        c.supports_onion = true;
        return c;
    }

    if (str_eq(key, "p2p") || str_eq(suffix, "p2p") ||
        str_eq(suffix, "direct_p2p")) {
        c.service_name = "direct_p2p";
        c.service_contract_name = "bootstrap";
        c.recommended_operation_id =
            "bootstrap.inspect_peer_bootstrap_readiness";
        c.next_action = "connect_direct_p2p_and_verify_peer_readiness";
        c.transport = "p2p";
        c.endpoint_kind = "direct_peer_endpoint";
        c.is_endpoint_hint = true;
        c.supports_direct_p2p = true;
        return c;
    }

    if (str_eq(key, "bootstrap") || str_eq(suffix, "bootstrap")) {
        c.service_name = "bootstrap";
        c.service_contract_name = "bootstrap";
        c.recommended_operation_id = "bootstrap.read_bootstrap_status";
        c.next_action = "read_bootstrap_status_before_using_peer";
        c.transport = "p2p_or_onion";
        c.endpoint_kind = "bootstrap_hint";
        c.is_endpoint_hint = true;
        c.supports_bootstrap = true;
        return c;
    }

    if (str_eq(key, "service")) {
        c.service_name = "declared_service";
        c.service_contract_name = "znam_names";
        c.recommended_operation_id = "znam_names.resolve_name";
        c.endpoint_kind = "service_hint";
        return c;
    }

    if (suffix && suffix[0]) {
        c.service_name = suffix;
        c.service_contract_name = suffix;
        c.recommended_operation_id = "";
        c.next_action = "inspect_declared_service_catalog_entry";
        c.endpoint_kind = "service_metadata";
    }

    return c;
}

static void push_service_route_links(
    struct json_value *obj,
    const struct service_record_classification *classification)
{
    char catalog_route[160];
    char operation_route[192];

    if (!obj || !classification)
        return;

    if (classification->service_contract_name &&
        classification->service_contract_name[0]) {
        snprintf(catalog_route, sizeof(catalog_route),
                 "/api/v1/service-catalog/%s",
                 classification->service_contract_name);
        catalog_route[sizeof(catalog_route) - 1] = '\0';
        json_push_kv_str(obj, "service_contract",
                         classification->service_contract_name);
        json_push_kv_str(obj, "service_catalog_route", catalog_route);
    }

    json_push_kv_str(obj, "recommended_operation_id",
                     classification->recommended_operation_id
                         ? classification->recommended_operation_id : "");
    if (classification->recommended_operation_id &&
        classification->recommended_operation_id[0]) {
        snprintf(operation_route, sizeof(operation_route),
                 "/api/v1/service-operations/%s",
                 classification->recommended_operation_id);
        operation_route[sizeof(operation_route) - 1] = '\0';
        json_push_kv_str(obj, "service_operation_route", operation_route);
    }

    json_push_kv_str(obj, "next_action",
                     classification->next_action
                         ? classification->next_action : "");
}

static void append_service_directory(struct json_value *obj,
                                     const struct json_value *services,
                                     int service_count,
                                     const struct json_value *endpoints,
                                     int endpoint_count,
                                     bool supports_onion,
                                     bool supports_direct_p2p,
                                     bool supports_bootstrap)
{
    struct json_value directory = {0};

    json_set_object(&directory);
    json_push_kv_str(&directory, "schema",
                     "zcl.names.service_directory.v1");
    json_push_kv_int(&directory, "schema_version", 1);
    json_push_kv_str(&directory, "source", "znam_text_records");
    json_push_kv_str(&directory, "transport_model",
                     "records_advertise_tor_or_p2p_endpoints");
    json_push_kv_str(&directory, "base_layer", "zclassic_l1");
    json_push_kv_str(&directory, "routing_policy",
                     "verify_zcl_name_record_then_prefer_direct_p2p_then_onion");
    json_push_kv_str(&directory, "service_contract_route",
                     "/api/v1/service-catalog/{service}");
    json_push_kv_str(&directory, "operation_contract_route",
                     "/api/v1/service-operations/{operation_id}");
    json_push_kv_bool(&directory, "has_services", service_count > 0);
    json_push_kv_int(&directory, "service_record_count", service_count);
    json_push_kv_int(&directory, "endpoint_count", endpoint_count);
    json_push_kv_bool(&directory, "supports_onion", supports_onion);
    json_push_kv_bool(&directory, "supports_direct_p2p",
                      supports_direct_p2p);
    json_push_kv_bool(&directory, "supports_bootstrap",
                      supports_bootstrap);
    json_push_kv(&directory, "records", services);
    json_push_kv(&directory, "endpoints", endpoints);
    json_push_kv(obj, "service_directory", &directory);
    json_free(&directory);
}

static bool is_service_record_key(const char *key)
{
    if (!key)
        return false;
    return strcmp(key, "service") == 0 ||
           strcmp(key, "onion") == 0 ||
           strcmp(key, "p2p") == 0 ||
           strcmp(key, "bootstrap") == 0 ||
           has_prefix(key, "service.") ||
           has_prefix(key, "svc.");
}

static void text_record_to_json(const struct znam_text_record *rec,
                                struct json_value *obj)
{
    json_set_object(obj);
    json_push_kv_str(obj, "name", rec->name);
    json_push_kv_str(obj, "key", rec->key);
    json_push_kv_str(obj, "value", rec->value);
}

static void service_record_to_json(
    const struct znam_text_record *rec,
    const struct service_record_classification *classification,
    struct json_value *obj)
{
    json_set_object(obj);
    json_push_kv_str(obj, "schema", "zcl.names.service_record.v1");
    json_push_kv_str(obj, "name", rec->name);
    json_push_kv_str(obj, "key", rec->key);
    json_push_kv_str(obj, "value", rec->value);
    json_push_kv_str(obj, "service_name",
                     classification->service_name);
    push_service_route_links(obj, classification);
    json_push_kv_str(obj, "transport", classification->transport);
    json_push_kv_str(obj, "endpoint_kind",
                     classification->endpoint_kind);
    json_push_kv_str(obj, "endpoint", rec->value);
    json_push_kv_bool(obj, "is_endpoint_hint",
                      classification->is_endpoint_hint);
    json_push_kv_bool(obj, "chain_verified", true);
    json_push_kv_str(obj, "verified_by", "confirmed_znam_text_record");
    json_push_kv_str(obj, "reachability_proof",
                     "requires_runtime_peer_or_onion_probe");
}

static void addr_record_to_json(const struct znam_addr_record *rec,
                                struct json_value *obj)
{
    json_set_object(obj);
    json_push_kv_str(obj, "name", rec->name);
    json_push_kv_int(obj, "coin_type", rec->coin_type);
    json_push_kv_str(obj, "type", type_name(rec->coin_type));
    json_push_kv_str(obj, "address", rec->address);
}

static void append_record_arrays(const char *name, struct json_value *obj)
{
    struct json_value texts = {0};
    struct json_value services = {0};
    struct json_value endpoints = {0};
    struct json_value addrs = {0};
    struct znam_text_record text_rows[ZNAM_API_RECORD_LIMIT];
    struct znam_addr_record addr_rows[ZNAM_API_RECORD_LIMIT];
    int text_count = 0;
    int service_count = 0;
    int endpoint_count = 0;
    int addr_count = 0;
    bool supports_onion = false;
    bool supports_direct_p2p = false;
    bool supports_bootstrap = false;

    json_set_array(&texts);
    json_set_array(&services);
    json_set_array(&endpoints);
    json_set_array(&addrs);

    if (g_name_ndb && name) {
        text_count = db_znam_text_list(g_name_ndb, name, text_rows,
                                       ZNAM_API_RECORD_LIMIT);
        for (int i = 0; i < text_count; i++) {
            struct json_value row = {0};
            text_record_to_json(&text_rows[i], &row);
            json_push_back(&texts, &row);
            if (is_service_record_key(text_rows[i].key)) {
                struct service_record_classification classification =
                    classify_service_record(text_rows[i].key,
                                            text_rows[i].value);
                struct json_value svc = {0};
                service_record_to_json(&text_rows[i], &classification,
                                       &svc);
                json_push_back(&services, &svc);
                if (classification.is_endpoint_hint) {
                    json_push_back(&endpoints, &svc);
                    endpoint_count++;
                }
                supports_onion = supports_onion ||
                                 classification.supports_onion;
                supports_direct_p2p = supports_direct_p2p ||
                                      classification.supports_direct_p2p;
                supports_bootstrap = supports_bootstrap ||
                                     classification.supports_bootstrap;
                json_free(&svc);
                service_count++;
            }
            json_free(&row);
        }

        addr_count = db_znam_addr_list(g_name_ndb, name, addr_rows,
                                       ZNAM_API_RECORD_LIMIT);
        for (int i = 0; i < addr_count; i++) {
            struct json_value row = {0};
            addr_record_to_json(&addr_rows[i], &row);
            json_push_back(&addrs, &row);
            json_free(&row);
        }
    }

    json_push_kv(obj, "text_records", &texts);
    json_push_kv_int(obj, "text_record_count", text_count);
    json_push_kv(obj, "service_records", &services);
    json_push_kv_int(obj, "service_record_count", service_count);
    json_push_kv(obj, "address_records", &addrs);
    json_push_kv_int(obj, "address_record_count", addr_count);
    append_service_directory(obj, &services, service_count, &endpoints,
                             endpoint_count, supports_onion,
                             supports_direct_p2p, supports_bootstrap);

    json_free(&texts);
    json_free(&services);
    json_free(&endpoints);
    json_free(&addrs);
}

static void entry_to_show_json(const struct znam_entry *e,
                               struct json_value *obj)
{
    entry_to_json(e, obj);
    json_push_kv_str(obj, "schema", "zcl.names.show.v1");
    append_name_verification(obj);
    append_name_crud_links(obj, e->name);
    append_record_arrays(e->name, obj);
}

/* ── name_resolve ───────────────────────────────────────────────── */

static bool rpc_name_resolve(const struct json_value *params, bool help,
                             struct json_value *result)
{
    if (help || !params || json_size(params) < 1) {
        json_set_str(result,
            "name_resolve \"name\"\n"
            "\nResolve a ZCL Name to its target and resolver records.\n"
            "\nArguments:\n"
            "1. name (string, required) The name to resolve\n"
            "\nResult: the name entry or null.\n");
        return true;
    }

    const struct json_value *arg0 = json_at(params, 0);
    const char *name = arg0 ? json_get_str(arg0) : NULL;
    if (!name) {
        json_set_str(result, "name required");
        return false;
    }

    struct znam_entry entry;
    if (!g_name_ndb) {
        LOG_WARN("controller", "name_resolve: name DB not initialized; cannot resolve '%s'", name);
        json_set_str(result, "Name not found");
        return true;
    }
    if (!db_znam_find(g_name_ndb, name, &entry)) {
        json_set_str(result, "Name not found");
        return true;
    }

    entry_to_show_json(&entry, result);
    return true;
}

/* ── name_list ──────────────────────────────────────────────────── */

static bool name_index_to_json(const char *owner, struct json_value *result)
{
    struct json_value names = {0};
    struct znam_entry entries[ZNAM_API_LIST_LIMIT];
    bool owner_filter = owner && owner[0];
    int count = 0;

    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.names.index.v1");
    append_name_verification(result);
    append_name_crud_links(result, NULL);
    json_push_kv_int(result, "limit", ZNAM_API_LIST_LIMIT);
    json_push_kv_bool(result, "filtered", owner_filter);
    if (owner_filter)
        json_push_kv_str(result, "owner", owner);

    json_set_array(&names);
    if (g_name_ndb) {
        if (owner_filter)
            count = db_znam_list_by_owner(g_name_ndb, owner, entries,
                                          ZNAM_API_LIST_LIMIT);
        else
            count = db_znam_list(g_name_ndb, entries, ZNAM_API_LIST_LIMIT);
    }

    for (int i = 0; i < count; i++) {
        struct json_value e = {0};
        entry_to_json(&entries[i], &e);
        json_push_back(&names, &e);
        json_free(&e);
    }

    json_push_kv(result, "names", &names);
    json_push_kv_int(result, "count", count);
    json_free(&names);
    return true;
}

static bool rpc_name_list(const struct json_value *params, bool help,
                          struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "name_list [\"owner_address\"]\n"
            "\nList registered ZCL Names as zcl.names.index.v1, optionally filtered by owner.\n");
        return true;
    }

    const struct json_value *arg0 = params ? json_at(params, 0) : NULL;
    const char *owner = arg0 ? json_get_str(arg0) : NULL;
    return name_index_to_json(owner, result);
}

/* ── name_register ──────────────────────────────────────────────── */

static bool rpc_name_register(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || !params || json_size(params) < 3) {
        json_set_str(result,
            "name_register \"name\" \"type\" \"value\"\n"
            "\nRegister a ZCL Name on-chain via OP_RETURN transaction.\n"
            "\nArguments:\n"
            "1. name  (string) Name to register (1-63 chars, lowercase+hyphens)\n"
            "2. type  (string) Target type: onion, zaddr, taddr, btc, ltc, doge, content\n"
            "3. value (string) Target value (.onion, address, or content hash)\n"
            "\nNote: Requires wallet to create and broadcast the transaction.\n"
            "For now, returns the OP_RETURN hex that needs to be included\n"
            "in a transaction's first output.\n");
        return true;
    }

    const char *name = json_get_str(json_at(params, 0));
    const char *type_str = json_get_str(json_at(params, 1));
    const char *value = json_get_str(json_at(params, 2));

    if (!name || !type_str || !value) {
        json_set_str(result, "Missing arguments");
        return false;
    }

    if (!znam_validate_name(name)) {
        json_set_str(result, "Invalid name (1-63 chars, lowercase alphanumeric + hyphens)");
        return false;
    }

    uint8_t target_type = parse_type(type_str);
    if (target_type == 0) {
        json_set_str(result,
            "Invalid type (use: onion, zaddr, taddr, btc, ltc, doge, content)");
        return false;
    }

    /* Check if name already exists */
    struct znam_entry existing;
    if (g_name_ndb && db_znam_find(g_name_ndb, name, &existing)) {
        json_set_str(result, "Name already registered");
        return false;
    }

    /* Build the OP_RETURN script */
    uint8_t script[512];
    size_t script_len = znam_build_register(script, sizeof(script),
                                            name, target_type, value);
    if (script_len == 0) {
        json_set_str(result, "Failed to build OP_RETURN script");
        return false;
    }

    /* If wallet is available, build and broadcast the transaction */
    if (g_name_wallet && g_name_mempool) {
        struct wallet_tx wtx;
        memset(&wtx, 0, sizeof(wtx));
        int64_t fee_paid = 0;
        const char *tx_error = NULL;

        /* Build base tx with a dust output (546 satoshi) */
        if (!zslp_command_build_genesis_base_tx(g_name_wallet, &wtx,
                                                &fee_paid, &tx_error).ok) {
            json_set_str(result, tx_error ? tx_error : "Failed to build transaction");
            return false;
        }

        /* Prepend OP_RETURN, re-sign, broadcast */
        if (!zslp_command_commit_with_op_return(g_name_wallet, g_name_mempool,
                                                &wtx, script, script_len).ok) {
            json_set_str(result, "Failed to broadcast transaction");
            return false;
        }

        /* Return success with txid */
        json_set_object(result);
        json_push_kv_str(result, "name", name);
        json_push_kv_str(result, "type", type_name(target_type));
        json_push_kv_str(result, "value", value);

        char txid_hex[65];
        uint256_get_hex(&wtx.tx.hash, txid_hex);
        json_push_kv_str(result, "txid", txid_hex);
        json_push_kv_int(result, "fee", fee_paid);
        json_push_kv_str(result, "status", "broadcast");

        printf("znam: registered '%s' -> %s (txid: %s)\n",
               name, value, txid_hex);
        return true;
    }

    /* No wallet — return the OP_RETURN hex for manual inclusion */
    json_set_object(result);
    json_push_kv_str(result, "name", name);
    json_push_kv_str(result, "type", type_name(target_type));
    json_push_kv_str(result, "value", value);

    char hex[1025];
    size_t hex_bytes = script_len < 512 ? script_len : 512;
    HexStr(script, hex_bytes, false, hex, sizeof(hex));
    json_push_kv_str(result, "op_return_hex", hex);
    json_push_kv_int(result, "op_return_size", (int64_t)script_len);
    json_push_kv_str(result, "status", "ready");
    json_push_kv_str(result, "note",
        "Wallet not loaded. Include this OP_RETURN as vout[0] manually.");

    return true;
}

/* ── REST API ───────────────────────────────────────────────────── */

bool api_name_list(struct json_value *result)
{
    return name_index_to_json(NULL, result);
}

bool rpc_name_resolve_api(const char *name, struct json_value *result)
{
    if (!name) LOG_FAIL("name", "rpc_name_resolve_api called with NULL name");
    if (!g_name_ndb) LOG_FAIL("name", "rpc_name_resolve_api: name database not initialized");
    struct znam_entry entry;
    if (!db_znam_find(g_name_ndb, name, &entry)) LOG_FAIL("name", "name '%s' not found in database", name);
    entry_to_show_json(&entry, result);
    return true;
}

/* ── Registration ───────────────────────────────────────────────── */

void register_name_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "names", "name_register", rpc_name_register, true },
        { "names", "name_resolve",  rpc_name_resolve,  true },
        { "names", "name_list",     rpc_name_list,     true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
