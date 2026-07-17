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
#include "config/runtime.h"
#include "crypto/sha3.h"
#include "views/format_helpers.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <inttypes.h>

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

        /* Total cost estimate */
        double total_zcl = price_zcl * size_mb;
        json_push_kv_real(&entry, "total_cost_zcl", total_zcl);

        json_push_kv_int(&entry, "peer_port", offers[i].peer_port);
        json_push_kv_int(&entry, "ttl", offers[i].ttl);
        json_push_kv_int(&entry, "last_seen", offers[i].last_seen);

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
            "\nAnnounce a file for sale on the ZCL Market.\n"
            "\nArguments:\n"
            "1. filepath         (string, required) Path to file to share\n"
            "2. price_per_mb_zat (number, required) Price per MB in zatoshis\n"
            "3. z_addr           (string, optional) Payment z-address\n"
            "\nResult: the file offer object.\n");
        return true;
    }

    /* Parse arguments */
    const struct json_value *arg0 = json_at(params, 0);
    const struct json_value *arg1 = json_at(params, 1);
    if (!arg0 || !arg1) {
        json_set_str(result, "Missing required arguments");
        return false;
    }

    const char *filepath = json_get_str(arg0);
    int64_t price = json_get_int(arg1);

    /* Verify file exists */
    struct stat st;
    if (stat(filepath, &st) != 0) {
        json_set_str(result, "File not found");
        return false;
    }

    /* Build file offer */
    struct file_offer offer;
    memset(&offer, 0, sizeof(offer));

    /* Extract filename from path */
    const char *name = strrchr(filepath, '/');
    name = name ? name + 1 : filepath;
    snprintf(offer.filename, sizeof(offer.filename), "%s", name);

    if (st.st_size < 0) {
        json_set_str(result, "File size invalid");
        return false;
    }
    offer.size_bytes = (uint64_t)st.st_size;
    if (!file_market_num_chunks_for_size(offer.size_bytes,
                                         &offer.num_chunks)) {
        /* guard u32 num_chunks overflow (225 PB+ files). */
        json_set_str(result, "File too large to offer");
        return false;
    }
    offer.price_per_mb = price;
    offer.ttl = FILE_MARKET_MAX_TTL;
    offer.last_seen = (int64_t)platform_time_wall_time_t();

    /* Compute root hash: SHA3-256 of the file path + size as a simple
     * placeholder. Full implementation would hash the file manifest. */
    uint8_t preimage[512];
    size_t plen = snprintf((char *)preimage, sizeof(preimage),
                           "%s:%" PRIu64, filepath, offer.size_bytes);
    struct sha3_256_ctx sha3;
    sha3_256_init(&sha3);
    sha3_256_write(&sha3, preimage, plen);
    sha3_256_finalize(&sha3, offer.root_hash);

    /* Optional z-address */
    if (json_size(params) >= 3) {
        const struct json_value *arg2 = json_at(params, 2);
        const char *zstr = arg2 ? json_get_str(arg2) : NULL;
        if (zstr) {
            size_t zlen = strlen(zstr);
            if (zlen <= 43)
                memcpy(offer.z_addr, zstr, zlen);
        }
    }

    /* Store offer */
    file_market_add_offer(&offer);
    if (g_market_ndb)
        db_file_offer_save(g_market_ndb, &offer);

    /* Return the offer */
    json_set_object(result);
    char hex[65];
    HexStr(offer.root_hash, 32, false, hex, sizeof(hex));
    json_push_kv_str(result, "root_hash", hex);
    json_push_kv_str(result, "filename", offer.filename);
    json_push_kv_int(result, "size_bytes", (int64_t)offer.size_bytes);
    json_push_kv_int(result, "num_chunks", offer.num_chunks);
    json_push_kv_int(result, "price_per_mb_zat", offer.price_per_mb);
    json_push_kv_str(result, "status", "announced");

    printf("market: offering '%s' (%.1f MB, %" PRId64 " zat/MB)\n",
           offer.filename,
           offer.size_bytes / (1024.0 * 1024.0),
           offer.price_per_mb);

    return true;
}

/* ── zmarket_buy ────────────────────────────────────────────────── */

static bool rpc_zmarket_buy(const struct json_value *params, bool help,
                            struct json_value *result)
{
    if (help || !params || json_size(params) < 1) {
        json_set_str(result,
            "zmarket_buy \"root_hash\" [\"output_path\"]\n"
            "\nInitiate purchase and download of a file from the market.\n"
            "\nArguments:\n"
            "1. root_hash   (string, required) SHA3 hash of file offer\n"
            "2. output_path (string, optional) Where to save the file\n"
            "\nResult: download session status.\n");
        return true;
    }

    const struct json_value *arg0 = json_at(params, 0);
    const char *hash_hex = arg0 ? json_get_str(arg0) : NULL;
    if (!zcl_is_hex_string(hash_hex, 64)) {
        json_set_str(result, "Invalid root_hash (expected 64-char hex)");
        return false;
    }

    /* Parse hex hash (length already validated above) */
    uint8_t root_hash[32];
    if (ParseHex(hash_hex, root_hash, 32) != 32) {
        json_set_str(result, "Invalid root_hash (must be 64 hex chars)");
        return false;
    }

    /* Find the offer */
    struct file_offer offer;
    if (!file_market_find_offer(root_hash, &offer)) {
        json_set_str(result, "File offer not found in market");
        return false;
    }

    const char *output = NULL;
    if (json_size(params) >= 2) {
        const struct json_value *arg1 = json_at(params, 1);
        if (arg1) output = json_get_str(arg1);
    }

    int idx = file_market_start_download(root_hash, output);
    if (idx < 0) {
        json_set_str(result, "Failed to start download (max concurrent reached?)");
        return false;
    }

    json_set_object(result);
    char out_hex[65];
    HexStr(root_hash, 32, false, out_hex, sizeof(out_hex));
    json_push_kv_str(result, "root_hash", out_hex);
    json_push_kv_str(result, "filename", offer.filename);
    json_push_kv_int(result, "size_bytes", (int64_t)offer.size_bytes);
    json_push_kv_str(result, "state", "challenging");
    json_push_kv_int(result, "session_index", idx);

    double size_mb = offer.size_bytes / (1024.0 * 1024.0);
    double total_zcl = (offer.price_per_mb / 100000000.0) * size_mb;
    json_push_kv_real(result, "estimated_cost_zcl", total_zcl);

    printf("market: buying '%s' from market (%.1f MB, ~%.8f ZCL)\n",
           offer.filename, size_mb, total_zcl);

    return true;
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

/* ── Registration ───────────────────────────────────────────────── */

void register_market_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "market", "zmarket_list",   rpc_zmarket_list,   true },
        { "market", "zmarket_offer",  rpc_zmarket_offer,  true },
        { "market", "zmarket_buy",    rpc_zmarket_buy,    true },
        { "market", "zmarket_status", rpc_zmarket_status, true },
        { "market", "romseed_register", rpc_romseed_register, true },
        { "market", "romseed_list",     rpc_romseed_list,     true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
