/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Focused app-model tests. */

#include "test/test_helpers.h"
#include "models/file_service.h"
#include <unistd.h>

int test_model_app(void)
{
    int failures = 0;

    printf("Contact model validates and orders recent contacts... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_contacts_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);
        if (ok) {
            struct db_contact bad, a, b, out[2];
            struct ar_errors e;
            memset(&bad, 0, sizeof(bad));
            ar_errors_clear(&e);
            db_contact_validate(&bad, &e);
            ok = ar_errors_any(&e);

            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));
            snprintf(a.address, sizeof(a.address), "%s", "t1Alice");
            snprintf(a.name, sizeof(a.name), "%s", "Alice");
            a.last_used = 10;
            snprintf(b.address, sizeof(b.address), "%s", "t1Bob");
            snprintf(b.name, sizeof(b.name), "%s", "Bob");
            b.last_used = 20;

            ok = ok && db_contact_save(&ndb, &a);
            ok = ok && db_contact_save(&ndb, &b);
            if (ok && db_contact_recent(&ndb, out, 2) != 2) {
                ok = false;
            }
            if (ok && strcmp(out[0].name, "Bob") != 0) {
                ok = false;
            }
            if (ok && strcmp(out[1].name, "Alice") != 0) {
                ok = false;
            }
            node_db_close(&ndb);
        }
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Contact model trims whitespace before save... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_contact_trim_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);
        if (ok) {
            struct db_contact c;
            struct db_contact out[1];
            memset(&c, 0, sizeof(c));
            memset(out, 0, sizeof(out));
            snprintf(c.address, sizeof(c.address), "%s", "  t1TrimMe  ");
            snprintf(c.name, sizeof(c.name), "%s", "  Alice Trim  ");
            ok = db_contact_save(&ndb, &c);
            if (ok && strcmp(c.address, "t1TrimMe") != 0) {
                ok = false;
            }
            if (ok && strcmp(c.name, "Alice Trim") != 0) {
                ok = false;
            }
            if (ok && db_contact_recent(&ndb, out, 1) != 1) {
                ok = false;
            }
            if (ok && strcmp(out[0].address, "t1TrimMe") != 0) {
                ok = false;
            }
            if (ok && strcmp(out[0].name, "Alice Trim") != 0) {
                ok = false;
            }
            node_db_close(&ndb);
        }
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Onion announcement model validates suffix and persists... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_onion_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);
        if (ok) {
            struct db_onion_announcement bad, good;
            struct ar_errors e;
            memset(&bad, 0, sizeof(bad));
            snprintf(bad.onion_address, sizeof(bad.onion_address), "%s", "not-onion");
            ar_errors_clear(&e);
            db_onion_announcement_validate(&bad, &e);
            ok = ar_errors_any(&e);

            memset(&good, 0, sizeof(good));
            snprintf(good.onion_address, sizeof(good.onion_address),
                     "%s",
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion");
            snprintf(good.script_hex, sizeof(good.script_hex), "%s", "6a01");
            ok = ok && db_onion_announcement_save(&ndb, &good);
            ok = ok && db_onion_announcement_exists(&ndb, good.onion_address);
            node_db_close(&ndb);
        }
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Onion announcement model normalizes and lists recent rows... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_onion_recent_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);
        if (ok) {
            struct db_onion_announcement first, second, listed[2];

            memset(&first, 0, sizeof(first));
            memset(&second, 0, sizeof(second));
            memset(listed, 0, sizeof(listed));

            snprintf(first.onion_address, sizeof(first.onion_address),
                     "%s", "  BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB.onion  ");
            snprintf(first.script_hex, sizeof(first.script_hex), "%s", "  6A01AA  ");
            first.announced_at = 10;
            ok = db_onion_announcement_save(&ndb, &first);
            if (ok && strcmp(first.onion_address,
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.onion") != 0) {
                ok = false;
            }
            if (ok && strcmp(first.script_hex, "6a01aa") != 0) {
                ok = false;
            }

            snprintf(second.onion_address, sizeof(second.onion_address),
                     "%s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion");
            snprintf(second.script_hex, sizeof(second.script_hex), "%s", "6a02");
            second.announced_at = 20;
            ok = ok && db_onion_announcement_save(&ndb, &second);
            if (ok && db_onion_announcement_recent(&ndb, listed, 2) != 2) {
                ok = false;
            }
            if (ok && strcmp(listed[0].onion_address,
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion") != 0) {
                ok = false;
            }
            if (ok && strcmp(listed[1].onion_address,
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.onion") != 0) {
                ok = false;
            }
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Store product/order models validate and persist... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_store_models_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);
        if (ok) {
            struct db_store_product p;
            struct db_store_order o;
            struct ar_errors e;
            struct db_store_product got_product;
            struct db_store_product active_products[4];
            struct db_store_order_view order_view;
            struct db_store_order_summary order_summaries[4];
            struct db_store_pending_payment pending[4];

            memset(&p, 0, sizeof(p));
            ar_errors_clear(&e);
            db_store_product_validate(&p, &e);
            ok = ar_errors_any(&e);

            memset(&p, 0, sizeof(p));
            snprintf(p.name, sizeof(p.name), "%s", "Widget");
            snprintf(p.description, sizeof(p.description), "%s", "Premium widget");
            snprintf(p.token_id, sizeof(p.token_id), "%s", " widget ");
            p.price_zatoshi = 1000;
            p.tokens_per_purchase = 2;
            p.active = true;
            ok = ok && db_store_product_save(&ndb, &p);
            if (ok && strcmp(p.token_id, "WIDGET") != 0) {
                fprintf(stderr, "store: product token_id='%s'\n", p.token_id);
                ok = false;
            }

            memset(&got_product, 0, sizeof(got_product));
            memset(active_products, 0, sizeof(active_products));
            if (ok && !db_store_product_find_active(&ndb, 1, &got_product)) {
                fprintf(stderr, "store: find active product failed\n");
                ok = false;
            }
            if (ok && strcmp(got_product.token_id, "WIDGET") != 0) {
                fprintf(stderr, "store: got_product.token_id='%s'\n", got_product.token_id);
                ok = false;
            }
            if (ok && db_store_product_list_active(&ndb, active_products, 4) != 1) {
                fprintf(stderr, "store: active product count mismatch\n");
                ok = false;
            }

            memset(&o, 0, sizeof(o));
            o.product_id = 1;
            snprintf(o.customer_addr, sizeof(o.customer_addr), "%s", " t1Buyer ");
            snprintf(o.payment_addr, sizeof(o.payment_addr), "%s", " zs1paymentaddress ");
            o.amount_zatoshi = 1000;
            o.status = 0;
            ok = ok && db_store_order_save(&ndb, &o);
            ok = ok && (o.id > 0);
            if (ok && strcmp(o.customer_addr, "t1Buyer") != 0) {
                fprintf(stderr, "store: customer_addr='%s'\n", o.customer_addr);
                ok = false;
            }
            if (ok && strcmp(o.payment_addr, "zs1paymentaddress") != 0) {
                fprintf(stderr, "store: payment_addr='%s'\n", o.payment_addr);
                ok = false;
            }
            if (ok && !db_store_order_mark_paid(&ndb, o.id, 2)) {
                fprintf(stderr, "store: mark_paid failed\n");
                ok = false;
            }

            memset(&order_view, 0, sizeof(order_view));
            memset(order_summaries, 0, sizeof(order_summaries));
            memset(pending, 0, sizeof(pending));
            if (ok && !db_store_order_find_view(&ndb, o.id, &order_view)) {
                fprintf(stderr, "store: order view lookup failed\n");
                ok = false;
            }
            if (ok && strcmp(order_view.customer_addr, "t1Buyer") != 0) {
                fprintf(stderr, "store: order_view.customer_addr='%s'\n",
                        order_view.customer_addr);
                ok = false;
            }
            if (ok && db_store_order_list_recent(&ndb, order_summaries, 4) != 1) {
                fprintf(stderr, "store: order recent count mismatch\n");
                ok = false;
            }
            if (ok && db_store_order_list_pending_payments(&ndb, pending, 4, 0) != 0) {
                fprintf(stderr, "store: pending payment count mismatch\n");
                ok = false;
            }
            node_db_close(&ndb);
        }
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("File service model normalizes defaults and orders recent rows... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_file_services_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);
        if (ok) {
            struct db_file_service a, b, listed[2];

            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));
            memset(listed, 0, sizeof(listed));
            memset(a.ip, 0x11, sizeof(a.ip));
            a.port = 8080;
            a.p2p_port = 0;
            a.last_seen = 0;
            a.is_zcl23 = true;
            ok = db_file_service_save(&ndb, &a);
            ok = ok && (a.p2p_port == 8080);
            ok = ok && (a.last_seen > 0);

            memset(b.ip, 0x22, sizeof(b.ip));
            b.port = 8081;
            b.p2p_port = 9001;
            b.last_seen = a.last_seen + 5;
            b.is_zcl23 = false;
            ok = ok && db_file_service_save(&ndb, &b);
            ok = ok && (db_file_service_recent(&ndb, listed, 2) == 2);
            ok = ok && (listed[0].port == 8081);
            ok = ok && (listed[1].p2p_port == 8080);
            node_db_close(&ndb);
        }
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Peer model normalizes defaults and orders recent rows... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_peers_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);
        if (ok) {
            struct db_peer a, b, c, d, listed[2], fast[4];

            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));
            memset(&c, 0, sizeof(c));
            memset(&d, 0, sizeof(d));
            memset(listed, 0, sizeof(listed));
            memset(fast, 0, sizeof(fast));
            memset(a.ip, 0x31, sizeof(a.ip));
            a.port = 8333;
            a.services = 9;
            a.last_seen = 0;
            a.last_try = -9;
            a.attempts = -1;
            a.bandwidth_score = 40;
            a.is_zcl23 = true;
            ok = db_peer_save(&ndb, &a);
            ok = ok && (a.last_seen > 0);
            ok = ok && (a.last_try == 0);
            ok = ok && (a.attempts == 0);

            memset(b.ip, 0x32, sizeof(b.ip));
            memset(b.source, 0x44, sizeof(b.source));
            b.has_source = true;
            b.port = 8334;
            b.services = 1;
            b.last_seen = a.last_seen + 5;
            b.bandwidth_score = 100;
            ok = ok && db_peer_save(&ndb, &b);
            ok = ok && (db_peer_recent(&ndb, listed, 2) == 2);
            ok = ok && (listed[0].port == 8334);
            ok = ok && listed[0].has_source;
            ok = ok && (listed[1].port == 8333);

            memset(c.ip, 0x33, sizeof(c.ip));
            c.port = 53100;
            c.services = 1;
            c.last_seen = a.last_seen + 10;
            c.bandwidth_score = 255;
            c.is_zcl23 = true;
            ok = ok && db_peer_save(&ndb, &c);

            memset(d.ip, 0x34, sizeof(d.ip));
            d.port = 8033;
            d.services = 1;
            d.last_seen = a.last_seen + 11;
            d.bandwidth_score = 1;
            d.is_zcl23 = true;
            ok = ok && db_peer_save(&ndb, &d);
            ok = ok && (db_peer_fast_zcl23(&ndb, fast, 4) == 1);
            ok = ok && (fast[0].port == 8033);
            node_db_close(&ndb);
        }
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
