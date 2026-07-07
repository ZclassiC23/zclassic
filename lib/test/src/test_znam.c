/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for ZCL Names (ZNAM) — parser, builder, validator, DB persistence. */

#include "test/test_helpers.h"
#include "models/database.h"
#include "models/znam.h"

int test_znam(void)
{
    int failures = 0;

    /* ── Name validation ──────────────────────────────────────── */

    printf("\n=== ZNAM Tests ===\n");

    printf("znam_validate_name: valid lowercase alpha... ");
    if (znam_validate_name("alice")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: valid alphanumeric... ");
    if (znam_validate_name("node42")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: valid with hyphens... ");
    if (znam_validate_name("my-node-1")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject NULL... ");
    if (!znam_validate_name(NULL)) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject empty... ");
    if (!znam_validate_name("")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject uppercase... ");
    if (!znam_validate_name("Alice")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject leading hyphen... ");
    if (!znam_validate_name("-alice")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject trailing hyphen... ");
    if (!znam_validate_name("alice-")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject special chars... ");
    if (!znam_validate_name("alice@bob")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject spaces... ");
    if (!znam_validate_name("alice bob")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: single char... ");
    if (znam_validate_name("a")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: max length (63 chars)... ");
    {
        char name64[64];
        memset(name64, 'a', 63);
        name64[63] = '\0';
        if (znam_validate_name(name64)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_validate_name: too long (64 chars)... ");
    {
        char name65[65];
        memset(name65, 'a', 64);
        name65[64] = '\0';
        if (!znam_validate_name(name65)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_validate_name: reject dots... ");
    if (!znam_validate_name("alice.bob")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: reject underscore... ");
    if (!znam_validate_name("alice_bob")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("znam_validate_name: all digits... ");
    if (znam_validate_name("12345")) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    /* ── Build + Parse roundtrip: REGISTER ────────────────────── */

    printf("znam build+parse REGISTER roundtrip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "mynode", ZNAM_TYPE_TADDR,
                                         "t1abc123");
        if (len == 0) { printf("FAIL (build)\n"); failures++; }
        else {
            struct znam_message msg;
            bool ok = znam_parse(buf, len, &msg);
            if (ok && msg.command == ZNAM_CMD_REGISTER &&
                strcmp(msg.name, "mynode") == 0 &&
                msg.target_type == ZNAM_TYPE_TADDR &&
                strcmp(msg.target_value, "t1abc123") == 0) {
                printf("OK\n");
            } else {
                printf("FAIL (parse: ok=%d cmd=%d name=%s type=%d val=%s)\n",
                       ok, msg.command, msg.name, msg.target_type,
                       msg.target_value);
                failures++;
            }
        }
    }

    /* ── Build + Parse roundtrip: UPDATE ──────────────────────── */

    printf("znam build+parse UPDATE roundtrip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_update(buf, sizeof(buf),
                                       "mynode", ZNAM_TYPE_ONION,
                                       "abc123.onion");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        if (ok && msg.command == ZNAM_CMD_UPDATE &&
            strcmp(msg.name, "mynode") == 0 &&
            msg.target_type == ZNAM_TYPE_ONION &&
            strcmp(msg.target_value, "abc123.onion") == 0) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
    }

    /* ── Build + Parse roundtrip: TRANSFER ────────────────────── */

    printf("znam build+parse TRANSFER roundtrip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_transfer(buf, sizeof(buf),
                                         "mynode", "t1newowner");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        if (ok && msg.command == ZNAM_CMD_TRANSFER &&
            strcmp(msg.name, "mynode") == 0 &&
            strcmp(msg.new_owner, "t1newowner") == 0) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
    }

    /* ── Build + Parse roundtrip: RENEW ───────────────────────── */

    printf("znam build+parse RENEW roundtrip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_renew(buf, sizeof(buf), "mynode");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        if (ok && msg.command == ZNAM_CMD_RENEW &&
            strcmp(msg.name, "mynode") == 0) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
    }

    /* ── Build + Parse roundtrip: SET_RECORD ──────────────────── */

    printf("znam build+parse SET_RECORD roundtrip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_set_record(buf, sizeof(buf),
                                           "mynode", ZNAM_TYPE_BTC,
                                           "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        if (ok && msg.command == ZNAM_CMD_SET_RECORD &&
            strcmp(msg.name, "mynode") == 0 &&
            msg.target_type == ZNAM_TYPE_BTC &&
            strcmp(msg.target_value, "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa") == 0) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
    }

    /* ── Build + Parse roundtrip: SET_TEXT ─────────────────────── */

    printf("znam build+parse SET_TEXT roundtrip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_set_text(buf, sizeof(buf),
                                         "mynode", "email",
                                         "user@example.com");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        if (ok && msg.command == ZNAM_CMD_SET_TEXT &&
            strcmp(msg.name, "mynode") == 0 &&
            strcmp(msg.text_key, "email") == 0 &&
            strcmp(msg.text_value, "user@example.com") == 0) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
    }

    /* ── SET_TEXT with empty value ─────────────────────────────── */

    printf("znam build+parse SET_TEXT empty value roundtrip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_set_text(buf, sizeof(buf),
                                         "mynode", "avatar", "");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_SET_TEXT;
        ok = ok && strcmp(msg.name, "mynode") == 0;
        ok = ok && strcmp(msg.text_key, "avatar") == 0;
        ok = ok && msg.text_value[0] == '\0';
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Parser edge cases ────────────────────────────────────── */

    printf("znam_parse: reject empty script... ");
    {
        struct znam_message msg;
        if (!znam_parse(NULL, 0, &msg)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_parse: reject null output message... ");
    {
        uint8_t script[] = {0x6a};
        if (!znam_parse(script, sizeof(script), NULL)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_parse: reject non-OP_RETURN... ");
    {
        uint8_t bad[] = {0x76, 0x04, 'Z', 'N', 'A', 'M'};
        struct znam_message msg;
        if (!znam_parse(bad, sizeof(bad), &msg)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_parse: reject wrong lokad ID... ");
    {
        uint8_t bad[] = {0x6a, 0x04, 'Z', 'S', 'L', 'P', 0x01, 0x01, 0x01, 0x01, 0x05, 'h', 'e', 'l', 'l', 'o'};
        struct znam_message msg;
        if (!znam_parse(bad, sizeof(bad), &msg)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Builder validation ───────────────────────────────────── */

    printf("znam_build_register: reject invalid name... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "INVALID", ZNAM_TYPE_TADDR, "t1abc");
        if (len == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_register: reject invalid target_type... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "valid", 0, "t1abc");
        if (len == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── REGISTER/UPDATE accept multi-coin types (parser parity) ── */

    printf("znam_build_register: accepts ZNAM_TYPE_BTC... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "alice", ZNAM_TYPE_BTC,
                                         "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg) &&
                  msg.command == ZNAM_CMD_REGISTER &&
                  msg.target_type == ZNAM_TYPE_BTC;
        if (ok) printf("OK\n");
        else { printf("FAIL (len=%zu)\n", len); failures++; }
    }

    printf("znam_build_register: accepts ZNAM_TYPE_LTC... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "bob", ZNAM_TYPE_LTC,
                                         "LcHKJaR1U8nzzcRhMqg9PbM9Nqv9UJbJKV");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg) &&
                  msg.target_type == ZNAM_TYPE_LTC;
        if (ok) printf("OK\n");
        else { printf("FAIL (len=%zu)\n", len); failures++; }
    }

    printf("znam_build_register: accepts ZNAM_TYPE_DOGE... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "carol", ZNAM_TYPE_DOGE,
                                         "DH5yaieqoZN36fDVciNyRueRGvGLR3mr7L");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg) &&
                  msg.target_type == ZNAM_TYPE_DOGE;
        if (ok) printf("OK\n");
        else { printf("FAIL (len=%zu)\n", len); failures++; }
    }

    printf("znam_build_register: accepts ZNAM_TYPE_CONTENT... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "dave", ZNAM_TYPE_CONTENT,
                                         "a1b2c3d4e5f6");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg) &&
                  msg.target_type == ZNAM_TYPE_CONTENT;
        if (ok) printf("OK\n");
        else { printf("FAIL (len=%zu)\n", len); failures++; }
    }

    printf("znam_build_update: accepts ZNAM_TYPE_BTC... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_update(buf, sizeof(buf),
                                       "alice", ZNAM_TYPE_BTC,
                                       "1NewAddress");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg) &&
                  msg.command == ZNAM_CMD_UPDATE &&
                  msg.target_type == ZNAM_TYPE_BTC;
        if (ok) printf("OK\n");
        else { printf("FAIL (len=%zu)\n", len); failures++; }
    }

    printf("znam_build_register: rejects type > ZNAM_TYPE_CONTENT... ");
    {
        uint8_t buf[256];
        /* 8 is above ZNAM_TYPE_CONTENT (7) — still out of spec. */
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "eve", 8, "value");
        if (len == 0) printf("OK\n");
        else { printf("FAIL (accepted type=8)\n"); failures++; }
    }

    printf("znam_build_update: rejects type > ZNAM_TYPE_CONTENT... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_update(buf, sizeof(buf),
                                       "eve", 255, "value");
        if (len == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_register: reject null value... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_register(buf, sizeof(buf),
                                         "valid", ZNAM_TYPE_TADDR, NULL);
        if (len == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_set_text: reject empty key... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_set_text(buf, sizeof(buf),
                                         "valid", "", "val");
        if (len == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_transfer: reject null new_owner... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_transfer(buf, sizeof(buf),
                                         "valid", NULL);
        if (len == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── SET_RECORD with all target types ─────────────────────── */

    printf("znam SET_RECORD all target types roundtrip... ");
    {
        bool all_ok = true;
        for (uint8_t t = 1; t <= ZNAM_TYPE_CONTENT; t++) {
            uint8_t buf[256];
            size_t len = znam_build_set_record(buf, sizeof(buf),
                                               "multi", t, "someaddr");
            struct znam_message msg;
            if (!znam_parse(buf, len, &msg) || msg.target_type != t) {
                all_ok = false;
                break;
            }
        }
        if (all_ok) printf("OK (types 1-%d)\n", ZNAM_TYPE_CONTENT);
        else { printf("FAIL\n"); failures++; }
    }

    /* ── SQLite persistence ───────────────────────────────────── */

    printf("znam DB save+find+list roundtrip... ");
    {
        sqlite3 *db = NULL;
        int rc = sqlite3_open(":memory:", &db);
        if (rc != SQLITE_OK) { printf("FAIL (open)\n"); failures++; }
        else {
            /* Create tables */
            sqlite3_exec(db,
                "CREATE TABLE znam_names("
                "name TEXT PRIMARY KEY, owner_address TEXT,"
                "target_type INTEGER, target_value TEXT,"
                "reg_txid BLOB, reg_height INTEGER,"
                "last_update_txid BLOB)", NULL, NULL, NULL);
            sqlite3_exec(db,
                "CREATE TABLE znam_text_records("
                "name TEXT, key TEXT, value TEXT,"
                "PRIMARY KEY(name,key))", NULL, NULL, NULL);
            sqlite3_exec(db,
                "CREATE TABLE znam_addr_records("
                "name TEXT, coin_type INTEGER, address TEXT,"
                "PRIMARY KEY(name,coin_type))", NULL, NULL, NULL);

            struct node_db ndb = { .db = db, .open = true };

            struct znam_entry entry = {0};
            snprintf(entry.name, sizeof(entry.name), "alice");
            snprintf(entry.owner_address, sizeof(entry.owner_address), "t1owner");
            entry.target_type = ZNAM_TYPE_TADDR;
            snprintf(entry.target_value, sizeof(entry.target_value), "t1target");
            memset(entry.reg_txid, 0xAA, 32);
            entry.reg_height = 12345;
            memset(entry.last_update_txid, 0xBB, 32);

            bool save_ok = db_znam_save(&ndb, &entry);
            struct znam_entry found = {0};
            bool find_ok = db_znam_find(&ndb, "alice", &found);

            struct znam_entry list[10];
            int count = db_znam_list(&ndb, list, 10);

            if (save_ok && find_ok &&
                strcmp(found.name, "alice") == 0 &&
                strcmp(found.owner_address, "t1owner") == 0 &&
                found.target_type == ZNAM_TYPE_TADDR &&
                strcmp(found.target_value, "t1target") == 0 &&
                found.reg_height == 12345 &&
                count == 1) {
                printf("OK\n");
            } else {
                printf("FAIL (save=%d find=%d count=%d)\n",
                       save_ok, find_ok, count);
                failures++;
            }

            /* Text records */
            printf("znam DB text records save+get+list... ");
            bool t_save = db_znam_text_save(&ndb, "alice", "email", "a@b.com");
            bool t_save2 = db_znam_text_save(&ndb, "alice", "url", "https://x.com");
            char val[256] = {0};
            bool t_get = db_znam_text_get(&ndb, "alice", "email", val, sizeof(val));
            struct znam_text_record texts[10];
            int t_count = db_znam_text_list(&ndb, "alice", texts, 10);

            if (t_save && t_save2 && t_get &&
                strcmp(val, "a@b.com") == 0 && t_count == 2) {
                printf("OK\n");
            } else {
                printf("FAIL (save=%d get=%d val=%s count=%d)\n",
                       t_save && t_save2, t_get, val, t_count);
                failures++;
            }

            /* Address records */
            printf("znam DB addr records save+get+list... ");
            bool a_save = db_znam_addr_save(&ndb, "alice", ZNAM_TYPE_BTC, "1abc");
            bool a_save2 = db_znam_addr_save(&ndb, "alice", ZNAM_TYPE_LTC, "Labc");
            char addr[256] = {0};
            bool a_get = db_znam_addr_get(&ndb, "alice", ZNAM_TYPE_BTC, addr, sizeof(addr));
            struct znam_addr_record addrs[10];
            int a_count = db_znam_addr_list(&ndb, "alice", addrs, 10);
            if (a_save && a_save2 && a_get && strcmp(addr, "1abc") == 0 &&
                a_count == 2 && addrs[0].coin_type == ZNAM_TYPE_BTC &&
                addrs[1].coin_type == ZNAM_TYPE_LTC &&
                strcmp(addrs[1].address, "Labc") == 0) {
                printf("OK\n");
            } else {
                printf("FAIL (save=%d get=%d count=%d)\n",
                       a_save && a_save2, a_get, a_count);
                failures++;
            }

            /* List by owner */
            printf("znam DB list_by_owner... ");
            struct znam_entry by_owner[10];
            int owner_count = db_znam_list_by_owner(&ndb, "t1owner", by_owner, 10);
            if (owner_count == 1 && strcmp(by_owner[0].name, "alice") == 0) {
                printf("OK\n");
            } else {
                printf("FAIL (count=%d)\n", owner_count); failures++;
            }

            /* Find non-existent */
            printf("znam DB find non-existent... ");
            struct znam_entry none;
            bool found_none = db_znam_find(&ndb, "nonexistent", &none);
            if (!found_none) printf("OK\n");
            else { printf("FAIL\n"); failures++; }

            /* Null DB guard */
            printf("znam DB null guard... ");
            if (!db_znam_save(NULL, &entry) &&
                !db_znam_find(NULL, "x", &none)) {
                printf("OK\n");
            } else { printf("FAIL\n"); failures++; }

            sqlite3_close(db);
        }
    }

    printf("\n%d ZNAM test(s) failed\n", failures);
    return failures;
}
