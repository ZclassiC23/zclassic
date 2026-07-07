/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression test: `getsyncdiag` RPC crashes via `json_free` on uninitialized
 * stack memory.
 *
 * The bug: `rpc_getsyncdiag` in `app/controllers/src/health_controller.c`
 * declares `struct json_value wd;` (and `hdr`) without `json_init()` or
 * `= {0}`. `json_set_object(&wd)` internally calls `json_free(&wd)`,
 * which reads uninitialized `type`, `num_children`, and `children` —
 * typically crashing with SIGSEGV/SIGABRT once the stack region holds
 * non-zero residue from earlier RPCs (which is always the case on a
 * live node).
 *
 * This test dirties the lower stack with 0xCC before calling the RPC
 * to force the uninitialized read to observe garbage in a fresh test
 * process, making the repro deterministic. Post-fix (wd/hdr explicitly
 * zero-initialized), the RPC must return a well-formed JSON object
 * containing non-empty `watchdog` and `headers` sub-objects. */

#include "test/test_helpers.h"
#include "chain/checkpoints.h"
#include "controllers/agent_controller.h"
#include "controllers/agent_resources.h"
#include "controllers/agent_restart_watchdog.h"
#include "controllers/diagnostics_controller.h"
#include "controllers/event_controller.h"
#include "controllers/health_controller.h"
#include "controllers/network_controller.h"
#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "jobs/tip_finalize_stage.h"
#include "models/block.h"
#include "models/database.h"
#include "services/block_source_policy.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/node_health_service.h"
#include "services/sync_monitor.h"
#include "storage/boot_auto_reindex.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "validation/mirror_consensus.h"
#include "event/event.h"
#include "net/connman.h"
#include "net/download.h"
#include "net/fast_sync.h"
#include "net/netbase.h"
#include "net/peer_lifecycle.h"
#include "net/version.h"
#include "platform/time_compat.h"
#include "rpc/httpserver.h"
#include "rpc/server.h"
#include "json/json.h"
#include "util/alerts.h"
#include "util/clientversion.h"
#include "validation/main_state.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* Push a 64 KiB frame filled with 0xCC onto the stack, then return.
 * The frame is freed on return but the bytes persist in memory — any
 * subsequent callee with a smaller combined frame size reuses that
 * region, observing 0xCC where `= {0}` would have given zeros. */
static __attribute__((noinline)) void dirty_stack_region(void)
{
    volatile unsigned char junk[65536];
    for (size_t i = 0; i < sizeof(junk); i++)
        junk[i] = 0xCC;
    /* Force the compiler to materialize the writes. */
    __asm__ volatile("" : : "r"(junk) : "memory");
}

static const struct json_value *find_service(const struct json_value *arr,
                                             const char *name)
{
    if (!arr || arr->type != JSON_ARR || !name)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *svc = json_at(arr, i);
        const struct json_value *n = json_get(svc, "name");
        if (n && strcmp(json_get_str(n), name) == 0)
            return svc;
    }
    return NULL;
}

static const struct json_value *find_source_json(const struct json_value *arr,
                                                 const char *source)
{
    if (!arr || arr->type != JSON_ARR || !source)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *child = json_at(arr, i);
        const struct json_value *name = json_get(child, "source");
        if (name && strcmp(json_get_str(name), source) == 0)
            return child;
    }
    return NULL;
}

static bool syncdiag_touch_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static bool syncdiag_set_coins_applied(sqlite3 *db, int32_t height)
{
    char *err = NULL;

    if (!db)
        return false;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    bool ok = coins_kv_set_applied_height_in_tx(db, height);
    const char *finish = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, finish, NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (err) sqlite3_free(err);
    return ok;
}

static const struct json_value *find_object_with_str(const struct json_value *arr,
                                                     const char *key,
                                                     const char *value)
{
    if (!arr || arr->type != JSON_ARR || !key || !value)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *child = json_at(arr, i);
        const struct json_value *field = json_get(child, key);
        if (field && strcmp(json_get_str(field), value) == 0)
            return child;
    }
    return NULL;
}

static bool json_array_has_str(const struct json_value *arr, const char *value)
{
    if (!arr || arr->type != JSON_ARR || !value)
        return false;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *child = json_at(arr, i);
        if (child && strcmp(json_get_str(child), value) == 0)
            return true;
    }
    return false;
}

static bool json_array_has_substr(const struct json_value *arr,
                                  const char *needle)
{
    if (!arr || arr->type != JSON_ARR || !needle)
        return false;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *child = json_at(arr, i);
        const char *s = json_get_str(child);
        if (s && strstr(s, needle))
            return true;
    }
    return false;
}

static void syncdiag_set_ipv4(struct net_address *addr,
                              uint8_t a, uint8_t b,
                              uint8_t c, uint8_t d,
                              uint16_t port)
{
    net_address_init(addr);
    addr->svc.addr.ip[10] = 0xff;
    addr->svc.addr.ip[11] = 0xff;
    addr->svc.addr.ip[12] = a;
    addr->svc.addr.ip[13] = b;
    addr->svc.addr.ip[14] = c;
    addr->svc.addr.ip[15] = d;
    addr->svc.port = port;
}

static void syncdiag_set_hash(struct uint256 *hash, uint8_t tag)
{
    if (!hash)
        return;
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = tag;
    hash->data[31] = (uint8_t)(0xffu ^ tag);
}

static bool syncdiag_exec_sql(sqlite3 *db, const char *sql)
{
    if (!db || !sql)
        return false;
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK)
        return true;
    if (err)
        sqlite3_free(err);
    return false;
}

static bool syncdiag_seed_cursor(sqlite3 *db, const char *name, int cursor)
{
    sqlite3_stmt *st = NULL;
    if (!db || !name)
        return false;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
            "VALUES(?, ?, 1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool syncdiag_seed_reducer_frontier_at_anchor(sqlite3 *db,
                                                     int32_t anchor)
{
    static const char *const ddl =
        "CREATE TABLE IF NOT EXISTS validate_headers_log ("
        "  height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL,"
        "  fail_reason TEXT, validated_at INTEGER);"
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
        "  block_hash BLOB);"
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "  height INTEGER PRIMARY KEY, source TEXT, ok INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "  height INTEGER PRIMARY KEY, ok INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "  height INTEGER PRIMARY KEY, ok INTEGER NOT NULL,"
        "  spent_count INTEGER, added_count INTEGER);"
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
        "  tip_hash BLOB);";

    return db &&
        syncdiag_exec_sql(db, ddl) &&
        syncdiag_seed_cursor(db, "validate_headers", anchor + 1) &&
        syncdiag_seed_cursor(db, "script_validate", anchor + 1) &&
        syncdiag_seed_cursor(db, "body_persist", anchor + 1) &&
        syncdiag_seed_cursor(db, "proof_validate", anchor + 1) &&
        syncdiag_seed_cursor(db, "utxo_apply", anchor + 1) &&
        syncdiag_seed_cursor(db, "tip_finalize", anchor);
}

static void syncdiag_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void syncdiag_write_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static bool syncdiag_seed_meta_blob(sqlite3 *db, const char *key,
                                    const void *blob, int len)
{
    sqlite3_stmt *st = NULL;
    if (!db || !key || (!blob && len > 0) || len < 0)
        return false;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?1,?2)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, blob, len, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool syncdiag_create_height_log(sqlite3 *db, const char *table)
{
    char sql[160];
    if (!db || !table)
        return false;
    snprintf(sql, sizeof(sql),
             "CREATE TABLE IF NOT EXISTS %s(height INTEGER PRIMARY KEY)",
             table);
    return syncdiag_exec_sql(db, sql);
}

static bool syncdiag_seed_log_point(sqlite3 *db, const char *table,
                                    int64_t height)
{
    char sql[160];
    sqlite3_stmt *st = NULL;
    if (!db || !table)
        return false;
    snprintf(sql, sizeof(sql),
             "INSERT OR REPLACE INTO %s(height) VALUES(?1)", table);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)height);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool syncdiag_seed_anchorstatus_progress(const char *dir)
{
    char path[512];
    sqlite3 *db = NULL;
    if (!dir ||
        snprintf(path, sizeof(path), "%s/progress.kv", dir) >=
            (int)sizeof(path))
        return false;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    bool ok = true;
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS stage_cursor("
        "name TEXT PRIMARY KEY, cursor INTEGER, updated_at INTEGER)");
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS progress_meta("
        "key TEXT PRIMARY KEY, value BLOB)");

    const char *logs[] = {
        "header_admit_log", "validate_headers_log", "body_fetch_log",
        "body_persist_log", "script_validate_log", "proof_validate_log",
        "utxo_apply_log", "tip_finalize_log",
    };
    for (size_t i = 0; ok && i < sizeof(logs) / sizeof(logs[0]); i++)
        ok = syncdiag_create_height_log(db, logs[i]);

    ok = ok && syncdiag_seed_cursor(db, "header_admit", 2791000);
    ok = ok && syncdiag_seed_cursor(db, "validate_headers", 2791000);
    ok = ok && syncdiag_seed_cursor(db, "body_fetch", 2791000);
    ok = ok && syncdiag_seed_cursor(db, "body_persist", 2791000);
    ok = ok && syncdiag_seed_cursor(db, "script_validate", 2791000);
    ok = ok && syncdiag_seed_cursor(db, "proof_validate", 2791000);
    ok = ok && syncdiag_seed_cursor(db, "utxo_apply", 164000);
    ok = ok && syncdiag_seed_cursor(db, "tip_finalize", 164000);

    ok = ok && syncdiag_seed_log_point(db, "header_admit_log", 0);
    ok = ok && syncdiag_seed_log_point(db, "header_admit_log", 3166384);
    ok = ok && syncdiag_seed_log_point(db, "proof_validate_log", 0);
    ok = ok && syncdiag_seed_log_point(db, "proof_validate_log", 2790999);
    ok = ok && syncdiag_seed_log_point(db, "utxo_apply_log", 0);
    ok = ok && syncdiag_seed_log_point(db, "utxo_apply_log", 163999);

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    uint8_t marker[48] = {0};
    memcpy(marker, "ZAM1", 4);
    if (cp) {
        syncdiag_write_le32(marker + 4, (uint32_t)cp->height);
        syncdiag_write_le64(marker + 8, cp->utxo_count);
        memcpy(marker + 16, cp->sha3_hash, 32);
    }
    ok = ok && cp &&
        syncdiag_seed_meta_blob(db, "mint_anchor_in_progress_v1",
                                marker, sizeof(marker));

    uint8_t height_blob[8] = {0};
    syncdiag_write_le64(height_blob, 164000);
    ok = ok && syncdiag_seed_meta_blob(db, "coins_applied_height",
                                       height_blob, sizeof(height_blob));
    const uint8_t one = 1;
    ok = ok && syncdiag_seed_meta_blob(db, "refold_in_progress", &one, 1);

    sqlite3_close(db);
    return ok;
}

static bool syncdiag_seed_log_rows(sqlite3 *db, const char *insert_sql,
                                   int max_height)
{
    sqlite3_stmt *st = NULL;
    if (!db || !insert_sql || max_height < 0)
        return false;
    if (sqlite3_prepare_v2(db, insert_sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h <= max_height; h++) {
        sqlite3_bind_int(st, 1, h);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return false;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);
    return true;
}

static bool syncdiag_seed_lookahead_reducer_progress(int served_height)
{
    sqlite3 *db = progress_store_db();
    if (!db || served_height < 0)
        return false;

    int next_height = served_height + 1;
    bool ok = true;
    progress_store_tx_lock();
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS validate_headers_log ("
        "height INTEGER PRIMARY KEY, hash BLOB, ok INTEGER NOT NULL)");
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "height INTEGER PRIMARY KEY, status TEXT NOT NULL, "
        "ok INTEGER NOT NULL, tx_count INTEGER NOT NULL, "
        "input_count INTEGER NOT NULL, validated_at INTEGER NOT NULL, "
        "block_hash BLOB)");
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "height INTEGER PRIMARY KEY, source TEXT, ok INTEGER NOT NULL)");
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL)");
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "height INTEGER PRIMARY KEY, ok INTEGER NOT NULL, "
        "spent_count INTEGER, added_count INTEGER)");
    ok = ok && syncdiag_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "height INTEGER PRIMARY KEY, status TEXT NOT NULL, "
        "ok INTEGER NOT NULL, work_delta_high INTEGER NOT NULL, "
        "work_delta_low INTEGER NOT NULL, utxo_size_after INTEGER NOT NULL, "
        "reorg_depth INTEGER NOT NULL, finalized_at INTEGER NOT NULL, "
        "tip_hash BLOB)");

    ok = ok && syncdiag_seed_log_rows(db,
        "INSERT OR REPLACE INTO validate_headers_log(height, hash, ok) "
        "VALUES(?, zeroblob(32), 1)",
        served_height);
    ok = ok && syncdiag_seed_log_rows(db,
        "INSERT OR REPLACE INTO script_validate_log"
        "(height, status, ok, tx_count, input_count, validated_at, "
        "block_hash) VALUES(?, 'verified', 1, 1, 0, 1, zeroblob(32))",
        served_height);
    ok = ok && syncdiag_seed_log_rows(db,
        "INSERT OR REPLACE INTO body_persist_log(height, source, ok) "
        "VALUES(?, 'fixture', 1)",
        served_height);
    ok = ok && syncdiag_seed_log_rows(db,
        "INSERT OR REPLACE INTO proof_validate_log(height, status, ok) "
        "VALUES(?, 'verified', 1)",
        served_height);
    ok = ok && syncdiag_seed_log_rows(db,
        "INSERT OR REPLACE INTO utxo_apply_log"
        "(height, ok, spent_count, added_count) VALUES(?, 1, 0, 0)",
        served_height);
    ok = ok && syncdiag_seed_log_rows(db,
        "INSERT OR REPLACE INTO tip_finalize_log"
        "(height, status, ok, work_delta_high, work_delta_low, "
        "utxo_size_after, reorg_depth, finalized_at, tip_hash) "
        "VALUES(?, 'finalized', 1, 0, 0, 0, 0, 1, zeroblob(32))",
        served_height);

    ok = ok && syncdiag_seed_cursor(db, "validate_headers", next_height);
    ok = ok && syncdiag_seed_cursor(db, "script_validate", next_height);
    ok = ok && syncdiag_seed_cursor(db, "body_persist", next_height);
    ok = ok && syncdiag_seed_cursor(db, "proof_validate", next_height);
    ok = ok && syncdiag_seed_cursor(db, "utxo_apply", next_height);
    ok = ok && syncdiag_seed_cursor(db, "tip_finalize", served_height);
    progress_store_tx_unlock();
    return ok;
}

static struct p2p_node *syncdiag_add_peer(struct connman *cm,
                                          uint8_t last_octet,
                                          bool inbound,
                                          enum peer_state state)
{
    struct net_address addr;
    if (!cm)
        return NULL;
    if (!cm->manager.nodes) {
        cm->manager.nodes = zcl_calloc(4, sizeof(*cm->manager.nodes),
                                       "syncdiag_net_nodes");
        cm->manager.nodes_cap = 4;
        if (!cm->manager.nodes)
            return NULL;
    }
    if (cm->manager.num_nodes >= cm->manager.nodes_cap)
        return NULL;
    syncdiag_set_ipv4(&addr, 198, 51, 100, last_octet, 8033);
    struct p2p_node *node = p2p_node_create(
        &cm->manager, ZCL_INVALID_SOCKET, &addr, "syncdiag-net", inbound);
    if (!node)
        return NULL;
    node->state = state;
    node->services = NODE_NETWORK | NODE_ZCL23;
    snprintf(node->sub_ver, sizeof(node->sub_ver),
             "%s", "/ZClassic23:0.1.0/");
    snprintf(node->clean_sub_ver, sizeof(node->clean_sub_ver),
             "%s", node->sub_ver);
    node->starting_height = 3117074;
    cm->manager.nodes[cm->manager.num_nodes++] = node;
    return node;
}

static void syncdiag_note_peer_lifecycle_active(
    const struct p2p_node *node, enum peer_lifecycle_source source)
{
    if (!node)
        return;
    peer_lifecycle_note_connected(node, source);
    peer_lifecycle_note_version_received(node, node->services,
                                         node->starting_height,
                                         node->sub_ver);
    if (node->state == PEER_HANDSHAKE_COMPLETE) {
        peer_lifecycle_note_handshake_complete(node);
        peer_lifecycle_note_active(node);
    }
}

static void syncdiag_reset_rpc_globals_for_test(void)
{
    rpc_net_set_connman(NULL);
    rpc_net_set_boot_context(NULL, NULL);
    msg_version_clear_external_ip_for_test();
    peer_lifecycle_reset_for_test();
}

int test_syncdiag_rpc(void)
{
    int failures = 0;

    syncdiag_reset_rpc_globals_for_test();

    printf("rpc_getsyncdiag: returns valid JSON without abort "
           "(RED)... ");
    {
        dirty_stack_region();

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        rpc_health_set_state(NULL, NULL, NULL, NULL);
        register_health_rpc_commands(&tbl);
        sync_monitor_init();
        sync_monitor_record_snapshot_resnapshot(
            100, 110, 4, 101, 111,
            "block_failed_mask_exhausted",
            "condition:tip_wedged_resnapshot");
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "getsyncdiag",
                                          &params, &result);

        bool ok = executed && result.type == JSON_OBJ;

        const struct json_value *wd  = json_get(&result, "watchdog");
        const struct json_value *hdr = json_get(&result, "headers");
        ok = ok && wd  && wd->type  == JSON_OBJ && wd->num_children  > 0;
        ok = ok && hdr && hdr->type == JSON_OBJ && hdr->num_children > 0;
        ok = ok && json_get(wd, "last_recovery_reason") != NULL;
        ok = ok && json_get(wd, "last_recovery_local_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_peer_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_peer_count") != NULL;
        ok = ok && json_get(wd, "last_recovery_target_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_manifest_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_trigger") != NULL;
        ok = ok && json_get(wd, "recoveries_total") != NULL &&
            json_get_int(json_get(wd, "recoveries_total")) == 1;
        ok = ok && json_get(wd, "last_recovery") != NULL &&
            strcmp(json_get_str(json_get(wd, "last_recovery")),
                   "SNAPSHOT_RESNAPSHOT") == 0;
        ok = ok && strcmp(json_get_str(json_get(
            wd, "last_recovery_reason")),
            "condition:tip_wedged_resnapshot") == 0;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_local_height")) == 100;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_peer_height")) == 110;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_peer_count")) == 4;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_target_height")) == 101;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_manifest_height")) == 111;
        ok = ok && strcmp(json_get_str(json_get(
            wd, "last_recovery_trigger")),
            "block_failed_mask_exhausted") == 0;

        ok = ok && json_get(&result, "sync_state")         != NULL;
        ok = ok && json_get(&result, "chain_height")       != NULL;
        ok = ok && json_get(&result, "best_header_height") != NULL;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("anchorstatus: names anchor mint UTXO backlog blocker "
           "(RED)... ");
    {
        checkpoints_reset_sha3_override_for_test();
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "syncdiag_anchorstatus",
                         "backlog");
        bool ok = syncdiag_seed_anchorstatus_progress(dir);

        struct json_value params;
        json_init(&params);
        json_set_array(&params);
        struct json_value arg;
        json_init(&arg);
        json_set_str(&arg, dir);
        json_push_back(&params, &arg);
        json_free(&arg);

        struct json_value result;
        json_init(&result);
        ok = ok && rpc_agent_anchor_status(&params, false, &result);
        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get(&result, "schema") != NULL &&
            strcmp(json_get_str(json_get(&result, "schema")),
                   "zcl.anchor_mint_status.v1") == 0;
        ok = ok && json_get(&result, "status") != NULL &&
            strcmp(json_get_str(json_get(&result, "status")), "ok") == 0;
        ok = ok && json_get(&result, "summary") != NULL &&
            strcmp(json_get_str(json_get(&result, "summary")),
                   "mint_utxo_apply_far_behind_validated_backlog") == 0;
        ok = ok && json_get(&result, "agent_next_action") != NULL &&
            strcmp(json_get_str(json_get(&result, "agent_next_action")),
                   "inspect_utxo_apply_idle_reason_before_waiting_more") == 0;
        ok = ok && json_get(&result, "mint_marker_present") != NULL &&
            json_get_bool(json_get(&result, "mint_marker_present"));
        ok = ok && json_get(&result, "mint_marker_matches_checkpoint") != NULL &&
            json_get_bool(json_get(&result,
                                   "mint_marker_matches_checkpoint"));
        ok = ok && json_get(&result, "refold_in_progress_present") != NULL &&
            json_get_bool(json_get(&result, "refold_in_progress_present"));
        ok = ok && json_get(&result, "coins_applied_height") != NULL &&
            json_get_int(json_get(&result, "coins_applied_height")) == 164000;
        ok = ok && json_get(&result, "durable_applied_through_height") != NULL &&
            json_get_int(json_get(&result,
                                  "durable_applied_through_height")) == 163999;
        ok = ok && json_get(&result, "validated_backlog_blocks") != NULL &&
            json_get_int(json_get(&result,
                                  "validated_backlog_blocks")) == 2627000;
        ok = ok && json_get(&result, "stale_header_rows_above_anchor") != NULL &&
            json_get_int(json_get(&result,
                                  "stale_header_rows_above_anchor")) == 1;
        ok = ok && json_get(&result, "stale_rows_above_anchor") != NULL &&
            json_get_bool(json_get(&result, "stale_rows_above_anchor"));

        const struct json_value *utxo =
            find_object_with_str(json_get(&result, "stages"),
                                 "name", "utxo_apply");
        ok = ok && utxo != NULL;
        ok = ok && json_get(utxo, "cursor") != NULL &&
            json_get_int(json_get(utxo, "cursor")) == 164000;
        ok = ok && json_get(utxo, "log_max_height") != NULL &&
            json_get_int(json_get(utxo, "log_max_height")) == 163999;

        json_free(&params);
        json_free(&result);
        test_cleanup_tmpdir(dir);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getsyncwatchdog: exposes last recovery context "
           "(RED)... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_health_rpc_commands(&tbl);
        sync_monitor_init();
        sync_monitor_record_snapshot_resnapshot(
            120, 130, 2, 121, 131,
            "local_import_exhausted",
            "condition:tip_wedged_resnapshot");
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool ok = rpc_table_execute(&tbl, "getsyncwatchdog",
                                    &params, &result);

        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get(&result, "enabled") != NULL;
        ok = ok && json_get(&result, "recoveries_total") != NULL &&
            json_get_int(json_get(&result, "recoveries_total")) == 1;
        ok = ok && json_get(&result, "last_recovery") != NULL &&
            strcmp(json_get_str(json_get(&result, "last_recovery")),
                   "SNAPSHOT_RESNAPSHOT") == 0;
        ok = ok && json_get(&result, "last_recovery_time") != NULL &&
            json_get_int(json_get(&result, "last_recovery_time")) > 0;
        ok = ok && json_get(&result, "last_recovery_reason") != NULL &&
            strcmp(json_get_str(json_get(
            &result, "last_recovery_reason")),
            "condition:tip_wedged_resnapshot") == 0;
        ok = ok && json_get(&result, "last_recovery_local_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_local_height")) == 120;
        ok = ok && json_get(&result, "last_recovery_peer_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_peer_height")) == 130;
        ok = ok && json_get(&result, "last_recovery_peer_count") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_peer_count")) == 2;
        ok = ok && json_get(
            &result, "last_recovery_target_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_target_height")) == 121;
        ok = ok && json_get(
            &result, "last_recovery_manifest_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_manifest_height")) == 131;
        ok = ok && json_get(&result, "last_recovery_trigger") != NULL &&
            strcmp(json_get_str(json_get(
            &result, "last_recovery_trigger")),
            "local_import_exhausted") == 0;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_http response envelope: dirty stack still builds JSON "
           "(RED)... ");
    {
        dirty_stack_region();

        struct json_value result;
        json_init(&result);
        json_set_object(&result);
        json_push_kv_str(&result, "watchdog", "ok");

        struct json_value id;
        json_init(&id);
        json_set_int(&id, 1);

        struct json_value response;
        bool ok = rpc_http_test_build_response_envelope(
            true, "getsyncdiag", &result, &id, &response);

        ok = ok && response.type == JSON_OBJ;
        ok = ok && json_get(&response, "result") != NULL;
        ok = ok && json_get(&response, "error") != NULL;
        ok = ok && json_get(&response, "id") != NULL;

        json_free(&result);
        json_free(&id);
        json_free(&response);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getnetworkinfo: reports stable startup reachability schema "
           "(RED)... ");
    {
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;

        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(NULL);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        bool ok = rpc_table_execute(&tbl, "getnetworkinfo",
                                    &params, &result);

        const struct json_value *inbound =
            json_get(&result, "inbound_connections");
        const struct json_value *outbound =
            json_get(&result, "outbound_connections");
        const struct json_value *handshaked =
            json_get(&result, "handshaked_connections");
        const struct json_value *inbound_hs =
            json_get(&result, "inbound_handshaked_connections");
        const struct json_value *outbound_hs =
            json_get(&result, "outbound_handshaked_connections");
        const struct json_value *listen_count =
            json_get(&result, "listen_socket_count");
        const struct json_value *listening =
            json_get(&result, "listening");
        const struct json_value *inbound_seen =
            json_get(&result, "inbound_handshake_seen");
        const struct json_value *remote_seen =
            json_get(&result, "remote_handshake_seen");
        const struct json_value *life =
            json_get(&result, "peer_lifecycle");
        const struct json_value *life_sources =
            life ? json_get(life, "sources") : NULL;
        const struct json_value *addnodes =
            json_get(&result, "addnode_status");

        ok = ok && result.type == JSON_OBJ;
        ok = ok && inbound && json_get_int(inbound) == 0;
        ok = ok && outbound && json_get_int(outbound) == 0;
        ok = ok && handshaked && json_get_int(handshaked) == 0;
        ok = ok && inbound_hs && json_get_int(inbound_hs) == 0;
        ok = ok && outbound_hs && json_get_int(outbound_hs) == 0;
        ok = ok && listen_count && json_get_int(listen_count) == 0;
        ok = ok && listening && !json_get_bool(listening);
        ok = ok && inbound_seen && !json_get_bool(inbound_seen);
        ok = ok && remote_seen && !json_get_bool(remote_seen);
        ok = ok && life && life->type == JSON_OBJ;
        ok = ok && life && json_get(life, "attempted") != NULL;
        ok = ok && life && json_get(life, "connected") != NULL;
        ok = ok && life && json_get(life, "version_sent") != NULL;
        ok = ok && life && json_get(life, "version_received") != NULL;
        ok = ok && life && json_get(life, "verack_received") != NULL;
        ok = ok && life && json_get(life, "handshake_complete") != NULL;
        ok = ok && life && json_get(life, "active") != NULL;
        ok = ok && life && json_get(life, "disconnected") != NULL;
        ok = ok && life && json_get(life, "timeout") != NULL;
        ok = ok && life && json_get(life, "rejected") != NULL;
        ok = ok && life && json_get(life, "cache_skipped") != NULL;
        ok = ok && life && json_get(life, "magicbean_handshakes") != NULL;
        ok = ok && life && json_get(life, "zclassic23_handshakes") != NULL;
        ok = ok && life && json_get(life, "zclassic_c23_handshakes") != NULL;
        ok = ok && life_sources && life_sources->type == JSON_ARR;
        ok = ok && addnodes && addnodes->type == JSON_ARR;
        ok = ok && json_size(addnodes) == 0;
        ok = ok && find_source_json(life_sources, "unknown") != NULL;
        ok = ok && find_source_json(life_sources, "inbound") != NULL;
        ok = ok && find_source_json(life_sources, "addnode") != NULL;
        ok = ok && find_source_json(life_sources, "addrman") != NULL;
        ok = ok && find_source_json(life_sources, "zcl23_db") != NULL;
        ok = ok && find_source_json(life_sources, "manual") != NULL;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("peerincidents: exposes compact duplicate host telemetry "
           "(RED)... ");
    {
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;
        struct p2p_node zigma_a;
        struct p2p_node zigma_b;

        peer_lifecycle_reset_for_test();
        memset(&zigma_a, 0, sizeof(zigma_a));
        syncdiag_set_ipv4(&zigma_a.addr, 40, 160, 53, 56, 45474);
        zigma_a.id = 7701;
        zigma_a.inbound = true;
        zigma_a.state = PEER_HANDSHAKE_COMPLETE;
        zigma_a.services = NODE_NETWORK;
        snprintf(zigma_a.addr_name, sizeof(zigma_a.addr_name),
                 "40.160.53.56:45474");
        snprintf(zigma_a.sub_ver, sizeof(zigma_a.sub_ver),
                 "%s", "/Zigma:0.1.0/");

        memset(&zigma_b, 0, sizeof(zigma_b));
        syncdiag_set_ipv4(&zigma_b.addr, 40, 160, 53, 56, 39030);
        zigma_b.id = 7702;
        zigma_b.inbound = true;
        zigma_b.state = PEER_HANDSHAKE_COMPLETE;
        zigma_b.services = NODE_NETWORK;
        snprintf(zigma_b.addr_name, sizeof(zigma_b.addr_name),
                 "40.160.53.56:39030");
        snprintf(zigma_b.sub_ver, sizeof(zigma_b.sub_ver),
                 "%s", "/Zigma:0.1.0/");

        peer_lifecycle_note_connected(&zigma_a,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_received(&zigma_a, zigma_a.services,
                                             3172229, zigma_a.sub_ver);
        peer_lifecycle_note_handshake_complete(&zigma_a);
        peer_lifecycle_note_active(&zigma_a);

        peer_lifecycle_note_connected(&zigma_b,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_received(&zigma_b, zigma_b.services,
                                             3172230, zigma_b.sub_ver);
        peer_lifecycle_note_handshake_complete(&zigma_b);
        peer_lifecycle_note_active(&zigma_b);

        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        bool ok = rpc_table_execute(&tbl, "peerincidents",
                                    &params, &result);
        const struct json_value *primary =
            json_get(&result, "primary_host_issue");
        const struct json_value *hosts =
            json_get(&result, "duplicate_host_groups");
        const struct json_value *top_hosts =
            json_get(&result, "top_host_incidents");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.peer_incidents.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "method")),
                          "peerincidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "native_command")),
                          "zclassic23 peerincidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "mcp_tool")),
                          "zcl_peer_incidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "contract_source")),
                          "agent_contracts.def") == 0;
        ok = ok && json_get_bool(json_get(&result, "bounded"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "bootstrap_readiness")),
                          "ready") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "fast_sync_readiness")),
                          "no_zclassic23_fast_sync_peer") == 0;
        ok = ok && !json_get_bool(json_get(&result,
                                           "bootstrap_blocked"));
        ok = ok && json_get_bool(json_get(&result,
                                          "fast_sync_blocked"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "incident_severity")),
                          "attention") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "stability_blocker"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "primary_issue_host")),
                          "40.160.53.56") == 0;
        ok = ok && json_get_int(json_get(&result,
                                         "primary_issue_score")) > 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "primary_issue_class")),
                          "duplicate_handshaked_connections") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "primary_issue_next_action")),
                          "inspect_duplicate_current_connections_for_host")
            == 0;
        ok = ok && json_get_int(json_get(&result,
                         "duplicate_host_group_count")) == 1;
        ok = ok && json_get_int(json_get(&result,
                         "duplicate_open_host_group_count")) == 1;
        ok = ok && json_get_int(json_get(&result,
                         "duplicate_handshaked_host_group_count")) == 1;
        ok = ok && primary && primary->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(primary, "host")),
                          "40.160.53.56") == 0;
        ok = ok && json_get_bool(json_get(primary,
                                          "duplicate_current_connections"));
        ok = ok && json_get_bool(json_get(primary,
                                          "bootstrap_useful"));
        ok = ok && hosts && hosts->type == JSON_ARR;
        ok = ok && json_size(hosts) == 1;
        ok = ok && top_hosts && top_hosts->type == JSON_ARR;
        ok = ok && json_size(top_hosts) == 1;

        json_free(&params);
        json_free(&result);
        peer_lifecycle_reset_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("peerincidents: normalizes dumpstate compatibility fallback "
           "(RED)... ");
    {
        struct json_value state;
        struct json_value dumpstate;
        struct json_value result;

        peer_lifecycle_reset_for_test();
        json_init(&state);
        json_init(&dumpstate);
        json_init(&result);
        bool ok = peer_lifecycle_incidents_json(&state);
        json_set_object(&dumpstate);
        json_push_kv_str(&dumpstate, "subsystem", "peer_lifecycle");
        json_push_kv_str(&dumpstate, "description", "fixture");
        json_push_kv(&dumpstate, "state", &state);
        ok = ok && peer_incidents_from_dumpstate_result_json(
            &dumpstate, &result, "target_peerincidents_method_not_found");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.peer_incidents.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "method")),
                          "peerincidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "native_command")),
                          "zclassic23 peerincidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "mcp_tool")),
                          "zcl_peer_incidents") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "compatibility_fallback"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "compatibility_source")),
                          "dumpstate peer_lifecycle incidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "compatibility_reason")),
                          "target_peerincidents_method_not_found") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "fallback_native_command")),
                          "zclassic23 dumpstate peer_lifecycle incidents")
            == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "fallback_mcp_tool")),
                          "zcl_state subsystem=peer_lifecycle key=incidents")
            == 0;

        json_free(&result);
        json_free(&dumpstate);
        json_free(&state);
        peer_lifecycle_reset_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getnetworkinfo: exposes configured external endpoint "
           "(RED)... ");
    {
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;

        msg_version_clear_external_ip_for_test();
        msg_version_set_external_ip("203.0.113.7:8023", 8033);
        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(NULL);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        bool ok = rpc_table_execute(&tbl, "getnetworkinfo",
                                    &params, &result);

        const struct json_value *localaddrs =
            json_get(&result, "localaddresses");
        const struct json_value *first =
            localaddrs && localaddrs->type == JSON_ARR
                ? json_at(localaddrs, 0)
                : NULL;
        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(&result,
                                          "externalip_configured"));
        ok = ok && localaddrs && localaddrs->type == JSON_ARR;
        ok = ok && json_size(localaddrs) == 1;
        ok = ok && first && strcmp(json_get_str(json_get(first, "address")),
                                   "203.0.113.7") == 0;
        ok = ok && first &&
             json_get_int(json_get(first, "port")) == 8023;
        ok = ok && first &&
             json_get_int(json_get(first, "score")) == 1;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "advertised_subver")),
                          msg_version_user_agent()) == 0;

        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        msg_version_clear_external_ip_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("bootstrapstatus: exposes versioned P2P and beta6 "
           "snapshot contract (RED)... ");
    {
        progress_store_close();
        struct connman cm;
        struct node_signals sigs;
        struct rpc_table tbl;
        struct json_value params = {0};
        struct json_value result = {0};
        char tmp_template[] = "/tmp/zcl-bootstrapstatus-XXXXXX";
        char *tmp_dir = mkdtemp(tmp_template);
        char snap_path[512] = {0};
        char index_path[512] = {0};
        int snap_n = tmp_dir ? snprintf(snap_path, sizeof(snap_path),
                                        "%s/utxo-seed-3170000.snapshot",
                                        tmp_dir) : -1;
        int index_n = tmp_dir ? snprintf(index_path, sizeof(index_path),
                                         "%s/block_index.bin", tmp_dir) : -1;

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        bool ok = tmp_dir != NULL &&
                  snap_n > 0 && (size_t)snap_n < sizeof(snap_path) &&
                  index_n > 0 && (size_t)index_n < sizeof(index_path);
        ok = ok && syncdiag_touch_file(snap_path);
        ok = ok && syncdiag_touch_file(index_path);
        ok = ok && connman_init(&cm, chain_params_get(), &sigs);
        if (ok) {
            cm.manager.listen_sockets =
                zcl_calloc(1, sizeof(*cm.manager.listen_sockets),
                           "syncdiag_listen_socket");
            ok = cm.manager.listen_sockets != NULL;
        }
        if (ok) {
            cm.manager.listen_sockets[0].socket = ZCL_INVALID_SOCKET;
            cm.manager.num_listen_sockets = 1;
            cm.manager.listen_sockets_cap = 1;
        }
        reducer_frontier_provable_tip_set(3170000);
        msg_version_clear_external_ip_for_test();
        msg_version_set_external_ip("203.0.113.7:8033", 8033);

        if (ok) {
            struct net_address addr;
            struct net_addr src;
            syncdiag_set_ipv4(&addr, 8, 8, 8, 8, 8033);
            addr.nServices = NODE_NETWORK;
            net_addr_init(&src);
            unsigned char src_ip[4] = {1, 2, 3, 4};
            net_addr_set_ipv4(&src, src_ip);
            ok = addrman_add(&cm.manager.addrman, &addr, &src, 0);
        }

        struct p2p_node *zcl_a = ok
            ? syncdiag_add_peer(&cm, 21, false, PEER_HANDSHAKE_COMPLETE)
            : NULL;
        struct p2p_node *zcl_b = ok
            ? syncdiag_add_peer(&cm, 22, true, PEER_HANDSHAKE_COMPLETE)
            : NULL;
        struct p2p_node *legacy_peer = ok
            ? syncdiag_add_peer(&cm, 23, false, PEER_HANDSHAKE_COMPLETE)
            : NULL;
        ok = ok && zcl_a && zcl_b && legacy_peer;
        if (ok) {
            snprintf(zcl_a->addr_name, sizeof(zcl_a->addr_name),
                     "198.51.100.21:8033");
            zcl_a->starting_height = 3170000;
            syncdiag_note_peer_lifecycle_active(
                zcl_a, PEER_LIFECYCLE_SOURCE_ADDNODE);

            snprintf(zcl_b->addr_name, sizeof(zcl_b->addr_name),
                     "198.51.100.22:8033");
            zcl_b->starting_height = 3169999;
            syncdiag_note_peer_lifecycle_active(
                zcl_b, PEER_LIFECYCLE_SOURCE_INBOUND);

            legacy_peer->services = NODE_NETWORK;
            snprintf(legacy_peer->addr_name,
                     sizeof(legacy_peer->addr_name),
                     "198.51.100.23:8033");
            snprintf(legacy_peer->sub_ver, sizeof(legacy_peer->sub_ver),
                     "%s", "/MagicBean:2.1.2-beta6/");
            snprintf(legacy_peer->clean_sub_ver,
                     sizeof(legacy_peer->clean_sub_ver),
                     "%s", legacy_peer->sub_ver);
            legacy_peer->starting_height = 3170000;
            syncdiag_note_peer_lifecycle_active(
                legacy_peer, PEER_LIFECYCLE_SOURCE_ADDRMAN);
        }

        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(&cm);
        rpc_net_set_boot_context(tmp_dir, snap_path);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "bootstrapstatus",
                                     &params, &result);

        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.bootstrap_status.v1") == 0;
        ok = ok && json_get_int(json_get(&result,
                                          "schema_version")) == 1;
        ok = ok && json_get_bool(json_get(&result,
                                          "serving_p2p_bootstrap"));
        ok = ok && json_get_bool(json_get(&result,
                                          "serving_addr_bootstrap"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "serving_snapshot_bootstrap"));
        ok = ok && strcmp(json_get_str(json_get(&result, "readiness")),
                          "ready_p2p_and_addr") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
            "fresh_node_next_action")),
                          "connect_direct_p2p_and_request_headers_blocks") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "zclassic23_fast_sync_compatible"));
        ok = ok && json_get_bool(json_get(&result,
            "zclassicd_beta6_p2p_compatible"));
        ok = ok && !json_get_bool(json_get(&result,
            "zclassicd_beta6_fast_bootstrap_compatible"));

        const struct json_value *p2p = json_get(&result, "p2p");
        ok = ok && p2p && p2p->type == JSON_OBJ;
        ok = ok && json_get_int(json_get(p2p, "protocolversion")) ==
                  PROTOCOL_VERSION;
        ok = ok && json_get_int(json_get(p2p,
                                          "minimum_peer_protocol")) ==
                  MIN_PEER_PROTO_VERSION;
        ok = ok && json_get_bool(json_get(p2p, "node_network"));
        ok = ok && json_get_bool(json_get(p2p, "node_zclassic23"));
        ok = ok && !json_get_bool(json_get(p2p, "node_bootstrap"));
        ok = ok && json_get_int(json_get(p2p,
                                          "advertised_start_height")) ==
                  3170000;

        const struct json_value *peers = json_get(&result, "peers");
        const struct json_value *verified =
            peers ? json_get(peers,
                "verified_zclassic23_bootstrap_peers") : NULL;
        const struct json_value *first_verified =
            verified && json_size(verified) > 0 ? json_at(verified, 0) : NULL;
        ok = ok && peers && peers->type == JSON_OBJ;
        ok = ok && json_get_int(json_get(peers, "connections")) == 3;
        ok = ok && json_get_int(json_get(peers,
            "zclassic23_peers")) == 2;
        ok = ok && json_get_int(json_get(peers,
            "legacy_compatible_peers")) == 1;
        ok = ok && json_get_int(json_get(peers,
            "verified_zclassic23_bootstrap_peer_count")) == 2;
        ok = ok && json_get_int(json_get(peers,
            "fast_sync_useful_zclassic23_peer_count")) == 2;
        ok = ok && json_get_int(json_get(peers,
            "zclassic23_bootstrap_quorum_target")) == 2;
        ok = ok && json_get_bool(json_get(peers,
            "zclassic23_bootstrap_quorum_met"));
        ok = ok && json_get_bool(json_get(peers,
            "zclassic23_fast_sync_quorum_met"));
        ok = ok && strcmp(json_get_str(json_get(peers,
            "zclassic23_bootstrap_quorum_status")),
                          "redundant") == 0;
        ok = ok && verified && verified->type == JSON_ARR;
        ok = ok && json_size(verified) == 2;
        ok = ok && first_verified && first_verified->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(first_verified,
            "verified_by")), "live_handshake") == 0;
        ok = ok && json_get_bool(json_get(first_verified,
            "bootstrap_useful"));
        ok = ok && json_get_bool(json_get(first_verified,
            "fast_sync_useful"));
        ok = ok && strcmp(json_get_str(json_get(first_verified,
            "bootstrap_readiness")), "useful") == 0;

        const struct json_value *addrman = json_get(&result, "addrman");
        ok = ok && addrman && addrman->type == JSON_OBJ;
        ok = ok && json_get_int(json_get(addrman, "entries")) == 1;
        ok = ok && json_get_bool(json_get(addrman,
                                          "addr_relay_ready"));

        const struct json_value *zcl23 =
            json_get(&result, "zclassic23_bootstrap");
        ok = ok && zcl23 && zcl23->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(zcl23, "schema")),
                          "zcl.bootstrap.zclassic23.v1") == 0;
        ok = ok && json_get_bool(json_get(zcl23, "serving"));
        ok = ok && json_get_bool(json_get(zcl23,
            "preferred_for_fresh_zclassic23"));
        ok = ok && json_get_bool(json_get(zcl23, "full_node_bootstrap"));
        ok = ok && json_get_bool(json_get(zcl23, "addr_relay_ready"));
        ok = ok && strcmp(json_get_str(json_get(zcl23,
            "route_preference")),
                          "direct_p2p_then_znam_onion_fallback") == 0;
        ok = ok && strcmp(json_get_str(json_get(zcl23,
            "endpoint_record_schema")),
                          "zcl.names.service_record.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(zcl23,
            "clearnet_address")), "203.0.113.7") == 0;
        ok = ok && json_get_int(json_get(zcl23, "p2p_port")) == 8033;
        ok = ok && json_array_has_str(json_get(zcl23,
            "fresh_node_flow"), "fallback_to_onion_endpoint");

        const struct json_value *loader =
            json_get(&result, "snapshot_loader");
        ok = ok && loader && loader->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(loader, "schema")),
                          "zcl.snapshot_loader.v1") == 0;
        ok = ok && json_get_int(json_get(loader, "schema_version")) == 1;
        ok = ok && json_get_bool(json_get(loader, "bundle_present"));
        ok = ok && json_get_int(json_get(loader,
                                         "bundle_seed_height")) == 3170000;
        ok = ok && strcmp(json_get_str(json_get(loader, "bundle_path")),
                          snap_path) == 0;
        ok = ok && json_get_bool(json_get(loader,
                                          "block_index_present"));
        ok = ok && json_get_bool(json_get(loader, "bootable_bundle"));
        ok = ok && json_get_bool(json_get(loader,
            "active_loader_configured"));
        ok = ok && strcmp(json_get_str(json_get(loader,
            "active_loader_path")), snap_path) == 0;
        ok = ok && json_get_bool(json_get(loader,
            "active_loader_matches_bundle"));
        ok = ok && strcmp(json_get_str(json_get(loader,
            "recovery_hint")), "loader_active") == 0;
        const struct json_value *authority = json_get(loader, "authority");
        ok = ok && authority && authority->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(authority, "schema")),
                          "zcl.snapshot_loader_authority.v1") == 0;
        ok = ok && !json_get_bool(json_get(authority,
                                           "progress_store_open"));
        ok = ok && !json_get_bool(json_get(authority,
                                           "coins_kv_proven_authority"));
        ok = ok && !json_get_bool(json_get(authority,
                                           "fast_rebuild_authority_ready"));
        ok = ok && strcmp(json_get_str(json_get(authority,
            "authority_posture")), "unknown_no_progress_store") == 0;

        const struct json_value *legacy =
            json_get(&result, "legacy_p2p_bootstrap");
        ok = ok && legacy && legacy->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(legacy, "serving"));
        ok = ok && json_array_has_str(json_get(legacy, "messages"),
                                      "getheaders");
        ok = ok && json_array_has_str(json_get(legacy, "messages"),
                                      "getaddr");

        const struct json_value *beta6 =
            json_get(&result, "beta6_snapshot_bootstrap");
        ok = ok && beta6 && beta6->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(beta6,
                                                "required_service_bit")),
                          "NODE_BOOTSTRAP") == 0;
        ok = ok && json_get_int(json_get(beta6,
                                          "required_service_bit_value")) ==
                  NODE_BOOTSTRAP;
        ok = ok && !json_get_bool(json_get(beta6, "advertised"));
        ok = ok && !json_get_bool(json_get(beta6, "serving"));
        ok = ok && json_array_has_str(json_get(beta6, "messages"),
                                      "getbsman");
        ok = ok && json_array_has_str(json_get(beta6, "messages"),
                                      "getbschk");

        ok = ok && json_array_has_str(json_get(&result, "blockers"),
                                      "beta6_NODE_BOOTSTRAP_not_advertised");

        ok = ok && unlink(index_path) == 0;
        rpc_net_set_boot_context(tmp_dir, NULL);
        json_free(&result);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "bootstrapstatus",
                                     &params, &result);
        loader = json_get(&result, "snapshot_loader");
        ok = ok && loader && loader->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(loader, "bundle_present"));
        ok = ok && !json_get_bool(json_get(loader,
                                           "block_index_present"));
        ok = ok && !json_get_bool(json_get(loader, "bootable_bundle"));
        ok = ok && !json_get_bool(json_get(loader,
            "active_loader_configured"));
        ok = ok && strcmp(json_get_str(json_get(loader,
            "recovery_hint")), "install_tip_seed_snapshot") == 0;

        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        rpc_net_set_boot_context(NULL, NULL);
        msg_version_clear_external_ip_for_test();
        reducer_frontier_provable_tip_reset();
        connman_free(&cm);
        if (tmp_dir) {
            unlink(snap_path);
            unlink(index_path);
            rmdir(tmp_dir);
        }

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("bootstrapstatus: exposes snapshot authority posture (RED)... ");
    {
        test_reset_shared_globals();
        progress_store_close();
        chain_params_select(CHAIN_MAIN);

        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "syncdiag", "bootstrap_authority");

        struct rpc_table tbl;
        struct json_value params = {0};
        struct json_value result = {0};
        sqlite3 *pdb = NULL;
        uint8_t txid[32] = {0};
        const uint8_t one = 0x01;
        bool ok = progress_store_open(dir);
        if (ok)
            pdb = progress_store_db();
        if (ok) {
            memset(txid, 0xB7, sizeof(txid));
            ok = pdb &&
                 coins_kv_ensure_schema(pdb) &&
                 syncdiag_seed_reducer_frontier_at_anchor(
                     pdb, REDUCER_FRONTIER_TRUSTED_ANCHOR) &&
                 coins_kv_add(pdb, txid, 0, 5000000000LL,
                              REDUCER_FRONTIER_TRUSTED_ANCHOR, true,
                              NULL, 0) &&
                 syncdiag_set_coins_applied(
                     pdb, REDUCER_FRONTIER_TRUSTED_ANCHOR + 1) &&
                 progress_meta_set(pdb, COINS_KV_MIGRATION_COMPLETE_KEY,
                                   &one, sizeof(one));
        }

        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(NULL);
        rpc_net_set_boot_context(dir, NULL);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "bootstrapstatus",
                                     &params, &result);

        const struct json_value *loader =
            json_get(&result, "snapshot_loader");
        const struct json_value *authority =
            loader ? json_get(loader, "authority") : NULL;
        ok = ok && authority && authority->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(authority,
                                          "progress_store_open"));
        ok = ok && json_get_bool(json_get(authority,
                                          "hstar_available"));
        ok = ok && json_get_int(json_get(authority, "hstar")) ==
                  REDUCER_FRONTIER_TRUSTED_ANCHOR;
        ok = ok && json_get_bool(json_get(authority,
                                          "coins_applied_height_readable"));
        ok = ok && json_get_bool(json_get(authority,
                                          "coins_applied_height_present"));
        ok = ok && json_get_int(json_get(authority,
                                         "coins_applied_height")) ==
                  REDUCER_FRONTIER_TRUSTED_ANCHOR + 1;
        ok = ok && json_get_bool(json_get(authority,
                                          "coins_kv_proven_authority"));
        ok = ok && json_get_bool(json_get(authority,
                                          "coins_cover_hstar"));
        ok = ok && json_get_bool(json_get(authority,
                                          "fast_rebuild_authority_ready"));
        ok = ok && !json_get_bool(json_get(authority,
                                           "self_folded_marker"));
        ok = ok && !json_get_bool(json_get(authority,
            "self_derived_tip_static_checks"));
        ok = ok && strcmp(json_get_str(json_get(authority,
            "self_derived_reason")), "borrowed_seed_no_refold_marker") == 0;
        ok = ok && strcmp(json_get_str(json_get(authority,
            "authority_posture")), "proven_but_not_self_folded") == 0;

        json_free(&result);
        json_init(&result);
        ok = ok && coins_kv_mark_self_folded(pdb);
        ok = ok && rpc_table_execute(&tbl, "bootstrapstatus",
                                     &params, &result);
        loader = json_get(&result, "snapshot_loader");
        authority = loader ? json_get(loader, "authority") : NULL;
        ok = ok && authority && authority->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(authority,
                                          "self_folded_marker"));
        ok = ok && json_get_bool(json_get(authority,
            "self_derived_tip_static_checks"));
        ok = ok && strcmp(json_get_str(json_get(authority,
            "self_derived_reason")), "ok") == 0;
        ok = ok && strcmp(json_get_str(json_get(authority,
            "authority_posture")), "self_folded_marker_present") == 0;

        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        rpc_net_set_boot_context(NULL, NULL);
        progress_store_close();
        test_cleanup_tmpdir(dir);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getnetworkinfo: separates inbound reachability from outbound "
           "handshakes (RED)... ");
    {
        struct connman cm;
        struct node_signals sigs;
        struct rpc_table tbl;
        struct json_value params = {0};
        struct json_value result = {0};

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, chain_params_get(), &sigs);
        ok = ok && syncdiag_add_peer(&cm, 11, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        ok = ok && syncdiag_add_peer(&cm, 12, true,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        if (!ok)
            goto syncdiag_net_split_done;
        if (ok) {
            struct net_address addr;
            struct net_service svc;
            net_address_init(&addr);
            ok = lookup_numeric("51.178.179.75:8033", &svc,
                                cm.manager.default_port);
            if (ok) {
                addr.svc = svc;
                cm.addnodes[cm.num_addnodes++] = addr;
                connman_record_addnode_failure(&cm, 0,
                                               CONNMAN_ADDNODE_FAILURE_TCP);
            }
        }

        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(&cm);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getnetworkinfo",
                                     &params, &result);

        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get_int(json_get(&result, "handshaked_connections"))
                  == 2;
        ok = ok && json_get_int(json_get(&result,
                                          "inbound_handshaked_connections"))
                  == 1;
        ok = ok && json_get_int(json_get(&result,
                                          "outbound_handshaked_connections"))
                  == 1;
        ok = ok && json_get_bool(json_get(&result,
                                          "inbound_handshake_seen"));
        ok = ok && json_get_bool(json_get(&result,
                                          "remote_handshake_seen"));
        ok = ok && json_get_int(json_get(&result,
                                          "legacy_compatible_peers")) ==
                  json_get_int(json_get(&result, "magicbean_peers"));
        ok = ok && json_get_int(json_get(&result,
                                          "legacy_magicbean_peers")) ==
                  json_get_int(json_get(&result, "magicbean_peers"));
        ok = ok && json_get_int(json_get(&result, "zclassic23_peers")) ==
                  json_get_int(json_get(&result, "zclassic_c23_peers"));
        const struct json_value *addnodes =
            json_get(&result, "addnode_status");
        const struct json_value *first =
            addnodes && addnodes->type == JSON_ARR ? json_at(addnodes, 0)
                                                   : NULL;
        ok = ok && addnodes && addnodes->type == JSON_ARR;
        ok = ok && json_size(addnodes) == 1;
        ok = ok && first && json_get(first, "address") != NULL;
        ok = ok && first && json_get_int(json_get(first, "index")) == 0;
        ok = ok && first && !json_get_bool(json_get(first, "connected"));
        ok = ok && first &&
             json_get_int(json_get(first, "backoff_seconds")) > 0;
        ok = ok && first &&
             json_get_int(json_get(first, "backoff_remaining_seconds")) >= 0;
        ok = ok && first &&
             json_get_int(json_get(first, "tcp_failures")) == 1;
        ok = ok && first &&
             json_get_int(json_get(first, "protocol_failures")) == 0;
        if (!ok)
            goto syncdiag_net_split_done;

        json_free(&params);
        json_init(&params);
        json_set_array(&params);
        struct json_value v;
        json_init(&v);
        json_set_str(&v, "51.178.179.75:8033");
        ok = ok && json_push_back(&params, &v);
        json_free(&v);
        json_init(&v);
        json_set_str(&v, "remove");
        ok = ok && json_push_back(&params, &v);
        json_free(&v);

        json_free(&result);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "addnode", &params, &result);
        if (!ok)
            goto syncdiag_net_split_done;

        json_free(&params);
        json_init(&params);
        json_set_array(&params);
        json_free(&result);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getnetworkinfo",
                                     &params, &result);
        addnodes = json_get(&result, "addnode_status");
        ok = ok && addnodes && addnodes->type == JSON_ARR;
        ok = ok && json_size(addnodes) == 0;
        if (!ok)
            goto syncdiag_net_split_done;

        json_free(&params);
        json_init(&params);
        json_set_array(&params);
        json_init(&v);
        json_set_str(&v, "51.178.179.75:8033");
        ok = ok && json_push_back(&params, &v);
        json_free(&v);
        json_init(&v);
        json_set_str(&v, "remove");
        ok = ok && json_push_back(&params, &v);
        json_free(&v);
        json_free(&result);
        json_init(&result);
        ok = ok && !rpc_table_execute(&tbl, "addnode", &params, &result);
        ok = ok && strstr(json_get_str(&result), "not found") != NULL;
        if (!ok)
            goto syncdiag_net_split_done;

        json_free(&params);
        json_init(&params);
        json_set_array(&params);
        json_init(&v);
        json_set_str(&v, "51.178.179.75:8033");
        ok = ok && json_push_back(&params, &v);
        json_free(&v);
        json_init(&v);
        json_set_str(&v, "bogus");
        ok = ok && json_push_back(&params, &v);
        json_free(&v);
        json_free(&result);
        json_init(&result);
        ok = ok && !rpc_table_execute(&tbl, "addnode", &params, &result);
        ok = ok && strstr(json_get_str(&result), "must be") != NULL;
        if (!ok)
            goto syncdiag_net_split_done;

        json_free(&params);
        json_init(&params);
        json_set_array(&params);
        json_free(&result);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getpeerinfo",
                                     &params, &result);
        const struct json_value *peer0 =
            result.type == JSON_ARR ? json_at(&result, 0) : NULL;
        ok = ok && peer0 && json_get_bool(json_get(peer0, "zclassic23"));
        ok = ok && peer0 && json_get_bool(json_get(peer0, "zclassic_c23"));

syncdiag_net_split_done:
        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        connman_free(&cm);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getservicehealth: exposes chain advance coordinator "
           "(RED)... ");
    {
        struct json_value result;
        json_init(&result);
        block_source_policy_reset_for_test();
        condition_engine_reset_for_testing();
        mirror_consensus_reset_for_test();
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_blocker("body-hash-mismatch");
        bool seeded = block_source_policy_snapshot_offer_allowed(
            100, 10000, 10100, true, "manifest_ok", NULL);
        bool ok = seeded && api_getservicehealth(&result);
        const struct json_value *svc =
            find_service(&result, "chain_advance_coordinator");
        ok = ok && result.type == JSON_ARR && svc != NULL;
        ok = ok && json_get(svc, "authority") != NULL;
        ok = ok && json_get(svc, "decision") != NULL;
        ok = ok && json_get(svc, "selected_source") != NULL;
        ok = ok && json_get(svc, "selected_source_trust") != NULL;
        ok = ok && json_get(svc, "mirror_fallback_allowed") != NULL;
        ok = ok && json_get(svc, "activation_allowed") != NULL;
        ok = ok && json_get(svc, "best_header_height") != NULL;
        ok = ok && json_get(svc, "projection_height") != NULL;
        ok = ok && json_get(svc, "projection_lag") != NULL;
        ok = ok && json_get(svc, "projection_deferred") != NULL;
        ok = ok && json_get(svc, "projection_state") != NULL;
        ok = ok && json_get(svc, "projection_deferred_total") != NULL;
        ok = ok && json_get(svc, "last_projection_deferred_height") != NULL;
        ok = ok && json_get(svc, "last_projection_deferred_time") != NULL;
        ok = ok && json_get(svc, "last_projection_deferred_reason") != NULL;
        ok = ok && json_get(svc, "reason") != NULL;
        ok = ok && json_get(svc, "initialized") != NULL;
        ok = ok && json_get(svc, "has_connman") != NULL;
        ok = ok && json_get(svc, "has_main_state") != NULL;
        ok = ok && json_get(svc, "has_node_db") != NULL;
        const struct json_value *sources =
            svc ? json_get(svc, "sources") : NULL;
        const struct json_value *current_snapshot =
            find_source_json(sources, "snapshot");
        ok = ok && sources && sources->type == JSON_ARR;
        ok = ok && current_snapshot && current_snapshot->type == JSON_OBJ;
        ok = ok && json_get(current_snapshot, "selectable") != NULL;
        ok = ok && json_get(current_snapshot, "selection_blocker") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_target_lag_penalty") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_failure_penalty") != NULL;
        const struct json_value *has_last =
            svc ? json_get(svc, "has_last_decision") : NULL;
        const struct json_value *last =
            svc ? json_get(svc, "last_decision") : NULL;
        ok = ok && has_last && json_get_bool(has_last);
        ok = ok && last && last->type == JSON_OBJ;
        ok = ok && json_get(last, "op") != NULL &&
            strcmp(json_get_str(json_get(last, "op")),
                   "snapshot_offer") == 0;
        ok = ok && json_get(last, "selected_source_reason") != NULL &&
            strcmp(json_get_str(json_get(last, "selected_source_reason")),
                   "manifest_ok") == 0;
        ok = ok && json_get(last, "selected_source_selectable") != NULL;
        ok = ok && json_get_bool(json_get(
            last, "selected_source_selectable"));
        ok = ok && json_get(last,
                            "selected_source_selection_blocker") != NULL;
        ok = ok && strcmp(json_get_str(json_get(
            last, "selected_source_selection_blocker")), "") == 0;
        ok = ok && json_get(last, "selected_source_score_base") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_base")) == 85;
        ok = ok && json_get(last,
                            "selected_source_score_target_lag_penalty")
                 != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_target_lag_penalty")) == 25;
        ok = ok && json_get(last,
                            "selected_source_score_failure_penalty") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_failure_penalty")) == 0;
        const struct json_value *last_sources = json_get(last, "sources");
        const struct json_value *snapshot =
            find_source_json(last_sources, "snapshot");
        ok = ok && last_sources && last_sources->type == JSON_ARR;
        ok = ok && snapshot && snapshot->type == JSON_OBJ;
        ok = ok && json_get(snapshot, "trust") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "trust")),
                   "native_snapshot_proof_validated") == 0;
        ok = ok && json_get(snapshot, "reason") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "reason")),
                   "manifest_ok") == 0;

        json_free(&result);
        block_source_policy_reset_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getservicehealth: exposes canonical mirror trust "
           "(RED)... ");
    {
        struct legacy_mirror_sync_stats stats;
        struct json_value result;

        sync_monitor_init();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_override(200, "body-hash-mismatch");
        mirror_consensus_record_blocker("body-hash-mismatch");

        memset(&stats, 0, sizeof(stats));
        stats.enabled = true;
        stats.running = true;
        stats.reachable = true;
        stats.legacy_height = 200;
        stats.local_height = 199;
        stats.target_height = 200;
        stats.stalls_total = 3;
        snprintf(stats.last_blocker_id, sizeof(stats.last_blocker_id),
                 "%s", "body-hash-mismatch");
        legacy_mirror_sync_test_set_stats(&stats, NULL);

        json_init(&result);
        bool ok = api_getservicehealth(&result);
        const struct json_value *svc = find_service(&result, "legacy_mirror");
        ok = ok && result.type == JSON_ARR && svc != NULL;
        ok = ok && json_get(svc, "state") != NULL &&
            strcmp(json_get_str(json_get(svc, "state")), "blocked") == 0;
        ok = ok && json_get(svc, "consensus_authority") != NULL &&
            strcmp(json_get_str(json_get(svc, "consensus_authority")),
                   "local_consensus_validation") == 0;
        ok = ok && json_get(svc, "candidate_source") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_source")),
                   "legacy_advisory") == 0;
        ok = ok && json_get(svc, "mirror_authorization_enabled") == NULL;
        ok = ok && json_get(svc, "mirror_source_trust") == NULL;
        ok = ok && json_get(svc, "candidate_trust") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_trust")),
                   "bounded_advisory_fallback") == 0;
        ok = ok && json_get(svc, "candidate_lag_observed") != NULL &&
            json_is_null(json_get(svc, "candidate_lag_observed"));
        ok = ok && json_get(svc, "candidate_lag") != NULL &&
            json_get_int(json_get(svc, "candidate_lag")) == 0;
        ok = ok && json_get(svc, "tip_hashes_agree") != NULL &&
            !json_get_bool(json_get(svc, "tip_hashes_agree"));
        ok = ok && json_get(svc, "blocker_recovered_by_tip_agreement") !=
                 NULL &&
            !json_get_bool(json_get(
                svc, "blocker_recovered_by_tip_agreement"));
        ok = ok && json_get(svc, "mirror_monitor_running") != NULL &&
            json_get_bool(json_get(svc, "mirror_monitor_running"));
        ok = ok && json_get(svc, "zclassicd_rpc_transport_reachable") != NULL &&
            json_get_bool(json_get(svc, "zclassicd_rpc_transport_reachable"));
        ok = ok && json_get(svc, "legacy_oracle_usable") != NULL &&
            json_get_bool(json_get(svc, "legacy_oracle_usable"));
        ok = ok && json_get(svc, "zclassicd_rpc_error_code") != NULL &&
            json_get_int(json_get(svc, "zclassicd_rpc_error_code")) == 0;
        ok = ok && json_get(svc, "zclassicd_rpc_error_message") != NULL &&
            strcmp(json_get_str(json_get(svc,
                                         "zclassicd_rpc_error_message")),
                   "") == 0;
        ok = ok && json_get(svc, "candidate_blocker") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc, "candidate_blocker_scope") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_blocker_scope")),
                   "advisory_source") == 0;
        ok = ok && json_get(svc, "activation_blocker") != NULL &&
            strcmp(json_get_str(json_get(svc, "activation_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc, "last_blocker_code") != NULL &&
            strcmp(json_get_str(json_get(svc, "last_blocker_code")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc,
                             "legacy_advisory_gated_by_native_retries") != NULL;
        ok = ok && json_get(svc,
                             "mirror_repair_gated_by_local_retries") != NULL;
        ok = ok && json_get(svc, "local_retries_exhausted") != NULL;
        ok = ok && json_get(svc, "overrides_total") != NULL;
        ok = ok && json_get(svc, "unsafe_overrides_total") != NULL &&
            json_get_int(json_get(svc, "unsafe_overrides_total")) == 1;
        ok = ok && json_get(svc, "last_override_safe") != NULL &&
            !json_get_bool(json_get(svc, "last_override_safe"));
        ok = ok && json_get(svc, "last_override_reason") != NULL &&
            strcmp(json_get_str(json_get(svc, "last_override_reason")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc, "last_override_scope") != NULL &&
            strcmp(json_get_str(json_get(svc, "last_override_scope")),
                   "unsafe_no_authorized_scope") == 0;
        ok = ok && json_get(svc, "blockers_total") != NULL &&
            json_get_int(json_get(svc, "blockers_total")) == 1;
        ok = ok && json_get(svc, "stalls_total") != NULL &&
            json_get_int(json_get(svc, "stalls_total")) == 3;
        ok = ok && json_get(svc, "lag_observed") != NULL &&
            json_is_null(json_get(svc, "lag_observed"));
        ok = ok && json_get(svc, "lag") != NULL &&
            json_get_int(json_get(svc, "lag")) == 0;

        json_free(&result);
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("healthcheck: exposes chain advance decision "
           "(RED)... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_object(&params);
        json_push_kv_bool(&params, "full", true);

        struct json_value result;
        json_init(&result);

        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        struct legacy_mirror_sync_stats stats;
        memset(&stats, 0, sizeof(stats));
        stats.enabled = true;
        stats.running = true;
        stats.reachable = false;
        stats.legacy_height = 0;
        stats.local_height = 3157703;
        snprintf(stats.last_blocker_id, sizeof(stats.last_blocker_id),
                 "%s", "rpc-unreachable");
        snprintf(stats.last_error, sizeof(stats.last_error),
                 "%s", "connect failed");
        legacy_mirror_sync_test_set_stats(&stats, NULL);
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_override(100, "body-hash-mismatch");
        mirror_consensus_record_blocker("body-hash-mismatch");
        bool seeded = block_source_policy_snapshot_offer_allowed(
            100, 10000, 10100, true, "manifest_ok", NULL);
        bool executed = rpc_table_execute(&tbl, "healthcheck",
                                          &params, &result);

        const struct json_value *checks = json_get(&result, "checks");
        const struct json_value *ca =
            checks ? json_get(checks, "chain_advance") : NULL;
        const struct json_value *ce =
            checks ? json_get(checks, "chain_evidence") : NULL;
        const struct json_value *condition_engine =
            checks ? json_get(checks, "condition_engine") : NULL;
        bool ok = seeded && executed && result.type == JSON_OBJ;
        ok = ok && json_get(&result, "build_commit") != NULL &&
            strcmp(json_get_str(json_get(&result, "build_commit")),
                   zcl_build_commit()) == 0;
        ok = ok && checks && checks->type == JSON_OBJ;
        ok = ok && json_get(checks, "error_total") != NULL;
        ok = ok && json_get(checks, "last_error_age_seconds") != NULL;
        ok = ok && json_get(checks, "last_error_recent") != NULL;
        ok = ok && json_get(&result, "candidate_lag_known") != NULL &&
            !json_get_bool(json_get(&result, "candidate_lag_known"));
        ok = ok && json_get(&result, "candidate_lag_valid") != NULL &&
            !json_get_bool(json_get(&result, "candidate_lag_valid"));
        ok = ok && json_get(&result, "mirror_tip_hashes_agree") != NULL &&
            !json_get_bool(json_get(&result, "mirror_tip_hashes_agree"));
        ok = ok && json_get(&result,
                            "mirror_blocker_recovered_by_tip_agreement")
                 != NULL &&
            !json_get_bool(json_get(
                &result, "mirror_blocker_recovered_by_tip_agreement"));
        ok = ok && json_get(&result, "candidate_lag") != NULL &&
            json_get_int(json_get(&result, "candidate_lag")) == 0;
        ok = ok && json_get(&result, "candidate_lag_observed") != NULL &&
            json_is_null(json_get(&result, "candidate_lag_observed"));
        ok = ok && json_get(&result, "mirror_lag") != NULL &&
            json_get_int(json_get(&result, "mirror_lag")) == 0;
        ok = ok && json_get(&result, "mirror_lag_observed") != NULL &&
            json_is_null(json_get(&result, "mirror_lag_observed"));
        ok = ok && json_get(&result, "mirror_monitor_running") != NULL &&
            json_get_bool(json_get(&result, "mirror_monitor_running"));
        ok = ok && json_get(&result,
                            "zclassicd_rpc_transport_reachable") != NULL &&
            !json_get_bool(json_get(&result,
                                    "zclassicd_rpc_transport_reachable"));
        ok = ok && json_get(&result, "legacy_oracle_usable") != NULL &&
            !json_get_bool(json_get(&result, "legacy_oracle_usable"));
        ok = ok && json_get(&result, "zclassicd_rpc_error_code") != NULL &&
            json_get_int(json_get(&result, "zclassicd_rpc_error_code")) == 0;
        ok = ok && json_get(&result,
                            "zclassicd_rpc_error_message") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "zclassicd_rpc_error_message")),
                   "connect failed") == 0;
        ok = ok && json_get(&result, "mirror_rpc_errors") != NULL &&
            json_get_int(json_get(&result, "mirror_rpc_errors")) == 0;
        ok = ok && json_get(&result, "mirror_active_error_code") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_active_error_code")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_active_error_detail") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_active_error_detail")),
                   "connect failed") == 0;
        ok = ok && json_get(&result, "candidate_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result, "candidate_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "candidate_blocker_scope") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "candidate_blocker_scope")),
                   "active_or_safety") == 0;
        ok = ok && json_get(&result, "legacy_advisory_blocker") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "legacy_advisory_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_blockers_total") != NULL &&
            json_get_int(json_get(&result, "mirror_blockers_total")) == 1;
        ok = ok && json_get(&result, "mirror_stalls_total") != NULL &&
            json_get_int(json_get(&result, "mirror_stalls_total")) == 0;
        ok = ok && json_get(&result,
                            "mirror_unsafe_overrides_total") != NULL &&
            json_get_int(json_get(&result,
                                  "mirror_unsafe_overrides_total")) == 1;
        ok = ok && json_get(&result, "mirror_last_override_safe") != NULL &&
            !json_get_bool(json_get(&result, "mirror_last_override_safe"));
        ok = ok && json_get(&result, "mirror_last_override_reason") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_reason")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_last_override_scope") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_scope")),
                   "unsafe_no_authorized_scope") == 0;
        ok = ok && json_get(&result, "mirror_activation_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_activation_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && condition_engine && condition_engine->type == JSON_OBJ;
        ok = ok && json_get(condition_engine, "registered_count") != NULL;
        ok = ok && json_get(condition_engine, "active_count") != NULL;
        ok = ok && json_get(condition_engine, "unresolved_count") != NULL;
        ok = ok && json_get(condition_engine, "conditions") != NULL;
        ok = ok && ce && ce->type == JSON_OBJ;
        ok = ok && json_get(ce, "state") != NULL;
        ok = ok && json_get(ce, "publish_state") != NULL;
        ok = ok && json_get(ce, "active_tip_source_class") != NULL;
        ok = ok && json_get(ce, "active_tip") != NULL;
        ok = ok && json_get(ce, "header_tip") != NULL;
        ok = ok && json_get(ce, "persisted_active_tip") != NULL;
        ok = ok && json_get(ce, "utxo_max_height") != NULL;
        ok = ok && json_get(ce, "coins_best_block_height") != NULL;
        ok = ok && json_get(ce, "csr_sqlite_max_height") != NULL;
        ok = ok && json_get(ce, "missing_active_tip_evidence") != NULL;
        ok = ok && json_get(ce, "publish_state_not_local") != NULL;
        ok = ok && json_get(ce, "active_tip_hash_mismatch") != NULL;
        ok = ok && json_get(ce, "csr_cursor_mismatch") != NULL;
        ok = ok && ca && ca->type == JSON_OBJ;
        ok = ok && json_get(ca, "authority") != NULL;
        ok = ok && json_get(ca, "decision") != NULL;
        ok = ok && json_get(ca, "selected_source") != NULL;
        ok = ok && json_get(ca, "selected_source_trust") != NULL;
        ok = ok && json_get(ca, "mirror_fallback_allowed") != NULL;
        ok = ok && json_get(ca, "activation_allowed") != NULL;
        ok = ok && json_get(ca, "best_header_height") != NULL;
        ok = ok && json_get(ca, "projection_height") != NULL;
        ok = ok && json_get(ca, "projection_lag") != NULL;
        ok = ok && json_get(ca, "projection_deferred") != NULL;
        ok = ok && json_get(ca, "projection_state") != NULL;
        ok = ok && json_get(ca, "projection_deferred_total") != NULL;
        ok = ok && json_get(ca, "last_projection_deferred_height") != NULL;
        ok = ok && json_get(ca, "last_projection_deferred_time") != NULL;
        ok = ok && json_get(ca, "last_projection_deferred_reason") != NULL;
        ok = ok && json_get(ca, "reason") != NULL;
        ok = ok && json_get(ca, "initialized") != NULL;
        ok = ok && json_get(ca, "has_connman") != NULL;
        ok = ok && json_get(ca, "has_main_state") != NULL;
        ok = ok && json_get(ca, "has_node_db") != NULL;
        const struct json_value *sources =
            ca ? json_get(ca, "sources") : NULL;
        const struct json_value *current_snapshot =
            find_source_json(sources, "snapshot");
        ok = ok && sources && sources->type == JSON_ARR;
        ok = ok && current_snapshot && current_snapshot->type == JSON_OBJ;
        ok = ok && json_get(current_snapshot, "selectable") != NULL;
        ok = ok && json_get(current_snapshot, "selection_blocker") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_target_lag_penalty") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_failure_penalty") != NULL;
        const struct json_value *has_last = json_get(ca, "has_last_decision");
        const struct json_value *last = json_get(ca, "last_decision");
        ok = ok && has_last && json_get_bool(has_last);
        ok = ok && last && last->type == JSON_OBJ;
        ok = ok && json_get(last, "op") != NULL &&
            strcmp(json_get_str(json_get(last, "op")),
                   "snapshot_offer") == 0;
        ok = ok && json_get(last, "selected_source_reason") != NULL &&
            strcmp(json_get_str(json_get(last, "selected_source_reason")),
                   "manifest_ok") == 0;
        ok = ok && json_get(last, "selected_source_selectable") != NULL;
        ok = ok && json_get_bool(json_get(
            last, "selected_source_selectable"));
        ok = ok && json_get(last,
                            "selected_source_selection_blocker") != NULL;
        ok = ok && strcmp(json_get_str(json_get(
            last, "selected_source_selection_blocker")), "") == 0;
        ok = ok && json_get(last, "selected_source_score_base") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_base")) == 85;
        ok = ok && json_get(last,
                            "selected_source_score_target_lag_penalty")
                 != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_target_lag_penalty")) == 25;
        ok = ok && json_get(last,
                            "selected_source_score_failure_penalty") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_failure_penalty")) == 0;
        const struct json_value *last_sources = json_get(last, "sources");
        const struct json_value *snapshot =
            find_source_json(last_sources, "snapshot");
        ok = ok && last_sources && last_sources->type == JSON_ARR;
        ok = ok && snapshot && snapshot->type == JSON_OBJ;
        ok = ok && json_get(snapshot, "trust") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "trust")),
                   "native_snapshot_proof_validated") == 0;
        ok = ok && json_get(snapshot, "reason") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "reason")),
                   "manifest_ok") == 0;

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
        json_free(&params);
        json_free(&result);
        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
    }

    printf("healthcheck: default is bounded first-call JSON... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "healthcheck",
                                          &params, &result);
        const struct json_value *checks = json_get(&result, "checks");
        const struct json_value *agent = json_get(&result, "agent");
        const struct json_value *ca =
            checks ? json_get(checks, "chain_advance") : NULL;
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.healthcheck.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                          "consensus_authority")),
                          "local_consensus_validation") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                          "candidate_source")),
                          "agent_cached_summary") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                          "candidate_trust")),
                          "bounded_cached_status") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                          "result_completeness")), "bounded") == 0;
        ok = ok && json_get_bool(json_get(&result, "partial_result"));
        const struct json_value *first_call =
            json_get(&result, "first_call");
        ok = ok && first_call && first_call->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(first_call, "schema")),
                          "zcl.first_call_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(first_call, "api")),
                          "healthcheck") == 0;
        ok = ok && strcmp(json_get_str(json_get(first_call,
                          "result_completeness")), "bounded") == 0;
        ok = ok && strcmp(json_get_str(json_get(first_call, "source")),
                          "agent_cached_summary") == 0;
        ok = ok && json_get_bool(json_get(first_call, "partial_result"));
        ok = ok && json_get_int(json_get(first_call, "budget_ms")) == 500;
        ok = ok && json_get(first_call, "elapsed_ms") != NULL;
        ok = ok && json_get(first_call, "budget_exceeded") != NULL;
        ok = ok && json_get(&result, "full_mode_command") != NULL;
        ok = ok && json_get(&result, "healthy") != NULL;
        ok = ok && json_get(&result, "serving") != NULL;
        ok = ok && json_get(&result, "readiness_status") != NULL;
        ok = ok && json_get(&result, "chain_serving_ready") != NULL;
        ok = ok && json_get(&result, "height_contract_status") != NULL;
        ok = ok && json_get(&result, "normal_lookahead") != NULL;
        ok = ok && json_get(&result, "sync_fsm_at_tip") != NULL;
        ok = ok && checks && checks->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(checks, "bounded"));
        ok = ok && json_get_bool(json_get(checks, "partial_result"));
        ok = ok && json_get(checks, "height_contract_status") != NULL;
        ok = ok && json_get(checks, "normal_lookahead") != NULL;
        ok = ok && json_get(checks, "sync_fsm_at_tip") != NULL;
        ok = ok && json_get(checks, "chain_serving_ready") != NULL;
        ok = ok && json_get(checks, "serving_ready") != NULL;
        ok = ok && json_get(checks, "index_projection_ready") != NULL;
        ok = ok && json_get(checks, "agent_work_ready") != NULL;
        ok = ok && json_get(checks, "peer_count") != NULL;
        ok = ok && json_get(checks, "log_head") != NULL;
        ok = ok && json_get(checks, "chain_evidence") == NULL;
        ok = ok && ca && ca->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(ca, "source")),
                          "cached_first_call") == 0;
        ok = ok && json_get(ca, "block_source_status_cached") != NULL;
        ok = ok && agent && agent->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(agent, "schema")),
                          "zcl.public_status.v1") == 0;

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
        json_free(&params);
        json_free(&result);
    }

    printf("api: native RPC returns versioned discovery document... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "api", &params, &result);
        const struct json_value *resources = json_get(&result, "resources");
        const struct json_value *mcp = json_get(&result, "mcp");
        const struct json_value *cli = json_get(&result, "cli");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.rest_index.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "api_version")),
                          "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "base_path")),
                          "/api/v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "compat_base_path")),
                          "/api") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "first_call")),
                          "/api/v1/agent") == 0;
        ok = ok && resources && resources->type == JSON_ARR &&
            json_size(resources) >= 4;
        ok = ok && mcp && mcp->type == JSON_OBJ &&
            strcmp(json_get_str(json_get(mcp, "first_tool")),
                   "zcl_agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "map_tool")),
                          "zcl_agent_map") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "lanes_tool")),
                          "zcl_agent_lanes") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "impact_tool")),
                          "zcl_agent_impact") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "contracts_tool")),
                          "zcl_agent_contracts") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "build_tool")),
                          "zcl_agent_build") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "interface_tool")),
                          "zcl_agent_interface") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "deploy_guard_tool")),
                          "zcl_agent_deploy_guard") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "api_tool")),
                          "zcl_openapi") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp,
                                                "app_protocols_tool")),
                          "zcl_app_protocols") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp,
                                                "service_catalog_tool")),
                          "zcl_service_catalog") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "drilldown_tool")),
                          "zcl_health") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "milestone_tool")),
                          "zcl_milestone") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "refold_tool")),
                          "zcl_refold_status") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp,
                                                "peer_incidents_tool")),
                          "zcl_peer_incidents") == 0;
        ok = ok && cli && cli->type == JSON_OBJ &&
            strcmp(json_get_str(json_get(cli, "api_command")),
                   "zclassic23 api") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli,
                                                "app_protocols_command")),
                          "zclassic23 appprotocols") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli,
                                                "service_catalog_command")),
                          "zclassic23 servicecatalog") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "first_command")),
                          "zclassic23 agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "map_command")),
                          "zclassic23 agentmap") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "lanes_command")),
                          "zclassic23 agentlanes") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "impact_command")),
                          "zclassic23 agentimpact <files...>") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "contracts_command")),
                          "zclassic23 agentcontracts") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "build_command")),
                          "zclassic23 agentbuild") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli,
                                                "interface_command")),
                          "zclassic23 agentinterface") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli,
                                                "deploy_guard_command")),
                          "zclassic23 agentdeployguard [action]") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "drilldown_command")),
                          "zclassic23 healthcheck") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "milestone_command")),
                          "zclassic23 milestone") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "refold_command")),
                          "zclassic23 refold") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli,
                                                "peer_incidents_command")),
                          "zclassic23 peerincidents") == 0;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "apiindex",
                                                &params, &alias);
        ok = ok && alias_executed && alias.type == JSON_OBJ &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.rest_index.v1") == 0;

        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC returns application protocol catalog... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "appprotocols",
                                          &params, &result);
        const struct json_value *protocols =
            json_get(&result, "protocols");
        const struct json_value *zlsp =
            find_object_with_str(protocols, "name", "zlsp");
        const struct json_value *zslp =
            find_object_with_str(protocols, "name", "zslp");
        const struct json_value *znam =
            find_object_with_str(protocols, "name", "znam");
        const struct json_value *market =
            find_object_with_str(protocols, "name", "market");
        const struct json_value *script_contracts =
            find_object_with_str(protocols, "name", "script_contracts");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.application_protocols.index.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "base_layer")),
                          "zclassic_l1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "service_layer")),
                          "zclassic23_application_layer") == 0;
        ok = ok && protocols && protocols->type == JSON_ARR &&
            json_get_int(json_get(&result, "protocol_count")) ==
            (int64_t)json_size(protocols);
        ok = ok && zlsp &&
            strcmp(json_get_str(json_get(zlsp, "status")),
                   "design") == 0;
        ok = ok && zlsp &&
            strcmp(json_get_str(json_get(zlsp, "family")),
                   "application_protocol_framework") == 0;
        ok = ok && zlsp &&
            json_array_has_str(json_get(zlsp, "crud_capabilities"),
                               "construct_transaction");
        ok = ok && zslp &&
            json_array_has_str(json_get(zslp, "crud_capabilities"),
                               "read_collection");
        ok = ok && zslp &&
            strcmp(json_get_str(json_get(zslp, "anchor_kind")),
                   "op_return") == 0;
        ok = ok && zslp &&
            json_array_has_str(json_get(zslp, "object_types"),
                               "token_genesis");
        ok = ok && znam &&
            json_array_has_str(json_get(znam, "ux_surfaces"),
                               "identity_profile");
        ok = ok && znam &&
            strstr(json_get_str(json_get(znam, "crypto_model")),
                   "owner_authority") != NULL;
        ok = ok && market &&
            json_array_has_str(json_get(market, "object_types"),
                               "signed_listing");
        ok = ok && market &&
            strstr(json_get_str(json_get(market, "privacy_model")),
                   "allowlist") != NULL;
        ok = ok && script_contracts &&
            strcmp(json_get_str(json_get(script_contracts, "anchor_kind")),
                   "standard_script") == 0;
        ok = ok && script_contracts &&
            strstr(json_get_str(json_get(script_contracts, "crypto_model")),
                   "legacy_valid_zclassic_script") != NULL;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "protocols",
                                                &params, &alias);
        ok = ok && alias_executed &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.application_protocols.index.v1") == 0;

        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC returns sovereign service catalog... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "servicecatalog",
                                          &params, &result);
        const struct json_value *services = json_get(&result, "services");
        const struct json_value *bootstrap =
            find_object_with_str(services, "name", "bootstrap");
        const struct json_value *names =
            find_object_with_str(services, "name", "znam_names");
        const struct json_value *onion =
            find_object_with_str(services, "name", "onion_directory");
        const struct json_value *contracts =
            find_object_with_str(services, "name", "script_contracts");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.service_catalog.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "base_layer")),
                          "zclassic_l1") == 0;
        const struct json_value *ux = json_get(&result, "sovereign_ux");
        ok = ok && ux &&
            strcmp(json_get_str(json_get(ux, "schema")),
                   "zcl.sovereign_ux_contract.v1") == 0;
        ok = ok && ux &&
            json_array_has_str(json_get(ux, "flow"),
                               "verify_service_records");
        ok = ok && ux &&
            json_array_has_str(json_get(ux, "primary_entities"),
                               "endpoint_record");
        ok = ok && services && services->type == JSON_ARR &&
            json_get_int(json_get(&result, "service_count")) ==
            (int64_t)json_size(services);
        ok = ok && bootstrap &&
            strcmp(json_get_str(json_get(bootstrap, "rest_collection")),
                   "/api/v1/bootstrap") == 0;
        ok = ok && bootstrap &&
            json_array_has_str(json_get(bootstrap, "transports"), "p2p");
        ok = ok && bootstrap &&
            json_array_has_str(json_get(bootstrap, "depends_on_services"),
                               "full_node");
        const struct json_value *bootstrap_status_op =
            find_object_with_str(json_get(bootstrap, "operations"),
                                 "operation", "read_bootstrap_status");
        ok = ok && bootstrap_status_op &&
            strcmp(json_get_str(json_get(bootstrap_status_op, "mcp_tool")),
                   "zcl_bootstrapstatus") == 0;
        ok = ok && names &&
            strcmp(json_get_str(json_get(names, "application_protocol")),
                   "znam") == 0;
        ok = ok && names &&
            json_array_has_str(json_get(names, "crud_capabilities"),
                               "construct_transaction");
        ok = ok && names &&
            strcmp(json_get_str(json_get(names, "read_model")),
                   "znam_projection_confirmed_chain_records") == 0;
        ok = ok && names &&
            strcmp(json_get_str(json_get(names, "write_model")),
                   "construct_znam_op_return_transactions") == 0;
        const struct json_value *name_register_op =
            find_object_with_str(json_get(names, "operations"), "operation",
                                 "construct_name_register");
        ok = ok && name_register_op &&
            strcmp(json_get_str(json_get(name_register_op, "rpc_method")),
                   "name_register") == 0;
        ok = ok && name_register_op &&
            json_get_bool(json_get(name_register_op, "destructive"));
        ok = ok && onion &&
            json_array_has_str(json_get(onion, "transports"), "onion");
        ok = ok && contracts &&
            strstr(json_get_str(json_get(contracts, "verified_by")),
                   "zclassic_script") != NULL;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "service_catalog",
                                                &params, &alias);
        ok = ok && alias_executed &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.service_catalog.v1") == 0;

        struct json_value one_params;
        json_init(&one_params);
        json_set_array(&one_params);
        struct json_value one_name;
        json_init(&one_name);
        json_set_str(&one_name, "bootstrap");
        json_push_back(&one_params, &one_name);
        json_free(&one_name);

        struct json_value one;
        json_init(&one);
        bool one_executed = rpc_table_execute(&tbl, "servicecatalog",
                                              &one_params, &one);
        ok = ok && one_executed &&
            strcmp(json_get_str(json_get(&one, "schema")),
                   "zcl.service_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&one, "name")),
                          "bootstrap") == 0;
        ok = ok && strcmp(json_get_str(json_get(&one, "self_route")),
                          "/api/v1/service-catalog/bootstrap") == 0;
        ok = ok && json_array_has_str(json_get(&one, "transports"), "p2p");
        ok = ok && json_array_has_str(json_get(&one,
                             "depends_on_services"), "full_node");
        ok = ok && strcmp(json_get_str(json_get(&one, "read_model")),
                          "network_bootstrap_status_and_peer_projection") == 0;
        bootstrap_status_op = find_object_with_str(json_get(&one,
                             "operations"), "operation",
                             "read_bootstrap_status");
        ok = ok && bootstrap_status_op &&
            strcmp(json_get_str(json_get(bootstrap_status_op, "rpc_method")),
                   "bootstrapstatus") == 0;

        struct json_value bad_params;
        json_init(&bad_params);
        json_set_array(&bad_params);
        struct json_value bad_name;
        json_init(&bad_name);
        json_set_str(&bad_name, "not_real");
        json_push_back(&bad_params, &bad_name);
        json_free(&bad_name);

        struct json_value bad;
        json_init(&bad);
        bool bad_executed = rpc_table_execute(&tbl, "servicecatalog",
                                              &bad_params, &bad);
        ok = ok && bad_executed &&
            strcmp(json_get_str(json_get(&bad, "schema")),
                   "zcl.service_catalog_error.v1") == 0;
        ok = ok && json_array_has_str(json_get(&bad, "valid_services"),
                                      "bootstrap");

        json_free(&bad);
        json_free(&bad_params);
        json_free(&one);
        json_free(&one_params);
        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC agent names health blocking reason... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        alerts_shutdown();
        unsetenv("ZCL_ALERTS_DISABLE");
        unsetenv("ZCL_ALERT_WEBHOOK_URL");
        event_log_init();
        alerts_init();
        alerts_reset();
        const char *long_blocker =
            "check=window.consistency I4.3 utxo_apply log hole: contiguous "
            "ok=1 prefix h=3056758 but cursor=3171120 first_hole_h=3056759 "
            "repair_owner=reducer_frontier_reconcile_light";
        event_emitf(EV_OPERATOR_NEEDED, 0, "%s", long_blocker);
        setenv("ZCL_AGENT_EXPECT_BUILD_COMMIT", "expected-agent-test", 1);
        setenv("ZCL_AGENT_EXPECT_BUILD_SOURCE", "unit-test", 1);
        rpc_agent_set_boot_context("dev", "full", "/tmp/zcl-agent-dev",
                                   18252, 8053, 8443, 18034);

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "agent", &params, &result);
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.public_status.v1") == 0;
        const struct json_value *first_call =
            json_get(&result, "first_call");
        ok = ok && first_call && first_call->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(first_call, "schema")),
                          "zcl.first_call_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(first_call, "api")),
                          "agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(first_call, "source")),
                          "cached_fast_fields") == 0;
        ok = ok && !json_get_bool(json_get(first_call, "partial_result"));
        ok = ok && json_get_int(json_get(first_call, "budget_ms")) == 250;
        ok = ok && json_get(first_call, "elapsed_ms") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&result, "build_commit")),
                          zcl_build_commit()) == 0;
        const struct json_value *runtime_build =
            json_get(&result, "runtime_build");
        ok = ok && runtime_build && runtime_build->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(runtime_build, "schema")),
                          "zcl.runtime_build.v1") == 0;
        ok = ok && json_get_int(json_get(runtime_build,
                                         "schema_version")) == 1;
        ok = ok && strcmp(json_get_str(json_get(runtime_build,
                                                "running_build_commit")),
                          zcl_build_commit()) == 0;
        ok = ok && strcmp(json_get_str(json_get(runtime_build,
                                                "expected_build_commit")),
                          "expected-agent-test") == 0;
        ok = ok && strcmp(json_get_str(json_get(runtime_build,
                                                "expected_source")),
                          "unit-test") == 0;
        ok = ok && json_get_bool(json_get(runtime_build,
                                          "expected_present"));
        ok = ok && !json_get_bool(json_get(runtime_build,
                                           "matches_expected"));
        ok = ok && json_get_bool(json_get(runtime_build, "stale"));
        ok = ok && strcmp(json_get_str(json_get(runtime_build,
                                                "freshness")),
                          "stale") == 0;
        ok = ok && json_get(runtime_build, "dirty_build") != NULL;
        ok = ok && json_get(runtime_build, "semantics") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&result, "status")),
                          "blocked") == 0;
        ok = ok && !json_get_bool(json_get(&result, "healthy"));
        ok = ok && !json_get_bool(json_get(&result, "serving"));
        ok = ok && json_get_bool(json_get(&result, "operator_needed"));
        const char *primary =
            json_get_str(json_get(&result, "primary_blocker"));
        ok = ok && primary &&
             strstr(primary, "operator_needed:check=window.consistency") != NULL;
        ok = ok && primary &&
             strstr(primary, "first_hole_h=3056759") != NULL;
        ok = ok && primary &&
             strstr(primary, "reducer_frontier_reconcile_light") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&result, "summary")),
                          "node has an active health blocker") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "next")),
                          "zclassic23 healthcheck") == 0;
        const struct json_value *readiness = json_get(&result, "readiness");
        ok = ok && readiness && readiness->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(readiness, "schema")),
                          "zcl.agent_readiness.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(readiness, "status")),
                          "not_serving") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "readiness_status")),
                          "not_serving") == 0;
        ok = ok && !json_get_bool(json_get(readiness,
                                           "chain_serving_ready"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "chain_serving_ready"));
        ok = ok && !json_get_bool(json_get(readiness,
                                           "index_projection_ready"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "index_projection_ready"));
        ok = ok && !json_get_bool(json_get(readiness,
                                           "agent_work_ready"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "agent_work_ready"));
        ok = ok && json_get_bool(json_get(readiness,
                                          "operator_action_required"));
        ok = ok && json_get_bool(json_get(&result,
                                          "operator_action_required"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "readiness_next_action")),
                          "operator_intervention_required") == 0;
        const struct json_value *operator_latch =
            json_get(&result, "operator_latch");
        ok = ok && operator_latch && operator_latch->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(operator_latch, "schema")),
                          "zcl.operator_latch.v1") == 0;
        ok = ok && json_get_int(json_get(operator_latch,
                                         "schema_version")) == 1;
        ok = ok && json_get_bool(json_get(operator_latch, "active"));
        ok = ok && json_get_bool(json_get(operator_latch,
                                          "operator_action_required"));
        ok = ok && !json_get_bool(json_get(operator_latch,
                                           "recovered_this_call"));
        ok = ok && !json_get_bool(json_get(operator_latch,
                                           "suppressed_by_mirror_contract"));
        ok = ok && json_get(operator_latch, "since_unix") != NULL;
        ok = ok && strstr(json_get_str(json_get(operator_latch, "detail")),
                          "window.consistency") != NULL;
        ok = ok && strcmp(json_get_str(json_get(operator_latch,
                                                "state_tool")),
                          "zcl_state subsystem=condition_engine") == 0;
        const struct json_value *condition_summary =
            json_get(&result, "conditions");
        ok = ok && condition_summary && condition_summary->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(condition_summary,
                                                "schema")),
                          "zcl.condition_engine_summary.v1") == 0;
        ok = ok && json_get(condition_summary, "active_count") != NULL;
        ok = ok && json_get(condition_summary, "unresolved_count") != NULL;
        ok = ok && strcmp(json_get_str(json_get(condition_summary,
                                                "state_tool")),
                          "zcl_state subsystem=condition_engine") == 0;
        const struct json_value *mirror_contract =
            json_get(&result, "mirror_contract");
        ok = ok && mirror_contract && mirror_contract->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(mirror_contract,
                                                "schema")),
                          "zcl.mirror_status.v1") == 0;
        ok = ok && json_get_bool(json_get(mirror_contract,
                                          "advisory_only"));
        ok = ok && json_get(mirror_contract,
                            "operator_action_required") != NULL;
        const struct json_value *reducer = json_get(&result, "reducer");
        ok = ok && reducer && reducer->type == JSON_OBJ;
        ok = ok && json_get(reducer, "log_head") != NULL;
        ok = ok && json_get(reducer, "log_head_gap") != NULL;
        ok = ok && json_get(reducer, "tip_advance_age_seconds") != NULL;
        ok = ok && json_get(reducer, "validation_pack_ok") != NULL;
        ok = ok && json_get(reducer, "validation_pack_detail") != NULL;
        const struct json_value *health = json_get(&result, "health");
        ok = ok && health && health->type == JSON_OBJ;
        ok = ok && json_get(health, "warning_count") != NULL;
        ok = ok && json_get(health, "warning_reasons") != NULL;
        ok = ok && json_get(health, "last_error_age_seconds") != NULL;
        ok = ok && json_get(health, "last_error_type") != NULL;
        ok = ok && json_get(health, "blocking_reason") != NULL;
        ok = ok && json_get_bool(json_get(health,
                                          "operator_latch_active"));
        ok = ok && json_get_bool(json_get(health,
                                          "operator_action_required"));
        ok = ok && json_get(health,
                            "operator_latch_detail") != NULL;
        ok = ok && json_get(health,
                            "operator_latch_since_unix") != NULL;
        ok = ok && json_get(health,
                            "active_condition_count") != NULL;
        ok = ok && json_get(health,
                            "unresolved_condition_count") != NULL;
        const struct json_value *resources = json_get(&result, "resources");
        ok = ok && resources && resources->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(resources, "schema")),
                          "zcl.node_resources.v1") == 0;
        ok = ok && json_get(resources, "rss_mb") != NULL;
        ok = ok && json_get(resources, "rss_warn_threshold_mb") != NULL;
        ok = ok && json_get(resources, "rss_warning") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_available") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_current_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_high_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_max_mb") != NULL;
        ok = ok && json_get(resources,
                            "cgroup_memory_stat_available") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_anon_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_file_mb") != NULL;
        ok = ok && json_get(resources,
                            "cgroup_memory_kernel_mb") != NULL;
        ok = ok && json_get(resources,
                            "cgroup_memory_reclaimable_mb") != NULL;
        ok = ok && json_get(resources,
                            "cgroup_memory_working_set_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_watch") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_warning") != NULL;
        ok = ok && json_get(resources, "memory_pressure") != NULL;
        ok = ok && json_get(resources, "memory_pressure_detail") != NULL;
        ok = ok && json_get(resources, "pressure_basis") != NULL;
        ok = ok && json_get(resources, "uptime_seconds") != NULL;
        const struct json_value *restart_watchdog =
            json_get(&result, "restart_watchdog");
        ok = ok && restart_watchdog && restart_watchdog->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(restart_watchdog, "schema")),
                          "zcl.restart_watchdog.v1") == 0;
        ok = ok && json_get(restart_watchdog, "status") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "last_restart_autonomous") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "last_restart_reason") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "no_progress_restarts") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "restarts_remaining") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "deep_state") != NULL;
        const struct json_value *security =
            json_get(&result, "security_posture");
        ok = ok && security && security->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(security, "schema")),
                          "zcl.security_posture.v1") == 0;
        ok = ok && json_get(security, "bootstrap_model") != NULL;
        ok = ok && json_get(security,
                            "full_history_validation_state") != NULL;
        ok = ok && json_get(security,
                            "nullifier_history_state") != NULL;
        ok = ok && readiness && readiness->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(readiness, "schema")),
                          "zcl.agent_readiness.v1") == 0;
        ok = ok && json_get(readiness, "status") != NULL;
        ok = ok && json_get(&result, "readiness_status") != NULL;
        ok = ok && json_get(readiness, "chain_serving_ready") != NULL;
        ok = ok && json_get(&result, "chain_serving_ready") != NULL;
        ok = ok && json_get(readiness, "index_projection_ready") != NULL;
        ok = ok && json_get(&result, "index_projection_ready") != NULL;
        ok = ok && json_get(readiness, "agent_work_ready") != NULL;
        ok = ok && json_get(&result, "agent_work_ready") != NULL;
        ok = ok && json_get(readiness, "operator_action_required") != NULL;
        ok = ok && json_get(&result, "operator_action_required") != NULL;
        ok = ok && json_get(readiness, "next_action") != NULL;
        ok = ok && json_get(&result, "readiness_next_action") != NULL;
        ok = ok && json_get(readiness, "semantics") != NULL;
        const struct json_value *download = json_get(&result, "download");
        ok = ok && download && download->type == JSON_OBJ;
        ok = ok && json_get(download, "requested") != NULL;
        ok = ok && json_get(download, "received") != NULL;
        ok = ok && json_get(download, "timed_out") != NULL;
        ok = ok && json_get(download, "in_flight") != NULL;
        ok = ok && json_get(download, "queued") != NULL;
        ok = ok && json_get(download, "queue_peer_avoid_count") != NULL;
        ok = ok && json_get(download,
                            "queue_peer_avoid_max_seconds") != NULL;
        ok = ok && json_get(download, "bytes_received") != NULL;
        ok = ok && json_get(download, "mbps_avg") != NULL;
        const struct json_value *indexer = json_get(&result, "indexer");
        ok = ok && indexer && indexer->type == JSON_OBJ;
        ok = ok && json_get(indexer, "height") != NULL;
        ok = ok && json_get(indexer, "lag") != NULL;
        ok = ok && json_get(indexer, "projection_height") != NULL;
        ok = ok && json_get(indexer, "projection_lag") != NULL;
        ok = ok && json_get(indexer, "projection_deferred") != NULL;
        ok = ok && json_get(indexer, "projection_state") != NULL;
        ok = ok && json_get(indexer, "catchup_active") != NULL;
        ok = ok && json_get(indexer, "catchup_height") != NULL;
        ok = ok && json_get(indexer, "catchup_target_height") != NULL;
        ok = ok && json_get(indexer,
                            "catchup_progress_age_seconds") != NULL;
        ok = ok && json_get(indexer, "catchup_uptime_seconds") != NULL;
        const struct json_value *lane =
            json_get(&result, "operator_lane");
        ok = ok && lane && lane->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(lane, "schema")),
                          "zcl.operator_lane.v1") == 0;
        ok = ok && json_get_int(json_get(lane, "schema_version")) == 1;
        ok = ok && strcmp(json_get_str(json_get(lane, "lane")),
                          "dev") == 0;
        ok = ok && json_get_bool(json_get(lane, "development"));
        ok = ok && !json_get_bool(json_get(lane, "canonical"));
        ok = ok && strcmp(json_get_str(json_get(lane,
                                                "restart_policy")),
                          "frequent_deploy_ok") == 0;
        ok = ok && json_get_bool(json_get(lane,
                                          "automation_restart_ok"));
        ok = ok && json_get_bool(json_get(lane,
                                          "automation_deploy_ok"));
        ok = ok && !json_get_bool(json_get(lane,
                                           "requires_operator_confirmation"));
        const struct json_value *safety =
            json_get(lane, "deployment_safety");
        ok = ok && safety && safety->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(safety, "schema")),
                          "zcl.operator_deployment_safety.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(safety,
                                                "preferred_deploy_target")),
                          "dev") == 0;
        ok = ok && strcmp(json_get_str(json_get(safety,
                                                "safe_default_action")),
                          "deploy_dev_lane") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "operator_lane_name")),
                          "dev") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "automation_restart_ok"));
        ok = ok && json_get_bool(json_get(&result,
                                          "automation_deploy_ok"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "requires_operator_confirmation"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "preferred_deploy_target")),
                          "dev") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_default_action")),
                          "deploy_dev_lane") == 0;

        json_free(&params);
        json_free(&result);
        unsetenv("ZCL_AGENT_EXPECT_BUILD_COMMIT");
        unsetenv("ZCL_AGENT_EXPECT_BUILD_SOURCE");
        rpc_agent_set_boot_context(NULL, NULL, NULL, 0, 0, 0, 0);
        alerts_shutdown();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: mirror cached snapshot avoids live height refresh... ");
    {
        legacy_mirror_sync_reset_for_test();
        struct legacy_mirror_sync_stats seeded = {0};
        seeded.enabled = true;
        seeded.running = true;
        seeded.reachable = true;
        seeded.legacy_height = 100;
        seeded.legacy_headers = 100;
        seeded.local_height = 99;
        seeded.best_header_height = 100;
        seeded.target_height = 100;
        snprintf(seeded.zclassic23_hash, sizeof(seeded.zclassic23_hash),
                 "%s", "cached-local-hash");
        snprintf(seeded.zclassicd_hash, sizeof(seeded.zclassicd_hash),
                 "%s", "cached-legacy-hash");
        legacy_mirror_sync_test_set_stats(&seeded, NULL);

        struct legacy_mirror_sync_stats snap = {0};
        legacy_mirror_sync_stats_cached_snapshot(&snap);

        bool ok = snap.enabled && snap.running && snap.reachable;
        ok = ok && snap.local_height == 99;
        ok = ok && snap.best_header_height == 100;
        ok = ok && snap.lag_known;
        ok = ok && snap.lag == 1;
        ok = ok && strcmp(snap.zclassic23_hash,
                          "cached-local-hash") == 0;
        ok = ok && strcmp(snap.zclassicd_hash,
                          "cached-legacy-hash") == 0;

        legacy_mirror_sync_reset_for_test();
        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC status aliases bounded agent status... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool ok = rpc_table_execute(&tbl, "status", &params, &result);
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "api_version")),
                          "v1") == 0;
        const struct json_value *first_call = json_get(&result, "first_call");
        ok = ok && first_call && first_call->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(first_call, "schema")),
                          "zcl.first_call_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(first_call, "api")),
                          "agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(first_call,
                                                "budget_semantics")),
                          "first-call path must use cached/bounded sources "
                          "and return valid JSON") == 0;
        const struct json_value *security =
            json_get(&result, "security_posture");
        ok = ok && security && security->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(security, "schema")),
                          "zcl.security_posture.v1") == 0;
        ok = ok && json_get(security, "bootstrap_model") != NULL;
        ok = ok && json_get(security,
                            "full_history_validation_state") != NULL;
        ok = ok && json_get(security,
                            "snapshot_full_validation_complete") != NULL;
        ok = ok && json_get(security,
                            "nullifier_history_complete") != NULL;
        ok = ok && json_get(security,
                            "nullifier_activation_cursor") != NULL;
        ok = ok && strstr(json_get_str(json_get(security, "semantics")),
                          "serving/healthy are liveness signals") != NULL;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC agent bounds optional detail when budget is spent... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        setenv("ZCL_AGENT_TEST_ELAPSED_OFFSET_MS", "200", 1);
        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "agent", &params, &result);
        const struct json_value *first_call =
            json_get(&result, "first_call");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && json_get_bool(json_get(&result, "partial_result"));
        ok = ok && strstr(json_get_str(json_get(&result, "partial_reason")),
                          "optional_detail_budget_guard:resources") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&result,
                          "deferred_components")),
                          "resources,restart_watchdog") == 0;
        ok = ok && first_call && first_call->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(first_call, "partial_result"));
        ok = ok && strstr(json_get_str(json_get(first_call,
                          "partial_reason")),
                          "optional_detail_budget_guard:resources") != NULL;
        ok = ok && json_get(&result, "resources") == NULL;
        ok = ok && json_get(&result, "restart_watchdog") == NULL;
        ok = ok && json_get(&result, "readiness") != NULL;
        ok = ok && json_get(&result, "height_contract") != NULL;
        ok = ok && json_get(&result, "mirror_contract") != NULL;
        ok = ok && json_get(&result, "download") != NULL;

        unsetenv("ZCL_AGENT_TEST_ELAPSED_OFFSET_MS");
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC agent suppresses stale mirror latch... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        alerts_shutdown();
        unsetenv("ZCL_ALERTS_DISABLE");
        event_log_init();
        alerts_init();
        alerts_reset();
        legacy_mirror_sync_reset_for_test();
        struct legacy_mirror_sync_stats stats = {0};
        stats.enabled = true;
        stats.running = true;
        stats.reachable = true;
        stats.legacy_height = 100;
        stats.legacy_headers = 100;
        stats.local_height = 99;
        snprintf(stats.zclassic23_hash, sizeof(stats.zclassic23_hash),
                 "%064x", 1);
        snprintf(stats.zclassicd_hash, sizeof(stats.zclassicd_hash),
                 "%064x", 2);
        legacy_mirror_sync_test_set_stats(&stats, NULL);
        event_emitf(EV_OPERATOR_NEEDED, 0,
                    "chain_advance_hash-disagreement height=99");

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "agent", &params, &result);
        const struct json_value *operator_latch =
            json_get(&result, "operator_latch");
        const struct json_value *mirror_contract =
            json_get(&result, "mirror_contract");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && operator_latch && operator_latch->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(operator_latch, "active"));
        ok = ok && !json_get_bool(json_get(operator_latch,
                                           "operator_action_required"));
        ok = ok && json_get_bool(json_get(operator_latch,
                                          "suppressed_by_mirror_contract"));
        ok = ok && strstr(json_get_str(json_get(operator_latch, "detail")),
                          "chain_advance_hash-disagreement") != NULL;
        ok = ok && mirror_contract && mirror_contract->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(mirror_contract,
                                                "schema")),
                          "zcl.mirror_status.v1") == 0;
        ok = ok && !json_get_bool(json_get(mirror_contract,
                                           "operator_action_required"));
        const char *primary =
            json_get_str(json_get(&result, "primary_blocker"));
        ok = ok && (!primary || strstr(primary, "operator_needed") == NULL);

        json_free(&params);
        json_free(&result);
        alerts_shutdown();
        legacy_mirror_sync_reset_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC agent flags stalled catch-up telemetry... ");
    {
        struct connman cm;
        struct node_signals sigs;
        struct main_state ms;
        struct block_index tip, best_header;
        struct uint256 h_tip, h_hdr, h_inflight, h_queued;
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        memset(&ms, 0, sizeof(ms));
        memset(&tip, 0, sizeof(tip));
        memset(&best_header, 0, sizeof(best_header));
        memset(&h_tip, 0, sizeof(h_tip));
        memset(&h_hdr, 0, sizeof(h_hdr));
        memset(&h_inflight, 0, sizeof(h_inflight));
        memset(&h_queued, 0, sizeof(h_queued));

        bool ok = connman_init(&cm, chain_params_get(), &sigs);
        main_state_init(&ms);
        block_index_init(&tip);
        block_index_init(&best_header);
        syncdiag_set_hash(&h_tip, 0x41);
        syncdiag_set_hash(&h_hdr, 0x42);
        tip.phashBlock = &h_tip;
        tip.nHeight = 100;
        tip.nTime = (uint32_t)platform_time_wall_time_t();
        tip.nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_TREE;
        best_header.phashBlock = &h_hdr;
        best_header.nHeight = 125;
        best_header.pprev = &tip;
        best_header.nTime = tip.nTime;
        best_header.nStatus = BLOCK_VALID_TREE;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        ms.pindex_best_header = &best_header;

        struct p2p_node *peer =
            syncdiag_add_peer(&cm, 44, false, PEER_HANDSHAKE_COMPLETE);
        ok = ok && peer != NULL;
        if (peer)
            peer->starting_height = 125;

        struct download_manager *dm = msg_get_download_mgr();
        dl_drain_for_backpressure(dm);
        syncdiag_set_hash(&h_inflight, 0x51);
        syncdiag_set_hash(&h_queued, 0x52);
        int32_t queued_h = 102;
        ok = ok && dl_mark_requested(dm, &h_inflight, 101, 44);
        ok = ok && dl_queue_blocks(dm, &h_queued, &queued_h, 1) == 1;

        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();
        rpc_net_set_connman(&cm);
        sync_monitor_set_context(&cm, dm, &ms);
        reducer_frontier_provable_tip_set(100);
        sync_monitor_test_set_tip_advance_ts(
            (int64_t)platform_time_wall_time_t() - 180);
        sync_set_state(SYNC_IDLE, "agent stalled reset");
        sync_set_state(SYNC_FINDING_PEERS, "agent stalled");
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "agent stalled");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "agent stalled");

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agent", &params, &result);
        const struct json_value *download = json_get(&result, "download");
        const struct json_value *health = json_get(&result, "health");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "status")),
                          "degraded") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "primary_blocker")),
                          "catchup_stalled") == 0;
        ok = ok && json_get_bool(json_get(&result, "operator_needed"));
        ok = ok && strcmp(json_get_str(json_get(&result, "next")),
                          "zclassic23 getsyncdiag") == 0;
        ok = ok && json_get_int(json_get(&result, "gap")) == 25;
        ok = ok && download && download->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(download, "active"));
        ok = ok && json_get_bool(json_get(download, "catchup_stalled"));
        ok = ok && json_get_int(json_get(download,
                                          "catchup_stall_seconds")) >= 120;
        ok = ok && json_get(download, "request_timeout_seconds") != NULL;
        ok = ok && json_get(download,
                            "oldest_in_flight_age_seconds") != NULL;
        ok = ok && json_get(download, "overdue_in_flight") != NULL;
        ok = ok && json_get(download, "in_flight_peer_count") != NULL;
        ok = ok && json_get(download, "queue_peer_avoid_count") != NULL;
        ok = ok && json_get(download,
                            "queue_peer_avoid_max_seconds") != NULL;
        ok = ok && json_get(download, "assign_attempts") != NULL;
        ok = ok && json_get(download, "assign_successes") != NULL;
        ok = ok && json_get(download, "assign_zero_results") != NULL;
        ok = ok && json_get(download, "dispatch_wakes") != NULL;
        ok = ok && json_get(download, "message_cycles") != NULL;
        ok = ok && json_get(download, "message_send_calls") != NULL;
        ok = ok && json_get(download, "message_process_calls") != NULL;
        ok = ok && json_get(download, "message_recv_ready") != NULL;
        ok = ok && json_get(download, "message_idle_waits") != NULL;
        ok = ok && json_get(download, "message_wakes") != NULL;
        ok = ok && json_get(download, "last_assign_result") != NULL;
        ok = ok && json_get_int(json_get(download, "in_flight")) >= 1;
        ok = ok && json_get_int(json_get(download, "queued")) >= 1;
        ok = ok && health && health->type == JSON_OBJ;
        ok = ok && strstr(json_get_str(json_get(health,
                                                "warning_reasons")),
                          "catchup_stalled") != NULL;

        json_free(&params);
        json_free(&result);
        dl_drain_for_backpressure(dm);
        sync_monitor_set_context(NULL, NULL, NULL);
        rpc_net_set_connman(NULL);
        reducer_frontier_provable_tip_reset();
        sync_monitor_test_set_tip_advance_ts(0);
        sync_set_state(SYNC_IDLE, "agent stalled cleanup");
        main_state_free(&ms);
        connman_free(&cm);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC agent flags idle download dispatch... ");
    {
        struct connman cm;
        struct node_signals sigs;
        struct main_state ms;
        struct block_index tip, best_header;
        struct uint256 h_tip, h_hdr, h_queued;
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        memset(&ms, 0, sizeof(ms));
        memset(&tip, 0, sizeof(tip));
        memset(&best_header, 0, sizeof(best_header));
        memset(&h_tip, 0, sizeof(h_tip));
        memset(&h_hdr, 0, sizeof(h_hdr));
        memset(&h_queued, 0, sizeof(h_queued));

        bool ok = connman_init(&cm, chain_params_get(), &sigs);
        main_state_init(&ms);
        block_index_init(&tip);
        block_index_init(&best_header);
        syncdiag_set_hash(&h_tip, 0x61);
        syncdiag_set_hash(&h_hdr, 0x62);
        tip.phashBlock = &h_tip;
        tip.nHeight = 100;
        tip.nTime = (uint32_t)platform_time_wall_time_t();
        tip.nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_TREE;
        best_header.phashBlock = &h_hdr;
        best_header.nHeight = 125;
        best_header.pprev = &tip;
        best_header.nTime = tip.nTime;
        best_header.nStatus = BLOCK_VALID_TREE;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        ms.pindex_best_header = &best_header;

        struct p2p_node *peer =
            syncdiag_add_peer(&cm, 45, false, PEER_HANDSHAKE_COMPLETE);
        ok = ok && peer != NULL;
        if (peer)
            peer->starting_height = 125;

        struct download_manager *dm = msg_get_download_mgr();
        dl_drain_for_backpressure(dm);
        syncdiag_set_hash(&h_queued, 0x63);
        int32_t queued_h = 101;
        ok = ok && dl_queue_blocks(dm, &h_queued, &queued_h, 1) == 1;

        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();
        rpc_net_set_connman(&cm);
        sync_monitor_set_context(&cm, dm, &ms);
        reducer_frontier_provable_tip_set(100);
        sync_monitor_test_set_tip_advance_ts(
            (int64_t)platform_time_wall_time_t() - 45);
        sync_set_state(SYNC_IDLE, "agent dispatch idle reset");
        sync_set_state(SYNC_FINDING_PEERS, "agent dispatch idle");
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "agent dispatch idle");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "agent dispatch idle");

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agent", &params, &result);
        const struct json_value *download = json_get(&result, "download");
        const struct json_value *health = json_get(&result, "health");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "status")),
                          "degraded") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "primary_blocker")),
                          "download_dispatch_idle") == 0;
        ok = ok && json_get_bool(json_get(&result, "operator_needed"));
        ok = ok && strcmp(json_get_str(json_get(&result, "next")),
                          "zclassic23 getsyncdiag") == 0;
        ok = ok && json_get_int(json_get(&result, "gap")) == 25;
        ok = ok && download && download->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(download, "active"));
        ok = ok && !json_get_bool(json_get(download, "catchup_stalled"));
        ok = ok && json_get_bool(json_get(download, "dispatch_idle"));
        ok = ok && json_get_bool(json_get(download, "dispatch_stalled"));
        ok = ok && json_get_int(json_get(download,
                                          "dispatch_idle_seconds")) >= 30;
        ok = ok && json_get_int(json_get(download, "in_flight")) == 0;
        ok = ok && json_get_int(json_get(download, "queued")) >= 1;
        ok = ok && json_get(download, "queue_peer_avoid_count") != NULL;
        ok = ok && json_get(download,
                            "queue_peer_avoid_max_seconds") != NULL;
        ok = ok && health && health->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(health,
                                                "blocking_reason")),
                          "download_dispatch_idle") == 0;
        ok = ok && strstr(json_get_str(json_get(health,
                                                "warning_reasons")),
                          "download_dispatch_idle") != NULL;

        json_free(&params);
        json_free(&result);
        dl_drain_for_backpressure(dm);
        sync_monitor_set_context(NULL, NULL, NULL);
        rpc_net_set_connman(NULL);
        reducer_frontier_provable_tip_reset();
        sync_monitor_test_set_tip_advance_ts(0);
        sync_set_state(SYNC_IDLE, "agent dispatch idle cleanup");
        main_state_free(&ms);
        connman_free(&cm);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC agent keeps projection detail on fast cache miss... ");
    {
        struct connman cm;
        struct node_signals sigs;
        struct main_state ms;
        struct block_index tip;
        struct uint256 h_tip;
        struct node_db ndb;
        struct db_block blk;
        uint8_t solution[] = {0x01, 0x02, 0x03};
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;
        const int served_height = ZCL_NODE_HEALTH_LAG_WARN_BLOCKS + 10;
        const int projection_height = 5;

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        memset(&ms, 0, sizeof(ms));
        memset(&tip, 0, sizeof(tip));
        memset(&h_tip, 0, sizeof(h_tip));
        memset(&ndb, 0, sizeof(ndb));
        memset(&blk, 0, sizeof(blk));

        bool ok = node_db_open(&ndb, ":memory:");
        ok = ok && connman_init(&cm, chain_params_get(), &sigs);
        main_state_init(&ms);
        block_index_init(&tip);
        syncdiag_set_hash(&h_tip, 0x71);
        tip.phashBlock = &h_tip;
        tip.nHeight = served_height;
        tip.nTime = (uint32_t)platform_time_wall_time_t();
        tip.nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_TREE;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        ms.pindex_best_header = &tip;

        struct p2p_node *peer =
            syncdiag_add_peer(&cm, 46, false, PEER_HANDSHAKE_COMPLETE);
        ok = ok && peer != NULL;
        if (peer)
            peer->starting_height = served_height;

        memset(blk.hash, 0xA5, sizeof(blk.hash));
        memset(blk.prev_hash, 0x5A, sizeof(blk.prev_hash));
        memset(blk.merkle_root, 0xC3, sizeof(blk.merkle_root));
        memset(blk.nonce, 0x3C, sizeof(blk.nonce));
        blk.height = projection_height;
        blk.version = 4;
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.solution = solution;
        blk.solution_len = sizeof(solution);
        blk.status = 5;
        blk.file_num = 1;
        blk.data_pos = 8192;
        blk.num_tx = 1;
        ok = ok && db_block_save(&ndb, &blk);

        struct download_manager *dm = msg_get_download_mgr();
        dl_drain_for_backpressure(dm);
        block_source_policy_reset_for_test();
        block_source_policy_init(&cm, &ms, &ndb);

        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();
        rpc_net_set_connman(&cm);
        sync_monitor_set_context(&cm, dm, &ms);
        reducer_frontier_provable_tip_set(served_height);
        sync_set_state(SYNC_IDLE, "agent projection lag");

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agent", &params, &result);
        const struct json_value *indexer = json_get(&result, "indexer");
        const struct json_value *health = json_get(&result, "health");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "status")),
                          "healthy") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "primary_blocker")),
                          "none") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "next")),
                          "none") == 0;
        ok = ok && !json_get_bool(json_get(&result, "operator_needed"));
        ok = ok && json_get_bool(json_get(&result,
                                          "provable_tip_published"));
        ok = ok && json_get_int(json_get(&result, "served_height")) ==
            served_height;
        ok = ok && json_get_int(json_get(&result, "indexed_height")) ==
            served_height;
        ok = ok && json_get_int(json_get(&result, "index_gap")) == 0;
        const struct json_value *readiness = json_get(&result, "readiness");
        ok = ok && readiness && readiness->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(readiness, "schema")),
                          "zcl.agent_readiness.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(readiness, "status")),
                          "ready") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "readiness_status")),
                          "ready") == 0;
        ok = ok && json_get_bool(json_get(readiness,
                                          "chain_serving_ready"));
        ok = ok && json_get_bool(json_get(&result,
                                          "chain_serving_ready"));
        ok = ok && json_get_bool(json_get(readiness,
                                          "index_projection_ready"));
        ok = ok && json_get_bool(json_get(&result,
                                          "index_projection_ready"));
        ok = ok && json_get_bool(json_get(readiness,
                                          "agent_work_ready"));
        ok = ok && json_get_bool(json_get(&result,
                                          "agent_work_ready"));
        ok = ok && !json_get_bool(json_get(readiness,
                                           "operator_action_required"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "operator_action_required"));
        ok = ok && json_get_int(json_get(readiness, "tip_gap_blocks")) == 0;
        ok = ok && json_get_int(json_get(readiness, "index_gap_blocks")) == 0;
        ok = ok && strcmp(json_get_str(json_get(readiness,
                                                "next_action")),
                          "none") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "readiness_next_action")),
                          "none") == 0;
        ok = ok && indexer && indexer->type == JSON_OBJ;
        ok = ok && !json_get_bool(json_get(indexer,
                                           "block_source_status_cached"));
        ok = ok && json_get_int(json_get(indexer, "height")) ==
            served_height;
        ok = ok && json_get_int(json_get(indexer, "projection_height")) ==
            -1;
        ok = ok && json_get_int(json_get(indexer, "lag")) == -1;
        ok = ok && json_get_int(json_get(indexer, "projection_lag")) == -1;
        ok = ok && !json_get_bool(json_get(indexer, "projection_deferred"));
        ok = ok && strcmp(json_get_str(json_get(indexer,
                                                "projection_state")),
                          "cached_status_unavailable") == 0;
        ok = ok && json_get(indexer, "catchup_active") != NULL;
        ok = ok && json_get(indexer, "catchup_height") != NULL;
        ok = ok && health && health->type == JSON_OBJ;
        ok = ok && strstr(json_get_str(json_get(health,
                                                "warning_reasons")),
                          "block_source_status_busy") != NULL;

        json_free(&params);
        json_free(&result);
        sync_monitor_set_context(NULL, NULL, NULL);
        rpc_net_set_connman(NULL);
        reducer_frontier_provable_tip_reset();
        block_source_policy_reset_for_test();
        dl_drain_for_backpressure(dm);
        main_state_free(&ms);
        connman_free(&cm);
        node_db_close(&ndb);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: agentdiagnose treats one-block lookahead as chain-ok... ");
    {
        char dir[256];
        struct connman cm;
        struct node_signals sigs;
        struct main_state ms;
        struct block_index tip;
        struct uint256 h_tip;
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;
        const int served_height = 100;
        const int target_height = 101;

        chain_params_select(CHAIN_MAIN);
        test_fmt_tmpdir(dir, sizeof(dir), "syncdiag", "diagnose_lookahead");
        test_cleanup_tmpdir(dir);
        mkdir("./test-tmp", 0777);
        mkdir(dir, 0777);

        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        memset(&ms, 0, sizeof(ms));
        memset(&tip, 0, sizeof(tip));
        memset(&h_tip, 0, sizeof(h_tip));

        peer_lifecycle_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        bool ok = progress_store_open(dir);
        ok = ok && syncdiag_seed_lookahead_reducer_progress(served_height);
        ok = ok && connman_init(&cm, chain_params_get(), &sigs);
        main_state_init(&ms);
        block_index_init(&tip);
        syncdiag_set_hash(&h_tip, 0x81);
        tip.phashBlock = &h_tip;
        tip.nHeight = target_height;
        tip.nTime = (uint32_t)platform_time_wall_time_t();
        tip.nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_TREE;
        ok = ok && block_map_insert(&ms.map_block_index, tip.phashBlock,
                                    &tip);
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        ms.pindex_best_header = &tip;
        ok = ok && tip_finalize_stage_init(&ms);

        struct p2p_node *peer =
            syncdiag_add_peer(&cm, 47, false, PEER_HANDSHAKE_COMPLETE);
        ok = ok && peer != NULL;
        if (peer) {
            peer->starting_height = target_height;
            syncdiag_note_peer_lifecycle_active(
                peer, PEER_LIFECYCLE_SOURCE_ADDRMAN);
        }
        struct p2p_node flaky;
        memset(&flaky, 0, sizeof(flaky));
        syncdiag_set_ipv4(&flaky.addr, 149, 50, 116, 7, 20022);
        flaky.id = 404;
        flaky.state = PEER_CONNECTING;
        snprintf(flaky.addr_name, sizeof(flaky.addr_name),
                 "149.50.116.7:20022");
        peer_lifecycle_note_connected(&flaky,
                                      PEER_LIFECYCLE_SOURCE_ADDRMAN);
        peer_lifecycle_note_disconnected(&flaky, "cleanup");

        struct download_manager *dm = msg_get_download_mgr();
        dl_drain_for_backpressure(dm);
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();
        rpc_net_set_connman(&cm);
        sync_monitor_set_context(&cm, dm, &ms);
        reducer_frontier_provable_tip_set(served_height);
        sync_set_state(SYNC_IDLE, "diagnose lookahead");
        struct legacy_mirror_sync_stats mirror_stats = {0};
        mirror_stats.enabled = true;
        mirror_stats.running = true;
        mirror_stats.reachable = true;
        mirror_stats.legacy_height = target_height;
        mirror_stats.legacy_headers = target_height;
        mirror_stats.local_height = target_height;
        mirror_stats.best_header_height = target_height;
        mirror_stats.target_height = target_height;
        legacy_mirror_sync_test_set_stats(&mirror_stats, &ms);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose", &params,
                                     &result);

        const struct json_value *findings = json_get(&result, "findings");
        const struct json_value *chain_finding =
            find_object_with_str(findings, "name", "chain_serving");
        const struct json_value *peer_finding =
            find_object_with_str(findings, "name", "peer_lifecycle");
        const struct json_value *mirror_finding =
            find_object_with_str(findings, "name", "mirror");
        const struct json_value *default_first_call =
            json_get(&result, "first_call");
        const struct json_value *default_omitted =
            json_get(&result, "omitted_sections");
        const struct json_value *default_primary_host =
            json_get(&result, "peer_primary_host_issue");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.agent_diagnose.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "method")),
                          "agentdiagnose") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "native_command")),
                          "zclassic23 agentdiagnose") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "mcp_tool")),
                          "zcl_agent_diagnose") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "contract_source")),
                          "agent_contracts.def") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "detail_mode")),
                          "brief") == 0;
        ok = ok && !json_get_bool(json_get(&result,
                                           "embedded_drilldowns"));
        ok = ok && json_get_int(json_get(&result, "gap")) == 1;
        ok = ok && json_get_bool(json_get(&result,
                                          "chain_serving_ready"));
        ok = ok && json_get_bool(json_get(&result, "normal_lookahead"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "chain_readiness_status")),
                          "ready") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "height_contract_status")),
                          "normal_lookahead") == 0;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_incident_count")) == 1;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_host_incident_count")) >= 1;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_host_count_returned")) >= 1;
        ok = ok && default_primary_host != NULL;
        ok = ok && strcmp(json_get_str(json_get(default_primary_host,
                                                "object_completeness")),
                          "compact") == 0;
        ok = ok && strcmp(json_get_str(json_get(default_primary_host,
                                                "full_detail_command")),
                          "zclassic23 peerincidents") == 0;
        ok = ok && json_get(&result, "peer_primary_host") != NULL;
        ok = ok && json_get(&result,
                            "peer_primary_host_issue_class") != NULL;
        ok = ok && json_get(&result,
                            "peer_primary_host_next_action") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_incident_severity")),
                          "info") == 0;
        ok = ok && !json_get_bool(json_get(&result,
                                           "peer_stability_blocker"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_bootstrap_readiness")),
                          "ready") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_fast_sync_readiness")),
                          "ready") == 0;
        ok = ok && !json_get_bool(json_get(&result,
                                           "peer_bootstrap_blocker"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "peer_fast_sync_blocker"));
        ok = ok && json_get_int(json_get(&result,
                                         "peer_material_incident_count")) == 0;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_informational_incident_count"))
            == 1;
        ok = ok && strstr(json_get_str(json_get(&result,
                                                "peer_incident_summary")),
                          "minor peer lifecycle incidents") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&result, "verdict")),
                          "healthy") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_next_action")),
                          "monitor_agent_and_liveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "mirror_status")),
                          "healthy") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "mirror_severity")),
                          "ok") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "mirror_advisory_only"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "mirror_operator_action_required"));
        ok = ok && chain_finding && strcmp(json_get_str(json_get(
            chain_finding, "severity")), "ok") == 0;
        ok = ok && peer_finding && strcmp(json_get_str(json_get(
            peer_finding, "severity")), "info") == 0;
        ok = ok && mirror_finding && strcmp(json_get_str(json_get(
            mirror_finding, "severity")), "ok") == 0;
        ok = ok && json_get(&result, "agent") == NULL;
        ok = ok && json_get(&result, "healthcheck") == NULL;
        ok = ok && json_get(&result, "peer_incidents") == NULL;
        ok = ok && json_get(&result, "mirror") == NULL;
        ok = ok && json_get(&result, "timeline") == NULL;
        ok = ok && default_omitted &&
            json_array_has_str(default_omitted, "timeline");
        ok = ok && default_first_call &&
            strcmp(json_get_str(json_get(default_first_call, "source")),
                   "bounded_status_peer_mirror_brief") == 0;
        ok = ok && default_first_call &&
            strcmp(json_get_str(json_get(default_first_call,
                                         "full_mode_command")),
                   "zclassic23 agentdiagnose full") == 0;

        json_free(&result);

        struct json_value full_params;
        json_init(&full_params);
        json_set_array(&full_params);
        struct json_value full_arg;
        json_init(&full_arg);
        json_set_str(&full_arg, "full");
        json_push_back(&full_params, &full_arg);
        json_free(&full_arg);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose",
                                     &full_params, &result);
        const struct json_value *agent = json_get(&result, "agent");
        const struct json_value *height_contract =
            agent ? json_get(agent, "height_contract") : NULL;
        ok = ok && strcmp(json_get_str(json_get(&result, "detail_mode")),
                          "full") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "embedded_drilldowns"));
        ok = ok && agent != NULL;
        ok = ok && json_get(&result, "healthcheck") != NULL;
        ok = ok && json_get(&result, "peer_incidents") != NULL;
        ok = ok && json_get(&result, "mirror") != NULL;
        ok = ok && json_get(&result, "timeline") != NULL;
        ok = ok && height_contract && json_get_bool(json_get(
            height_contract, "normal_lookahead"));
        json_free(&result);
        json_free(&full_params);

        struct json_value bounded_health;
        json_init(&bounded_health);
        ok = ok && rpc_table_execute(&tbl, "healthcheck", &params,
                                     &bounded_health);
        const struct json_value *bounded_checks =
            json_get(&bounded_health, "checks");
        ok = ok && bounded_health.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&bounded_health,
                                                "height_contract_status")),
                          "normal_lookahead") == 0;
        ok = ok && json_get_bool(json_get(&bounded_health,
                                          "normal_lookahead"));
        ok = ok && json_get_bool(json_get(&bounded_health,
                                          "chain_serving_ready"));
        ok = ok && !json_get_bool(json_get(&bounded_health,
                                           "sync_fsm_at_tip"));
        ok = ok && bounded_checks && bounded_checks->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(bounded_checks,
                                                "height_contract_status")),
                          "normal_lookahead") == 0;
        ok = ok && json_get_bool(json_get(bounded_checks,
                                          "normal_lookahead"));
        ok = ok && json_get_bool(json_get(bounded_checks, "synced"));
        ok = ok && !json_get_bool(json_get(bounded_checks,
                                           "sync_fsm_at_tip"));
        ok = ok && json_get_bool(json_get(bounded_checks,
                                          "serving_ready"));
        json_free(&bounded_health);

        struct json_value brief_params;
        json_init(&brief_params);
        json_set_array(&brief_params);
        struct json_value brief_arg;
        json_init(&brief_arg);
        json_set_str(&brief_arg, "brief");
        json_push_back(&brief_params, &brief_arg);
        json_free(&brief_arg);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose",
                                     &brief_params, &result);
        const struct json_value *brief_first_call =
            json_get(&result, "first_call");
        const struct json_value *brief_omitted =
            json_get(&result, "omitted_sections");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.agent_diagnose.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "detail_mode")),
                          "brief") == 0;
        ok = ok && !json_get_bool(json_get(&result,
                                           "embedded_drilldowns"));
        ok = ok && strcmp(json_get_str(json_get(&result, "verdict")),
                          "healthy") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_next_action")),
                          "monitor_agent_and_liveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "mirror_status")),
                          "healthy") == 0;
        ok = ok && json_get(&result, "agent") == NULL;
        ok = ok && json_get(&result, "healthcheck") == NULL;
        ok = ok && json_get(&result, "peer_incidents") == NULL;
        ok = ok && json_get(&result, "mirror") == NULL;
        ok = ok && json_get(&result, "timeline") == NULL;
        ok = ok && brief_omitted &&
            json_array_has_str(brief_omitted, "timeline");
        ok = ok && brief_first_call &&
            strcmp(json_get_str(json_get(brief_first_call, "source")),
                   "bounded_status_peer_mirror_brief") == 0;
        ok = ok && brief_first_call &&
            strcmp(json_get_str(json_get(brief_first_call,
                                         "full_mode_command")),
                   "zclassic23 agentdiagnose full") == 0;
        json_free(&result);
        json_free(&brief_params);

        peer_lifecycle_reset_for_test();
        struct p2p_node limited_peer;
        memset(&limited_peer, 0, sizeof(limited_peer));
        syncdiag_set_ipv4(&limited_peer.addr, 203, 0, 113, 88, 8033);
        limited_peer.id = 407;
        limited_peer.state = PEER_HANDSHAKE_COMPLETE;
        limited_peer.services = 0;
        limited_peer.starting_height = target_height;
        snprintf(limited_peer.addr_name, sizeof(limited_peer.addr_name),
                 "203.0.113.88:8033");
        snprintf(limited_peer.sub_ver, sizeof(limited_peer.sub_ver),
                 "%s", "/LimitedPeer:0.1.0/");
        syncdiag_note_peer_lifecycle_active(
            &limited_peer, PEER_LIFECYCLE_SOURCE_ADDRMAN);

        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose", &params,
                                     &result);
        findings = json_get(&result, "findings");
        peer_finding = find_object_with_str(findings, "name",
                                            "peer_lifecycle");
        ok = ok && strcmp(json_get_str(json_get(&result, "verdict")),
                          "attention_needed") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_next_action")),
                          "inspect_peer_lifecycle_bootstrap_readiness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_bootstrap_readiness")),
                          "no_bootstrap_useful_peer") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_fast_sync_readiness")),
                          "no_bootstrap_useful_peer") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "peer_bootstrap_blocker"));
        ok = ok && json_get_bool(json_get(&result,
                                          "peer_fast_sync_blocker"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_incident_severity")),
                          "attention") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "peer_stability_blocker"));
        ok = ok && strstr(json_get_str(json_get(&result,
                                                "peer_incident_summary")),
                          "no currently bootstrap-useful peer") != NULL;
        ok = ok && peer_finding && strcmp(json_get_str(json_get(
            peer_finding, "severity")), "attention") == 0;
        ok = ok && peer_finding && strcmp(json_get_str(json_get(
            peer_finding, "next_action")),
            "inspect_peer_lifecycle_bootstrap_readiness") == 0;
        json_free(&result);

        peer_lifecycle_reset_for_test();
        if (peer)
            syncdiag_note_peer_lifecycle_active(
                peer, PEER_LIFECYCLE_SOURCE_ADDRMAN);

        tip.nHeight = served_height + 2;
        ms.pindex_best_header = &tip;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        if (peer)
            peer->starting_height = target_height;
        mirror_stats.legacy_height = tip.nHeight;
        mirror_stats.legacy_headers = tip.nHeight;
        mirror_stats.local_height = tip.nHeight;
        mirror_stats.best_header_height = tip.nHeight;
        mirror_stats.target_height = tip.nHeight;
        legacy_mirror_sync_test_set_stats(&mirror_stats, &ms);

        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose", &params,
                                     &result);
        findings = json_get(&result, "findings");
        chain_finding = find_object_with_str(findings, "name",
                                             "chain_serving");
        ok = ok && json_get_int(json_get(&result, "gap")) == 2;
        ok = ok && json_get_bool(json_get(&result,
                                          "chain_serving_ready"));
        ok = ok && !json_get_bool(json_get(&result, "normal_lookahead"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "chain_readiness_status")),
                          "ready") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "height_contract_status")),
                          "minor_lag") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "verdict")),
                          "healthy") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_next_action")),
                          "monitor_agent_and_liveness") == 0;
        ok = ok && chain_finding && strcmp(json_get_str(json_get(
            chain_finding, "severity")), "ok") == 0;

        json_free(&result);

        mirror_stats.reachable = false;
        legacy_mirror_sync_test_set_stats(&mirror_stats, &ms);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose", &params,
                                     &result);
        findings = json_get(&result, "findings");
        mirror_finding = find_object_with_str(findings, "name",
                                              "mirror");
        ok = ok && strcmp(json_get_str(json_get(&result, "verdict")),
                          "healthy") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_next_action")),
                          "monitor_agent_and_liveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "mirror_status")),
                          "observing") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "mirror_severity")),
                          "info") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "mirror_advisory_only"));
        ok = ok && !json_get_bool(json_get(&result,
                                           "mirror_operator_action_required"));
        ok = ok && mirror_finding && strcmp(json_get_str(json_get(
            mirror_finding, "severity")), "info") == 0;

        json_free(&result);

        mirror_stats.reachable = true;
        legacy_mirror_sync_test_set_stats(&mirror_stats, &ms);
        peer_lifecycle_reset_for_test();
        struct p2p_node zigma_a;
        struct p2p_node zigma_b;
        memset(&zigma_a, 0, sizeof(zigma_a));
        syncdiag_set_ipv4(&zigma_a.addr, 40, 160, 53, 56, 45474);
        zigma_a.id = 405;
        zigma_a.inbound = true;
        zigma_a.state = PEER_HANDSHAKE_COMPLETE;
        zigma_a.services = NODE_NETWORK;
        snprintf(zigma_a.addr_name, sizeof(zigma_a.addr_name),
                 "40.160.53.56:45474");
        snprintf(zigma_a.sub_ver, sizeof(zigma_a.sub_ver),
                 "%s", "/Zigma:0.1.0/");
        peer_lifecycle_note_connected(&zigma_a,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_received(&zigma_a, zigma_a.services,
                                             target_height,
                                             zigma_a.sub_ver);
        peer_lifecycle_note_handshake_complete(&zigma_a);
        peer_lifecycle_note_active(&zigma_a);
        peer_lifecycle_note_disconnected(&zigma_a, "cleanup");
        peer_lifecycle_note_connected(&zigma_a,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_received(&zigma_a, zigma_a.services,
                                             target_height,
                                             zigma_a.sub_ver);
        peer_lifecycle_note_handshake_complete(&zigma_a);
        peer_lifecycle_note_active(&zigma_a);

        memset(&zigma_b, 0, sizeof(zigma_b));
        syncdiag_set_ipv4(&zigma_b.addr, 40, 160, 53, 56, 39030);
        zigma_b.id = 406;
        zigma_b.inbound = true;
        zigma_b.state = PEER_CONNECTING;
        zigma_b.services = NODE_NETWORK;
        snprintf(zigma_b.addr_name, sizeof(zigma_b.addr_name),
                 "40.160.53.56:39030");
        snprintf(zigma_b.sub_ver, sizeof(zigma_b.sub_ver),
                 "%s", "/Zigma:0.1.0/");
        peer_lifecycle_note_connected(&zigma_b,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_timeout(&zigma_b, "handshake_timeout");

        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose", &params,
                                     &result);
        findings = json_get(&result, "findings");
        peer_finding = find_object_with_str(findings, "name",
                                            "peer_lifecycle");
        ok = ok && strcmp(json_get_str(json_get(&result, "verdict")),
                          "attention_needed") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_next_action")),
                          "inspect_peer_timeline_for_reconnect_timeouts") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_incident_severity")),
                          "attention") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "peer_stability_blocker"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_bootstrap_readiness")),
                          "ready") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_fast_sync_readiness")),
                          "no_zclassic23_fast_sync_peer") == 0;
        ok = ok && !json_get_bool(json_get(&result,
                                           "peer_bootstrap_blocker"));
        ok = ok && json_get_bool(json_get(&result,
                                          "peer_fast_sync_blocker"));
        ok = ok && json_get_int(json_get(&result,
                                         "duplicate_host_group_count")) == 1;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_host_incident_count")) == 1;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_host_count_returned")) == 1;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_primary_host")),
                          "40.160.53.56") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_primary_host_issue_class")),
                          "reconnect_timeout_pressure") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_primary_host_next_action")),
                          "inspect_peer_timeline_for_reconnect_timeouts") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "peer_primary_host_direction")),
                          "inbound") == 0;
        ok = ok && !json_get_bool(json_get(&result,
                                           "peer_primary_host_mixed_direction"));
        ok = ok && strcmp(json_get_str(json_get(&result,
            "peer_primary_host_bootstrap_readiness")),
            "useful") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
            "peer_primary_host_fast_sync_readiness")),
            "missing_zclassic23_fast_sync") == 0;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_primary_host_incident_score"))
            > 0;
        const struct json_value *primary_host_issue =
            json_get(&result, "peer_primary_host_issue");
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue, "schema")),
                   "zcl.peer_primary_host_issue.v1") == 0;
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue, "host")),
                   "40.160.53.56") == 0;
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue, "next_action")),
                   "inspect_peer_timeline_for_reconnect_timeouts") == 0;
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue, "direction")),
                   "inbound") == 0;
        ok = ok && primary_host_issue &&
            !json_get_bool(json_get(primary_host_issue, "mixed_direction"));
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue,
                                         "object_completeness")),
                   "compact") == 0;
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue,
                                         "full_detail_command")),
                   "zclassic23 peerincidents") == 0;
        ok = ok && primary_host_issue &&
            json_get(primary_host_issue, "current_open_direction") == NULL;
        ok = ok && primary_host_issue &&
            json_get(primary_host_issue, "current_handshaked_direction") == NULL;
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue,
                                         "bootstrap_readiness")),
                   "useful") == 0;
        ok = ok && primary_host_issue &&
            strcmp(json_get_str(json_get(primary_host_issue,
                                         "fast_sync_readiness")),
                   "missing_zclassic23_fast_sync") == 0;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_material_incident_count")) >= 1;
        ok = ok && json_get_int(json_get(&result,
                                         "peer_material_group_count")) >= 1;
        ok = ok && peer_finding && strcmp(json_get_str(json_get(
            peer_finding, "severity")), "attention") == 0;
        ok = ok && peer_finding && strcmp(json_get_str(json_get(
            peer_finding, "next_action")),
            "inspect_peer_timeline_for_reconnect_timeouts") == 0;

        json_free(&result);

        peer_lifecycle_reset_for_test();
        if (peer)
            syncdiag_note_peer_lifecycle_active(
                peer, PEER_LIFECYCLE_SOURCE_ADDRMAN);
        memset(&mirror_stats, 0, sizeof(mirror_stats));
        mirror_stats.enabled = true;
        mirror_stats.running = true;
        mirror_stats.reachable = true;
        mirror_stats.legacy_height = target_height;
        mirror_stats.legacy_headers = target_height;
        mirror_stats.local_height = target_height;
        mirror_stats.best_header_height = target_height;
        mirror_stats.target_height = target_height;
        snprintf(mirror_stats.last_blocker_id,
                 sizeof(mirror_stats.last_blocker_id),
                 "hash-disagreement");
        mirror_stats.last_blocker_class = BLOCKER_TRANSIENT;
        legacy_mirror_sync_test_set_stats(&mirror_stats, &ms);

        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose", &params,
                                     &result);
        findings = json_get(&result, "findings");
        mirror_finding = find_object_with_str(findings, "name",
                                              "mirror");
        ok = ok && strcmp(json_get_str(json_get(&result, "verdict")),
                          "attention_needed") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "safe_next_action")),
                          "inspect_mirror_status") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "mirror_status")),
                          "blocked") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "mirror_severity")),
                          "attention") == 0;
        ok = ok && json_get_bool(json_get(&result,
                                          "mirror_operator_action_required"));
        ok = ok && mirror_finding && strcmp(json_get_str(json_get(
            mirror_finding, "severity")), "attention") == 0;

        json_free(&params);
        json_free(&result);
        dl_drain_for_backpressure(dm);
        sync_monitor_set_context(NULL, NULL, NULL);
        rpc_net_set_connman(NULL);
        reducer_frontier_provable_tip_reset();
        tip_finalize_stage_shutdown();
        progress_store_close();
        block_source_policy_reset_for_test();
        peer_lifecycle_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        main_state_free(&ms);
        connman_free(&cm);
        test_cleanup_tmpdir(dir);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC returns agent code map... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool ok = rpc_table_execute(&tbl, "agentmap", &params, &result);
        const struct json_value *commands = json_get(&result, "commands");
        const struct json_value *telemetry =
            json_get(&result, "telemetry_drilldowns");
        const struct json_value *subsystems = json_get(&result, "subsystems");
        const struct json_value *shim = json_get(&result, "deprecated_shim");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.agent_map.v1") == 0;
        ok = ok &&
            agent_contract_command_surface_count("agentmap.commands.core") ==
                12;
        ok = ok &&
            agent_contract_command_surface_count(
                "agentmap.commands.drilldown") == 6;
        ok = ok &&
            agent_contract_command_surface_count("agentmap.telemetry") == 11;
        ok = ok &&
            agent_contract_command_surface_count("missing.surface") == 0;
        ok = ok && commands && commands->type == JSON_ARR;
        ok = ok && find_object_with_str(commands, "method", "agentmap") != NULL;
        ok = ok &&
            find_object_with_str(commands, "method", "agentdeployguard") != NULL;
        ok = ok && find_object_with_str(commands, "name", "impact") != NULL;
        ok = ok && find_object_with_str(commands, "name", "build") != NULL;
        ok = ok && find_object_with_str(commands, "method", "healthcheck")
                         != NULL;
        ok = ok && find_object_with_str(commands, "method", "statecatalog")
                         != NULL;
        ok = ok && find_object_with_str(commands, "method", "peerincidents")
                         != NULL;
        const struct json_value *map_command_center =
            find_object_with_str(commands, "name", "command_center");
        ok = ok && map_command_center &&
            strcmp(json_get_str(json_get(map_command_center, "native")),
                   "zclassic23 agent") == 0;
        ok = ok && map_command_center &&
            strcmp(json_get_str(json_get(map_command_center, "mcp")),
                   "zcl_operator_summary") == 0;
        const struct json_value *map_background_quality =
            find_object_with_str(commands, "name", "background_quality");
        ok = ok && map_background_quality &&
            strcmp(json_get_str(json_get(map_background_quality, "native")),
                   "make quality-linger-status") == 0;
        ok = ok && map_background_quality &&
            strcmp(json_get_str(json_get(map_background_quality, "mcp")),
                   "zcl_agent_build") == 0;
        const struct json_value *map_state =
            find_object_with_str(commands, "method", "dumpstate");
        const struct json_value *map_log =
            find_object_with_str(commands, "method", "getnodelog");
        ok = ok && map_state &&
            strcmp(json_get_str(json_get(map_state, "mcp")),
                   "zcl_state") == 0;
        ok = ok && map_log &&
            strcmp(json_get_str(json_get(map_log, "mcp")),
                   "zcl_node_log") == 0;
        ok = ok && telemetry && telemetry->type == JSON_ARR;
        const struct json_value *map_peer_incidents =
            find_object_with_str(telemetry, "method", "peerincidents");
        ok = ok && map_peer_incidents &&
            strcmp(json_get_str(json_get(map_peer_incidents, "mcp")),
                   "zcl_peer_incidents") == 0;
        const struct json_value *map_summary =
            find_object_with_str(telemetry, "name", "summary");
        ok = ok && map_summary &&
            strcmp(json_get_str(json_get(map_summary, "mcp")),
                   "zcl_operator_summary") == 0;
        const struct json_value *map_full_status =
            find_object_with_str(telemetry, "name", "full_status");
        ok = ok && map_full_status &&
            strcmp(json_get_str(json_get(map_full_status, "native")),
                   "zclassic23 healthcheck") == 0;
        ok = ok && map_full_status &&
            strcmp(json_get_str(json_get(map_full_status, "mcp")),
                   "zcl_status") == 0;
        ok = ok &&
            find_object_with_str(telemetry, "name", "node_log") != NULL;
        ok = ok &&
            find_object_with_str(telemetry, "method", "anchorstatus") != NULL;
        const struct json_value *map_db =
            find_object_with_str(telemetry, "method", "dbquery");
        ok = ok && map_db &&
            strcmp(json_get_str(json_get(map_db, "schema")),
                   "zcl.sql_result.v1") == 0;
        ok = ok && map_db &&
            strcmp(json_get_str(json_get(map_db, "native")),
                   "zclassic23 dbquery <SELECT>") == 0;
        ok = ok && map_db &&
            strcmp(json_get_str(json_get(map_db, "mcp")),
                   "zcl_sql") == 0;
        const struct json_value *map_events =
            find_object_with_str(telemetry, "method", "eventlog");
        ok = ok && map_events &&
            strcmp(json_get_str(json_get(map_events, "schema")),
                   "zcl.event_log.v1") == 0;
        ok = ok && map_events &&
            strcmp(json_get_str(json_get(map_events, "native")),
                   "zclassic23 eventlog <count>") == 0;
        ok = ok && map_events &&
            strcmp(json_get_str(json_get(map_events, "mcp")),
                   "zcl_events") == 0;
        const struct json_value *map_quality_lanes =
            find_object_with_str(telemetry, "name", "quality_lanes");
        ok = ok && map_quality_lanes &&
            strcmp(json_get_str(json_get(map_quality_lanes, "native")),
                   "make quality-linger-status") == 0;
        ok = ok && map_quality_lanes &&
            strcmp(json_get_str(json_get(map_quality_lanes, "mcp")),
                   "zcl_agent_build") == 0;
        ok = ok && subsystems && subsystems->type == JSON_ARR;
        ok = ok && find_object_with_str(subsystems, "name", "fast_ci") != NULL;
        ok = ok && shim && shim->type == JSON_OBJ;
        ok = ok && !json_get_bool(json_get(shim, "primary"));
        ok = ok && strcmp(json_get_str(json_get(shim, "status")),
                          "deprecated_compatibility_shim") == 0;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC maps changed files to tests... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);
        const char *files[] = {
            "app/controllers/src/agent_controller.c",
            "app/controllers/src/agent_lanes_controller.c",
            "app/controllers/src/event_healthcheck_controller.c",
            "app/controllers/include/controllers/event_healthcheck_controller.h",
            "tools/mcp/controllers/ops_controller.c",
            "docs/AGENT_API.md",
        };
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            struct json_value v;
            json_init(&v);
            json_set_str(&v, files[i]);
            json_push_back(&params, &v);
            json_free(&v);
        }

        struct json_value result;
        json_init(&result);

        bool ok = rpc_table_execute(&tbl, "agentimpact", &params, &result);
        const struct json_value *groups =
            json_get(&result, "relevant_test_groups");
        const struct json_value *commands =
            json_get(&result, "recommended_commands");
        const struct json_value *out_files = json_get(&result, "files");
        ok = ok && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.agent_impact.v1") == 0;
        ok = ok && json_get_int(json_get(&result, "files_count")) == 6;
        ok = ok && json_get_bool(json_get(&result, "code_changed"));
        ok = ok && !json_get_bool(json_get(&result, "docs_only"));
        ok = ok && json_get_bool(json_get(&result, "agent_api_changed"));
        ok = ok && json_get_bool(json_get(&result, "mcp_changed"));
        ok = ok && strcmp(json_get_str(json_get(&result, "mapping_source")),
                          "app/controllers/include/controllers/agent_impact_rules.def") == 0;
        ok = ok && json_get_int(json_get(&result, "shared_rule_count")) > 0;
        ok = ok && json_get_int(json_get(&result, "shared_rule_hits")) >= 6;
        ok = ok && json_get_int(json_get(
                     &result, "relevant_test_groups_count")) >= 3;
        ok = ok && out_files && json_size(out_files) == 6;
        ok = ok && json_array_has_str(groups, "syncdiag_rpc");
        ok = ok && json_array_has_str(groups, "mcp_controllers");
        ok = ok && json_array_has_str(groups, "make_lint_gates");
        ok = ok && json_array_has_substr(commands,
                                         "ZCL_FAST_TESTS=syncdiag_rpc");
        ok = ok && json_array_has_substr(commands, "make fast-ci");

        if (ok) {
            printf("OK\n");
        } else {
            char dbg[4096];
            json_write(&result, dbg, sizeof(dbg));
            printf("FAIL result=%s\n", dbg);
            failures++;
        }

        json_free(&params);
        json_free(&result);
    }

    printf("api: native RPC returns agent contracts and build contract... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        register_diagnostics_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value contracts;
        json_init(&contracts);
        bool ok = rpc_table_execute(&tbl, "agentcontracts",
                                    &params, &contracts);
        const struct json_value *schemas = json_get(&contracts, "schemas");
        const struct json_value *contract_list =
            json_get(&contracts, "contracts");
        const struct json_value *contract_summary =
            json_get(&contracts, "contract_summary");
        const struct json_value *transports =
            json_get(&contracts, "transports");
        const struct json_value *contract_agentops =
            find_object_with_str(contract_list, "method", "agentops");
        const struct json_value *contract_diagnose =
            find_object_with_str(contract_list, "method", "agentdiagnose");
        const struct json_value *contract_api =
            find_object_with_str(contract_list, "method", "api");
        const struct json_value *contract_app_protocols =
            find_object_with_str(contract_list, "method", "appprotocols");
        const struct json_value *contract_service_catalog =
            find_object_with_str(contract_list, "method", "servicecatalog");
        const struct json_value *contract_status =
            find_object_with_str(contract_list, "method", "status");
        const struct json_value *contract_dumpstate =
            find_object_with_str(contract_list, "method", "dumpstate");
        const struct json_value *contract_getnodelog =
            find_object_with_str(contract_list, "method", "getnodelog");
        const struct json_value *contract_dbquery =
            find_object_with_str(contract_list, "method", "dbquery");
        const struct json_value *contract_eventlog =
            find_object_with_str(contract_list, "method", "eventlog");
        const struct json_value *contract_healthcheck =
            find_object_with_str(contract_list, "method", "healthcheck");
        const struct json_value *contract_milestone =
            find_object_with_str(contract_list, "method", "milestone");
        const struct json_value *contract_refold =
            find_object_with_str(contract_list, "method", "refold");
        const struct json_value *contract_peerincidents =
            find_object_with_str(contract_list, "method", "peerincidents");
        ok = ok && contracts.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&contracts, "schema")),
                          "zcl.agent_contracts.v1") == 0;
        ok = ok && contract_list && contract_list->type == JSON_ARR;
        ok = ok && contract_summary &&
            json_get_int(json_get(contract_summary, "contract_count")) >= 20;
        ok = ok && contract_summary &&
            json_get_int(json_get(contract_summary, "mcp_declared_count"))
                >= 20;
        ok = ok && contract_summary &&
            strcmp(json_get_str(json_get(contract_summary,
                                         "registry_source")),
                   "agent_contracts.def + agent_contract_registry.c") == 0;
        ok = ok && contract_summary &&
            strcmp(json_get_str(json_get(contract_summary,
                                         "schema_registry_source")),
                   "agent_contract_schema_registry.c") == 0;
        ok = ok && contract_summary &&
            strcmp(json_get_str(json_get(contract_summary,
                                         "review_registry_source")),
                   "agent_contract_review_registry.c") == 0;
        ok = ok && contract_summary &&
            json_get_int(json_get(contract_summary,
                                  "schema_surface_count")) ==
                (int64_t)agent_contract_schema_surface_count();
        ok = ok && contract_summary &&
            json_get_int(json_get(contract_summary,
                                  "review_surface_count")) ==
                (int64_t)agent_contract_review_surface_total_count();
        ok = ok && agent_contract_schema_surface_count() >= 18;
        ok = ok && agent_contract_review_surface_total_count() >= 5;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops, "schema")),
                   "zcl.agent_ops.v1") == 0;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops, "native")),
                   "zclassic23 agentops") == 0;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops, "mcp")),
                   "zcl_agent_ops") == 0;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops,
                                         "api_cli_field")),
                   "ops_command") == 0;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops,
                                         "api_mcp_field")),
                   "ops_tool") == 0;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops,
                                         "ops_surface")),
                   "direct") == 0;
        ok = ok && contract_agentops &&
            json_get_int(json_get(contract_agentops, "ops_rank")) == 1;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops, "ops_name")),
                   "no_jq_contract") == 0;
        ok = ok && contract_agentops &&
            strcmp(json_get_str(json_get(contract_agentops,
                                         "ops_purpose")),
                   "compact top-level fields for common agent decisions") == 0;
        ok = ok && contract_status &&
            strcmp(json_get_str(json_get(contract_status, "capability")),
                   "runtime_status_alias") == 0;
        ok = ok && contract_status &&
            strcmp(json_get_str(json_get(contract_status, "schema")),
                   "zcl.public_status.v1") == 0;
        ok = ok && contract_status &&
            strcmp(json_get_str(json_get(contract_status, "native")),
                   "zclassic23 status") == 0;
        ok = ok && contract_status &&
            strcmp(json_get_str(json_get(contract_status, "mcp")),
                   "zcl_agent") == 0;
        ok = ok && contract_status &&
            strcmp(json_get_str(json_get(contract_status, "rest")),
                   "GET /api/v1/agent") == 0;
        ok = ok && contract_diagnose &&
            strcmp(json_get_str(json_get(contract_diagnose, "schema")),
                   "zcl.agent_diagnose.v1") == 0;
        ok = ok && contract_diagnose &&
            strcmp(json_get_str(json_get(contract_diagnose, "mcp")),
                   "zcl_agent_diagnose") == 0;
        ok = ok && contract_api &&
            strcmp(json_get_str(json_get(contract_api, "api_cli_field")),
                   "api_command") == 0;
        ok = ok && contract_api &&
            strcmp(json_get_str(json_get(contract_api, "api_mcp_field")),
                   "api_tool") == 0;
        ok = ok && contract_app_protocols &&
            strcmp(json_get_str(json_get(contract_app_protocols, "schema")),
                   "zcl.application_protocols.index.v1") == 0;
        ok = ok && contract_app_protocols &&
            strcmp(json_get_str(json_get(contract_app_protocols, "native")),
                   "zclassic23 appprotocols") == 0;
        ok = ok && contract_app_protocols &&
            strcmp(json_get_str(json_get(contract_app_protocols, "mcp")),
                   "zcl_app_protocols") == 0;
        ok = ok && contract_app_protocols &&
            strcmp(json_get_str(json_get(contract_app_protocols, "rest")),
                   "GET /api/v1/protocols") == 0;
        ok = ok && contract_app_protocols &&
            strcmp(json_get_str(json_get(contract_app_protocols,
                                         "api_cli_field")),
                   "app_protocols_command") == 0;
        ok = ok && contract_app_protocols &&
            strcmp(json_get_str(json_get(contract_app_protocols,
                                         "api_mcp_field")),
                   "app_protocols_tool") == 0;
        ok = ok && contract_service_catalog &&
            strcmp(json_get_str(json_get(contract_service_catalog, "schema")),
                   "zcl.service_catalog.v1") == 0;
        ok = ok && contract_service_catalog &&
            strcmp(json_get_str(json_get(contract_service_catalog, "native")),
                   "zclassic23 servicecatalog") == 0;
        ok = ok && contract_service_catalog &&
            strcmp(json_get_str(json_get(contract_service_catalog, "mcp")),
                   "zcl_service_catalog") == 0;
        ok = ok && contract_service_catalog &&
            strcmp(json_get_str(json_get(contract_service_catalog, "rest")),
                   "GET /api/v1/service-catalog") == 0;
        ok = ok && contract_service_catalog &&
            strcmp(json_get_str(json_get(contract_service_catalog,
                                         "api_cli_field")),
                   "service_catalog_command") == 0;
        ok = ok && contract_service_catalog &&
            strcmp(json_get_str(json_get(contract_service_catalog,
                                         "api_mcp_field")),
                   "service_catalog_tool") == 0;
        ok = ok && contract_dumpstate &&
            strcmp(json_get_str(json_get(contract_dumpstate, "native")),
                   "zclassic23 dumpstate <subsystem> [key]") == 0;
        ok = ok && contract_dumpstate &&
            strcmp(json_get_str(json_get(contract_dumpstate, "mcp")),
                   "zcl_state") == 0;
        ok = ok && contract_dumpstate &&
            strcmp(json_get_str(json_get(contract_dumpstate,
                                         "ops_name")),
                   "state_drilldown") == 0;
        ok = ok && contract_getnodelog &&
            strcmp(json_get_str(json_get(contract_getnodelog, "native")),
                   "zclassic23 getnodelog <pattern>") == 0;
        ok = ok && contract_getnodelog &&
            strcmp(json_get_str(json_get(contract_getnodelog, "mcp")),
                   "zcl_node_log") == 0;
        ok = ok && contract_dbquery &&
            strcmp(json_get_str(json_get(contract_dbquery, "schema")),
                   "zcl.sql_result.v1") == 0;
        ok = ok && contract_dbquery &&
            strcmp(json_get_str(json_get(contract_dbquery, "native")),
                   "zclassic23 dbquery <SELECT>") == 0;
        ok = ok && contract_dbquery &&
            strcmp(json_get_str(json_get(contract_dbquery, "mcp")),
                   "zcl_sql") == 0;
        ok = ok && contract_dbquery &&
            strstr(json_get_str(json_get(contract_dbquery,
                                         "probe_params_json")),
                   "sqlite_master") != NULL;
        ok = ok && strcmp(agent_contract_probe_params_json("agent"), "[]")
                         == 0;
        ok = ok && strstr(agent_contract_probe_params_json("dbquery"),
                          "sqlite_master") != NULL;
        ok = ok && contract_eventlog &&
            strcmp(json_get_str(json_get(contract_eventlog, "schema")),
                   "zcl.event_log.v1") == 0;
        ok = ok && contract_eventlog &&
            strcmp(json_get_str(json_get(contract_eventlog, "native")),
                   "zclassic23 eventlog <count>") == 0;
        ok = ok && contract_eventlog &&
            strcmp(json_get_str(json_get(contract_eventlog, "mcp")),
                   "zcl_events") == 0;
        ok = ok && contract_eventlog &&
            strcmp(json_get_str(json_get(contract_eventlog,
                                         "probe_params_json")),
                   "[1]") == 0;
        ok = ok && strcmp(agent_contract_probe_params_json("eventlog"), "[1]")
                         == 0;
        ok = ok && contract_healthcheck &&
            strcmp(json_get_str(json_get(contract_healthcheck, "schema")),
                   "zcl.healthcheck.v1") == 0;
        ok = ok && contract_healthcheck &&
            strcmp(json_get_str(json_get(contract_healthcheck, "mcp")),
                   "zcl_health") == 0;
        ok = ok && contract_healthcheck &&
            strcmp(json_get_str(json_get(contract_healthcheck,
                                         "api_cli_field")),
                   "drilldown_command") == 0;
        ok = ok && contract_healthcheck &&
            strcmp(json_get_str(json_get(contract_healthcheck,
                                         "api_mcp_field")),
                   "drilldown_tool") == 0;
        ok = ok && contract_milestone &&
            strcmp(json_get_str(json_get(contract_milestone, "rest")),
                   "GET /api/v1/milestone") == 0;
        ok = ok && contract_milestone &&
            strcmp(json_get_str(json_get(contract_milestone, "mcp")),
                   "zcl_milestone") == 0;
        ok = ok && contract_refold &&
            strcmp(json_get_str(json_get(contract_refold, "rest")),
                   "GET /api/v1/refold") == 0;
        ok = ok && contract_refold &&
            strcmp(json_get_str(json_get(contract_refold, "mcp")),
                   "zcl_refold_status") == 0;
        ok = ok && contract_peerincidents &&
            strcmp(json_get_str(json_get(contract_peerincidents, "schema")),
                   "zcl.peer_incidents.v1") == 0;
        ok = ok && contract_peerincidents &&
            strcmp(json_get_str(json_get(contract_peerincidents, "native")),
                   "zclassic23 peerincidents") == 0;
        ok = ok && contract_peerincidents &&
            strcmp(json_get_str(json_get(contract_peerincidents, "mcp")),
                   "zcl_peer_incidents") == 0;
        ok = ok && contract_peerincidents &&
            strcmp(json_get_str(json_get(contract_peerincidents,
                                         "api_cli_field")),
                   "peer_incidents_command") == 0;
        ok = ok && contract_peerincidents &&
            strcmp(json_get_str(json_get(contract_peerincidents,
                                         "api_mcp_field")),
                   "peer_incidents_tool") == 0;
        ok = ok && contract_peerincidents &&
            strcmp(json_get_str(json_get(contract_peerincidents,
                                         "ops_name")),
                   "peer_incidents") == 0;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_build.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.peer_incidents.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.background_quality_runtime.v1")
            != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_readiness.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.runtime_build.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_runtime_availability.v1")
            != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.operator_latch.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.service_catalog.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.service_contract.v1") != NULL;
        ok = ok &&
            find_object_with_str(schemas, "schema",
                                 "zcl.condition_engine_summary.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_interface.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_ops.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_diagnose.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.timeline.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.state_catalog.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "subsystem-specific zcl_state JSON")
            != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.node_log.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.sql_result.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.event_log.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.healthcheck.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.milestone_status.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.refold_status.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_lanes.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_liveness.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_runtime_services.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_capability.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_machine_contract.v1")
            != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.agent_deploy_guard.v1") != NULL;
        ok = ok && find_object_with_str(schemas, "schema",
                                        "zcl.operator_lane.v1") != NULL;
        ok = ok &&
            find_object_with_str(schemas, "schema",
                                 "zcl.operator_deployment_safety.v1") != NULL;
        ok = ok && json_array_has_substr(transports, "zcl_agent_build");
        ok = ok && json_array_has_substr(transports,
                                         "zclassic23 agentops");
        ok = ok && json_array_has_substr(transports, "zcl_agent_ops");
        ok = ok && json_array_has_substr(transports, "zcl_agent_diagnose");
        ok = ok && json_array_has_substr(transports, "zcl_app_protocols");
        ok = ok && json_array_has_substr(transports, "zcl_service_catalog");
        ok = ok && json_array_has_substr(transports, "zcl_state_catalog");
        ok = ok && json_array_has_substr(transports, "zcl_state");
        ok = ok && json_array_has_substr(transports, "zcl_node_log");
        ok = ok && json_array_has_substr(transports, "zcl_sql");
        ok = ok && json_array_has_substr(transports, "zcl_events");
        ok = ok && json_array_has_substr(transports, "zcl_timeline");
        ok = ok && json_array_has_substr(transports, "zcl_health");
        ok = ok && json_array_has_substr(transports, "zcl_milestone");
        ok = ok && json_array_has_substr(transports, "zcl_refold_status");
        ok = ok && json_array_has_substr(transports, "zclassic23 status");

        struct json_value ops;
        json_init(&ops);
        agent_runtime_availability_reset();
        ok = ok && rpc_table_execute(&tbl, "agentops", &params, &ops);
        const struct json_value *ops_work = json_get(&ops, "top_next_work");
        const struct json_value *ops_gaps = json_get(&ops, "api_gaps");
        const struct json_value *ops_availability =
            json_get(&ops, "runtime_availability");
        const struct json_value *ops_availability_methods =
            ops_availability ? json_get(ops_availability, "methods") : NULL;
        const struct json_value *ops_direct_commands =
            json_get(&ops, "direct_commands");
        const struct json_value *ops_method_agentops =
            find_object_with_str(ops_availability_methods, "method",
                                 "agentops");
        const struct json_value *ops_direct_agentops =
            find_object_with_str(ops_direct_commands, "method", "agentops");
        ok = ok && ops.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&ops, "schema")),
                          "zcl.agent_ops.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops, "method")),
                          "agentops") == 0;
        ok = ok && json_get_bool(json_get(&ops, "no_jq_required"));
        ok = ok && strcmp(json_get_str(json_get(&ops, "native_command")),
                          "zclassic23 agentops") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops, "mcp_tool")),
                          "zcl_agent_ops") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops, "contract_source")),
                          "agent_contracts.def") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "diagnostics_catalog_command")),
                          "zclassic23 statecatalog") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "diagnostics_catalog_tool")),
                          "zcl_state_catalog") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops, "timeline_tool")),
                          "zcl_timeline") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "diagnose_command")),
                          "zclassic23 agentdiagnose") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops, "diagnose_tool")),
                          "zcl_agent_diagnose") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "liveness_tool")),
                          "zcl_agent_liveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "diagnostics_drilldown_tool")),
                          "zcl_state") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "anchor_status_command")),
                          "zclassic23 anchorstatus [-datadir=<anchor-datadir>]")
            == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "peer_incidents_command")),
                          "zclassic23 peerincidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(&ops,
                                                "peer_incidents_tool")),
                          "zcl_peer_incidents") == 0;
        ok = ok && ops_direct_agentops &&
            strcmp(json_get_str(json_get(ops_direct_agentops, "schema")),
                   "zcl.agent_ops.v1") == 0;
        ok = ok && ops_direct_agentops &&
            strcmp(json_get_str(json_get(ops_direct_agentops, "native")),
                   "zclassic23 agentops") == 0;
        ok = ok && ops_direct_agentops &&
            strcmp(json_get_str(json_get(ops_direct_agentops, "mcp")),
                   "zcl_agent_ops") == 0;
        const struct json_value *ops_direct_diagnose =
            find_object_with_str(ops_direct_commands, "method",
                                 "agentdiagnose");
        const struct json_value *ops_direct_app_protocols =
            find_object_with_str(ops_direct_commands, "method",
                                 "appprotocols");
        const struct json_value *ops_direct_dumpstate =
            find_object_with_str(ops_direct_commands, "method",
                                 "dumpstate");
        const struct json_value *ops_direct_getnodelog =
            find_object_with_str(ops_direct_commands, "method",
                                 "getnodelog");
        const struct json_value *ops_direct_peerincidents =
            find_object_with_str(ops_direct_commands, "method",
                                 "peerincidents");
        ok = ok && ops_direct_diagnose &&
            strcmp(json_get_str(json_get(ops_direct_diagnose, "schema")),
                   "zcl.agent_diagnose.v1") == 0;
        ok = ok && ops_direct_diagnose &&
            strcmp(json_get_str(json_get(ops_direct_diagnose, "mcp")),
                   "zcl_agent_diagnose") == 0;
        ok = ok && ops_direct_app_protocols &&
            strcmp(json_get_str(json_get(ops_direct_app_protocols,
                                         "schema")),
                   "zcl.application_protocols.index.v1") == 0;
        ok = ok && ops_direct_app_protocols &&
            strcmp(json_get_str(json_get(ops_direct_app_protocols,
                                         "mcp")),
                   "zcl_app_protocols") == 0;
        ok = ok && ops_direct_dumpstate &&
            strcmp(json_get_str(json_get(ops_direct_dumpstate, "native")),
                   "zclassic23 dumpstate <subsystem> [key]") == 0;
        ok = ok && ops_direct_dumpstate &&
            strcmp(json_get_str(json_get(ops_direct_dumpstate, "mcp")),
                   "zcl_state") == 0;
        ok = ok && ops_direct_getnodelog &&
            strcmp(json_get_str(json_get(ops_direct_getnodelog, "mcp")),
                   "zcl_node_log") == 0;
        ok = ok && ops_direct_peerincidents &&
            strcmp(json_get_str(json_get(ops_direct_peerincidents, "native")),
                   "zclassic23 peerincidents") == 0;
        ok = ok && ops_direct_peerincidents &&
            strcmp(json_get_str(json_get(ops_direct_peerincidents, "mcp")),
                   "zcl_peer_incidents") == 0;
        ok = ok && strstr(json_get_str(json_get(&ops,
                                                "refold_plain_english")),
                          "borrowed snapshot seed") != NULL;
        ok = ok &&
            agent_contract_work_surface_count("agentops.api_gaps") == 3;
        ok = ok &&
            agent_contract_work_surface_count("agentops.top_next_work") == 5;
        ok = ok &&
            agent_contract_work_surface_count("missing.surface") == 0;
        ok = ok &&
            agent_contract_field_surface_count("agentops.first_call") == 9;
        ok = ok &&
            agent_contract_field_surface_count("missing.surface") == 0;
        ok = ok &&
            agent_contract_review_surface_count(
                "agentops.architecture_review") == 5;
        ok = ok &&
            agent_contract_review_surface_count("missing.surface") == 0;
        ok = ok && ops_gaps && json_size(ops_gaps) == 3;
        ok = ok && find_object_with_str(ops_gaps, "name",
                                        "runtime_identity_everywhere") != NULL;
        ok = ok && find_object_with_str(ops_gaps, "name",
                                        "timeline_query") != NULL;
        ok = ok && ops_work && json_size(ops_work) == 5;
        ok = ok && find_object_with_str(ops_work, "name",
                                        "finish_self_verified_utxo_anchor_rebuild")
            != NULL;
        ok = ok && find_object_with_str(ops_work, "name",
                                        "harden_peer_bootstrap_lifecycle")
            != NULL;
        ok = ok && find_object_with_str(ops_work, "name",
                                        "promote_mvp_operator_proofs") != NULL;
        ok = ok && find_object_with_str(ops_work, "name",
                                        "shrink_boot_refold_supervised_units")
            != NULL;
        ok = ok && find_object_with_str(ops_work, "name",
                                        "dry_agent_contract_registry") == NULL;
        const struct json_value *ops_review =
            json_get(&ops, "architecture_review");
        ok = ok && ops_review != NULL;
        ok = ok && ops_review &&
            strstr(json_get_str(json_get(ops_review, "architecture_center")),
                   "progress.kv fact log") != NULL;
        ok = ok && ops_review &&
            strcmp(json_get_str(json_get(ops_review, "preferred_payload")),
                   "versioned JSON with direct decision fields and explicit drill-down commands")
                == 0;
        ok = ok && ops_availability &&
            strcmp(json_get_str(json_get(ops_availability, "schema")),
                   "zcl.agent_runtime_availability.v1") == 0;
        ok = ok && ops_method_agentops &&
            strcmp(json_get_str(json_get(ops_method_agentops,
                                         "target_runtime_support")),
                   "supported") == 0;

        event_log_init();
        event_emitf(EV_SYNC_HEARTBEAT, 0, "diagnose sync heartbeat");
        struct json_value diagnose_full_params;
        json_init(&diagnose_full_params);
        json_set_array(&diagnose_full_params);
        struct json_value diagnose_full_arg;
        json_init(&diagnose_full_arg);
        json_set_str(&diagnose_full_arg, "full");
        json_push_back(&diagnose_full_params, &diagnose_full_arg);
        json_free(&diagnose_full_arg);
        struct json_value diagnose;
        json_init(&diagnose);
        ok = ok && rpc_table_execute(&tbl, "agentdiagnose",
                                     &diagnose_full_params, &diagnose);
        const struct json_value *diagnose_first_call =
            json_get(&diagnose, "first_call");
        const struct json_value *diagnose_peers =
            json_get(&diagnose, "peer_incidents");
        const struct json_value *diagnose_timeline =
            json_get(&diagnose, "timeline");
        ok = ok && diagnose.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&diagnose, "schema")),
                          "zcl.agent_diagnose.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&diagnose, "method")),
                          "agentdiagnose") == 0;
        ok = ok && json_get_bool(json_get(&diagnose, "no_jq_required"));
        ok = ok && strcmp(json_get_str(json_get(&diagnose,
                                                "native_command")),
                          "zclassic23 agentdiagnose") == 0;
        ok = ok && strcmp(json_get_str(json_get(&diagnose, "mcp_tool")),
                          "zcl_agent_diagnose") == 0;
        ok = ok && strcmp(json_get_str(json_get(&diagnose,
                                                "contract_source")),
                          "agent_contracts.def") == 0;
        ok = ok && json_get(&diagnose, "verdict") != NULL;
        ok = ok && json_get(&diagnose, "safe_next_action") != NULL;
        ok = ok && json_get(&diagnose, "findings") != NULL;
        ok = ok && json_get(&diagnose, "agent") != NULL;
        ok = ok && json_get(&diagnose, "healthcheck") != NULL;
        ok = ok && diagnose_peers && diagnose_peers->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(diagnose_peers, "schema")),
                          "zcl.peer_incidents.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(diagnose_peers, "method")),
                          "peerincidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(diagnose_peers,
                                                "native_command")),
                          "zclassic23 peerincidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(diagnose_peers,
                                                "mcp_tool")),
                          "zcl_peer_incidents") == 0;
        ok = ok && strcmp(json_get_str(json_get(diagnose_peers,
                                                "contract_source")),
                          "agent_contracts.def") == 0;
        ok = ok && diagnose_timeline && diagnose_timeline->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(diagnose_timeline,
                                                "schema")),
                          "zcl.timeline.v1") == 0;
        ok = ok && diagnose_first_call &&
            strcmp(json_get_str(json_get(diagnose_first_call, "schema")),
                   "zcl.first_call_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(diagnose_first_call, "api")),
                          "agentdiagnose") == 0;
        ok = ok && json_get_int(json_get(diagnose_first_call,
                                         "budget_ms")) == 900;
        ok = ok && json_get_bool(json_get(diagnose_first_call,
                                          "partial_result"));
        json_free(&diagnose);
        json_free(&diagnose_full_params);

        struct json_value inferred_ops;
        json_init(&inferred_ops);
        agent_runtime_availability_reset();
        rpc_agent_set_boot_context("unknown", "full", "~/.zclassic-c23",
                                   18232, 8033, 8443, 18034);
        ok = ok && rpc_table_execute(&tbl, "agentops", &params,
                                     &inferred_ops);
        const struct json_value *inferred_ops_lane =
            json_get(&inferred_ops, "current_runtime_lane");
        const struct json_value *inferred_ops_availability =
            json_get(&inferred_ops, "runtime_availability");
        ok = ok && inferred_ops_lane && inferred_ops_lane->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(inferred_ops_lane, "lane")),
                          "canonical") == 0;
        ok = ok && strcmp(json_get_str(json_get(inferred_ops_lane,
                                                "lane_source")),
                          "inferred_exact_topology") == 0;
        ok = ok && json_get_bool(json_get(inferred_ops_lane,
                                          "lane_inferred"));
        ok = ok && inferred_ops_availability &&
            strcmp(json_get_str(json_get(inferred_ops_availability,
                                         "operator_lane_name")),
                   "canonical") == 0;
        ok = ok && inferred_ops_availability &&
            strcmp(json_get_str(json_get(inferred_ops_availability,
                                         "operator_lane_source")),
                   "inferred_exact_topology") == 0;
        json_free(&inferred_ops);
        rpc_agent_set_boot_context("unknown", "full", "", 0, 0, 0, 0);
        agent_runtime_availability_reset();

        event_log_init();
        event_emitf(EV_SYNC_STATE_CHANGE, 0, "idle->headers");
        event_emitf(EV_MSG_RECEIVED, 0, "noise");
        event_emitf(EV_SYNC_HEARTBEAT, 0, "state=headers h=10");
        event_emitf(EV_TIP_STALE, 7,
                    "state=headers since=600 peers=0 max_peer=20");
        struct json_value timeline_params;
        json_init(&timeline_params);
        json_set_array(&timeline_params);
        struct json_value timeline_category;
        json_init(&timeline_category);
        json_set_str(&timeline_category, "sync");
        json_push_back(&timeline_params, &timeline_category);
        json_free(&timeline_category);
        struct json_value timeline_count;
        json_init(&timeline_count);
        json_set_int(&timeline_count, 3);
        json_push_back(&timeline_params, &timeline_count);
        json_free(&timeline_count);
        struct json_value timeline;
        json_init(&timeline);
        ok = ok && rpc_table_execute(&tbl, "timeline", &timeline_params,
                                     &timeline);
        const struct json_value *timeline_events =
            json_get(&timeline, "events");
        const struct json_value *timeline_summary =
            json_get(&timeline, "semantic_summary");
        const struct json_value *timeline_type_counts =
            json_get(&timeline, "type_counts");
        const struct json_value *timeline_tip_stale =
            find_object_with_str(timeline_type_counts, "type",
                                 "sync.tip_stale");
        const struct json_value *timeline_drilldowns =
            json_get(&timeline, "recommended_drilldowns");
        ok = ok && timeline.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&timeline, "schema")),
                          "zcl.timeline.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&timeline, "status")),
                          "ok") == 0;
        ok = ok && strcmp(json_get_str(json_get(&timeline, "category")),
                          "sync") == 0;
        ok = ok && strcmp(json_get_str(json_get(&timeline, "mcp_tool")),
                          "zcl_timeline") == 0;
        ok = ok && json_get_int(json_get(&timeline, "head_seq")) >= 4;
        ok = ok && timeline_events && timeline_events->type == JSON_ARR &&
            json_size(timeline_events) == 3;
        ok = ok && timeline_summary &&
            json_get_int(json_get(timeline_summary, "event_count")) == 3;
        ok = ok && timeline_summary &&
            json_get_int(json_get(timeline_summary,
                                  "problem_event_count")) == 1;
        ok = ok && timeline_summary &&
            json_get_bool(json_get(timeline_summary, "has_problem_events"));
        ok = ok && timeline_summary &&
            strcmp(json_get_str(json_get(timeline_summary,
                                         "dominant_type")),
                   "sync.state_change") == 0;
        ok = ok && timeline_tip_stale &&
            json_get_bool(json_get(timeline_tip_stale, "problem"));
        ok = ok && timeline_drilldowns &&
            json_array_has_substr(timeline_drilldowns, "reducer_frontier");
        ok = ok && timeline_drilldowns &&
            json_array_has_substr(timeline_drilldowns, "fail|reject|stale");

        event_emitf(EV_CONDITION_DETECTED, 9,
                    "name=download_queue_starved stage=body_fetch "
                    "lane=dev deploy=make-deploy height=42");
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=deploy lane=dev deploy=make-deploy build=abc "
                    "height=42");
        event_emitf(EV_SYNC_HEARTBEAT, 9,
                    "state=headers h=420 stage=body_fetch lane=dev");
        event_emitf(EV_SYNC_HEARTBEAT, 9,
                    "state=headers h=42 stage=body_fetch lane=dev");

        struct json_value timeline_filter_params;
        json_init(&timeline_filter_params);
        json_set_object(&timeline_filter_params);
        json_push_kv_str(&timeline_filter_params, "category", "all");
        json_push_kv_int(&timeline_filter_params, "count", 5);
        json_push_kv_int(&timeline_filter_params, "scan_count", 16);
        json_push_kv_int(&timeline_filter_params, "since_secs", 3600);
        json_push_kv_int(&timeline_filter_params, "peer", 9);
        json_push_kv_int(&timeline_filter_params, "height", 42);
        json_push_kv_str(&timeline_filter_params, "reducer_stage",
                         "body_fetch");
        json_push_kv_str(&timeline_filter_params, "condition",
                         "download_queue_starved");
        json_push_kv_str(&timeline_filter_params, "deploy", "make-deploy");
        json_push_kv_str(&timeline_filter_params, "lane", "dev");
        struct json_value timeline_filtered;
        json_init(&timeline_filtered);
        ok = ok && rpc_table_execute(&tbl, "timeline",
                                     &timeline_filter_params,
                                     &timeline_filtered);
        const struct json_value *timeline_filtered_events =
            json_get(&timeline_filtered, "events");
        const struct json_value *timeline_filtered_filters =
            json_get(&timeline_filtered, "filters");
        const struct json_value *timeline_filtered_refs =
            json_get(&timeline_filtered, "log_references");
        const struct json_value *timeline_filtered_first =
            timeline_filtered_events && timeline_filtered_events->type == JSON_ARR
                && json_size(timeline_filtered_events) > 0
                    ? json_at(timeline_filtered_events, 0) : NULL;
        ok = ok && timeline_filtered.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&timeline_filtered,
                                                "schema")),
                          "zcl.timeline.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&timeline_filtered,
                                                "status")),
                          "ok") == 0;
        ok = ok && strcmp(json_get_str(json_get(&timeline_filtered,
                                                "filter_model")),
                          "bounded_server_side_scan_then_filter") == 0;
        ok = ok &&
            json_get_int(json_get(&timeline_filtered, "scan_count")) == 16;
        ok = ok &&
            json_get_int(json_get(&timeline_filtered,
                                  "matched_before_limit")) == 1;
        ok = ok &&
            json_get_int(json_get(&timeline_filtered,
                                  "count_returned")) == 1;
        ok = ok && timeline_filtered_filters &&
            json_get_bool(json_get(timeline_filtered_filters, "active"));
        ok = ok && timeline_filtered_filters &&
            json_get_int(json_get(timeline_filtered_filters, "peer")) == 9;
        ok = ok && timeline_filtered_filters &&
            json_get_int(json_get(timeline_filtered_filters, "height")) == 42;
        ok = ok && timeline_filtered_filters &&
            strcmp(json_get_str(json_get(timeline_filtered_filters,
                                         "reducer_stage")),
                   "body_fetch") == 0;
        ok = ok && timeline_filtered_filters &&
            strcmp(json_get_str(json_get(timeline_filtered_filters,
                                         "condition")),
                   "download_queue_starved") == 0;
        ok = ok && timeline_filtered_filters &&
            strcmp(json_get_str(json_get(timeline_filtered_filters,
                                         "deploy")),
                   "make-deploy") == 0;
        ok = ok && timeline_filtered_filters &&
            strcmp(json_get_str(json_get(timeline_filtered_filters,
                                         "lane")),
                   "dev") == 0;
        ok = ok && timeline_filtered_first &&
            json_get_int(json_get(timeline_filtered_first, "peer")) == 9;
        ok = ok && timeline_filtered_first &&
            strstr(json_get_str(json_get(timeline_filtered_first, "data")),
                   "download_queue_starved") != NULL;
        ok = ok && timeline_filtered_refs &&
            json_array_has_substr(timeline_filtered_refs,
                                  "download_queue_starved");
        ok = ok && json_get(&timeline_filtered,
                            "safe_next_action") != NULL;

        struct json_value timeline_cli_params;
        json_init(&timeline_cli_params);
        json_set_array(&timeline_cli_params);
        struct json_value timeline_cli_arg;
        json_init(&timeline_cli_arg);
        json_set_str(&timeline_cli_arg,
                     "{\"category\":\"sync\",\"count\":2,"
                     "\"since_secs\":3600,\"peer\":9,\"height\":42,"
                     "\"reducer_stage\":\"body_fetch\",\"lane\":\"dev\"}");
        json_push_back(&timeline_cli_params, &timeline_cli_arg);
        json_free(&timeline_cli_arg);
        struct json_value timeline_cli;
        json_init(&timeline_cli);
        ok = ok && rpc_table_execute(&tbl, "timeline",
                                     &timeline_cli_params, &timeline_cli);
        const struct json_value *timeline_cli_events =
            json_get(&timeline_cli, "events");
        const struct json_value *timeline_cli_filters =
            json_get(&timeline_cli, "filters");
        const struct json_value *timeline_cli_first =
            timeline_cli_events && timeline_cli_events->type == JSON_ARR &&
            json_size(timeline_cli_events) > 0
                ? json_at(timeline_cli_events, 0) : NULL;
        ok = ok && timeline_cli.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&timeline_cli, "schema")),
                          "zcl.timeline.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&timeline_cli, "status")),
                          "ok") == 0;
        ok = ok && strcmp(json_get_str(json_get(&timeline_cli, "category")),
                          "sync") == 0;
        ok = ok &&
            json_get_int(json_get(&timeline_cli, "matched_before_limit")) == 1;
        ok = ok &&
            json_get_int(json_get(&timeline_cli, "count_returned")) == 1;
        ok = ok && timeline_cli_filters &&
            json_get_bool(json_get(timeline_cli_filters, "active"));
        ok = ok && timeline_cli_first &&
            strstr(json_get_str(json_get(timeline_cli_first, "data")),
                   "h=42") != NULL;
        ok = ok && timeline_cli_first &&
            strstr(json_get_str(json_get(timeline_cli_first, "data")),
                   "h=420") == NULL;
        json_free(&timeline_cli);
        json_free(&timeline_cli_params);

        struct json_value catalog;
        json_init(&catalog);
        ok = ok && rpc_table_execute(&tbl, "statecatalog", &params,
                                     &catalog);
        const struct json_value *catalog_subsystems =
            json_get(&catalog, "subsystems");
        const struct json_value *block_index_cat =
            find_object_with_str(catalog_subsystems, "name", "block_index");
        const struct json_value *frontier_cat =
            find_object_with_str(catalog_subsystems, "name",
                                 "reducer_frontier");
        ok = ok && catalog.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&catalog, "schema")),
                          "zcl.state_catalog.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&catalog, "build_commit")),
                          zcl_build_commit()) == 0;
        ok = ok && strcmp(json_get_str(json_get(&catalog,
                                                "catalog_mcp_tool")),
                          "zcl_state_catalog") == 0;
        ok = ok && catalog_subsystems &&
            catalog_subsystems->type == JSON_ARR &&
            json_size(catalog_subsystems) >= 50;
        ok = ok && block_index_cat &&
            json_get_bool(json_get(block_index_cat, "accepts_key"));
        ok = ok && block_index_cat &&
            strcmp(json_get_str(json_get(block_index_cat, "key_hint")),
                   "height or 64-char block hash") == 0;
        ok = ok && block_index_cat &&
            strcmp(json_get_str(json_get(block_index_cat, "mcp_tool")),
                   "zcl_state") == 0;
        ok = ok && block_index_cat &&
            strcmp(json_get_str(json_get(block_index_cat, "subsystem")),
                   "block_index") == 0;
        ok = ok && block_index_cat &&
            strcmp(json_get_str(json_get(block_index_cat, "owner_file")),
                   "app/controllers/src/diagnostics_block_index.c") == 0;
        ok = ok && block_index_cat &&
            strcmp(json_get_str(json_get(block_index_cat, "safety_level")),
                   "read_only") == 0;
        ok = ok && block_index_cat &&
            json_array_has_str(json_get(block_index_cat, "accepted_keys"),
                               "height or 64-char block hash");
        ok = ok && block_index_cat &&
            json_array_has_str(json_get(block_index_cat, "key_examples"),
                               "3170000");
        ok = ok && block_index_cat &&
            json_array_has_substr(json_get(block_index_cat, "tests"),
                                  "test_block_index_integrity.c");
        ok = ok && block_index_cat &&
            json_array_has_substr(json_get(block_index_cat, "drilldowns"),
                                  "zcl_state {\"subsystem\":\"block_index\"");
        ok = ok && frontier_cat &&
            strcmp(json_get_str(json_get(frontier_cat, "state_class")),
                   "reducer_stage") == 0;
        ok = ok && frontier_cat &&
            strcmp(json_get_str(json_get(frontier_cat, "owner_file")),
                   "app/jobs/src/reducer_frontier_dump.c") == 0;
        ok = ok && frontier_cat &&
            strcmp(json_get_str(json_get(frontier_cat, "safety_level")),
                   "read_only") == 0;
        ok = ok && frontier_cat &&
            json_array_has_substr(json_get(frontier_cat, "tests"),
                                  "test_reducer_frontier.c");
        ok = ok && frontier_cat &&
            json_array_has_str(json_get(frontier_cat, "accepted_keys"), "") ==
            false;
        ok = ok && frontier_cat &&
            json_array_has_substr(json_get(frontier_cat, "drilldowns"),
                                  "zclassic23 dumpstate reducer_frontier");

        struct json_value lanes;
        json_init(&lanes);
        ok = ok && rpc_table_execute(&tbl, "agentlanes", &params, &lanes);
        const struct json_value *lane_arr = json_get(&lanes, "lanes");
        const struct json_value *runtime_services =
            json_get(&lanes, "current_runtime_services");
        const struct json_value *runtime_availability =
            json_get(&lanes, "current_runtime_availability");
        const struct json_value *lane_commands =
            json_get(&lanes, "commands");
        const struct json_value *lane_status_cmd =
            find_object_with_str(lane_commands, "name", "status");
        const struct json_value *lane_topology_cmd =
            find_object_with_str(lane_commands, "name", "lane_topology");
        const struct json_value *deploy_guard_cmd =
            find_object_with_str(lane_commands, "name", "deploy_guard");
        const struct json_value *lane_health_cmd =
            find_object_with_str(lane_commands, "name", "lane_health");
        const struct json_value *canonical =
            find_object_with_str(lane_arr, "lane", "canonical");
        const struct json_value *dev =
            find_object_with_str(lane_arr, "lane", "dev");
        const struct json_value *canonical_safety =
            canonical ? json_get(canonical, "deployment_safety") : NULL;
        const struct json_value *dev_safety =
            dev ? json_get(dev, "deployment_safety") : NULL;
        ok = ok && lanes.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&lanes, "schema")),
                          "zcl.agent_lanes.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&lanes,
                                                "default_deploy_target")),
                          "dev") == 0;
        ok = ok && lane_commands && lane_commands->type == JSON_ARR &&
            json_size(lane_commands) >= 4;
        ok = ok && lane_status_cmd &&
            strcmp(json_get_str(json_get(lane_status_cmd, "method")),
                   "agent") == 0;
        ok = ok && lane_status_cmd &&
            strcmp(json_get_str(json_get(lane_status_cmd, "native")),
                   "zclassic23 agent") == 0;
        ok = ok && lane_status_cmd &&
            strcmp(json_get_str(json_get(lane_status_cmd, "mcp")),
                   "zcl_agent") == 0;
        ok = ok && lane_status_cmd &&
            strcmp(json_get_str(json_get(lane_status_cmd, "schema")),
                   "zcl.public_status.v1") == 0;
        ok = ok && lane_topology_cmd &&
            strcmp(json_get_str(json_get(lane_topology_cmd, "method")),
                   "agentlanes") == 0;
        ok = ok && lane_topology_cmd &&
            strcmp(json_get_str(json_get(lane_topology_cmd, "mcp")),
                   "zcl_agent_lanes") == 0;
        ok = ok && deploy_guard_cmd &&
            strcmp(json_get_str(json_get(deploy_guard_cmd, "method")),
                   "agentdeployguard") == 0;
        ok = ok && deploy_guard_cmd &&
            strcmp(json_get_str(json_get(deploy_guard_cmd, "native")),
                   "zclassic23 agentdeployguard [action]") == 0;
        ok = ok && deploy_guard_cmd &&
            strcmp(json_get_str(json_get(deploy_guard_cmd, "mcp")),
                   "zcl_agent_deploy_guard") == 0;
        ok = ok && lane_health_cmd &&
            strcmp(json_get_str(json_get(lane_health_cmd, "native")),
                   "tools/scripts/lane_health.sh --json") == 0;
        ok = ok && runtime_services &&
            strcmp(json_get_str(json_get(runtime_services, "schema")),
                   "zcl.agent_runtime_services.v1") == 0;
        ok = ok && runtime_availability &&
            strcmp(json_get_str(json_get(runtime_availability, "schema")),
                   "zcl.agent_runtime_availability.v1") == 0;
        ok = ok && runtime_availability &&
            strcmp(json_get_str(json_get(runtime_availability,
                                         "availability_scope")),
                   "producer_runtime") == 0;
        ok = ok && runtime_services &&
            json_get_int(json_get(runtime_services,
                                  "rpc_configured_port")) == 0;
        ok = ok && runtime_services &&
            !json_get_bool(json_get(runtime_services, "rpc_running"));
        ok = ok && runtime_services &&
            json_get_int(json_get(runtime_services,
                                  "https_configured_port")) == 0;
        ok = ok && runtime_services &&
            !json_get_bool(json_get(runtime_services, "https_running"));
        ok = ok && runtime_services &&
            json_get_int(json_get(runtime_services, "https_bound_port")) == 0;
        ok = ok && runtime_services &&
            !json_get_bool(json_get(runtime_services, "fs_running"));
        ok = ok && runtime_services &&
            json_get_int(json_get(runtime_services, "fs_bound_port")) == 0;
        ok = ok && lane_arr && lane_arr->type == JSON_ARR &&
            json_size(lane_arr) >= 3;
        ok = ok && canonical &&
            strcmp(json_get_str(json_get(canonical, "unit")),
                   "zclassic23") == 0;
        ok = ok && canonical &&
            json_get_int(json_get(canonical, "https_port")) == 8443;
        ok = ok && canonical &&
            json_get_int(json_get(canonical, "fs_port")) == 0;
        ok = ok && canonical_safety &&
            json_get_bool(json_get(canonical_safety,
                                   "requires_operator_confirmation"));
        ok = ok && !json_get_bool(json_get(canonical_safety,
                                           "automation_deploy_ok"));
        ok = ok && dev &&
            strcmp(json_get_str(json_get(dev, "unit")), "zcl23-dev") == 0;
        ok = ok && dev && json_get_int(json_get(dev, "https_port")) == 0;
        ok = ok && dev && json_get_int(json_get(dev, "fs_port")) == 18034;
        ok = ok && dev_safety &&
            json_get_bool(json_get(dev_safety, "automation_deploy_ok"));
        ok = ok && strcmp(json_get_str(json_get(dev_safety,
                                                "safe_default_action")),
                          "deploy_dev_lane") == 0;

        const char *old_quality_env = getenv("ZCL_QUALITY_STATE_DIR");
        char old_quality_env_buf[4096];
        bool old_quality_env_set = old_quality_env != NULL;
        bool old_quality_env_saved = true;
        char quality_tmp[] = "/tmp/zcl_quality_rpc_XXXXXX";
        char quality_status_dir[4096];
        char quality_fuzz_file[4096];
        char *quality_root = mkdtemp(quality_tmp);
        bool quality_fixture_ok = quality_root != NULL;
        if (old_quality_env_set) {
            int n = snprintf(old_quality_env_buf,
                             sizeof(old_quality_env_buf), "%s",
                             old_quality_env);
            old_quality_env_saved = n >= 0 &&
                (size_t)n < sizeof(old_quality_env_buf);
        }
        if (quality_fixture_ok) {
            int n = snprintf(quality_status_dir,
                             sizeof(quality_status_dir), "%s/status",
                             quality_root);
            quality_fixture_ok = n >= 0 &&
                (size_t)n < sizeof(quality_status_dir) &&
                mkdir(quality_status_dir, 0700) == 0;
        }
        if (quality_fixture_ok) {
            int n = snprintf(quality_fuzz_file, sizeof(quality_fuzz_file),
                             "%s/fuzz.json", quality_status_dir);
            quality_fixture_ok = n >= 0 &&
                (size_t)n < sizeof(quality_fuzz_file);
        }
        if (quality_fixture_ok) {
            FILE *f = fopen(quality_fuzz_file, "wb");
            quality_fixture_ok = f != NULL;
            if (f) {
                fputs("{\"schema\":\"zcl.background_quality_lane.v1\","
                      "\"lane\":\"fuzz\",\"status\":\"passed\","
                      "\"started_at\":\"2026-07-05T00:00:00Z\","
                      "\"finished_at\":\"2026-07-05T00:01:00Z\","
                      "\"elapsed_seconds\":60,\"exit_code\":0,"
                      "\"commit\":\"deadbeef1234\",\"log\":\"/tmp/fuzz.log\","
                      "\"artifacts\":\"/tmp/artifacts\","
                      "\"detail\":\"fixture\"}\n", f);
                quality_fixture_ok = fclose(f) == 0;
            }
        }
        if (quality_fixture_ok)
            quality_fixture_ok =
                setenv("ZCL_QUALITY_STATE_DIR", quality_root, 1) == 0;
        ok = ok && old_quality_env_saved && quality_fixture_ok;

        struct json_value build;
        json_init(&build);
        ok = ok && rpc_table_execute(&tbl, "agentbuild", &params, &build);
        const struct json_value *loop =
            json_get(&build, "recommended_loop");
        const struct json_value *incremental =
            json_get(&build, "incremental_compile");
        const struct json_value *dev_binary =
            json_get(&build, "dev_node_binary");
        const struct json_value *cache = json_get(&build, "cache");
        const struct json_value *commands = json_get(&build, "commands");
        const struct json_value *repro =
            json_get(&build, "reproducible_release");
        const struct json_value *quality_status =
            json_get(&build, "background_quality_status");
        const struct json_value *quality_lanes =
            quality_status ? json_get(quality_status, "lanes") : NULL;
        const struct json_value *fuzz_lane =
            find_object_with_str(quality_lanes, "lane", "fuzz");
        const struct json_value *coverage_lane =
            find_object_with_str(quality_lanes, "lane", "coverage");
        const struct json_value *latest_fuzz =
            fuzz_lane ? json_get(fuzz_lane, "latest") : NULL;
        ok = ok && build.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&build, "schema")),
                          "zcl.agent_build.v1") == 0;
        ok = ok && loop && strcmp(json_get_str(json_get(loop, "schema")),
                                  "zcl.agent_build_loop.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(loop,
                           "direct_changed_compile")),
                          "make fast-changed-compile") == 0;
        ok = ok && strcmp(json_get_str(json_get(loop,
                           "fast_no_link_compile")),
                          "make fast-compile") == 0;
        ok = ok && strcmp(json_get_str(json_get(loop,
                           "fast_ci_compile_default")),
                          "ZCL_FAST_COMPILE=changed -> make fast-changed-compile with safe fallback") == 0;
        ok = ok && strstr(json_get_str(json_get(loop, "rule")),
                          ".h/.def edits") != NULL;
        ok = ok && strcmp(json_get_str(json_get(loop,
                           "pre_push_compile_default")),
                          "ZCL_FAST_COMPILE=strict -> make build-only") == 0;
        ok = ok && incremental && json_get_bool(json_get(incremental,
                                                         "header_depfiles"));
        ok = ok && strcmp(json_get_str(json_get(incremental,
                                                "changed_compile_check")),
                          "make fast-changed-compile") == 0;
        ok = ok && strstr(json_get_str(json_get(incremental, "behavior")),
                          "build/dev-obj/.complete") != NULL;
        ok = ok && strstr(json_get_str(json_get(incremental,
                                                "changed_compile_fallbacks")),
                          "unwarmed depfiles") != NULL;
        ok = ok && strcmp(json_get_str(json_get(incremental,
                                                "fast_compile_check")),
                          "make fast-compile") == 0;
        ok = ok && strcmp(json_get_str(json_get(incremental,
                                                "dev_binary_command")),
                          "make fast-rebuild") == 0;
        ok = ok && dev_binary && json_get_bool(json_get(dev_binary,
                                                        "enabled"));
        ok = ok && strcmp(json_get_str(json_get(dev_binary, "binary")),
                          "build/bin/zclassic23-dev") == 0;
        ok = ok && !json_get_bool(json_get(dev_binary,
                                           "release_or_deploy_artifact"));
        ok = ok && strstr(json_get_str(json_get(dev_binary,
                                                "hot_path_buckets")),
                          "lib/crypto") != NULL;
        ok = ok && cache && strstr(json_get_str(json_get(cache,
                                                         "auto_select_order")),
                                   "sccache cc") != NULL;
        ok = ok && strstr(json_get_str(json_get(cache,
                                                "makefile_auto_wrapper")),
                          "sccache cc") != NULL;
        ok = ok && find_object_with_str(json_get(cache, "knobs"), "name",
                                        "ZCL_FAST_CHANGED_FILES_ONLY") != NULL;
        ok = ok && find_object_with_str(commands, "name",
                                        "fast_changed_compile") != NULL;
        ok = ok && find_object_with_str(commands, "name",
                                        "fast_compile") != NULL;
        ok = ok && find_object_with_str(commands, "name",
                                        "compile_check") != NULL;
        ok = ok && find_object_with_str(commands, "name",
                                        "fast_rebuild") != NULL;
        ok = ok && find_object_with_str(commands, "name",
                                        "dev_node_binary") != NULL;
        ok = ok && find_object_with_str(commands, "name",
                                        "byte_identity") != NULL;
        ok = ok && repro && strcmp(json_get_str(json_get(repro, "command")),
                                   "make ci-reproducible") == 0;
        ok = ok && strcmp(json_get_str(json_get(repro, "portable_isa")),
                          "x86-64-v3") == 0;
        ok = ok && quality_status && quality_status->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(quality_status, "schema")),
                          "zcl.background_quality_runtime.v1") == 0;
        ok = ok && json_get_bool(json_get(quality_status,
                                          "native_status_reader"));
        ok = ok && !json_get_bool(json_get(quality_status,
                                           "requires_python"));
        ok = ok && strcmp(json_get_str(json_get(quality_status,
                                                "state_dir")),
                          quality_root ? quality_root : "") == 0;
        ok = ok && strcmp(json_get_str(json_get(quality_status,
                                                "summary")),
                          "background_quality_stale") == 0;
        ok = ok && strcmp(json_get_str(json_get(quality_status,
                                                "agent_next_action")),
                          "restart_or_wait_for_current_commit_quality_lanes")
            == 0;
        ok = ok && json_get_int(json_get(quality_status,
                                         "status_files_present")) == 1;
        ok = ok && json_get_int(json_get(quality_status,
                                         "status_files_valid")) == 1;
        ok = ok && json_get_int(json_get(quality_status,
                                         "passed_count")) == 1;
        ok = ok && json_get_int(json_get(quality_status,
                                         "current_commit_count")) == 0;
        ok = ok && json_get_int(json_get(quality_status,
                                         "stale_commit_count")) == 1;
        ok = ok && json_get_int(json_get(quality_status,
                                         "unknown_commit_count")) == 0;
        ok = ok && quality_lanes && quality_lanes->type == JSON_ARR &&
            json_size(quality_lanes) == 3;
        ok = ok && fuzz_lane &&
            json_get_bool(json_get(fuzz_lane, "status_file_present"));
        ok = ok && fuzz_lane &&
            json_get_bool(json_get(fuzz_lane, "latest_json_valid"));
        ok = ok && fuzz_lane &&
            strcmp(json_get_str(json_get(fuzz_lane, "latest_status")),
                   "passed") == 0;
        ok = ok && latest_fuzz && latest_fuzz->type == JSON_OBJ;
        ok = ok && latest_fuzz &&
            strcmp(json_get_str(json_get(latest_fuzz, "commit")),
                   "deadbeef1234") == 0;
        ok = ok && fuzz_lane &&
            strcmp(json_get_str(json_get(fuzz_lane, "latest_commit")),
                   "deadbeef1234") == 0;
        ok = ok && fuzz_lane &&
            strcmp(json_get_str(json_get(fuzz_lane, "expected_commit")),
                   zcl_build_commit()) == 0;
        ok = ok && fuzz_lane &&
            json_get_bool(json_get(fuzz_lane, "commit_present"));
        ok = ok && fuzz_lane &&
            !json_get_bool(json_get(fuzz_lane, "commit_matches_expected"));
        ok = ok && fuzz_lane &&
            strcmp(json_get_str(json_get(fuzz_lane, "commit_freshness")),
                   "stale") == 0;
        ok = ok && coverage_lane &&
            !json_get_bool(json_get(coverage_lane, "status_file_present"));
        ok = ok && coverage_lane &&
            strcmp(json_get_str(json_get(coverage_lane, "commit_freshness")),
                   "no_verdict") == 0;

        struct json_value liveness;
        json_init(&liveness);
        ok = ok && rpc_table_execute(&tbl, "agentliveness", &params,
                                     &liveness);
        const struct json_value *live_summary =
            json_get(&liveness, "liveness_summary");
        const struct json_value *live_runtime =
            json_get(&liveness, "runtime_services");
        const struct json_value *live_availability =
            json_get(&liveness, "runtime_availability");
        const struct json_value *live_quality =
            json_get(&liveness, "background_quality_status");
        const struct json_value *live_supervisor =
            json_get(&liveness, "supervisor_state");
        const struct json_value *live_drilldowns =
            json_get(&liveness, "recommended_drilldowns");
        const struct json_value *live_first_call =
            json_get(&liveness, "first_call");
        const struct json_value *live_omitted =
            json_get(&liveness, "omitted_sections");
        ok = ok && liveness.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&liveness, "schema")),
                          "zcl.agent_liveness.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&liveness, "method")),
                          "agentliveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&liveness,
                                                "native_command")),
                          "zclassic23 agentliveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&liveness, "mcp_tool")),
                          "zcl_agent_liveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(&liveness,
                                                "contract_source")),
                          "agent_contracts.def") == 0;
        ok = ok && strcmp(json_get_str(json_get(&liveness, "detail_mode")),
                          "brief") == 0;
        ok = ok && !json_get_bool(json_get(&liveness,
                                           "embedded_drilldowns"));
        ok = ok && strcmp(json_get_str(json_get(&liveness,
                                                "full_mode_command")),
                          "zclassic23 agentliveness full") == 0;
        ok = ok && live_first_call && live_first_call->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(live_first_call, "schema")),
                          "zcl.first_call_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(live_first_call, "api")),
                          "agentliveness") == 0;
        ok = ok && strcmp(json_get_str(json_get(live_first_call,
                                                "result_completeness")),
                          "bounded") == 0;
        ok = ok && strcmp(json_get_str(json_get(live_first_call, "source")),
                          "runtime_supervisor_quality_status_brief") == 0;
        ok = ok && json_get_bool(json_get(live_first_call,
                                          "partial_result"));
        ok = ok && strcmp(json_get_str(json_get(live_first_call,
                                                "partial_reason")),
                          "brief_mode_omits_embedded_drilldowns") == 0;
        ok = ok && strcmp(json_get_str(json_get(live_first_call,
                                                "full_mode_command")),
                          "zclassic23 agentliveness full") == 0;
        ok = ok && json_get_int(json_get(live_first_call,
                                         "budget_ms")) == 750;
        ok = ok && json_get(live_first_call, "elapsed_ms") != NULL;
        ok = ok && json_get(live_first_call, "budget_exceeded") != NULL;
        ok = ok && live_omitted &&
            json_array_has_str(live_omitted, "runtime_availability.methods");
        ok = ok && live_omitted &&
            json_array_has_str(live_omitted, "background_quality_status.lanes");
        ok = ok && live_omitted &&
            json_array_has_str(live_omitted, "supervisor_state.domains");
        ok = ok && strcmp(json_get_str(json_get(&liveness,
                                                "overall_liveness")),
                          "static_or_offline_context") == 0;
        ok = ok && live_summary &&
            strcmp(json_get_str(json_get(live_summary,
                                         "background_quality_summary")),
                   "background_quality_stale") == 0;
        ok = ok && live_summary &&
            json_get_int(json_get(live_summary,
                                  "background_quality_status_files_valid")) == 1;
        ok = ok && live_runtime &&
            strcmp(json_get_str(json_get(live_runtime, "schema")),
                   "zcl.agent_runtime_services.v1") == 0;
        ok = ok && live_availability &&
            strcmp(json_get_str(json_get(live_availability, "schema")),
                   "zcl.agent_runtime_availability.v1") == 0;
        ok = ok && live_availability &&
            strcmp(json_get_str(json_get(live_availability,
                                         "object_completeness")),
                   "compact") == 0;
        ok = ok && live_availability &&
            json_get(live_availability, "methods") == NULL;
        ok = ok && live_availability &&
            !json_get_bool(json_get(live_availability,
                                    "target_rpc_attempted"));
        ok = ok && live_summary &&
            !json_get_bool(json_get(live_summary,
                                    "target_runtime_reachable"));
        ok = ok && live_summary &&
            !json_get_bool(json_get(live_summary,
                                    "effective_runtime_reachable"));
        ok = ok && live_summary &&
            strcmp(json_get_str(json_get(live_summary,
                                         "producer_runtime_state")),
                   "inactive_or_static_probe") == 0;
        ok = ok && live_summary &&
            strcmp(json_get_str(json_get(live_summary,
                                         "target_runtime_state")),
                   "not_probed") == 0;
        ok = ok && live_summary &&
            strcmp(json_get_str(json_get(live_summary,
                                         "effective_runtime_scope")),
                   "none") == 0;
        ok = ok && live_summary &&
            strcmp(json_get_str(json_get(live_summary,
                                         "runtime_observation_scope")),
                   "producer_runtime") == 0;
        ok = ok && live_quality &&
            strcmp(json_get_str(json_get(live_quality, "schema")),
                   "zcl.background_quality_runtime.v1") == 0;
        ok = ok && live_quality &&
            strcmp(json_get_str(json_get(live_quality,
                                         "object_completeness")),
                   "compact") == 0;
        ok = ok && live_quality &&
            json_get(live_quality, "lanes") == NULL;
        ok = ok && live_supervisor &&
            json_get(live_supervisor, "running") != NULL;
        ok = ok && live_supervisor &&
            strcmp(json_get_str(json_get(live_supervisor,
                                         "object_completeness")),
                   "compact") == 0;
        ok = ok && live_supervisor &&
            json_get(live_supervisor, "domains") == NULL;
        ok = ok && live_drilldowns &&
            json_array_has_substr(live_drilldowns, "zcl_state");

        struct json_value liveness_full_params, full_mode, liveness_full;
        json_init(&liveness_full_params);
        json_set_array(&liveness_full_params);
        json_init(&full_mode);
        json_set_str(&full_mode, "full");
        json_push_back(&liveness_full_params, &full_mode);
        json_free(&full_mode);
        json_init(&liveness_full);
        ok = ok && rpc_table_execute(&tbl, "agentliveness",
                                     &liveness_full_params, &liveness_full);
        const struct json_value *full_availability =
            json_get(&liveness_full, "runtime_availability");
        const struct json_value *full_quality =
            json_get(&liveness_full, "background_quality_status");
        const struct json_value *full_supervisor =
            json_get(&liveness_full, "supervisor_state");
        const struct json_value *full_first_call =
            json_get(&liveness_full, "first_call");
        ok = ok && strcmp(json_get_str(json_get(&liveness_full,
                                                "detail_mode")),
                          "full") == 0;
        ok = ok && json_get_bool(json_get(&liveness_full,
                                          "embedded_drilldowns"));
        ok = ok && json_get(&liveness_full, "omitted_sections") == NULL;
        ok = ok && full_availability &&
            json_get(full_availability, "methods") != NULL;
        ok = ok && full_quality && json_get(full_quality, "lanes") != NULL;
        ok = ok && full_supervisor &&
            json_get(full_supervisor, "domains") != NULL;
        ok = ok && full_first_call &&
            strcmp(json_get_str(json_get(full_first_call, "source")),
                   "runtime_supervisor_quality_status_full") == 0;
        json_free(&liveness_full);
        json_free(&liveness_full_params);

        agent_runtime_availability_begin_probe("test_target_rpc",
                                               "/tmp/zcl-canonical",
                                               18232, "ok");
        agent_runtime_availability_record_method("agent", "supported", 0, "");
        struct json_value probed_liveness;
        json_init(&probed_liveness);
        ok = ok && rpc_table_execute(&tbl, "agentliveness", &params,
                                     &probed_liveness);
        const struct json_value *probed_summary =
            json_get(&probed_liveness, "liveness_summary");
        const struct json_value *probed_availability =
            json_get(&probed_liveness, "runtime_availability");
        ok = ok && strcmp(json_get_str(json_get(&probed_liveness,
                                                "overall_liveness")),
                          "target_runtime_reachable") == 0;
        ok = ok && probed_summary &&
            json_get_bool(json_get(probed_summary,
                                   "target_runtime_reachable"));
        ok = ok && probed_summary &&
            json_get_bool(json_get(probed_summary,
                                   "effective_runtime_reachable"));
        ok = ok && probed_summary &&
            strcmp(json_get_str(json_get(probed_summary,
                                         "producer_runtime_state")),
                   "inactive_or_static_probe") == 0;
        ok = ok && probed_summary &&
            strcmp(json_get_str(json_get(probed_summary,
                                         "target_runtime_state")),
                   "reachable") == 0;
        ok = ok && probed_summary &&
            strcmp(json_get_str(json_get(probed_summary,
                                         "effective_runtime_scope")),
                   "target_rpc_probe") == 0;
        ok = ok && probed_summary &&
            strcmp(json_get_str(json_get(probed_summary,
                                         "runtime_observation_scope")),
                   "target_rpc_probe") == 0;
        ok = ok && probed_availability &&
            json_get_bool(json_get(probed_availability,
                                   "target_rpc_reachable"));
        ok = ok && strcmp(json_get_str(json_get(&probed_liveness,
                                                "agent_next_action")),
                          "monitor_target_runtime_or_use_mcp") == 0;
        json_free(&probed_liveness);
        agent_runtime_availability_reset();

        struct json_value interface;
        json_init(&interface);
        agent_runtime_availability_begin_probe("test_target_rpc",
                                               "/tmp/zcl-canonical",
                                               18232, "ok");
        agent_runtime_availability_set_target_build_commit("oldbuild");
        agent_runtime_availability_record_method("agent", "supported", 0, "");
        agent_runtime_availability_record_method(
            "agentops", "unsupported_method_not_found",
            RPC_METHOD_NOT_FOUND, "Method not found");
        ok = ok && rpc_table_execute(&tbl, "agentinterface",
                                     &params, &interface);
        const struct json_value *interface_transports =
            json_get(&interface, "transports");
        const struct json_value *must_live_in_c =
            json_get(&interface, "must_live_in_c");
        const struct json_value *avoid = json_get(&interface, "avoid");
        const struct json_value *capabilities =
            json_get(&interface, "capabilities");
        const struct json_value *runtime_status =
            find_object_with_str(capabilities, "name", "runtime_status");
        const struct json_value *runtime_status_alias =
            find_object_with_str(capabilities, "name", "runtime_status_alias");
        const struct json_value *mirror_status =
            find_object_with_str(capabilities, "name", "mirror_status");
        const struct json_value *lane_topology =
            find_object_with_str(capabilities, "name", "lane_topology");
        const struct json_value *unified_liveness =
            find_object_with_str(capabilities, "name", "unified_liveness");
        const struct json_value *deploy_cap =
            find_object_with_str(capabilities, "name", "deploy_guard");
        const struct json_value *state_catalog_cap =
            find_object_with_str(capabilities, "name", "state_catalog");
        const struct json_value *timeline_cap =
            find_object_with_str(capabilities, "name", "semantic_timeline");
        const struct json_value *app_protocols_cap =
            find_object_with_str(capabilities, "name",
                                 "application_protocol_catalog");
        const struct json_value *service_catalog_cap =
            find_object_with_str(capabilities, "name",
                                 "sovereign_service_catalog");
        const struct json_value *subsystem_state_cap =
            find_object_with_str(capabilities, "name", "subsystem_state");
        const struct json_value *semantic_state_cap =
            find_object_with_str(capabilities, "name", "semantic_state");
        const struct json_value *node_log_cap =
            find_object_with_str(capabilities, "name", "node_log_search");
        const struct json_value *bounded_logs_cap =
            find_object_with_str(capabilities, "name", "bounded_logs");
        const struct json_value *sql_cap =
            find_object_with_str(capabilities, "name", "sql_inspection");
        const struct json_value *select_sql_cap =
            find_object_with_str(capabilities, "name", "select_sql");
        const struct json_value *machine =
            json_get(&interface, "machine_contract");
        const struct json_value *runtime =
            json_get(&interface, "runtime_identity");
        const struct json_value *development_loop =
            json_get(&interface, "development_loop");
        const struct json_value *availability =
            json_get(&interface, "runtime_availability");
        const struct json_value *availability_methods =
            availability ? json_get(availability, "methods") : NULL;
        const struct json_value *agent_method =
            find_object_with_str(availability_methods, "method", "agent");
        const struct json_value *agentops_method =
            find_object_with_str(availability_methods, "method", "agentops");
        ok = ok && interface.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&interface, "schema")),
                          "zcl.agent_interface.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&interface, "build_commit")),
                          zcl_build_commit()) == 0;
        ok = ok && strcmp(json_get_str(json_get(&interface,
                                                "preferred_transport")),
                          "mcp") == 0;
        ok = ok && strcmp(json_get_str(json_get(&interface,
                                                "capabilities_schema")),
                          "zcl.agent_capability.v1") == 0;
        ok = ok && interface_transports &&
            json_size(interface_transports) >= 3;
        ok = ok && json_array_has_substr(must_live_in_c,
                                         "deploy/restart safety decisions");
        ok = ok && json_array_has_substr(avoid,
                                         "do not require Python");
        ok = ok && capabilities && json_size(capabilities) >= 8;
        ok = ok && app_protocols_cap &&
            strcmp(json_get_str(json_get(app_protocols_cap, "schema")),
                   "zcl.application_protocols.index.v1") == 0;
        ok = ok && app_protocols_cap &&
            strcmp(json_get_str(json_get(app_protocols_cap, "mcp")),
                   "zcl_app_protocols") == 0;
        ok = ok && service_catalog_cap &&
            strcmp(json_get_str(json_get(service_catalog_cap, "schema")),
                   "zcl.service_catalog.v1") == 0;
        ok = ok && service_catalog_cap &&
            strcmp(json_get_str(json_get(service_catalog_cap, "mcp")),
                   "zcl_service_catalog") == 0;
        ok = ok && lane_topology &&
            strcmp(json_get_str(json_get(lane_topology, "schema")),
                   "zcl.agent_lanes.v1") == 0;
        ok = ok && lane_topology &&
            strcmp(json_get_str(json_get(lane_topology, "mcp")),
                   "zcl_agent_lanes") == 0;
        ok = ok && unified_liveness &&
            strcmp(json_get_str(json_get(unified_liveness, "schema")),
                   "zcl.agent_liveness.v1") == 0;
        ok = ok && unified_liveness &&
            strcmp(json_get_str(json_get(unified_liveness, "mcp")),
                   "zcl_agent_liveness") == 0;
        ok = ok && runtime_status &&
            strcmp(json_get_str(json_get(runtime_status, "mcp")),
                   "zcl_agent") == 0;
        ok = ok && runtime_status &&
            strcmp(json_get_str(json_get(runtime_status, "schema")),
                   "zcl.public_status.v1") == 0;
        ok = ok && runtime_status_alias &&
            strcmp(json_get_str(json_get(runtime_status_alias, "method")),
                   "status") == 0;
        ok = ok && runtime_status_alias &&
            strcmp(json_get_str(json_get(runtime_status_alias, "native")),
                   "zclassic23 status") == 0;
        ok = ok && runtime_status_alias &&
            strcmp(json_get_str(json_get(runtime_status_alias, "mcp")),
                   "zcl_agent") == 0;
        ok = ok && runtime_status_alias &&
            strcmp(json_get_str(json_get(runtime_status_alias, "schema")),
                   "zcl.public_status.v1") == 0;
        ok = ok && mirror_status &&
            strcmp(json_get_str(json_get(mirror_status, "schema")),
                   "zcl.mirror_status.v1") == 0;
        ok = ok && mirror_status &&
            strcmp(json_get_str(json_get(mirror_status, "native")),
                   "zclassic23 getmirrorstatus") == 0;
        ok = ok && mirror_status &&
            strcmp(json_get_str(json_get(mirror_status, "mcp")),
                   "zcl_mirror_status") == 0;
        ok = ok && deploy_cap &&
            strcmp(json_get_str(json_get(deploy_cap, "schema")),
                   "zcl.agent_deploy_guard.v1") == 0;
        ok = ok && state_catalog_cap &&
            strcmp(json_get_str(json_get(state_catalog_cap, "schema")),
                   "zcl.state_catalog.v1") == 0;
        ok = ok && state_catalog_cap &&
            strcmp(json_get_str(json_get(state_catalog_cap, "mcp")),
                   "zcl_state_catalog") == 0;
        ok = ok && timeline_cap &&
            strcmp(json_get_str(json_get(timeline_cap, "schema")),
                   "zcl.timeline.v1") == 0;
        ok = ok && timeline_cap &&
            strcmp(json_get_str(json_get(timeline_cap, "mcp")),
                   "zcl_timeline") == 0;
        ok = ok && subsystem_state_cap &&
            strcmp(json_get_str(json_get(subsystem_state_cap, "method")),
                   "dumpstate") == 0;
        ok = ok && subsystem_state_cap &&
            strcmp(json_get_str(json_get(subsystem_state_cap, "mcp")),
                   "zcl_state") == 0;
        ok = ok && subsystem_state_cap &&
            strcmp(json_get_str(json_get(subsystem_state_cap,
                                         "contract_source")),
                   "agent_contracts.def") == 0;
        ok = ok && !json_get_bool(json_get(subsystem_state_cap,
                                           "registry_alias"));
        ok = ok && semantic_state_cap &&
            strcmp(json_get_str(json_get(semantic_state_cap, "method")),
                   "dumpstate") == 0;
        ok = ok && semantic_state_cap &&
            strcmp(json_get_str(json_get(semantic_state_cap, "schema")),
                   json_get_str(json_get(subsystem_state_cap, "schema"))) == 0;
        ok = ok && semantic_state_cap &&
            strcmp(json_get_str(json_get(semantic_state_cap, "mcp")),
                   json_get_str(json_get(subsystem_state_cap, "mcp"))) == 0;
        ok = ok && semantic_state_cap &&
            json_get_bool(json_get(semantic_state_cap, "registry_alias"));
        ok = ok && semantic_state_cap &&
            strcmp(json_get_str(json_get(semantic_state_cap,
                                         "canonical_capability")),
                   "subsystem_state") == 0;
        ok = ok && node_log_cap &&
            strcmp(json_get_str(json_get(node_log_cap, "method")),
                   "getnodelog") == 0;
        ok = ok && bounded_logs_cap &&
            strcmp(json_get_str(json_get(bounded_logs_cap, "method")),
                   "getnodelog") == 0;
        ok = ok && bounded_logs_cap &&
            strcmp(json_get_str(json_get(bounded_logs_cap, "mcp")),
                   json_get_str(json_get(node_log_cap, "mcp"))) == 0;
        ok = ok && bounded_logs_cap &&
            json_get_bool(json_get(bounded_logs_cap, "registry_alias"));
        ok = ok && sql_cap &&
            strcmp(json_get_str(json_get(sql_cap, "method")),
                   "dbquery") == 0;
        ok = ok && select_sql_cap &&
            strcmp(json_get_str(json_get(select_sql_cap, "method")),
                   "dbquery") == 0;
        ok = ok && select_sql_cap &&
            strcmp(json_get_str(json_get(select_sql_cap, "mcp")),
                   json_get_str(json_get(sql_cap, "mcp"))) == 0;
        ok = ok && select_sql_cap &&
            json_get_bool(json_get(select_sql_cap, "registry_alias"));
        ok = ok && development_loop &&
            strcmp(json_get_str(json_get(development_loop,
                                         "subsystem_state")),
                   "zcl_state") == 0;
        ok = ok && development_loop &&
            strcmp(json_get_str(json_get(development_loop, "logs")),
                   "zcl_node_log") == 0;
        ok = ok && development_loop &&
            strcmp(json_get_str(json_get(development_loop, "database")),
                   "zcl_sql") == 0;
        ok = ok && machine &&
            strcmp(json_get_str(json_get(machine, "schema")),
                   "zcl.agent_machine_contract.v1") == 0;
        ok = ok && machine &&
            strcmp(json_get_str(json_get(machine, "payload")),
                   "json_object") == 0;
        ok = ok && json_get_bool(json_get(machine, "schema_required"));
        ok = ok && json_get_bool(json_get(machine,
                                          "transport_equivalent_payloads"));
        ok = ok && json_get_bool(json_get(machine, "no_python_required"));
        ok = ok && json_get_bool(json_get(machine, "no_tools_z_required"));
        ok = ok && runtime &&
            strcmp(json_get_str(json_get(runtime, "schema")),
                   "zcl.agent_runtime_identity.v1") == 0;
        ok = ok && runtime &&
            strcmp(json_get_str(json_get(runtime, "build_commit")),
                   zcl_build_commit()) == 0;
        ok = ok && runtime &&
            strcmp(json_get_str(json_get(runtime, "binary")),
                   "zclassic23") == 0;
        ok = ok && availability &&
            strcmp(json_get_str(json_get(availability, "schema")),
                   "zcl.agent_runtime_availability.v1") == 0;
        ok = ok && availability &&
            strcmp(json_get_str(json_get(availability,
                                         "availability_scope")),
                   "target_rpc_probe") == 0;
        ok = ok && availability &&
            strcmp(json_get_str(json_get(availability, "probe_status")),
                   "ok") == 0;
        ok = ok && availability &&
            strcmp(json_get_str(json_get(availability,
                                         "target_build_commit")),
                   "oldbuild") == 0;
        ok = ok && availability &&
            json_get_int(json_get(availability, "unsupported_count")) >= 1;
        ok = ok && availability &&
            strstr(json_get_str(json_get(availability,
                                         "safe_next_action")),
                   "do not call unsupported methods") != NULL;
        ok = ok && agent_method &&
            strcmp(json_get_str(json_get(agent_method,
                                         "target_runtime_support")),
                   "supported") == 0;
        ok = ok && json_get_bool(json_get(agent_method,
                                          "safe_to_call_target"));
        ok = ok && agentops_method &&
            strcmp(json_get_str(json_get(agentops_method,
                                         "target_runtime_support")),
                   "unsupported_method_not_found") == 0;
        ok = ok && agentops_method &&
            !json_get_bool(json_get(agentops_method,
                                    "target_runtime_supports"));
        ok = ok && agentops_method &&
            !json_get_bool(json_get(agentops_method,
                                    "safe_to_call_target"));
        ok = ok && agentops_method &&
            json_get_int(json_get(agentops_method,
                                  "rpc_error_code")) ==
            RPC_METHOD_NOT_FOUND;

        struct json_value probed_ops;
        json_init(&probed_ops);
        ok = ok && rpc_table_execute(&tbl, "agentops", &params,
                                     &probed_ops);
        const struct json_value *probed_ops_availability =
            json_get(&probed_ops, "runtime_availability");
        const struct json_value *probed_ops_methods =
            probed_ops_availability
                ? json_get(probed_ops_availability, "methods") : NULL;
        const struct json_value *probed_ops_method =
            find_object_with_str(probed_ops_methods, "method", "agentops");
        ok = ok && probed_ops_availability &&
            strcmp(json_get_str(json_get(probed_ops_availability,
                                         "availability_scope")),
                   "target_rpc_probe") == 0;
        ok = ok && probed_ops_method &&
            strcmp(json_get_str(json_get(probed_ops_method,
                                         "target_runtime_support")),
                   "unsupported_method_not_found") == 0;
        json_free(&probed_ops);
        agent_runtime_availability_reset();

        struct agent_resource_snapshot fixed_resources = {
            .rss_mb = 5000,
            .rss_warn_threshold_mb = 4096,
            .rss_warning = true,
            .cgroup_memory_available = false,
            .cgroup_memory_current_mb = -1,
            .cgroup_memory_high_mb = -1,
            .cgroup_memory_max_mb = -1,
            .cgroup_memory_high_pct = -1,
            .cgroup_memory_max_pct = -1,
            .cgroup_memory_stat_available = false,
            .cgroup_memory_anon_mb = -1,
            .cgroup_memory_file_mb = -1,
            .cgroup_memory_kernel_mb = -1,
            .cgroup_memory_inactive_file_mb = -1,
            .cgroup_memory_slab_reclaimable_mb = -1,
            .cgroup_memory_reclaimable_mb = -1,
            .cgroup_memory_working_set_mb = -1,
            .cgroup_memory_working_set_high_pct = -1,
            .cgroup_memory_working_set_max_pct = -1,
            .cgroup_memory_reclaimable_dominant = false,
            .cgroup_memory_watch = false,
            .cgroup_memory_warning = false,
            .uptime_seconds = 123,
        };
        struct json_value resources_body;
        json_init(&resources_body);
        json_set_object(&resources_body);
        agent_push_resources_json(&resources_body, "resources",
                                  &fixed_resources);
        const struct json_value *fixed =
            json_get(&resources_body, "resources");
        ok = ok && fixed && fixed->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(fixed, "schema")),
                          "zcl.node_resources.v1") == 0;
        ok = ok && json_get_int(json_get(fixed, "schema_version")) == 1;
        ok = ok && json_get_int(json_get(fixed, "rss_mb")) == 5000;
        ok = ok && json_get_bool(json_get(fixed, "rss_warning"));
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure")),
                          "warn") == 0;
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure_detail")),
                          "rss_over_threshold") == 0;
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "pressure_basis")),
                          "rss") == 0;
        json_free(&resources_body);
        json_free(&timeline_filtered);
        json_free(&timeline_filter_params);

        struct agent_resource_snapshot cgroup_resources = {
            .rss_mb = 9000,
            .rss_warn_threshold_mb = 4096,
            .rss_warning = true,
            .cgroup_memory_available = true,
            .cgroup_memory_current_mb = 9000,
            .cgroup_memory_high_mb = 12000,
            .cgroup_memory_max_mb = 16000,
            .cgroup_memory_high_pct = 75,
            .cgroup_memory_max_pct = 56,
            .cgroup_memory_stat_available = true,
            .cgroup_memory_anon_mb = 3000,
            .cgroup_memory_file_mb = 5000,
            .cgroup_memory_kernel_mb = 200,
            .cgroup_memory_inactive_file_mb = 4500,
            .cgroup_memory_slab_reclaimable_mb = 200,
            .cgroup_memory_reclaimable_mb = 4700,
            .cgroup_memory_working_set_mb = 4300,
            .cgroup_memory_working_set_high_pct = 35,
            .cgroup_memory_working_set_max_pct = 26,
            .cgroup_memory_reclaimable_dominant = true,
            .cgroup_memory_watch = false,
            .cgroup_memory_warning = false,
            .uptime_seconds = 456,
        };
        json_init(&resources_body);
        json_set_object(&resources_body);
        agent_push_resources_json(&resources_body, "resources",
                                  &cgroup_resources);
        fixed = json_get(&resources_body, "resources");
        ok = ok && fixed && fixed->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(fixed,
                                          "cgroup_memory_available"));
        ok = ok && json_get_int(json_get(fixed,
                                         "cgroup_memory_current_mb")) == 9000;
        ok = ok && json_get_bool(json_get(fixed,
                                          "cgroup_memory_stat_available"));
        ok = ok && json_get_int(json_get(fixed,
                                         "cgroup_memory_working_set_mb")) ==
            4300;
        ok = ok && json_get_bool(json_get(fixed,
            "cgroup_memory_reclaimable_dominant"));
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure")),
                          "ok") == 0;
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure_detail")),
                          "within_limits") == 0;
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "pressure_basis")),
                          "cgroup_high") == 0;
        json_free(&resources_body);

        cgroup_resources.cgroup_memory_current_mb = 10320;
        cgroup_resources.cgroup_memory_high_pct = 86;
        cgroup_resources.cgroup_memory_max_pct = 64;
        cgroup_resources.cgroup_memory_working_set_mb = 4300;
        cgroup_resources.cgroup_memory_working_set_high_pct = 35;
        cgroup_resources.cgroup_memory_working_set_max_pct = 26;
        cgroup_resources.cgroup_memory_reclaimable_dominant = true;
        cgroup_resources.cgroup_memory_watch = true;
        cgroup_resources.cgroup_memory_warning = false;
        json_init(&resources_body);
        json_set_object(&resources_body);
        agent_push_resources_json(&resources_body, "resources",
                                  &cgroup_resources);
        fixed = json_get(&resources_body, "resources");
        ok = ok && fixed && fixed->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(fixed,
                                          "cgroup_memory_watch"));
        ok = ok && !json_get_bool(json_get(fixed,
                                           "cgroup_memory_warning"));
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure")),
                          "watch") == 0;
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure_detail")),
                          "cgroup_cache_watch") == 0;
        json_free(&resources_body);

        cgroup_resources.cgroup_memory_current_mb = 11520;
        cgroup_resources.cgroup_memory_high_pct = 96;
        cgroup_resources.cgroup_memory_max_pct = 72;
        cgroup_resources.cgroup_memory_working_set_mb = 4800;
        cgroup_resources.cgroup_memory_working_set_high_pct = 40;
        cgroup_resources.cgroup_memory_working_set_max_pct = 30;
        cgroup_resources.cgroup_memory_reclaimable_dominant = true;
        cgroup_resources.cgroup_memory_watch = true;
        cgroup_resources.cgroup_memory_warning = false;
        json_init(&resources_body);
        json_set_object(&resources_body);
        agent_push_resources_json(&resources_body, "resources",
                                  &cgroup_resources);
        fixed = json_get(&resources_body, "resources");
        ok = ok && fixed && fixed->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(fixed,
                                          "cgroup_memory_watch"));
        ok = ok && !json_get_bool(json_get(fixed,
                                           "cgroup_memory_warning"));
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure")),
                          "watch") == 0;
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure_detail")),
                          "cgroup_reclaimable_cache_high") == 0;
        json_free(&resources_body);

        cgroup_resources.cgroup_memory_current_mb = 11400;
        cgroup_resources.cgroup_memory_high_pct = 95;
        cgroup_resources.cgroup_memory_max_pct = 71;
        cgroup_resources.cgroup_memory_working_set_mb = 10320;
        cgroup_resources.cgroup_memory_working_set_high_pct = 86;
        cgroup_resources.cgroup_memory_working_set_max_pct = 64;
        cgroup_resources.cgroup_memory_reclaimable_dominant = false;
        cgroup_resources.cgroup_memory_watch = true;
        cgroup_resources.cgroup_memory_warning = true;
        json_init(&resources_body);
        json_set_object(&resources_body);
        agent_push_resources_json(&resources_body, "resources",
                                  &cgroup_resources);
        fixed = json_get(&resources_body, "resources");
        ok = ok && fixed && fixed->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(fixed,
                                          "cgroup_memory_warning"));
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure")),
                          "warn") == 0;
        ok = ok && strcmp(json_get_str(json_get(fixed,
                                                "memory_pressure_detail")),
                          "cgroup_working_set_high") == 0;
        json_free(&resources_body);

        struct agent_restart_watchdog_snapshot wd = {
            .registered = true,
            .highest_tip = 3171111,
            .last_advance_unix = 1783268402,
            .age_secs = 45,
            .escalation_level = 0,
            .fires_mirror = 2,
            .fires_restart = 0,
            .fires_operator_needed = 0,
            .threshold_restart_secs = 1200,
            .persisted_stuck_height = 3171109,
            .no_progress_restarts = 1,
            .max_restarts = 3,
            .operator_needed = false,
        };
        struct json_value watchdog_body;
        json_init(&watchdog_body);
        json_set_object(&watchdog_body);
        agent_push_restart_watchdog_json(&watchdog_body,
                                         "restart_watchdog", &wd);
        const struct json_value *wd_json =
            json_get(&watchdog_body, "restart_watchdog");
        ok = ok && wd_json && wd_json->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(wd_json, "schema")),
                          "zcl.restart_watchdog.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(wd_json, "status")),
                          "restart_budget_burning") == 0;
        ok = ok && json_get_bool(json_get(wd_json,
                                          "last_restart_autonomous"));
        ok = ok && strcmp(json_get_str(json_get(wd_json,
                                                "last_restart_reason")),
                          "no_progress_tip_stall") == 0;
        ok = ok && json_get_int(json_get(wd_json,
                                         "persisted_stuck_height")) ==
            3171109;
        ok = ok && json_get_int(json_get(wd_json,
                                         "no_progress_restarts")) == 1;
        ok = ok && json_get_int(json_get(wd_json,
                                         "restarts_remaining")) == 2;
        ok = ok && strcmp(json_get_str(json_get(wd_json, "deep_state")),
                          "zclassic23 dumpstate chain_tip_watchdog") == 0;
        json_free(&watchdog_body);

        wd.no_progress_restarts = 3;
        wd.fires_restart = 3;
        wd.operator_needed = false;
        json_init(&watchdog_body);
        json_set_object(&watchdog_body);
        agent_push_restart_watchdog_json(&watchdog_body,
                                         "restart_watchdog", &wd);
        wd_json = json_get(&watchdog_body, "restart_watchdog");
        ok = ok && wd_json && wd_json->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(wd_json, "status")),
                          "restart_budget_exhausted") == 0;
        ok = ok && json_get_bool(json_get(wd_json,
                                          "restart_budget_exhausted"));
        ok = ok && json_get_int(json_get(wd_json,
                                         "restarts_remaining")) == 0;
        json_free(&watchdog_body);

        rpc_agent_set_boot_context("canonical", "full",
                                   "/tmp/zcl-agent-canonical",
                                   18232, 8033, 8443, 18033);
        struct json_value guard;
        json_init(&guard);
        struct json_value guard_params;
        json_init(&guard_params);
        json_set_array(&guard_params);
        struct json_value action;
        json_init(&action);
        json_set_str(&action, "canonical-deploy");
        json_push_back(&guard_params, &action);
        json_free(&action);
        ok = ok && rpc_table_execute(&tbl, "agentdeployguard",
                                     &guard_params, &guard);
        ok = ok && guard.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&guard, "schema")),
                          "zcl.agent_deploy_guard.v1") == 0;
        ok = ok && !json_get_bool(json_get(&guard, "allowed"));
        ok = ok && strcmp(json_get_str(json_get(&guard, "decision")),
                          "refuse") == 0;
        ok = ok && strcmp(json_get_str(json_get(&guard, "reason")),
                          "operator_confirmation_required") == 0;
        ok = ok && json_get_int(json_get(&guard, "exit_code")) == 1;
        ok = ok && strcmp(json_get_str(json_get(&guard,
                                                "operator_lane_name")),
                          "canonical") == 0;
        ok = ok && !json_get_bool(json_get(&guard,
                                           "automation_restart_ok"));
        ok = ok && !json_get_bool(json_get(&guard,
                                           "automation_deploy_ok"));
        ok = ok && json_get_bool(json_get(&guard,
                                          "requires_operator_confirmation"));
        ok = ok && strcmp(json_get_str(json_get(&guard,
                                                "preferred_deploy_target")),
                          "dev") == 0;
        ok = ok && strcmp(json_get_str(json_get(&guard,
                                                "safe_default_action")),
                          "observe_only_or_use_dev_lane") == 0;
        ok = ok && strcmp(json_get_str(json_get(&guard, "action_scope")),
                          "explicit_target_lane") == 0;
        ok = ok && strcmp(json_get_str(json_get(&guard,
                                                "current_lane_name")),
                          "canonical") == 0;
        ok = ok && strcmp(json_get_str(json_get(&guard,
                                                "target_lane_name")),
                          "canonical") == 0;
        const struct json_value *canonical_target =
            json_get(&guard, "target_lane");
        ok = ok && canonical_target &&
            canonical_target->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(canonical_target, "lane")),
                          "canonical") == 0;
        ok = ok && json_get_int(json_get(canonical_target,
                                         "https_port")) == 8443;

        const char *old_home_env = getenv("HOME");
        char old_home_buf[4096];
        bool old_home_set = old_home_env != NULL;
        if (old_home_set)
            snprintf(old_home_buf, sizeof(old_home_buf), "%s",
                     old_home_env);
        ok = ok && setenv("HOME", "/tmp/zcl-agent-home", 1) == 0;
        rpc_agent_set_boot_context("unknown", "full",
                                   "/tmp/zcl-agent-home/.zclassic-c23",
                                   18232, 8033, 8443, 18034);
        struct json_value inferred_guard_params;
        json_init(&inferred_guard_params);
        json_set_array(&inferred_guard_params);
        json_init(&action);
        json_set_str(&action, "deploy");
        json_push_back(&inferred_guard_params, &action);
        json_free(&action);
        struct json_value inferred_guard;
        json_init(&inferred_guard);
        ok = ok && rpc_table_execute(&tbl, "agentdeployguard",
                                     &inferred_guard_params,
                                     &inferred_guard);
        const struct json_value *inferred_lane =
            json_get(&inferred_guard, "operator_lane");
        ok = ok && inferred_lane && inferred_lane->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(inferred_lane, "lane")),
                          "canonical") == 0;
        ok = ok && strcmp(json_get_str(json_get(inferred_lane,
                                                "lane_source")),
                          "inferred_exact_topology") == 0;
        ok = ok && !json_get_bool(json_get(inferred_lane,
                                           "lane_declared"));
        ok = ok && json_get_bool(json_get(inferred_lane,
                                          "lane_inferred"));
        ok = ok && strcmp(json_get_str(json_get(&inferred_guard,
                                                "current_lane_name")),
                          "canonical") == 0;
        ok = ok && strcmp(json_get_str(json_get(&inferred_guard,
                                                "operator_lane_name")),
                          "canonical") == 0;
        ok = ok && strcmp(json_get_str(json_get(&inferred_guard,
                                                "guard_env")),
                          "ZCL_DEPLOY_ALLOW_CANONICAL") == 0;
        ok = ok && !json_get_bool(json_get(&inferred_guard, "allowed"));
        ok = ok && strcmp(json_get_str(json_get(&inferred_guard,
                                                "reason")),
                          "operator_confirmation_required") == 0;
        json_free(&inferred_guard_params);
        json_free(&inferred_guard);
        if (old_home_set)
            setenv("HOME", old_home_buf, 1);
        else
            unsetenv("HOME");

        struct json_value guard_object_params;
        json_init(&guard_object_params);
        json_set_object(&guard_object_params);
        json_push_kv_str(&guard_object_params, "action",
                         "canonical-restart");
        struct json_value guard_object;
        json_init(&guard_object);
        ok = ok && rpc_table_execute(&tbl, "agentdeployguard",
                                     &guard_object_params, &guard_object);
        ok = ok && strcmp(json_get_str(json_get(&guard_object, "action")),
                          "canonical-restart") == 0;
        ok = ok && !json_get_bool(json_get(&guard_object, "allowed"));

        const char *guard_old_home_env = getenv("HOME");
        char guard_old_home_buf[4096];
        bool guard_old_home_set = guard_old_home_env != NULL;
        if (guard_old_home_set)
            snprintf(guard_old_home_buf, sizeof(guard_old_home_buf), "%s",
                     guard_old_home_env);
        char guard_home[512];
        test_make_tmpdir(guard_home, sizeof(guard_home), "syncdiag",
                         "deploy_guard_home");
        char guard_devdir[768];
        int guard_devdir_len = snprintf(guard_devdir, sizeof(guard_devdir),
                                        "%s/.zclassic-c23-dev",
                                        guard_home);
        ok = ok && guard_devdir_len > 0 &&
            (size_t)guard_devdir_len < sizeof(guard_devdir);
        ok = ok && mkdir(guard_devdir, 0755) == 0;
        ok = ok && setenv("HOME", guard_home, 1) == 0;

        struct json_value dev_guard_params;
        json_init(&dev_guard_params);
        json_set_array(&dev_guard_params);
        json_init(&action);
        json_set_str(&action, "deploy-dev");
        json_push_back(&dev_guard_params, &action);
        json_free(&action);
        struct json_value dev_guard;
        json_init(&dev_guard);
        ok = ok && rpc_table_execute(&tbl, "agentdeployguard",
                                     &dev_guard_params, &dev_guard);
        ok = ok && strcmp(json_get_str(json_get(&dev_guard, "action")),
                          "deploy-dev") == 0;
        ok = ok && json_get_bool(json_get(&dev_guard, "allowed"));
        ok = ok && strcmp(json_get_str(json_get(&dev_guard, "decision")),
                          "allow") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard, "reason")),
                          "deployment_safety_allows_action") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard, "action_scope")),
                          "explicit_target_lane") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard,
                                                "current_lane_name")),
                          "canonical") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard,
                                                "operator_lane_name")),
                          "dev") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard, "lane")),
                          "dev") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard,
                                                "target_lane_name")),
                          "dev") == 0;
        const struct json_value *dev_target =
            json_get(&dev_guard, "target_lane");
        ok = ok && dev_target && dev_target->type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(dev_target, "development"));
        ok = ok && json_get_bool(json_get(dev_target,
                                          "automation_deploy_ok"));
        ok = ok && !json_get_bool(json_get(dev_target,
                                           "requires_operator_confirmation"));
        ok = ok && !json_get_bool(json_get(&dev_guard,
                                           "recovery_deploy_blocker"));
        ok = ok && strcmp(json_get_str(json_get(&dev_guard,
                                                "recovery_status")),
                          "clean") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard,
                                                "explicit_recovery_env")),
                          "ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY") == 0;
        const struct json_value *dev_recovery =
            json_get(dev_target, "recovery_state");
        ok = ok && dev_recovery && dev_recovery->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(dev_recovery, "schema")),
                          "zcl.operator_lane_recovery.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(dev_recovery, "status")),
                          "clean") == 0;
        ok = ok && !json_get_bool(json_get(dev_recovery,
                                           "deploy_blocker"));
        ok = ok && strcmp(json_get_str(json_get(dev_recovery,
                                                "explicit_recovery_env")),
                          "ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY") == 0;
        ok = ok && strcmp(json_get_str(json_get(&dev_guard,
                                                "safe_default_action")),
                          "deploy_dev_lane") == 0;

        ok = ok && boot_auto_reindex_request(guard_devdir, 3172354) == 1;
        struct json_value pending_guard;
        json_init(&pending_guard);
        ok = ok && rpc_table_execute(&tbl, "agentdeployguard",
                                     &dev_guard_params, &pending_guard);
        ok = ok && strcmp(json_get_str(json_get(&pending_guard, "action")),
                          "deploy-dev") == 0;
        ok = ok && !json_get_bool(json_get(&pending_guard, "allowed"));
        ok = ok && strcmp(json_get_str(json_get(&pending_guard,
                                                "decision")),
                          "refuse") == 0;
        ok = ok && strcmp(json_get_str(json_get(&pending_guard,
                                                "reason")),
                          "pending_auto_reindex_requires_explicit_recovery_boot")
            == 0;
        ok = ok && json_get_bool(json_get(&pending_guard,
                                          "recovery_deploy_blocker"));
        ok = ok && strcmp(json_get_str(json_get(&pending_guard,
                                                "recovery_status")),
                          "pending_auto_reindex") == 0;
        ok = ok && strcmp(json_get_str(json_get(&pending_guard,
                                                "explicit_recovery_env")),
                          "ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY") == 0;
        ok = ok && json_get_int(json_get(&pending_guard, "exit_code")) == 1;
        const struct json_value *pending_target =
            json_get(&pending_guard, "target_lane");
        const struct json_value *pending_recovery =
            pending_target ? json_get(pending_target, "recovery_state") : NULL;
        ok = ok && pending_recovery && pending_recovery->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(pending_recovery, "status")),
                          "pending_auto_reindex") == 0;
        ok = ok && json_get_bool(json_get(pending_recovery,
                                          "auto_reindex_marker_present"));
        ok = ok && json_get_bool(json_get(pending_recovery,
                                          "auto_reindex_status_well_formed"));
        ok = ok && json_get_bool(json_get(pending_recovery,
                                          "auto_reindex_pending"));
        ok = ok && !json_get_bool(json_get(pending_recovery,
                                           "auto_reindex_terminal"));
        ok = ok && json_get_bool(json_get(pending_recovery,
                                          "deploy_blocker"));
        ok = ok && json_get_int(json_get(pending_recovery,
                                         "auto_reindex_anchor")) == 3172354;
        ok = ok && json_get_int(json_get(pending_recovery,
                                         "auto_reindex_count")) == 1;
        ok = ok && strcmp(json_get_str(json_get(pending_recovery,
                                                "deploy_blocker_reason")),
                          "pending_auto_reindex_requires_explicit_recovery_boot")
            == 0;

        ok = ok && boot_auto_reindex_mark_terminal(guard_devdir, 3172354);
        struct json_value terminal_guard;
        json_init(&terminal_guard);
        ok = ok && rpc_table_execute(&tbl, "agentdeployguard",
                                     &dev_guard_params, &terminal_guard);
        ok = ok && json_get_bool(json_get(&terminal_guard, "allowed"));
        ok = ok && strcmp(json_get_str(json_get(&terminal_guard,
                                                "decision")),
                          "allow") == 0;
        ok = ok && !json_get_bool(json_get(&terminal_guard,
                                           "recovery_deploy_blocker"));
        ok = ok && strcmp(json_get_str(json_get(&terminal_guard,
                                                "recovery_status")),
                          "terminal_auto_reindex") == 0;
        const struct json_value *terminal_target =
            json_get(&terminal_guard, "target_lane");
        const struct json_value *terminal_recovery =
            terminal_target ? json_get(terminal_target,
                                       "recovery_state") : NULL;
        ok = ok && terminal_recovery && terminal_recovery->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(terminal_recovery, "status")),
                          "terminal_auto_reindex") == 0;
        ok = ok && !json_get_bool(json_get(terminal_recovery,
                                           "auto_reindex_pending"));
        ok = ok && json_get_bool(json_get(terminal_recovery,
                                          "auto_reindex_terminal"));
        ok = ok && !json_get_bool(json_get(terminal_recovery,
                                           "deploy_blocker"));
        ok = ok && json_get_int(json_get(terminal_recovery,
                                         "auto_reindex_count")) ==
            BOOT_AUTO_REINDEX_TERMINAL;

        if (guard_old_home_set)
            setenv("HOME", guard_old_home_buf, 1);
        else
            unsetenv("HOME");
        test_cleanup_tmpdir(guard_home);

        rpc_agent_set_boot_context("dev", "full",
                                   "/tmp/zcl-agent-dev",
                                   18252, 8053, 0, 18034);
        struct json_value canonical_from_dev_params;
        json_init(&canonical_from_dev_params);
        json_set_array(&canonical_from_dev_params);
        json_init(&action);
        json_set_str(&action, "canonical-deploy");
        json_push_back(&canonical_from_dev_params, &action);
        json_free(&action);
        struct json_value canonical_from_dev;
        json_init(&canonical_from_dev);
        ok = ok && rpc_table_execute(&tbl, "agentdeployguard",
                                     &canonical_from_dev_params,
                                     &canonical_from_dev);
        ok = ok && strcmp(json_get_str(json_get(&canonical_from_dev,
                                                "action")),
                          "canonical-deploy") == 0;
        ok = ok && !json_get_bool(json_get(&canonical_from_dev,
                                           "allowed"));
        ok = ok && strcmp(json_get_str(json_get(&canonical_from_dev,
                                                "decision")),
                          "refuse") == 0;
        ok = ok && strcmp(json_get_str(json_get(&canonical_from_dev,
                                                "reason")),
                          "operator_confirmation_required") == 0;
        ok = ok && strcmp(json_get_str(json_get(&canonical_from_dev,
                                                "action_scope")),
                          "explicit_target_lane") == 0;
        ok = ok && strcmp(json_get_str(json_get(&canonical_from_dev,
                                                "current_lane_name")),
                          "dev") == 0;
        ok = ok && strcmp(json_get_str(json_get(&canonical_from_dev,
                                                "target_lane_name")),
                          "canonical") == 0;
        ok = ok && strcmp(json_get_str(json_get(&canonical_from_dev,
                                                "operator_lane_name")),
                          "canonical") == 0;
        ok = ok && !json_get_bool(json_get(&canonical_from_dev,
                                           "automation_deploy_ok"));
        ok = ok && json_get_bool(json_get(&canonical_from_dev,
                                          "requires_operator_confirmation"));

        json_free(&params);
        json_free(&contracts);
        json_free(&ops);
        json_free(&timeline);
        json_free(&timeline_params);
        json_free(&catalog);
        json_free(&lanes);
        json_free(&build);
        json_free(&liveness);
        json_free(&interface);
        json_free(&guard_params);
        json_free(&guard);
        json_free(&guard_object_params);
        json_free(&guard_object);
        json_free(&dev_guard_params);
        json_free(&dev_guard);
        json_free(&pending_guard);
        json_free(&terminal_guard);
        json_free(&canonical_from_dev_params);
        json_free(&canonical_from_dev);
        rpc_agent_set_boot_context(NULL, NULL, NULL, 0, 0, 0, 0);
        if (old_quality_env_set)
            setenv("ZCL_QUALITY_STATE_DIR", old_quality_env_buf, 1);
        else
            unsetenv("ZCL_QUALITY_STATE_DIR");
        if (quality_fixture_ok) {
            unlink(quality_fuzz_file);
            rmdir(quality_status_dir);
            rmdir(quality_tmp);
        }

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC returns milestone ASCII bars... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "milestone",
                                          &params, &result);
        const struct json_value *ascii = json_get(&result, "ascii");
        const struct json_value *bars = json_get(&result, "bars");
        const struct json_value *criteria = json_get(&result, "criteria");
        const struct json_value *live = json_get(&result, "live");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.milestone_status.v1") == 0;
        ok = ok && json_get_int(json_get(&result,
                          "mvp_readiness_score")) == 4;
        ok = ok && ascii && strstr(json_get_str(json_get(ascii, "goals")),
                                   "goals [#####-----] 4/8") != NULL;
        ok = ok && bars && strcmp(json_get_str(json_get(json_get(bars,
                          "subgoals"), "bar")), "[########--]") == 0;
        ok = ok && criteria && json_size(criteria) == 8;
        ok = ok && live && strcmp(json_get_str(json_get(live, "source")),
                                  "agent_cached_summary") == 0;
        ok = ok && strcmp(json_get_str(json_get(live, "source_schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && json_get(live, "agent_status") != NULL;
        ok = ok && json_get(live, "readiness_status") != NULL;
        ok = ok && json_get(live, "height_contract_status") != NULL;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "mvpstatus",
                                                &params, &alias);
        ok = ok && alias_executed && alias.type == JSON_OBJ &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.milestone_status.v1") == 0;

        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC returns UTXO anchor rebuild readiness... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "refold",
                                          &params, &result);
        const struct json_value *snap = json_get(&result, "anchor_snapshot");
        const struct json_value *commands = json_get(&result, "commands");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.refold_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "api_version")),
                          "v1") == 0;
        ok = ok && strstr(json_get_str(json_get(&result, "purpose")),
                          "UTXO anchor rebuild") != NULL;
        ok = ok && strstr(json_get_str(json_get(&result, "plain_english")),
                          "borrowed snapshot seed") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "internal_mechanism")),
                          "-refold-from-anchor") == 0;
        ok = ok && !json_get_bool(json_get(&result, "ready_for_refold"));
        ok = ok && snap && json_get(snap, "verification") != NULL;
        ok = ok && commands &&
            strcmp(json_get_str(json_get(commands, "native")),
                   "zclassic23 refold") == 0;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "refoldstatus",
                                                &params, &alias);
        ok = ok && alias_executed && alias.type == JSON_OBJ &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.refold_status.v1") == 0;

        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("healthcheck: scopes zclassicd warmup as advisory when P2P "
           "is active (RED)... ");
    {
        struct connman cm;
        struct node_signals sigs;
        struct main_state ms;
        struct block_index tip;
        struct block_index best_header;
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;
        struct legacy_mirror_sync_stats stats;

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        memset(&ms, 0, sizeof(ms));
        memset(&tip, 0, sizeof(tip));
        memset(&best_header, 0, sizeof(best_header));
        memset(&stats, 0, sizeof(stats));

        bool ok = connman_init(&cm, chain_params_get(), &sigs);
        main_state_init(&ms);
        tip.nHeight = 3117074;
        best_header.nHeight = 3117074;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        ms.pindex_best_header = &best_header;
        ok = ok && syncdiag_add_peer(&cm, 21, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        ok = ok && syncdiag_add_peer(&cm, 22, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        ok = ok && syncdiag_add_peer(&cm, 23, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;

        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        block_source_policy_init(&cm, &ms, NULL);

        stats.enabled = true;
        stats.running = true;
        stats.reachable = false;
        stats.legacy_height = 0;
        stats.local_height = 3117074;
        stats.best_header_height = 3117074;
        stats.target_height = 3117074;
        stats.rpc_errors = 940;
        stats.last_attempt = 123456;
        snprintf(stats.last_blocker_id, sizeof(stats.last_blocker_id),
                 "%s", "rpc-unreachable");
        snprintf(stats.last_error, sizeof(stats.last_error),
                 "%s",
                 "rpc error -28: Activating best chain... height 0 (1%)");
        legacy_mirror_sync_test_set_stats(&stats, &ms);

        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();
        json_init(&params);
        json_set_object(&params);
        json_push_kv_bool(&params, "full", true);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "healthcheck", &params, &result);

        const struct json_value *checks = json_get(&result, "checks");
        const struct json_value *ca =
            checks ? json_get(checks, "chain_advance") : NULL;
        ok = ok && result.type == JSON_OBJ;
        ok = ok && ca && ca->type == JSON_OBJ;
        ok = ok && json_get(ca, "selected_source") != NULL &&
            strcmp(json_get_str(json_get(ca, "selected_source")),
                   "p2p") == 0;
        ok = ok && json_get(ca, "selected_source_trust") != NULL &&
            strcmp(json_get_str(json_get(ca, "selected_source_trust")),
                   "native_peer_validated") == 0;
        ok = ok && json_get(&result, "active_source") != NULL &&
            strcmp(json_get_str(json_get(&result, "active_source")),
                   "p2p") == 0;
        ok = ok && json_get(&result, "active_source_trust") != NULL &&
            strcmp(json_get_str(json_get(&result, "active_source_trust")),
                   "native_peer_validated") == 0;
        ok = ok && json_get(&result, "active_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result, "active_blocker")),
                   "") == 0;
        ok = ok && json_get(&result, "candidate_source") != NULL &&
            strcmp(json_get_str(json_get(&result, "candidate_source")),
                   "legacy_advisory") == 0;
        ok = ok && json_get(&result, "candidate_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result, "candidate_blocker")),
                   "") == 0;
        ok = ok && json_get(&result, "candidate_blocker_scope") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "candidate_blocker_scope")),
                   "advisory_only") == 0;
        ok = ok && json_get(&result, "legacy_advisory_blocker") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "legacy_advisory_blocker")),
                   "rpc-unreachable") == 0;
        ok = ok && json_get(&result, "mirror_monitor_running") != NULL &&
            json_get_bool(json_get(&result, "mirror_monitor_running"));
        ok = ok && json_get(&result,
                            "zclassicd_rpc_transport_reachable") != NULL &&
            json_get_bool(json_get(&result,
                                   "zclassicd_rpc_transport_reachable"));
        ok = ok && json_get(&result, "legacy_oracle_usable") != NULL &&
            !json_get_bool(json_get(&result, "legacy_oracle_usable"));
        ok = ok && json_get(&result, "zclassicd_rpc_error_code") != NULL &&
            json_get_int(json_get(&result,
                                  "zclassicd_rpc_error_code")) == -28;
        ok = ok && json_get(&result,
                            "zclassicd_rpc_error_message") != NULL &&
            strstr(json_get_str(json_get(
                       &result, "zclassicd_rpc_error_message")),
                   "Activating best chain") != NULL;
        ok = ok && json_get(&result, "mirror_rpc_errors") != NULL &&
            json_get_int(json_get(&result, "mirror_rpc_errors")) == 940;
        ok = ok && json_get(&result, "mirror_last_attempt") != NULL &&
            json_get_int(json_get(&result, "mirror_last_attempt")) == 123456;
        ok = ok && json_get(&result, "mirror_active_error_code") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_active_error_code")),
                   "rpc-unreachable") == 0;
        ok = ok && json_get(&result, "mirror_active_error_detail") != NULL &&
            strstr(json_get_str(json_get(
                       &result, "mirror_active_error_detail")),
                   "Activating best chain") != NULL;

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        main_state_free(&ms);
        connman_free(&cm);
    }

    printf("getsyncdetail: exposes mirror override safety context "
           "(RED)... ");
    {
        struct json_value result;
        json_init(&result);
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_override(300, "body-hash-mismatch");
        mirror_consensus_record_blocker("body-hash-mismatch");

        bool ok = api_getsyncdetail(&result);
        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get(&result, "mirror_unsafe_overrides_total") != NULL &&
            json_get_int(json_get(&result,
                                  "mirror_unsafe_overrides_total")) == 1;
        ok = ok && json_get(&result, "mirror_last_override_safe") != NULL &&
            !json_get_bool(json_get(&result, "mirror_last_override_safe"));
        ok = ok && json_get(&result, "mirror_last_override_reason") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_reason")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_last_override_scope") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_scope")),
                   "unsafe_no_authorized_scope") == 0;

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
        json_free(&result);
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
    }

    syncdiag_reset_rpc_globals_for_test();
    return failures;
}
