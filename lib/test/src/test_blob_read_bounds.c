/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "models/database.h"
#include "models/file_offer.h"
#include "models/swap_contract.h"
#include "models/znam.h"
#include "models/zmsg.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "test_blob_read_bounds sql failed: %s\n",
                err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return false;
    }
    return true;
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != 0)
            return false;
    return true;
}

int test_blob_read_bounds(void)
{
    int failures = 0;

    printf("blob_read_bounds: malformed fixed blobs zero via model APIs... ");
    {
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        bool ok = node_db_open(&ndb, ":memory:");

        ok = ok && exec_sql(ndb.db,
            "INSERT INTO file_offers"
            "(root_hash,filename,size_bytes,num_chunks,price_per_mb,"
            "z_addr,peer_ip,peer_port,last_seen,ttl)"
            " VALUES(X'aa','bad.dat',100,1,0,NULL,X'0102',8033,1,1)");

        ok = ok && exec_sql(ndb.db,
            "INSERT INTO zswp_contracts"
            "(swap_id,role,state,chain,secret_hash,secret,amount,locktime,"
            "my_address,counter_address,funding_txid,funding_vout,"
            "redeem_script,redeem_script_len,p2sh_address,created_at)"
            " VALUES('bad',0,0,0,X'aa',X'bb',1,960,"
            "'me','them',X'cc',0,X'deadbeef',64,'p2sh',1)");

        ok = ok && exec_sql(ndb.db,
            "INSERT INTO znam_names"
            "(name,owner_address,target_type,target_value,"
            "reg_txid,reg_height,last_update_txid)"
            " VALUES('bad','owner',1,'target',X'aa',1,X'bb')");

        ok = ok && exec_sql(ndb.db,
            "INSERT INTO zmsg_messages"
            "(msg_id,direction,channel,sender,recipient,body,"
            "timestamp,txid,read)"
            " VALUES(X'aa',0,1,'sender','recipient','body',1,X'bb',0)");

        struct file_offer offers[1];
        memset(offers, 0xff, sizeof(offers));
        int offer_count = ok ? db_file_offer_list(&ndb, offers, 1) : 0;
        ok = ok && offer_count == 1;
        ok = ok && all_zero(offers[0].root_hash, sizeof(offers[0].root_hash));
        ok = ok && all_zero(offers[0].z_addr, sizeof(offers[0].z_addr));
        ok = ok && all_zero(offers[0].peer_ip, sizeof(offers[0].peer_ip));

        struct swap_contract swap;
        memset(&swap, 0xff, sizeof(swap));
        ok = ok && db_swap_find(&ndb, "bad", &swap);
        ok = ok && all_zero(swap.secret_hash, sizeof(swap.secret_hash));
        ok = ok && !swap.has_secret;
        ok = ok && all_zero(swap.secret, sizeof(swap.secret));
        ok = ok && all_zero(swap.funding_txid, sizeof(swap.funding_txid));
        ok = ok && swap.redeem_script_len == 0;
        ok = ok && all_zero(swap.redeem_script, sizeof(swap.redeem_script));

        struct znam_entry znams[1];
        memset(znams, 0xff, sizeof(znams));
        int znam_count = ok ? db_znam_list(&ndb, znams, 1) : 0;
        ok = ok && znam_count == 1;
        ok = ok && all_zero(znams[0].reg_txid, sizeof(znams[0].reg_txid));
        ok = ok && all_zero(znams[0].last_update_txid,
                            sizeof(znams[0].last_update_txid));

        struct zmsg_message msgs[1];
        memset(msgs, 0xff, sizeof(msgs));
        int msg_count = ok ? db_zmsg_list(&ndb, msgs, 1, false) : 0;
        ok = ok && msg_count == 1;
        ok = ok && all_zero(msgs[0].msg_id, sizeof(msgs[0].msg_id));
        ok = ok && all_zero(msgs[0].txid, sizeof(msgs[0].txid));

        node_db_close(&ndb);

        if (ok) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
    }

    return failures;
}
