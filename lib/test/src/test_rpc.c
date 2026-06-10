/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "controllers/diagnostics_controller.h"
#include "rpc/httpserver.h"
#include "rpc/legacy_rpc_client.h"
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int test_rpc(void) {
    int failures = 0;

    printf("json null/bool/int/str... ");
    {
        struct json_value v;
        json_init(&v);
        bool ok = json_is_null(&v);

        json_set_bool(&v, true);
        ok = ok && json_get_bool(&v);

        json_set_int(&v, 42);
        ok = ok && json_get_int(&v) == 42;

        json_set_str(&v, "hello");
        ok = ok && strcmp(json_get_str(&v), "hello") == 0;

        json_set_real(&v, 3.14);
        ok = ok && json_get_real(&v) > 3.13 && json_get_real(&v) < 3.15;

        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json object write... ");
    {
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "method", "getinfo");
        json_push_kv_int(&obj, "id", 1);

        char buf[256];
        json_write(&obj, buf, sizeof(buf));
        bool ok = strstr(buf, "\"method\":\"getinfo\"") != NULL;
        ok = ok && strstr(buf, "\"id\":1") != NULL;

        json_free(&obj);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json array write... ");
    {
        struct json_value arr;
        json_init(&arr);
        json_set_array(&arr);
        struct json_value v;
        json_init(&v);
        json_set_int(&v, 10);
        json_push_back(&arr, &v);
        json_set_int(&v, 20);
        json_push_back(&arr, &v);
        json_free(&v);

        char buf[64];
        json_write(&arr, buf, sizeof(buf));
        bool ok = strcmp(buf, "[10,20]") == 0;

        json_free(&arr);
        if (ok) printf("OK\n"); else { printf("FAIL (got: %s)\n", buf); failures++; }
    }

    printf("json read object... ");
    {
        const char *input = "{\"name\":\"zcl\",\"port\":8233,\"active\":true}";
        struct json_value v;
        bool ok = json_read(&v, input, strlen(input));
        ok = ok && v.type == JSON_OBJ;
        ok = ok && json_size(&v) == 3;

        const struct json_value *name = json_get(&v, "name");
        ok = ok && name && strcmp(json_get_str(name), "zcl") == 0;

        const struct json_value *port = json_get(&v, "port");
        ok = ok && port && json_get_int(port) == 8233;

        const struct json_value *active = json_get(&v, "active");
        ok = ok && active && json_get_bool(active);

        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json read array... ");
    {
        const char *input = "[1,\"two\",null,false]";
        struct json_value v;
        bool ok = json_read(&v, input, strlen(input));
        ok = ok && v.type == JSON_ARR;
        ok = ok && json_size(&v) == 4;
        ok = ok && json_get_int(json_at(&v, 0)) == 1;
        ok = ok && strcmp(json_get_str(json_at(&v, 1)), "two") == 0;
        ok = ok && json_is_null(json_at(&v, 2));
        ok = ok && !json_get_bool(json_at(&v, 3));

        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json roundtrip... ");
    {
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "result", "ok");
        json_push_kv_int(&obj, "code", 200);

        char buf[256];
        size_t n = json_write(&obj, buf, sizeof(buf));

        struct json_value parsed;
        bool ok = json_read(&parsed, buf, n);
        ok = ok && parsed.type == JSON_OBJ;
        const struct json_value *r = json_get(&parsed, "result");
        ok = ok && r && strcmp(json_get_str(r), "ok") == 0;
        const struct json_value *c = json_get(&parsed, "code");
        ok = ok && c && json_get_int(c) == 200;

        json_free(&obj);
        json_free(&parsed);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json_rpc_request... ");
    {
        struct json_value params, id;
        json_init(&params);
        json_set_array(&params);
        json_init(&id);
        json_set_int(&id, 1);

        char buf[512];
        json_rpc_request("getinfo", &params, &id, buf, sizeof(buf));
        bool ok = strstr(buf, "\"method\":\"getinfo\"") != NULL;
        ok = ok && strstr(buf, "\"id\":1") != NULL;

        json_free(&params);
        json_free(&id);
        if (ok) printf("OK\n"); else { printf("FAIL (got: %s)\n", buf); failures++; }
    }

    printf("json_rpc_error... ");
    {
        struct json_value err;
        json_rpc_error(&err, RPC_METHOD_NOT_FOUND, "Method not found");
        const struct json_value *code = json_get(&err, "code");
        const struct json_value *msg = json_get(&err, "message");
        bool ok = code && json_get_int(code) == RPC_METHOD_NOT_FOUND;
        ok = ok && msg && strcmp(json_get_str(msg), "Method not found") == 0;
        json_free(&err);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("legacy_rpc parse scalar results... ");
    {
        const char *sraw =
            "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n"
            "{\"result\":\"abc123\",\"error\":null,\"id\":1}";
        const char *iraw =
            "HTTP/1.1 200 OK\r\nContent-Length: 34\r\n\r\n"
            "{\"result\":8232,\"error\":null,\"id\":1}";
        const char *eraw =
            "HTTP/1.1 200 OK\r\nContent-Length: 60\r\n\r\n"
            "{\"result\":null,\"error\":{\"message\":\"boom\"},\"id\":1}";
        char out[16] = {0};
        char errbuf[64] = {0};
        int64_t n = 0;
        bool ok = legacy_rpc_parse_result_string(sraw, out, sizeof(out),
                                                 errbuf, sizeof(errbuf));
        ok = ok && strcmp(out, "abc123") == 0;
        ok = ok && legacy_rpc_parse_result_int(iraw, &n, errbuf,
                                               sizeof(errbuf));
        ok = ok && n == 8232;
        ok = ok && !legacy_rpc_parse_result_string(eraw, out, sizeof(out),
                                                   errbuf, sizeof(errbuf));
        ok = ok && strstr(errbuf, "boom") != NULL;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("legacy_rpc parse string array results... ");
    {
        const char *raw =
            "HTTP/1.1 200 OK\r\nContent-Length: 94\r\n\r\n"
            "[{\"result\":\"aa\",\"error\":null,\"id\":0},"
            "{\"result\":\"bb\",\"error\":null,\"id\":1}]";
        const char *bad =
            "HTTP/1.1 200 OK\r\nContent-Length: 56\r\n\r\n"
            "[{\"result\":null,\"error\":{\"message\":\"bad item\"},\"id\":0}]";
        char slots[2][8] = {{0}};
        char errbuf[64] = {0};
        bool ok = legacy_rpc_parse_result_string_array(raw, 2, slots[0],
                                                       sizeof(slots[0]),
                                                       errbuf,
                                                       sizeof(errbuf));
        ok = ok && strcmp(slots[0], "aa") == 0;
        ok = ok && strcmp(slots[1], "bb") == 0;
        ok = ok && !legacy_rpc_parse_result_string_array(bad, 1, slots[0],
                                                         sizeof(slots[0]),
                                                         errbuf,
                                                         sizeof(errbuf));
        ok = ok && strstr(errbuf, "bad item") != NULL;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("rpc_table init/append/find... ");
    {
        struct rpc_table t;
        rpc_table_init(&t);
        struct rpc_command cmd = { "control", "test_cmd", NULL, true };
        bool ok = rpc_table_append(&t, &cmd);
        ok = ok && rpc_table_find(&t, "test_cmd") != NULL;
        ok = ok && rpc_table_find(&t, "nonexistent") == NULL;
        ok = ok && !rpc_table_append(&t, &cmd);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("rpc warmup state... ");
    {
        set_rpc_warmup_status("Loading blocks...");
        char status[256];
        bool ok = rpc_is_in_warmup(status, sizeof(status));
        ok = ok && strcmp(status, "Loading blocks...") == 0;
        set_rpc_warmup_finished();
        ok = ok && !rpc_is_in_warmup(NULL, 0);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("value_from_amount... ");
    {
        struct json_value v;
        value_from_amount(123456789LL, &v);
        bool ok = v.type == JSON_STR;
        ok = ok && strcmp(json_get_str(&v), "1.23456789") == 0;
        json_free(&v);

        value_from_amount(-50000000LL, &v);
        ok = ok && strcmp(json_get_str(&v), "-0.50000000") == 0;
        json_free(&v);

        value_from_amount(0, &v);
        ok = ok && strcmp(json_get_str(&v), "0.00000000") == 0;
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("dbwrapper open/write/read/close... ");
    {
        struct db_wrapper db;
        bool ok = db_wrapper_open(&db, "/tmp/zcl_test_db", 1024 * 1024,
                                  false, true);
        if (ok) {
            ok = ok && db_is_empty(&db);

            ok = ok && db_write(&db, "key1", 4, "value1", 6, false);
            ok = ok && !db_is_empty(&db);
            ok = ok && db_exists(&db, "key1", 4);
            ok = ok && !db_exists(&db, "key2", 4);

            char *val = NULL;
            size_t vallen = 0;
            ok = ok && db_read(&db, "key1", 4, &val, &vallen);
            ok = ok && vallen == 6 && memcmp(val, "value1", 6) == 0;
            free(val);

            ok = ok && db_erase(&db, "key1", 4, false);
            ok = ok && !db_exists(&db, "key1", 4);

            db_wrapper_close(&db);
        }
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("dbwrapper batch... ");
    {
        struct db_wrapper db;
        bool ok = db_wrapper_open(&db, "/tmp/zcl_test_db2", 1024 * 1024,
                                  false, true);
        if (ok) {
            struct db_batch batch;
            db_batch_init(&batch);
            db_batch_put(&batch, "a", 1, "1", 1);
            db_batch_put(&batch, "b", 1, "2", 1);
            db_batch_put(&batch, "c", 1, "3", 1);
            ok = ok && db_write_batch(&db, &batch, false);
            db_batch_free(&batch);

            ok = ok && db_exists(&db, "a", 1);
            ok = ok && db_exists(&db, "b", 1);
            ok = ok && db_exists(&db, "c", 1);

            db_wrapper_close(&db);
        }
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("dbwrapper iterator... ");
    {
        struct db_wrapper db;
        bool ok = db_wrapper_open(&db, "/tmp/zcl_test_db3", 1024 * 1024,
                                  false, true);
        if (ok) {
            db_write(&db, "x", 1, "10", 2, false);
            db_write(&db, "y", 1, "20", 2, false);
            db_write(&db, "z", 1, "30", 2, false);

            struct db_iterator it;
            db_iter_init(&it, &db);
            db_iter_seek_to_first(&it);
            int count = 0;
            while (db_iter_valid(&it)) {
                count++;
                db_iter_next(&it);
            }
            ok = ok && count == 3;
            db_iter_free(&it);
            db_wrapper_close(&db);
        }
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("rpc_convert_values... ");
    {
        const char *params[] = { "1000", "abc123" };
        struct json_value result;
        bool ok = rpc_convert_values("getblockhash", params, 2, &result);
        ok = ok && result.type == JSON_ARR && json_size(&result) == 2;
        ok = ok && json_get_int(json_at(&result, 0)) == 1000;
        ok = ok && strcmp(json_get_str(json_at(&result, 1)), "abc123") == 0;
        json_free(&result);

        ok = ok && rpc_should_convert_param("estimatefee", 0);
        ok = ok && !rpc_should_convert_param("estimatefee", 1);
        ok = ok && rpc_should_convert_param("sendtoaddress", 1);
        ok = ok && !rpc_should_convert_param("sendtoaddress", 0);

        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("ecc_init_sanity_check... ");
    {
        if (ecc_init_sanity_check())
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("parse_script... ");
    {
        struct script s;
        bool ok = parse_script("OP_DUP OP_HASH160 OP_EQUAL", &s);
        if (ok && s.size == 3 &&
            s.data[0] == OP_DUP &&
            s.data[1] == OP_HASH160 &&
            s.data[2] == OP_EQUAL)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("parse_script number... ");
    {
        struct script s;
        bool ok = parse_script("1 2 OP_ADD", &s);
        if (ok && s.size >= 3)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("parse_script shorthand... ");
    {
        struct script s;
        bool ok = parse_script("DUP HASH160 EQUAL", &s);
        if (ok && s.size == 3 &&
            s.data[0] == OP_DUP &&
            s.data[1] == OP_HASH160 &&
            s.data[2] == OP_EQUAL)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("script_to_asm_str... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_DUP);
        script_push_op(&s, OP_HASH160);
        unsigned char hash[20] = {0};
        script_push_data(&s, hash, 20);
        script_push_op(&s, OP_EQUALVERIFY);
        script_push_op(&s, OP_CHECKSIG);
        char asm_str[256];
        script_to_asm_str(&s, false, asm_str, sizeof(asm_str));
        if (strstr(asm_str, "OP_DUP") && strstr(asm_str, "OP_HASH160") &&
            strstr(asm_str, "OP_CHECKSIG"))
            printf("OK (%s)\n", asm_str);
        else {
            printf("FAIL (%s)\n", asm_str);
            failures++;
        }
    }

    printf("decode_hex_tx roundtrip... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        tx.lock_time = 0;
        tx.vin[0].sequence = 0xffffffff;
        outpoint_set_null(&tx.vin[0].prevout);
        tx.vin[0].script_sig.size = 0;
        tx.vout[0].value = 5000000000LL;
        tx.vout[0].script_pub_key.size = 0;
        transaction_compute_hash(&tx);

        char hex[2048];
        encode_hex_tx(&tx, hex, sizeof(hex));

        struct transaction tx2;
        transaction_init(&tx2);
        bool ok = decode_hex_tx(&tx2, hex);
        if (ok && tx2.version == 1 && tx2.num_vin == 1 && tx2.num_vout == 1 &&
            tx2.vout[0].value == 5000000000LL)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        transaction_free(&tx);
        transaction_free(&tx2);
    }

    printf("parse_hash_str... ");
    {
        struct uint256 h;
        bool ok = parse_hash_str(
            "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
            &h);
        char hex[65];
        uint256_get_hex(&h, hex);
        if (ok && strcmp(hex, "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f") == 0)
            printf("OK\n");
        else {
            printf("FAIL (%s)\n", hex);
            failures++;
        }
    }

    printf("tx_to_json... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        tx.lock_time = 0;
        tx.vin[0].sequence = 0xffffffff;
        outpoint_set_null(&tx.vin[0].prevout);
        tx.vin[0].script_sig.size = 0;
        tx.vout[0].value = 5000000000LL;
        tx.vout[0].script_pub_key.size = 0;
        transaction_compute_hash(&tx);

        struct json_value entry;
        struct uint256 null_hash;
        uint256_set_null(&null_hash);
        tx_to_json(&tx, &null_hash, &entry);

        if (entry.type == JSON_OBJ && entry.num_children > 0) {
            const struct json_value *v = json_get(&entry, "version");
            if (v && v->type == JSON_INT && v->val.i == 1)
                printf("OK\n");
            else {
                printf("FAIL (version)\n");
                failures++;
            }
        } else {
            printf("FAIL (not obj)\n");
            failures++;
        }
        json_free(&entry);
        transaction_free(&tx);
    }

    printf("async_op init/state... ");
    {
        struct async_rpc_operation op;
        async_op_init(&op);
        if (async_op_is_ready(&op) &&
            strncmp(op.id, "opid-", 5) == 0 &&
            strcmp(async_op_state_str(ASYNC_OP_READY), "queued") == 0)
            printf("OK (%s)\n", op.id);
        else {
            printf("FAIL\n");
            failures++;
        }
        async_op_free(&op);
    }

    printf("async_op execute/result... ");
    {
        struct async_rpc_operation op;
        async_op_init(&op);
        async_op_default_main(&op);
        if (async_op_is_success(&op)) {
            struct json_value res;
            async_op_get_result_json(&op, &res);
            if (res.type == JSON_STR)
                printf("OK\n");
            else {
                printf("FAIL (result type=%d)\n", res.type);
                failures++;
            }
            json_free(&res);
        } else {
            printf("FAIL (state=%s)\n", async_op_state_str(async_op_get_state(&op)));
            failures++;
        }
        async_op_free(&op);
    }

    printf("async_op error... ");
    {
        struct async_rpc_operation op;
        async_op_init(&op);
        async_op_set_error(&op, 42, "test error");
        async_op_set_state(&op, ASYNC_OP_FAILED);
        struct json_value err;
        async_op_get_error_json(&op, &err);
        if (err.type == JSON_OBJ) {
            const struct json_value *code = json_get(&err, "code");
            if (code && code->type == JSON_INT && code->val.i == 42)
                printf("OK\n");
            else {
                printf("FAIL (code)\n");
                failures++;
            }
        } else {
            printf("FAIL (not obj)\n");
            failures++;
        }
        json_free(&err);
        async_op_free(&op);
    }

    printf("async_op status_json... ");
    {
        struct async_rpc_operation op;
        async_op_init(&op);
        struct json_value status;
        async_op_get_status_json(&op, &status);
        const struct json_value *id_val = json_get(&status, "id");
        const struct json_value *st_val = json_get(&status, "status");
        if (id_val && id_val->type == JSON_STR &&
            st_val && st_val->type == JSON_STR &&
            strcmp(st_val->val.s, "queued") == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        json_free(&status);
        async_op_free(&op);
    }

    printf("async_queue add/execute... ");
    {
        struct async_rpc_queue q;
        async_queue_init(&q);

        struct async_rpc_operation op;
        async_op_init(&op);
        char saved_id[ASYNC_OP_ID_SIZE];
        memcpy(saved_id, op.id, ASYNC_OP_ID_SIZE);

        async_queue_add_op(&q, &op);
        bool ok = async_queue_add_worker(&q);

        async_queue_finish_and_wait(&q);

        if (ok && async_op_is_success(&op))
            printf("OK\n");
        else {
            printf("FAIL (state=%s)\n",
                async_op_state_str(async_op_get_state(&op)));
            failures++;
        }
        async_op_free(&op);
        async_queue_free(&q);
    }

    printf("async_queue refuses workers after finish... ");
    {
        struct async_rpc_queue q;
        async_queue_init(&q);
        async_queue_finish(&q);
        if (!async_queue_add_worker(&q))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        async_queue_free(&q);
    }

    printf("async_queue tracks worker count across shutdown... ");
    {
        struct async_rpc_queue q;
        async_queue_init(&q);

        bool ok = async_queue_add_worker(&q);
        size_t started = async_queue_num_workers(&q);
        async_queue_finish_and_wait(&q);
        size_t after = async_queue_num_workers(&q);

        if (ok && started == 1 && after == 0)
            printf("OK\n");
        else {
            printf("FAIL (ok=%d started=%zu after=%zu)\n",
                   ok ? 1 : 0, started, after);
            failures++;
        }
        async_queue_free(&q);
    }

    /* ── Wave 11 #6: RPC TLS tests ─────────────────────────────────── */

    printf("rpc_http_tls_active when no TLS configured... ");
    {
        /* Without TLS env vars, tls_active should be false */
        unsetenv("ZCL_RPC_TLS_CERT");
        unsetenv("ZCL_RPC_TLS_KEY");
        bool active = rpc_http_tls_active();
        if (!active)
            printf("OK\n");
        else {
            printf("FAIL (expected false)\n");
            failures++;
        }
    }

    printf("rpc TLS start with self-signed cert... ");
    {
        /* Generate a self-signed cert+key in temp files */
        char cert_path[] = "/tmp/zcl_test_cert_XXXXXX";
        char key_path[] = "/tmp/zcl_test_key_XXXXXX";
        int cfd = mkstemp(cert_path);
        int kfd = mkstemp(key_path);
        bool ok = false;

        if (cfd >= 0 && kfd >= 0) {
            /* Generate RSA key + self-signed cert via OpenSSL */
            EVP_PKEY *pkey = EVP_PKEY_new();
            EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
            if (kctx && EVP_PKEY_keygen_init(kctx) > 0) {
                EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
                EVP_PKEY_keygen(kctx, &pkey);
            }
            if (kctx) EVP_PKEY_CTX_free(kctx);

            X509 *x509 = X509_new();
            if (x509 && pkey) {
                X509_set_version(x509, 2);
                ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
                X509_gmtime_adj(X509_getm_notBefore(x509), 0);
                X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
                X509_set_pubkey(x509, pkey);
                X509_NAME *name = X509_get_subject_name(x509);
                X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                    (const unsigned char *)"localhost", -1, -1, 0);
                X509_set_issuer_name(x509, name);
                X509_sign(x509, pkey, EVP_sha256());

                /* Write cert */
                FILE *cf = fdopen(cfd, "w");
                if (cf) {
                    PEM_write_X509(cf, x509);
                    fclose(cf);
                    cfd = -1;  /* fdopen took ownership */
                }
                /* Write key */
                FILE *kf = fdopen(kfd, "w");
                if (kf) {
                    PEM_write_PrivateKey(kf, pkey, NULL, NULL, 0, NULL, NULL);
                    fclose(kf);
                    kfd = -1;
                }

                /* Set env vars and start RPC with TLS */
                setenv("ZCL_RPC_TLS_CERT", cert_path, 1);
                setenv("ZCL_RPC_TLS_KEY", key_path, 1);
                setenv("ZCL_RPC_TLS_PORT", "19444", 1);

                /* Create a minimal RPC table */
                struct rpc_table tbl;
                rpc_table_init(&tbl);

                bool started = rpc_http_start(&tbl, 19443, NULL, NULL,
                                               "/tmp");
                if (started) {
                    ok = rpc_http_tls_active();

                    /* Try connecting with TLS */
                    if (ok) {
                        SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
                        if (cctx) {
                            int sock = socket(AF_INET, SOCK_STREAM, 0);
                            struct sockaddr_in sa;
                            memset(&sa, 0, sizeof(sa));
                            sa.sin_family = AF_INET;
                            sa.sin_port = htons(19444);
                            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                            if (connect(sock, (struct sockaddr *)&sa,
                                        sizeof(sa)) == 0) {
                                SSL *ssl = SSL_new(cctx);
                                SSL_set_fd(ssl, sock);
                                if (SSL_connect(ssl) == 1) {
                                    /* Send a minimal JSON-RPC request */
                                    const char *req =
                                        "POST / HTTP/1.1\r\n"
                                        "Content-Length: 44\r\n"
                                        "\r\n"
                                        "{\"method\":\"getblockcount\","
                                        "\"params\":[],\"id\":1}";
                                    SSL_write(ssl, req, (int)strlen(req));

                                    char rbuf[4096];
                                    int n = SSL_read(ssl, rbuf,
                                                     (int)sizeof(rbuf) - 1);
                                    if (n > 0) {
                                        rbuf[n] = '\0';
                                        /* Should get HTTP 200 back */
                                        ok = ok && (strstr(rbuf,
                                                    "HTTP/1.1 200") != NULL ||
                                                    strstr(rbuf,
                                                    "HTTP/1.1 401") != NULL);
                                    } else {
                                        ok = false;
                                    }
                                } else {
                                    ok = false;
                                }
                                SSL_shutdown(ssl);
                                SSL_free(ssl);
                            }
                            close(sock);
                            SSL_CTX_free(cctx);
                        }
                    }

                    rpc_http_stop();
                    (void)tbl;
                } else {
                    ok = false;
                    (void)tbl;
                }

                X509_free(x509);
            }
            if (pkey) EVP_PKEY_free(pkey);
        }
        if (cfd >= 0) close(cfd);
        if (kfd >= 0) close(kfd);
        unlink(cert_path);
        unlink(key_path);
        unsetenv("ZCL_RPC_TLS_CERT");
        unsetenv("ZCL_RPC_TLS_KEY");
        unsetenv("ZCL_RPC_TLS_PORT");
        /* Clean up cookie file */
        unlink("/tmp/.cookie");

        if (ok)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("rpc TLS not started without env vars... ");
    {
        unsetenv("ZCL_RPC_TLS_CERT");
        unsetenv("ZCL_RPC_TLS_KEY");
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        bool started = rpc_http_start(&tbl, 19445, NULL, NULL, "/tmp");
        bool tls = rpc_http_tls_active();
        if (started) rpc_http_stop();
        (void)tbl;
        unlink("/tmp/.cookie");
        if (started && !tls)
            printf("OK\n");
        else {
            printf("FAIL (started=%d tls=%d)\n", started, tls);
            failures++;
        }
    }

    return failures;
}
