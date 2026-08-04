/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Market RPC controller: file sharing marketplace commands.
 *
 * Commands:
 *   zmarket_list    — list available files on the network
 *   zmarket_offer   — announce a file for sale
 *   zmarket_buy     — initiate purchase of a file
 *   zmarket_status  — show active downloads/uploads */

#include "platform/time_compat.h"
#include "net/file_market.h"
#include "net/rom_seed.h"
#include "util/util.h"
#include "encoding/utilstrencodings.h"
#include "util/log_macros.h"
#include "json/json.h"
#include "rpc/server.h"
#include "models/database.h"
#include "models/market_content.h"
#include "services/file_market_content_service.h"
#include "config/runtime.h"
#include "views/format_helpers.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Context ────────────────────────────────────────────────────── */

static struct node_db *g_market_ndb = NULL;

void rpc_market_set_state(struct node_db *ndb)
{
    g_market_ndb = ndb;
}

/* ── zmarket_list ───────────────────────────────────────────────── */

static bool rpc_zmarket_list(const struct json_value *params, bool help,
                             struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "zmarket_list\n"
            "\nList files available on the ZCL Market network.\n"
            "\nResult: array of file offers with name, size, price, hash.\n");
        return true;
    }
    (void)params;

    json_set_array(result);

    struct file_offer offers[FILE_MARKET_MAX_OFFERS];
    int count = file_market_get_offers(offers, FILE_MARKET_MAX_OFFERS);

    for (int i = 0; i < count; i++) {
        struct json_value entry = {0};
        json_set_object(&entry);

        char hex[65];
        HexStr(offers[i].root_hash, 32, false, hex, sizeof(hex));
        json_push_kv_str(&entry, "root_hash", hex);
        json_push_kv_str(&entry, "filename", offers[i].filename);
        json_push_kv_int(&entry, "size_bytes", (int64_t)offers[i].size_bytes);

        double size_mb = offers[i].size_bytes / (1024.0 * 1024.0);
        json_push_kv_real(&entry, "size_mb", size_mb);

        json_push_kv_int(&entry, "num_chunks", offers[i].num_chunks);
        json_push_kv_int(&entry, "price_per_mb_zat", offers[i].price_per_mb);

        /* Price in ZCL */
        double price_zcl = offers[i].price_per_mb / 100000000.0;
        json_push_kv_real(&entry, "price_per_mb_zcl", price_zcl);

        int64_t total_zat = 0;
        if (file_market_offer_total_zat(&offers[i], &total_zat)) {
            json_push_kv_int(&entry, "total_cost_zat", total_zat);
            json_push_kv_real(&entry, "total_cost_zcl",
                              total_zat / 100000000.0);
        } else if (offers[i].price_per_mb == 0) {
            json_push_kv_int(&entry, "total_cost_zat", 0);
            json_push_kv_real(&entry, "total_cost_zcl", 0.0);
        }

        json_push_kv_int(&entry, "peer_port", offers[i].peer_port);
        json_push_kv_int(&entry, "ttl", offers[i].ttl);
        json_push_kv_int(&entry, "last_seen", offers[i].last_seen);
        json_push_kv_bool(&entry, "authenticated",
                          offers[i].auth_version ==
                              FILE_MARKET_OFFER_VERSION);
        if (offers[i].auth_version == FILE_MARKET_OFFER_VERSION) {
            char offer_hex[65];
            HexStr(offers[i].offer_id, 32, false,
                   offer_hex, sizeof(offer_hex));
            json_push_kv_str(&entry, "offer_id", offer_hex);
            json_push_kv_int(&entry, "expires_unix",
                             offers[i].expires_unix);
        }

        json_push_back(result, &entry);
        json_free(&entry);
    }

    return true;
}

/* ── zmarket_offer ──────────────────────────────────────────────── */

static bool rpc_zmarket_offer(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || !params || json_size(params) < 2) {
        json_set_str(result,
            "zmarket_offer \"filepath\" price_per_mb_zat [\"z_addr\"]\n"
            "\nContained legacy endpoint: it does not create an offer.\n"
            "\nArguments:\n"
            "1. filepath         (string, required) Path to file to share\n"
            "2. price_per_mb_zat (number, required) Price per MB in zatoshis\n"
            "3. z_addr           (string, optional) Payment z-address\n"
            "\nUse romseed_register for verified free artifacts. The typed "
            "signed paid-offer service is not available yet.\n");
        return true;
    }

    /* Parse arguments */
    const struct json_value *arg0 = json_at(params, 0);
    const struct json_value *arg1 = json_at(params, 1);
    if (!arg0 || !arg1) {
        json_set_str(result, "Missing required arguments");
        return false;
    }

    (void)arg0;
    (void)arg1;

    /* This compatibility RPC never hashed the file manifest and never
     * announced an authenticated origin. Refuse both free and paid calls:
     * free recovery artifacts use romseed_register; paid offers will use the
     * typed market-offer plan/commit service once local signing is wired. */
    json_set_str(result,
        "zmarket_offer is contained: use romseed_register for verified free "
        "recovery artifacts; paid offers require the signed market-offer "
        "plan/commit service");
    return false;
}

/* ── zmarket_buy ────────────────────────────────────────────────── */

static bool rpc_zmarket_buy(const struct json_value *params, bool help,
                            struct json_value *result)
{
    if (help || !params || json_size(params) < 1) {
        json_set_str(result,
            "zmarket_buy \"root_hash\" [\"output_path\"]\n"
            "\nContained legacy endpoint: it cannot spend or download.\n"
            "\nArguments:\n"
            "1. root_hash   (string, required) SHA3 hash of file offer\n"
            "2. output_path (string, optional) Where to save the file\n"
            "\nThe typed market-purchase plan/commit service is not "
            "available yet.\n");
        return true;
    }
    (void)params;
    json_set_str(result,
        "zmarket_buy is contained: exact payment verification and paid-file "
        "unlock are not wired; no download session or wallet action started");
    return false;
}

/* ── zmarket_status ─────────────────────────────────────────────── */

static bool rpc_zmarket_status(const struct json_value *params, bool help,
                               struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "zmarket_status\n"
            "\nShow ZCL Market status: offer count, active downloads.\n");
        return true;
    }
    (void)params;

    json_set_object(result);
    json_push_kv_int(result, "offers_cached", file_market_count());

    /* DB count */
    if (g_market_ndb) {
        struct file_offer db_offers[FILE_MARKET_MAX_OFFERS];
        int db_count = db_file_offer_list(g_market_ndb, db_offers,
                                          FILE_MARKET_MAX_OFFERS);
        json_push_kv_int(result, "offers_persisted", db_count);
    }

    return true;
}

/* ── private paid-content registry ───────────────────────────────── */

static bool market_content_index_json(struct json_value *result)
{
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.market_contents.index.v1");
    struct json_value rows = {0};
    json_set_array(&rows);
    if (g_market_ndb && g_market_ndb->open) {
        struct market_content_public_record content[FILE_MARKET_MAX_OFFERS];
        int count = db_market_content_list(g_market_ndb, content,
                                           FILE_MARKET_MAX_OFFERS);
        for (int i = 0; i < count; i++) {
            struct json_value row = {0};
            json_set_object(&row);
            char offer_hex[65], root_hex[65];
            HexStr(content[i].offer_id, 32, false,
                   offer_hex, sizeof(offer_hex));
            HexStr(content[i].root_hash, 32, false,
                   root_hex, sizeof(root_hex));
            json_push_kv_str(&row, "offer_id", offer_hex);
            json_push_kv_str(&row, "root_hash", root_hex);
            json_push_kv_int(&row, "size_bytes",
                             (int64_t)content[i].size_bytes);
            json_push_kv_int(&row, "num_chunks", content[i].num_chunks);
            json_push_kv_int(&row, "registered_at",
                             content[i].registered_at);
            json_push_back(&rows, &row);
            json_free(&row);
        }
    }
    json_push_kv(result, "contents", &rows);
    json_free(&rows);
    return true;
}

static bool rpc_zmarket_content_list(const struct json_value *params,
                                     bool help,
                                     struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "zmarket_content_list\n\nList owner-registered paid content "
            "without revealing private filesystem paths.\n");
        return true;
    }
    (void)params;
    return market_content_index_json(result);
}

static bool rpc_zmarket_content_register(const struct json_value *params,
                                         bool help,
                                         struct json_value *result)
{
    if (help || !params || json_size(params) < 2) {
        json_set_str(result,
            "zmarket_content_register \"offer_id\" \"content_path\"\n"
            "\nBind exact local bytes to an authenticated paid offer. "
            "The private path is never returned.\n");
        return true;
    }
    const char *offer_hex = json_get_str(json_at(params, 0));
    const char *content_path = json_get_str(json_at(params, 1));
    uint8_t offer_id[32];
    if (!offer_hex || strlen(offer_hex) != 64 || !IsHex(offer_hex) ||
        ParseHex(offer_hex, offer_id, sizeof(offer_id)) != 32 ||
        !content_path || !content_path[0]) {
        json_set_str(result,
                     "offer_id must be 64 hex characters and content_path is required");
        return false;
    }
    if (!g_market_ndb || !g_market_ndb->open) {
        json_set_str(result, "market database is unavailable");
        return false;
    }

    struct market_content_public_record registered;
    struct zcl_result saved = file_market_content_register(
        g_market_ndb, offer_id, content_path,
        (int64_t)platform_time_wall_time_t(), &registered);
    if (!saved.ok) {
        json_set_str(result, saved.message);
        return false;
    }

    char saved_offer_hex[65], root_hex[65];
    HexStr(registered.offer_id, 32, false,
           saved_offer_hex, sizeof(saved_offer_hex));
    HexStr(registered.root_hash, 32, false,
           root_hex, sizeof(root_hex));
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.market_content.v1");
    json_push_kv_str(result, "status", "registered");
    json_push_kv_str(result, "offer_id", saved_offer_hex);
    json_push_kv_str(result, "root_hash", root_hex);
    json_push_kv_int(result, "size_bytes", (int64_t)registered.size_bytes);
    json_push_kv_int(result, "num_chunks", registered.num_chunks);
    json_push_kv_int(result, "registered_at", registered.registered_at);
    return true;
}

/* ── romseed_register ───────────────────────────────────────────────
 *
 * Explicitly (re)register a ROM/sync artifact by basename inside the datadir.
 * Registration re-computes every digest from the bytes on disk (never a
 * sidecar); a corrupt / mis-named / out-of-band file is refused. */
static bool rpc_romseed_register(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    if (help || !params || json_size(params) < 1) {
        json_set_str(result,
            "romseed_register \"filename\"\n"
            "\n(Re)register a ROM/sync artifact for free P2P seeding.\n"
            "\nArguments:\n"
            "1. filename (string, required) Basename inside the datadir, e.g.\n"
            "             consensus-state-bundle-<height>.sqlite\n"
            "\nRegistration re-derives whole-file + per-chunk digests from disk\n"
            "and refuses a corrupt / mis-named file. Result: the artifact.\n");
        return true;
    }

    const struct json_value *arg0 = json_at(params, 0);
    const char *filename = arg0 ? json_get_str(arg0) : NULL;
    if (!filename || !filename[0]) {
        json_set_str(result, "Missing filename");
        return false;
    }

    char datadir[1024];
    GetDataDir(true, datadir, sizeof(datadir));

    struct rom_artifact art;
    enum rom_register_result rr =
        rom_seed_register(datadir, filename, NULL, &art);

    json_set_object(result);
    json_push_kv_str(result, "filename", filename);
    json_push_kv_int(result, "result_code", (int)rr);
    if (rr == ROM_REG_OK) {
        char hex[65];
        HexStr(art.chunk_root, 32, false, hex, sizeof(hex));
        json_push_kv_str(result, "status", "registered");
        json_push_kv_str(result, "digest", hex);
        json_push_kv_int(result, "size_bytes", (int64_t)art.size_bytes);
        json_push_kv_int(result, "chunk_size", (int64_t)art.chunk_size);
        json_push_kv_int(result, "chunks", (int64_t)art.num_chunks);

        /* Announce it as a price-0 offer so peers can discover + fetch it. */
        struct file_offer offer;
        uint8_t zero_ip[16] = {0};
        if (rom_seed_build_offer(&art, zero_ip, 0, &offer))
            file_market_add_offer(&offer);
    } else {
        json_push_kv_str(result, "status", "refused");
    }
    return true;
}

/* ── romseed_list ───────────────────────────────────────────────────── */

static bool rpc_romseed_list(const struct json_value *params, bool help,
                             struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "romseed_list\n"
            "\nList ROM/sync artifacts registered for free P2P seeding.\n"
            "\nResult: array of {kind, filename, digest, size_bytes, chunks}.\n");
        return true;
    }
    (void)params;

    json_set_array(result);
    struct rom_artifact arts[ROM_SEED_MAX_ARTIFACTS];
    int n = rom_seed_list(arts, ROM_SEED_MAX_ARTIFACTS);
    for (int i = 0; i < n; i++) {
        struct json_value entry = {0};
        json_set_object(&entry);
        char hex[65];
        HexStr(arts[i].chunk_root, 32, false, hex, sizeof(hex));
        json_push_kv_str(&entry, "kind",
            arts[i].kind == ROM_ARTIFACT_CONSENSUS_BUNDLE ? "consensus_bundle" :
            arts[i].kind == ROM_ARTIFACT_HEADER_SEED ? "header_seed" : "unknown");
        json_push_kv_str(&entry, "filename", arts[i].filename);
        json_push_kv_str(&entry, "digest", hex);
        json_push_kv_int(&entry, "size_bytes", (int64_t)arts[i].size_bytes);
        json_push_kv_int(&entry, "chunk_size", (int64_t)arts[i].chunk_size);
        json_push_kv_int(&entry, "chunks", (int64_t)arts[i].num_chunks);
        json_push_kv_bool(&entry, "free", true);
        json_push_back(result, &entry);
        json_free(&entry);
    }
    return true;
}

/* ── REST API ───────────────────────────────────────────────────── */

bool api_market_list(struct json_value *result)
{
    return rpc_zmarket_list(NULL, false, result);
}

bool api_market_content_list(struct json_value *result)
{
    return market_content_index_json(result);
}

/* ── Registration ───────────────────────────────────────────────── */

void register_market_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "market", "zmarket_list",   rpc_zmarket_list,   true },
        { "market", "zmarket_offer",  rpc_zmarket_offer,  true },
        { "market", "zmarket_buy",    rpc_zmarket_buy,    true },
        { "market", "zmarket_status", rpc_zmarket_status, true },
        { "market", "zmarket_content_list",
          rpc_zmarket_content_list, true },
        { "market", "zmarket_content_register",
          rpc_zmarket_content_register, true },
        { "market", "romseed_register", rpc_romseed_register, true },
        { "market", "romseed_list",     rpc_romseed_list,     true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
