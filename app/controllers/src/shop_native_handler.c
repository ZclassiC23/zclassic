/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Registry handlers for `app shop init` / `app shop status` — slice B of
 * docs/work/SHOP_COMMAND.md: one command that takes a node from "has a
 * store table" to "live private shop", and one read leaf that reports the
 * same verification block on demand with every unmet prerequisite named.
 *
 * Nothing here re-implements a primitive — the handlers compose:
 *
 *   - the persistent onion identity of slice A (onion_identity_ensure in
 *     lib/net/src/tor_integration.c); the stub-vs-real build fact is read
 *     off the same weak dynhost symbol network_telemetry_fill.c reads
 *   - wallet custody from the on-disk envelopes the wallet persistence
 *     layer already writes (WKS1/WKD1 magics, the wrapped-DEK row) — a
 *     plaintext wallet is a named refusal with the credential recipe,
 *     never a silently-minted plaintext shop wallet
 *   - the store schema + products.json provisioning loader
 *     (store_ensure_schema, store_controller_schema.c) against the SAME
 *     <datadir>/node.db the /store HTTP surface serves from, so a product
 *     loaded here is live on the next request with no restart
 *   - the directory announcement as one id in
 *     <datadir>/directory/apps.csv (ONION_DIR_EXTRA_APPS_REL), which
 *     lib/net's register_self() folds into the node's own /directory.json
 *     apps row on its next round — running node or not
 *
 * The datadir-local file/DB helpers those steps run on (the wallet probe,
 * the products.json copy, the announcement write, the read-only identity
 * read) live in shop_native_probes.c — small enough to keep this TU under
 * the app/ file-size ceiling, and reusable unchanged by a future isolated
 * storefront worker process.
 *
 * `init` is plan/commit: a first call with no confirm:true returns the
 * non-mutating plan (every check, every gap, the commit input); only the
 * confirmed call mints, writes, or announces. `status` never mutates.
 *
 * Bound in config/commands/store.def. */

#include "controllers/shop_native_handler.h"

#include "controllers/native_handler_body.h" /* json_get_bool_or/json_get_str_or */
#include "controllers/store_controller_internal.h" /* store_ensure_schema */
#include "command/native_command.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "models/activerecord.h"        /* AR_STEP_ROW */
#include "models/database.h"
#include "models/store.h"
#include "net/onion_service.h"
#include "net/tor_integration.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SHOP_TAG "native.app.shop"

/* The named custody recipe, one string so the refusal, the plan gap, and
 * the status gap never drift apart. */
#define SHOP_WALLET_RECIPE \
    "encrypt the wallet before opening a shop. Existing plaintext wallet: " \
    "run the walletencrypt RPC (zcl-rpc walletencrypt \"<passphrase>\") — " \
    "it wraps every plaintext secret under the passphrase and locks. New " \
    "wallet: boot with the systemd wallet-passphrase credential (or " \
    "ZCL_WALLET_PASSPHRASE set at first boot) so keys are wrapped before " \
    "they hit disk. -allow-plaintext-wallet is refused on the shop lane."

#define SHOP_REMEDY_INIT \
    "zclassic23 app shop init --input='{\"confirm\":true}'"

#define SHOP_REMEDY_TOR \
    "rebuild against the vendored Tor (make tor-full), then boot with -tor"

#define SHOP_REMEDY_PERSIST \
    "boot with -tor -onion-persist so the node installs the persistent " \
    "identity as its onion service (the identity itself is already ensured)"

/* ── failures ───────────────────────────────────────────────────────── */
static void sh_fail(struct zcl_command_reply *reply,
                    enum zcl_command_status status,
                    enum zcl_command_exit exit_code, const char *code,
                    const char *phase, const char *message,
                    const char *evidence)
{
    LOG_ERROR(SHOP_TAG, "%s: %s (%s)", code, message,
              evidence && evidence[0] ? evidence : "-");
    zcl_command_reply_fail(reply, status, exit_code, code, phase, false,
                           false, message, evidence ? evidence : "");
}

/* Explicit input.datadir wins, else the CLI's --datadir. NULL when neither
 * is set (the same rule the store merchant leaves use). */
static const char *sh_datadir(const struct zcl_command_request *request)
{
    const char *dd = json_get_str(json_get(request->input, "datadir"));
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

/* ── the posture snapshot both leaves render ────────────────────────── */
struct shop_snapshot {
    bool tor_real;
    bool identity_present;
    char address[64];            /* 56 chars + NUL, "" when absent */
    enum shop_wallet_posture wallet;
    bool node_db_present;
    bool store_schema;           /* the products table exists */
    int schema_version;          /* -1 when unknown */
    int product_count;           /* -1 when unknown */
    bool products_json_present;
    bool announced;
};

/* The read-only node.db facts: schema version, whether the store schema
 * exists, and the product count when it does. */
static void sh_read_store_state(const char *datadir,
                                struct shop_snapshot *snap)
{
    char path[1024];
    struct stat st;
    snap->node_db_present =
        shop_internal_path_join(path, sizeof(path), datadir, "node.db") &&
        stat(path, &st) == 0 && S_ISREG(st.st_mode);
    if (!snap->node_db_present)
        return;

    sqlite3 *db = NULL;
    struct node_db ndb;
    if (zcl_native_node_db_open_readonly(datadir, &db, &ndb, NULL, 0)
            != ZCL_NODE_DB_RO_OK)
        return;         /* unreadable db: every field stays "unknown" */

    snap->schema_version = node_db_schema_version(&ndb);

    static const char *const sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='products'";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) { // raw-controller-sql-ok
        if (AR_STEP_ROW(s))
            snap->store_schema = true;
        sqlite3_finalize(s);
    } else if (s) {
        sqlite3_finalize(s);
    }
    if (snap->store_schema)
        snap->product_count = db_store_product_count(&ndb);
    zcl_native_node_db_close_readonly(&db, &ndb);
}

static void shop_snapshot_collect(const char *datadir,
                                  struct shop_snapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->schema_version = -1;
    snap->product_count = -1;
    snap->tor_real = shop_tor_real_build_linked();
    snap->identity_present =
        shop_internal_read_identity(datadir, snap->address);
    snap->wallet = shop_probe_wallet_posture(datadir);
    sh_read_store_state(datadir, snap);
    char pj[1024];
    struct stat st;
    snap->products_json_present =
        shop_internal_path_join(pj, sizeof(pj), datadir,
                                "store/products.json") &&
        stat(pj, &st) == 0 && S_ISREG(st.st_mode);
    char csv[ONION_DIR_APPS_CSV_MAX + 1];
    if (onion_directory_extra_apps_csv(datadir, csv, sizeof(csv)) > 0) {
        /* token-exact membership, not substring ("shopify" is not "shop") */
        const char *p = csv;
        while (*p && !snap->announced) {
            const char *comma = strchr(p, ',');
            size_t tl = comma ? (size_t)(comma - p) : strlen(p);
            if (tl == strlen(SHOP_DIRECTORY_APP_ID) &&
                memcmp(p, SHOP_DIRECTORY_APP_ID, tl) == 0)
                snap->announced = true;
            p += tl + (comma ? 1 : 0);
        }
    }
}

static bool shop_snapshot_live(const struct shop_snapshot *snap)
{
    return snap->tor_real && snap->identity_present &&
           snap->wallet == SHOP_WALLET_ENCRYPTED &&
           snap->node_db_present && snap->store_schema && snap->announced;
}

/* ── rendering ──────────────────────────────────────────────────────── */
static void shop_push_snapshot(struct json_value *into,
                               const struct shop_snapshot *snap)
{
    struct json_value tor;
    json_init(&tor);
    json_set_object(&tor);
    (void)json_push_kv_str(&tor, "build",
                           snap->tor_real ? "real_tor" : "tor_stub");
    (void)json_push_kv_bool(&tor, "identity_present", snap->identity_present);
    if (snap->identity_present)
        (void)json_push_kv_str(&tor, "address", snap->address);
    (void)json_push_kv_str(&tor, "persistence_flag", "-onion-persist");
    (void)json_push_kv_str(&tor, "persistence_note",
        "the CLI cannot read the running node's boot flags; the identity "
        "is used only when the node is booted with -tor -onion-persist");
    (void)json_push_kv(into, "tor", &tor);
    json_free(&tor);

    struct json_value wallet;
    json_init(&wallet);
    json_set_object(&wallet);
    (void)json_push_kv_str(&wallet, "posture",
                           shop_wallet_posture_name(snap->wallet));
    (void)json_push_kv_bool(&wallet, "encrypted_at_rest",
                            snap->wallet == SHOP_WALLET_ENCRYPTED);
    (void)json_push_kv(into, "wallet", &wallet);
    json_free(&wallet);

    struct json_value store;
    json_init(&store);
    json_set_object(&store);
    (void)json_push_kv_bool(&store, "node_db_present", snap->node_db_present);
    (void)json_push_kv_bool(&store, "schema_present", snap->store_schema);
    if (snap->schema_version >= 0)
        (void)json_push_kv_int(&store, "schema_version",
                               snap->schema_version);
    if (snap->product_count >= 0)
        (void)json_push_kv_int(&store, "products", snap->product_count);
    (void)json_push_kv_bool(&store, "products_json_present",
                            snap->products_json_present);
    (void)json_push_kv(into, "store", &store);
    json_free(&store);

    struct json_value discovery;
    json_init(&discovery);
    json_set_object(&discovery);
    (void)json_push_kv_bool(&discovery, "announced", snap->announced);
    (void)json_push_kv_str(&discovery, "app_id", SHOP_DIRECTORY_APP_ID);
    (void)json_push_kv_str(&discovery, "apps_file",
                           ONION_DIR_EXTRA_APPS_REL);
    (void)json_push_kv_str(&discovery, "note",
        "the node's register_self() folds this file into its own "
        "/directory.json apps row on its next round");
    (void)json_push_kv(into, "discovery", &discovery);
    json_free(&discovery);
}

static void shop_push_gap(struct json_value *gaps, const char *gap,
                          const char *remedy)
{
    struct json_value g;
    json_init(&g);
    json_set_object(&g);
    (void)json_push_kv_str(&g, "gap", gap);
    (void)json_push_kv_str(&g, "remedy", remedy);
    (void)json_push_back(gaps, &g);
    json_free(&g);
}

static void shop_push_gaps(struct json_value *into,
                           const struct shop_snapshot *snap)
{
    struct json_value gaps;
    json_init(&gaps);
    json_set_array(&gaps);
    if (!snap->tor_real)
        shop_push_gap(&gaps, "tor_stub_build", SHOP_REMEDY_TOR);
    if (!snap->identity_present)
        shop_push_gap(&gaps, "no_persistent_onion_identity",
                      SHOP_REMEDY_INIT " mints the persistent identity; "
                      "then " SHOP_REMEDY_PERSIST);
    switch (snap->wallet) {
    case SHOP_WALLET_PLAINTEXT:
        shop_push_gap(&gaps, "wallet_plaintext_at_rest", SHOP_WALLET_RECIPE);
        break;
    case SHOP_WALLET_ABSENT:
        shop_push_gap(&gaps, "wallet_absent", SHOP_WALLET_RECIPE);
        break;
    case SHOP_WALLET_UNREADABLE:
        shop_push_gap(&gaps, "wallet_unreadable",
                      "boot the node once to create node.db, then re-run "
                      "zclassic23 app shop status");
        break;
    case SHOP_WALLET_ENCRYPTED:
        break;
    }
    if (!snap->node_db_present)
        shop_push_gap(&gaps, "node_db_missing",
                      "boot the node once: node.db and the store schema are "
                      "created on first boot");
    else if (!snap->store_schema)
        shop_push_gap(&gaps, "store_schema_missing", SHOP_REMEDY_INIT);
    if (!snap->announced)
        shop_push_gap(&gaps, "shop_not_announced",
                      SHOP_REMEDY_INIT " writes " ONION_DIR_EXTRA_APPS_REL);
    (void)json_push_kv(into, "gaps", &gaps);
    json_free(&gaps);
}

/* The printed "your shop is live" verification block, shared by the
 * successful commit and (with shop_live false) by plan/status. */
static void shop_push_verification(struct json_value *into,
                                   const struct shop_snapshot *snap)
{
    (void)json_push_kv_bool(into, "shop_live", shop_snapshot_live(snap));
    if (snap->identity_present) {
        char url[128];
        (void)snprintf(url, sizeof(url), "http://%s.onion/store",
                       snap->address);
        (void)json_push_kv_str(into, "shop_url", url);
    }
    (void)json_push_kv_str(into, "buyer_next_command",
                           "zclassic23 app store catalog");
    (void)json_push_kv_str(into, "buyer_note",
        "a buyer finds the shop at its onion /store URL, or by fetching "
        "/directory.json from any seed and looking for \"shop\" in the "
        "apps array");
}

/* ── app shop status (READ) ─────────────────────────────────────────── */
void zcl_native_handle_shop_status(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = sh_datadir(request);
    if (!datadir) {
        sh_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                "MISSING_DATADIR", "normalize",
                "no datadir given and no --datadir default", "datadir");
        return;
    }

    struct shop_snapshot snap;
    shop_snapshot_collect(datadir, &snap);

    (void)json_push_kv_str(&reply->data, "datadir", datadir);
    shop_push_snapshot(&reply->data, &snap);
    shop_push_verification(&reply->data, &snap);
    shop_push_gaps(&reply->data, &snap);
}

/* ── app shop init (plan/commit) ────────────────────────────────────── */
static void shop_init_plan(const char *datadir,
                           const struct shop_snapshot *snap,
                           struct zcl_command_reply *reply)
{
    (void)json_push_kv_str(&reply->data, "mode", "plan");
    (void)json_push_kv_str(&reply->data, "datadir", datadir);
    shop_push_snapshot(&reply->data, snap);
    shop_push_verification(&reply->data, snap);
    shop_push_gaps(&reply->data, snap);

    struct json_value plan;
    json_init(&plan);
    json_set_array(&plan);
    static const char *const steps[] = {
        "verify the wallet is encrypted at rest (refuse by name otherwise)",
        "verify this binary links the real vendored Tor (refuse by name "
            "on the stub build)",
        "ensure the persistent onion identity at "
            "<datadir>/tor_data/onion_service (mint on first run, reuse "
            "after) and print the address",
        "copy --input products.json to <datadir>/store/products.json when "
            "given",
        "ensure the store schema in <datadir>/node.db and provision "
            "products.json through the existing loader",
        "add \"shop\" to <datadir>/" ONION_DIR_EXTRA_APPS_REL " so the "
            "node's /directory.json apps row announces the storefront",
        "print the verification block: shop URL, product count, wallet "
            "state, discovery state, and the buyer's next command",
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        struct json_value step;
        json_init(&step);
        json_set_str(&step, steps[i]);
        (void)json_push_back(&plan, &step);
        json_free(&step);
    }
    (void)json_push_kv(&reply->data, "plan", &plan);
    json_free(&plan);
    /* The commit instruction rides in DATA, not in next[]: a next entry
     * pointing back at this same leaf is rejected by the envelope's
     * push_next_array (self-reference), which drops the WHOLE reply to an
     * empty RESPONSE_BUDGET_EXCEEDED — the front-door failure this field
     * now carries the fix for. */
    (void)json_push_kv_str(&reply->data, "commit_input",
                           "{\"confirm\":true}");
    (void)json_push_kv_str(&reply->data, "commit_command",
                           SHOP_REMEDY_INIT);
}

void zcl_native_handle_shop_init(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = sh_datadir(request);
    if (!datadir) {
        sh_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                "MISSING_DATADIR", "normalize",
                "no datadir given and no --datadir default", "datadir");
        return;
    }

    struct shop_snapshot snap;
    shop_snapshot_collect(datadir, &snap);

    if (!json_get_bool_or(request->input, "confirm", false)) {
        shop_init_plan(datadir, &snap, reply);
        return;
    }

    /* Commit. Custody gates BEFORE anything is minted or written: the
     * most dangerous misconfiguration (keys plaintext on disk) is named
     * first, even on a stub-Tor build where step 2 would otherwise mask
     * it. */
    if (snap.wallet != SHOP_WALLET_ENCRYPTED) {
        const char *code = snap.wallet == SHOP_WALLET_PLAINTEXT
            ? "WALLET_NOT_ENCRYPTED"
            : snap.wallet == SHOP_WALLET_ABSENT
                ? "WALLET_ABSENT" : "WALLET_UNAVAILABLE";
        sh_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_DENIED,
                code, "custody", SHOP_WALLET_RECIPE,
                shop_wallet_posture_name(snap.wallet));
        return;
    }
    if (!snap.tor_real) {
        sh_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                "TOR_STUB_BUILD", "tor", SHOP_REMEDY_TOR, "tor_stub");
        return;
    }

    /* Persistent identity: mint on first init, reuse after. A short or
     * corrupt seed is a named refusal inside onion_identity_ensure, never
     * a silent remint. */
    uint8_t seed[32];
    char address[64];
    bool identity_created = false;
    if (!onion_identity_ensure(datadir, seed, address, sizeof(address),
                               &identity_created)) {
        sh_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
                "IDENTITY_FAILED", "identity",
                "the persistent onion identity exists but is unreadable or "
                "corrupt — inspect <datadir>/tor_data/onion_service/"
                "identity_seed (it is never silently reminted)",
                datadir);
        return;
    }
    memset(seed, 0, sizeof(seed));

    /* --input products.json, copied before the schema step so the loader
     * provisions it. */
    const char *input = json_get_str_or(request->input, "input", NULL);
    if (input && input[0]) {
        char err[160] = "";
        if (!shop_provision_products_json(datadir, input, err, sizeof(err))) {
            sh_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                    ZCL_COMMAND_EXIT_INVALID, "INPUT_UNREADABLE", "provision",
                    "the --input products.json could not be provisioned",
                    err);
            return;
        }
    }

    /* Store schema + products, through the same loader the /store surface
     * uses, against the same node.db it serves from. The file must already
     * exist: node_db_open_runtime would happily mint one on a mistyped
     * datadir, so a missing file is named instead. */
    char db_path[1024];
    if (!shop_internal_path_join(db_path, sizeof(db_path), datadir,
                                 "node.db")) {
        sh_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                "DATADIR_PATH_TOO_LONG", "normalize",
                "datadir path is too long to address node.db", datadir);
        return;
    }
    struct stat st;
    if (stat(db_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        sh_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                "STORE_NOT_INITIALISED", "schema",
                "no node.db at this datadir — boot the node once to create "
                "the store schema, then re-run init", db_path);
        return;
    }
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open_runtime(&ndb, db_path, "shop.init")) {
        sh_fail(reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                "STORE_NOT_INITIALISED", "schema",
                "node.db exists but could not be opened for the store",
                db_path);
        return;
    }
    int products_before = db_store_product_count(&ndb);
    bool had_products_json = snap.products_json_present;
    store_ensure_schema(ndb.db, datadir);
    int products_after = db_store_product_count(&ndb);
    node_db_close(&ndb);
    int products_loaded = products_after - products_before;
    /* products.json present and a still-empty store means the loader
     * found nothing valid; a repopulated store with no products.json
     * means the demo seeds fell back. Both are named, neither is silent. */
    bool demo_seeded = products_before == 0 && products_after > 0 &&
                       !had_products_json && !(input && input[0]);

    char err[160] = "";
    if (!shop_announce_directory_app(datadir, err, sizeof(err))) {
        sh_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
                "ANNOUNCE_FAILED", "discovery",
                "the shop could not be registered on the node's "
                "/directory.json apps row", err);
        return;
    }

    /* The verification block describes what the operator now HAS, so
     * re-collect rather than trusting the pre-mutation snapshot. */
    struct shop_snapshot after;
    shop_snapshot_collect(datadir, &after);
    (void)json_push_kv_str(&reply->data, "mode", "commit");
    (void)json_push_kv_str(&reply->data, "datadir", datadir);
    (void)json_push_kv_bool(&reply->data, "identity_created",
                            identity_created);
    (void)json_push_kv_int(&reply->data, "products_loaded", products_loaded);
    (void)json_push_kv_bool(&reply->data, "demo_products_seeded",
                            demo_seeded);
    if (input && input[0] && products_before > 0)
        (void)json_push_kv_str(&reply->data, "products_note",
            "products.json was copied but the products table is not empty; "
            "the loader only provisions an empty store");
    if (!snap.identity_present)
        (void)json_push_kv_str(&reply->data, "persistence_note",
            SHOP_REMEDY_PERSIST);
    shop_push_snapshot(&reply->data, &after);
    shop_push_verification(&reply->data, &after);
    reply->error.mutated = true;
}
