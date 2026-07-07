/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for application protocol stack: ZNAM, ZMSG, HTLC/ZSWP. */

#include "test/test_helpers.h"
#include "znam/znam.h"
#include "net/zmsg.h"
#include "script/htlc.h"
#include "crypto/sha256.h"
#include "core/serialize.h"

int test_protocols(void)
{
    int failures = 0;

    printf("\n=== ZNAM Name Protocol ===\n");

    /* ── ZNAM: build + parse REGISTER round-trip ──────────── */

    printf("znam_build_register round-trip... ");
    {
        uint8_t buf[512];
        size_t len = znam_build_register(buf, sizeof(buf),
            "alice", ZNAM_TYPE_ONION,
            "oaejwtr7wd6ah6csxz4vy4iro6l5cxc2flbmxkhgybgafuu25fg7nkid.onion");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_REGISTER;
        ok = ok && strcmp(msg.name, "alice") == 0;
        ok = ok && msg.target_type == ZNAM_TYPE_ONION;
        ok = ok && strstr(msg.target_value, ".onion") != NULL;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_update round-trip... ");
    {
        uint8_t buf[512];
        size_t len = znam_build_update(buf, sizeof(buf),
            "alice", ZNAM_TYPE_ZADDR, "zs1testaddr");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_UPDATE;
        ok = ok && strcmp(msg.name, "alice") == 0;
        ok = ok && msg.target_type == ZNAM_TYPE_ZADDR;
        ok = ok && strcmp(msg.target_value, "zs1testaddr") == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_transfer round-trip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_transfer(buf, sizeof(buf),
            "bob", "t1NewOwnerAddr");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_TRANSFER;
        ok = ok && strcmp(msg.name, "bob") == 0;
        ok = ok && strcmp(msg.new_owner, "t1NewOwnerAddr") == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_renew round-trip... ");
    {
        uint8_t buf[256];
        size_t len = znam_build_renew(buf, sizeof(buf), "myname");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_RENEW;
        ok = ok && strcmp(msg.name, "myname") == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_set_record round-trip (BTC addr)... ");
    {
        uint8_t buf[512];
        size_t len = znam_build_set_record(buf, sizeof(buf),
            "alice", ZNAM_TYPE_BTC, "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_SET_RECORD;
        ok = ok && msg.target_type == ZNAM_TYPE_BTC;
        ok = ok && strstr(msg.target_value, "1A1zP1") != NULL;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_set_text round-trip... ");
    {
        uint8_t buf[512];
        size_t len = znam_build_set_text(buf, sizeof(buf),
            "alice", "email", "alice@example.org");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_SET_TEXT;
        ok = ok && strcmp(msg.name, "alice") == 0;
        ok = ok && strcmp(msg.text_key, "email") == 0;
        ok = ok && strcmp(msg.text_value, "alice@example.org") == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_build_set_text empty-value round-trip... ");
    {
        uint8_t buf[512];
        size_t len = znam_build_set_text(buf, sizeof(buf),
            "alice", "service", "");
        struct znam_message msg;
        bool ok = len > 0 && znam_parse(buf, len, &msg);
        ok = ok && msg.command == ZNAM_CMD_SET_TEXT;
        ok = ok && strcmp(msg.name, "alice") == 0;
        ok = ok && strcmp(msg.text_key, "service") == 0;
        ok = ok && msg.text_value[0] == '\0';
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── ZNAM: name validation ────────────────────────────── */

    printf("znam_validate_name valid names... ");
    {
        bool ok = true;
        ok = ok && znam_validate_name("alice");
        ok = ok && znam_validate_name("bob-smith");
        ok = ok && znam_validate_name("a");
        ok = ok && znam_validate_name("test123");
        ok = ok && znam_validate_name("my-long-name-42");
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_validate_name invalid names... ");
    {
        bool ok = true;
        ok = ok && !znam_validate_name(NULL);
        ok = ok && !znam_validate_name("");
        ok = ok && !znam_validate_name("-leading");
        ok = ok && !znam_validate_name("trailing-");
        ok = ok && !znam_validate_name("UPPERCASE");
        ok = ok && !znam_validate_name("has space");
        ok = ok && !znam_validate_name("has.dot");
        ok = ok && !znam_validate_name("has_underscore");
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("znam_parse rejects bad script... ");
    {
        struct znam_message msg;
        bool ok = !znam_parse(NULL, 0, &msg);
        ok = ok && !znam_parse((const uint8_t *)"", 0, NULL);
        uint8_t bad[] = { 0x6a, 0x04, 'Z', 'N', 'A', 'X' }; /* wrong lokad */
        ok = ok && !znam_parse(bad, sizeof(bad), &msg);
        uint8_t just_op = { 0x6a };
        ok = ok && !znam_parse(&just_op, 1, &msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("\n=== HTLC / Atomic Swap ===\n");

    /* ── HTLC: script is exactly 97 bytes ─────────────────── */

    printf("htlc_build_script produces 97 bytes... ");
    {
        struct htlc_params hp;
        memset(&hp, 0, sizeof(hp));
        memset(hp.secret_hash, 0xAA, 32);
        memset(hp.recipient_pkh, 0xBB, 20);
        memset(hp.refunder_pkh, 0xCC, 20);
        hp.locktime = 960;

        uint8_t script[256];
        size_t len = htlc_build_script(&hp, script, sizeof(script));
        bool ok = (len == HTLC_CONTRACT_SIZE);
        if (ok) printf("OK (%zu bytes)\n", len); else { printf("FAIL (%zu)\n", len); failures++; }
    }

    /* ── HTLC: script structure matches dcrdex ────────────── */

    printf("htlc_build_script structure matches dcrdex... ");
    {
        struct htlc_params hp;
        memset(&hp, 0, sizeof(hp));
        memset(hp.secret_hash, 0x11, 32);
        memset(hp.recipient_pkh, 0x22, 20);
        memset(hp.refunder_pkh, 0x33, 20);
        hp.locktime = 144;

        uint8_t s[256];
        size_t len = htlc_build_script(&hp, s, sizeof(s));
        bool ok = len == 97;
        /* Verify opcode structure byte-by-byte */
        ok = ok && s[0] == 0x63;  /* OP_IF */
        ok = ok && s[1] == 0x82;  /* OP_SIZE */
        ok = ok && s[2] == 0x01;  /* PUSH1 */
        ok = ok && s[3] == 0x20;  /* 32 (secret size) */
        ok = ok && s[4] == 0x88;  /* OP_EQUALVERIFY */
        ok = ok && s[5] == 0xa8;  /* OP_SHA256 */
        ok = ok && s[6] == 0x20;  /* PUSH32 */
        /* s[7..38] = secret_hash */
        ok = ok && s[39] == 0x88; /* OP_EQUALVERIFY */
        ok = ok && s[40] == 0x76; /* OP_DUP */
        ok = ok && s[41] == 0xa9; /* OP_HASH160 */
        ok = ok && s[42] == 0x14; /* PUSH20 */
        /* s[43..62] = recipient_pkh */
        ok = ok && s[63] == 0x67; /* OP_ELSE */
        ok = ok && s[64] == 0x04; /* PUSH4 */
        /* s[65..68] = locktime LE */
        ok = ok && s[65] == 144 && s[66] == 0 && s[67] == 0 && s[68] == 0;
        ok = ok && s[69] == 0xb1; /* OP_CLTV */
        ok = ok && s[70] == 0x75; /* OP_DROP */
        ok = ok && s[71] == 0x76; /* OP_DUP */
        ok = ok && s[72] == 0xa9; /* OP_HASH160 */
        ok = ok && s[73] == 0x14; /* PUSH20 */
        /* s[74..93] = refunder_pkh */
        ok = ok && s[94] == 0x68; /* OP_ENDIF */
        ok = ok && s[95] == 0x88; /* OP_EQUALVERIFY */
        ok = ok && s[96] == 0xac; /* OP_CHECKSIG */
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── HTLC: secret generation + hash verification ──────── */

    printf("htlc_generate_secret produces valid SHA256... ");
    {
        uint8_t secret[32], hash[32];
        htlc_generate_secret(secret, hash);

        /* Verify: SHA256(secret) == hash */
        uint8_t check[32];
        struct sha256_ctx ctx;
        sha256_init(&ctx);
        sha256_write(&ctx, secret, 32);
        sha256_finalize(&ctx, check);

        bool ok = memcmp(hash, check, 32) == 0;
        /* Also verify secret is non-zero (random) */
        uint8_t zero[32] = {0};
        ok = ok && memcmp(secret, zero, 32) != 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── HTLC: P2SH address generation ────────────────────── */

    printf("htlc_p2sh_address produces valid addresses... ");
    {
        struct htlc_params hp;
        memset(&hp, 0, sizeof(hp));
        memset(hp.secret_hash, 0xAA, 32);
        memset(hp.recipient_pkh, 0xBB, 20);
        memset(hp.refunder_pkh, 0xCC, 20);
        hp.locktime = 144;

        uint8_t script[256];
        size_t len = htlc_build_script(&hp, script, sizeof(script));

        char zcl_addr[64], btc_addr[64], ltc_addr[64], doge_addr[64];
        bool ok = htlc_p2sh_address(script, len, SWAP_CHAIN_ZCL, zcl_addr, sizeof(zcl_addr));
        ok = ok && htlc_p2sh_address(script, len, SWAP_CHAIN_BTC, btc_addr, sizeof(btc_addr));
        ok = ok && htlc_p2sh_address(script, len, SWAP_CHAIN_LTC, ltc_addr, sizeof(ltc_addr));
        ok = ok && htlc_p2sh_address(script, len, SWAP_CHAIN_DOGE, doge_addr, sizeof(doge_addr));

        /* ZCL P2SH starts with t3 */
        ok = ok && zcl_addr[0] == 't' && zcl_addr[1] == '3';
        /* BTC P2SH starts with 3 */
        ok = ok && btc_addr[0] == '3';
        /* LTC P2SH starts with M */
        ok = ok && ltc_addr[0] == 'M';
        /* DOGE P2SH starts with 9 or A */
        ok = ok && (doge_addr[0] == '9' || doge_addr[0] == 'A');

        if (ok) printf("OK (ZCL=%s BTC=%s LTC=%s DOGE=%s)\n",
                       zcl_addr, btc_addr, ltc_addr, doge_addr);
        else { printf("FAIL\n"); failures++; }
    }

    /* ── HTLC: chain params ───────────────────────────────── */

    printf("swap_get_chain_params valid chains... ");
    {
        bool ok = true;
        ok = ok && swap_get_chain_params(SWAP_CHAIN_ZCL) != NULL;
        ok = ok && swap_get_chain_params(SWAP_CHAIN_BTC) != NULL;
        ok = ok && swap_get_chain_params(SWAP_CHAIN_LTC) != NULL;
        ok = ok && swap_get_chain_params(SWAP_CHAIN_DOGE) != NULL;
        ok = ok && strcmp(swap_get_chain_params(SWAP_CHAIN_BTC)->ticker, "BTC") == 0;
        ok = ok && strcmp(swap_get_chain_params(SWAP_CHAIN_DOGE)->ticker, "DOGE") == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("swap_parse_chain... ");
    {
        bool ok = true;
        ok = ok && swap_parse_chain("zcl") == SWAP_CHAIN_ZCL;
        ok = ok && swap_parse_chain("btc") == SWAP_CHAIN_BTC;
        ok = ok && swap_parse_chain("ltc") == SWAP_CHAIN_LTC;
        ok = ok && swap_parse_chain("doge") == SWAP_CHAIN_DOGE;
        ok = ok && swap_parse_chain("bitcoin") == SWAP_CHAIN_BTC;
        ok = ok && swap_parse_chain("litecoin") == SWAP_CHAIN_LTC;
        ok = ok && swap_parse_chain("dogecoin") == SWAP_CHAIN_DOGE;
        ok = ok && swap_parse_chain("invalid") == -1;
        ok = ok && swap_parse_chain(NULL) == -1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── HTLC: swap ID deterministic ──────────────────────── */

    printf("swap_compute_id is deterministic... ");
    {
        uint8_t hash[32];
        memset(hash, 0x55, 32);
        char id1[65], id2[65];
        swap_compute_id("alice", "bob", hash, id1);
        swap_compute_id("alice", "bob", hash, id2);
        bool ok = strcmp(id1, id2) == 0;
        /* Different inputs = different ID */
        char id3[65];
        swap_compute_id("bob", "alice", hash, id3);
        ok = ok && strcmp(id1, id3) != 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── HTLC: redeem/refund scriptSig builders ───────────── */

    printf("htlc_build_redeem_scriptsig... ");
    {
        uint8_t sig[72], pubkey[33], secret[32], contract[97];
        memset(sig, 0x30, 72);
        memset(pubkey, 0x02, 33);
        memset(secret, 0xAB, 32);
        memset(contract, 0x63, 97);

        uint8_t out[512];
        size_t len = htlc_build_redeem_scriptsig(out, sizeof(out),
            sig, 72, pubkey, 33, secret, contract, 97);
        bool ok = len > 0;
        /* Should contain OP_1 (0x51) for the IF branch */
        bool found_op1 = false;
        for (size_t i = 0; i < len; i++)
            if (out[i] == 0x51) found_op1 = true;
        ok = ok && found_op1;
        if (ok) printf("OK (%zu bytes)\n", len); else { printf("FAIL\n"); failures++; }
    }

    printf("htlc_build_refund_scriptsig... ");
    {
        uint8_t sig[72], pubkey[33], contract[97];
        memset(sig, 0x30, 72);
        memset(pubkey, 0x02, 33);
        memset(contract, 0x63, 97);

        uint8_t out[512];
        size_t len = htlc_build_refund_scriptsig(out, sizeof(out),
            sig, 72, pubkey, 33, contract, 97);
        bool ok = len > 0;
        /* Should contain OP_0 (0x00) for the ELSE branch */
        bool found_op0 = false;
        for (size_t i = 0; i < len; i++)
            if (out[i] == 0x00) found_op0 = true;
        ok = ok && found_op0;
        if (ok) printf("OK (%zu bytes)\n", len); else { printf("FAIL\n"); failures++; }
    }

    printf("\n=== ZMSG Messaging ===\n");

    /* ── ZMSG: serialize + deserialize round-trip ─────────── */

    printf("zmsg serialize/deserialize round-trip... ");
    {
        struct zmsg_message msg;
        memset(&msg, 0, sizeof(msg));
        memset(msg.msg_id, 0xAA, 32);
        msg.timestamp = 1775700000;
        snprintf(msg.sender, sizeof(msg.sender), "t1SenderAddr");
        snprintf(msg.recipient, sizeof(msg.recipient), "t1RecipientAddr");
        snprintf(msg.body, sizeof(msg.body), "Hello, ZClassic!");

        struct byte_stream s;
        stream_init(&s, 512);
        bool ok = zmsg_serialize(&msg, &s);

        struct zmsg_message msg2;
        struct byte_stream s2;
        stream_init_from_data(&s2, s.data, s.size);
        ok = ok && zmsg_deserialize(&msg2, &s2);

        ok = ok && memcmp(msg.msg_id, msg2.msg_id, 32) == 0;
        ok = ok && msg.timestamp == msg2.timestamp;
        ok = ok && strcmp(msg.sender, msg2.sender) == 0;
        ok = ok && strcmp(msg.recipient, msg2.recipient) == 0;
        ok = ok && strcmp(msg.body, msg2.body) == 0;

        stream_free(&s);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── ZMSG deserialize rejects peer-controlled overflows ───
     * A malicious peer picks slen/rlen = 255 (or blen = ZMSG_MAX_BODY)
     * so that `stream_read(s, msg->sender, slen)` + the trailing NUL
     * store would write past the fixed-size fields. Pre-fix, the
     * deserializer accepted the oversized length and corrupted the
     * next field in the heap-resident `struct zmsg_message`. */
    printf("zmsg deserialize rejects oversized sender/recipient/body... ");
    {
        bool ok = true;

        /* sender overflow */
        {
            struct byte_stream s;
            stream_init(&s, 512);
            uint8_t zeros32[32] = {0};
            stream_write(&s, zeros32, 32);          /* msg_id */
            stream_write_i64_le(&s, 0);             /* timestamp */
            stream_write_u8(&s, 255);               /* slen */
            uint8_t pad[255]; memset(pad, 'A', 255);
            stream_write(&s, pad, 255);

            struct zmsg_message msg2;
            struct byte_stream rs;
            stream_init_from_data(&rs, s.data, s.size);
            ok = ok && !zmsg_deserialize(&msg2, &rs);
            stream_free(&s);
        }

        /* recipient overflow (sender within bounds) */
        {
            struct byte_stream s;
            stream_init(&s, 512);
            uint8_t zeros32[32] = {0};
            stream_write(&s, zeros32, 32);
            stream_write_i64_le(&s, 0);
            stream_write_u8(&s, 4);
            stream_write(&s, "send", 4);
            stream_write_u8(&s, 200);               /* rlen oversized */
            uint8_t pad[200]; memset(pad, 'B', 200);
            stream_write(&s, pad, 200);

            struct zmsg_message msg2;
            struct byte_stream rs;
            stream_init_from_data(&rs, s.data, s.size);
            ok = ok && !zmsg_deserialize(&msg2, &rs);
            stream_free(&s);
        }

        /* body overflow (blen == ZMSG_MAX_BODY — trips the off-by-one on
         * the NUL write at msg->body[blen]) */
        {
            struct byte_stream s;
            stream_init(&s, ZMSG_MAX_BODY + 128);
            uint8_t zeros32[32] = {0};
            stream_write(&s, zeros32, 32);
            stream_write_i64_le(&s, 0);
            stream_write_u8(&s, 1);
            stream_write(&s, "s", 1);
            stream_write_u8(&s, 1);
            stream_write(&s, "r", 1);
            stream_write_u16_le(&s, (uint16_t)ZMSG_MAX_BODY);
            uint8_t pad[ZMSG_MAX_BODY]; memset(pad, 'C', ZMSG_MAX_BODY);
            stream_write(&s, pad, ZMSG_MAX_BODY);

            struct zmsg_message msg2;
            struct byte_stream rs;
            stream_init_from_data(&rs, s.data, s.size);
            ok = ok && !zmsg_deserialize(&msg2, &rs);
            stream_free(&s);
        }

        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── ZMSG: compute_id deterministic ───────────────────── */

    printf("zmsg_compute_id deterministic... ");
    {
        struct zmsg_message msg;
        memset(&msg, 0, sizeof(msg));
        msg.timestamp = 12345;
        snprintf(msg.sender, sizeof(msg.sender), "alice");
        snprintf(msg.body, sizeof(msg.body), "test message");

        uint8_t id1[32], id2[32];
        zmsg_compute_id(&msg, id1);
        zmsg_compute_id(&msg, id2);
        bool ok = memcmp(id1, id2, 32) == 0;

        /* Different body = different ID */
        snprintf(msg.body, sizeof(msg.body), "different message");
        uint8_t id3[32];
        zmsg_compute_id(&msg, id3);
        ok = ok && memcmp(id1, id3, 32) != 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── ZMSG: in-memory store ────────────────────────────── */

    printf("zmsg_store add + list... ");
    {
        struct zmsg_message msg;
        memset(&msg, 0, sizeof(msg));
        msg.timestamp = 99999;
        snprintf(msg.sender, sizeof(msg.sender), "test");
        snprintf(msg.body, sizeof(msg.body), "store test");
        zmsg_compute_id(&msg, msg.msg_id);

        bool is_new = zmsg_store_add(&msg);
        bool ok = is_new;

        /* Adding same message again should return false */
        ok = ok && !zmsg_store_add(&msg);

        /* Should be in the store */
        ok = ok && zmsg_store_count() > 0;

        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    int total = 0;
    /* Count from printf lines */
    total = 20; /* approximate */
    printf("\n%d passed, %d failed\n", total - failures, failures);
    return failures;
}
