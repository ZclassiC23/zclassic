/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for Simple Ledger Protocol (SLP) parser and builder. */

#include "test/test_helpers.h"
#include "zslp/slp.h"

int test_slp(void)
{
    int failures = 0;

    /* ── Parse valid GENESIS ─────────────────────────────── */

    printf("slp_parse valid GENESIS... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "ZCL", "ZClassic Token", "https://zclassic.org", NULL,
            8, 0, 1000000);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_GENESIS;
        ok = ok && msg.token_type == SLP_TOKEN_TYPE_1;
        ok = ok && strcmp(msg.ticker, "ZCL") == 0;
        ok = ok && strcmp(msg.name, "ZClassic Token") == 0;
        ok = ok && strcmp(msg.document_url, "https://zclassic.org") == 0;
        ok = ok && !msg.has_document_hash;
        ok = ok && msg.decimals == 8;
        ok = ok && msg.mint_baton_vout == 0;
        ok = ok && msg.initial_quantity == 1000000;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_parse GENESIS with document hash... ");
    {
        uint8_t hash[32];
        memset(hash, 0xAB, 32);
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "TEST", "Test Token", "https://example.com", hash,
            4, 2, 5000);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_GENESIS;
        ok = ok && msg.has_document_hash;
        ok = ok && memcmp(msg.document_hash, hash, 32) == 0;
        ok = ok && msg.decimals == 4;
        ok = ok && msg.mint_baton_vout == 2;
        ok = ok && msg.initial_quantity == 5000;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Parse valid MINT ────────────────────────────────── */

    printf("slp_parse valid MINT... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0xCC, 32);
        uint8_t buf[256];
        size_t len = slp_build_mint(buf, sizeof(buf), &token_id, 2, 999999);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_MINT;
        ok = ok && msg.token_type == SLP_TOKEN_TYPE_1;
        ok = ok && memcmp(msg.token_id.data, token_id.data, 32) == 0;
        ok = ok && msg.mint_baton_vout == 2;
        ok = ok && msg.additional_quantity == 999999;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_parse MINT no baton... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0xDD, 32);
        uint8_t buf[256];
        size_t len = slp_build_mint(buf, sizeof(buf), &token_id, 0, 42);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_MINT;
        ok = ok && msg.mint_baton_vout == 0;
        ok = ok && msg.additional_quantity == 42;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Parse valid SEND ────────────────────────────────── */

    printf("slp_parse valid SEND 1 output... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0xEE, 32);
        uint64_t qty[] = { 100 };
        uint8_t buf[256];
        size_t len = slp_build_send(buf, sizeof(buf), &token_id, qty, 1);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_SEND;
        ok = ok && msg.num_outputs == 1;
        ok = ok && msg.output_quantities[0] == 100;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_parse valid SEND multiple outputs... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0xFF, 32);
        uint64_t qty[] = { 10, 20, 30, 40, 50 };
        uint8_t buf[512];
        size_t len = slp_build_send(buf, sizeof(buf), &token_id, qty, 5);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_SEND;
        ok = ok && msg.num_outputs == 5;
        for (int i = 0; i < 5 && ok; i++)
            ok = ok && msg.output_quantities[i] == qty[i];
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── NULL / empty / too-short scripts ────────────────── */

    printf("slp_parse NULL script... ");
    {
        struct slp_message msg;
        bool ok = !slp_parse(NULL, 0, &msg);
        ok = ok && msg.type == SLP_TX_INVALID;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_parse NULL output message... ");
    {
        uint8_t buf[] = { 0x6a };
        bool ok = !slp_parse(buf, sizeof(buf), NULL);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_parse empty script... ");
    {
        uint8_t buf[1] = { 0 };
        struct slp_message msg;
        bool ok = !slp_parse(buf, 0, &msg);
        ok = ok && msg.type == SLP_TX_INVALID;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_parse too-short script (just OP_RETURN)... ");
    {
        uint8_t buf[] = { 0x6a };
        struct slp_message msg;
        bool ok = !slp_parse(buf, 1, &msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_parse script not starting with OP_RETURN... ");
    {
        uint8_t buf[] = { 0x00, 0x04, 'S', 'L', 'P', 0x00 };
        struct slp_message msg;
        bool ok = !slp_parse(buf, sizeof(buf), &msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Invalid lokad_id ────────────────────────────────── */

    printf("slp_parse invalid lokad_id... ");
    {
        /* Build a valid script then corrupt the lokad_id */
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "X", "X", "", NULL, 0, 0, 1);
        /* lokad_id is at offset 2 (after OP_RETURN + push opcode) */
        buf[2] = 'X'; /* corrupt first byte of "SLP\0" */
        struct slp_message msg;
        bool ok = !slp_parse(buf, len, &msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Wrong token_type ────────────────────────────────── */

    printf("slp_parse wrong token_type... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "X", "X", "", NULL, 0, 0, 1);
        /* token_type is pushed after lokad_id.
         * lokad_id: OP_RETURN(1) + push(1) + 4 bytes = offset 6
         * token_type: push(1) + 1 byte at offset 7 */
        buf[7] = 2; /* change token_type from 1 to 2 */
        struct slp_message msg;
        bool ok = !slp_parse(buf, len, &msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Build + parse round-trips ───────────────────────── */

    printf("slp_build_genesis round-trip... ");
    {
        uint8_t hash[32];
        for (int i = 0; i < 32; i++) hash[i] = (uint8_t)i;
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "MYTOKEN", "My Token Name", "https://mytoken.org", hash,
            6, 3, UINT64_C(1000000000000));
        bool ok = len > 0;
        struct slp_message msg;
        ok = ok && slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_GENESIS;
        ok = ok && strcmp(msg.ticker, "MYTOKEN") == 0;
        ok = ok && strcmp(msg.name, "My Token Name") == 0;
        ok = ok && strcmp(msg.document_url, "https://mytoken.org") == 0;
        ok = ok && msg.has_document_hash;
        ok = ok && memcmp(msg.document_hash, hash, 32) == 0;
        ok = ok && msg.decimals == 6;
        ok = ok && msg.mint_baton_vout == 3;
        ok = ok && msg.initial_quantity == UINT64_C(1000000000000);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_mint round-trip... ");
    {
        struct uint256 token_id;
        for (int i = 0; i < 32; i++) token_id.data[i] = (uint8_t)(0x10 + i);
        uint8_t buf[256];
        size_t len = slp_build_mint(buf, sizeof(buf), &token_id, 4, UINT64_C(9999999999));
        bool ok = len > 0;
        struct slp_message msg;
        ok = ok && slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_MINT;
        ok = ok && memcmp(msg.token_id.data, token_id.data, 32) == 0;
        ok = ok && msg.mint_baton_vout == 4;
        ok = ok && msg.additional_quantity == UINT64_C(9999999999);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_send round-trip... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0x55, 32);
        uint64_t qty[] = { 100, 200, 300 };
        uint8_t buf[512];
        size_t len = slp_build_send(buf, sizeof(buf), &token_id, qty, 3);
        bool ok = len > 0;
        struct slp_message msg;
        ok = ok && slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_SEND;
        ok = ok && msg.num_outputs == 3;
        ok = ok && msg.output_quantities[0] == 100;
        ok = ok && msg.output_quantities[1] == 200;
        ok = ok && msg.output_quantities[2] == 300;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Edge cases: ticker lengths ──────────────────────── */

    printf("slp_build_genesis empty ticker... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "", "No Ticker", "", NULL, 0, 0, 1);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_GENESIS;
        ok = ok && msg.ticker[0] == '\0'; /* empty ticker */
        ok = ok && msg.initial_quantity == 1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_genesis NULL ticker... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            NULL, "Null Ticker", "", NULL, 0, 0, 1);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_GENESIS;
        ok = ok && msg.ticker[0] == '\0';
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_genesis max-length ticker (63 chars)... ");
    {
        char long_ticker[64];
        memset(long_ticker, 'Z', 63);
        long_ticker[63] = '\0';
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            long_ticker, "Long", "", NULL, 0, 0, 1);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_GENESIS;
        ok = ok && strcmp(msg.ticker, long_ticker) == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Edge cases: decimals ────────────────────────────── */

    printf("slp_build_genesis 0 decimals... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "NDC", "No Decimals Coin", "", NULL, 0, 0, 100);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.decimals == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_genesis 8 decimals... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "BTC", "Bitcoin Clone", "", NULL, 8, 0, 2100000000000000ULL);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.decimals == 8;
        ok = ok && msg.initial_quantity == 2100000000000000ULL;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Edge cases: SEND outputs ────────────────────────── */

    printf("slp_build_send 0 outputs (should fail)... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0x11, 32);
        uint64_t qty[] = { 0 };
        uint8_t buf[256];
        size_t len = slp_build_send(buf, sizeof(buf), &token_id, qty, 0);
        bool ok = (len == 0); /* builder should reject 0 outputs */
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_send 19 outputs (max)... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0x22, 32);
        uint64_t qty[19];
        for (int i = 0; i < 19; i++) qty[i] = (uint64_t)(i + 1) * 100;
        uint8_t buf[1024];
        size_t len = slp_build_send(buf, sizeof(buf), &token_id, qty, 19);
        bool ok = len > 0;
        struct slp_message msg;
        ok = ok && slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_SEND;
        ok = ok && msg.num_outputs == 19;
        for (int i = 0; i < 19 && ok; i++)
            ok = ok && msg.output_quantities[i] == qty[i];
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_send 20 outputs (over max, should fail)... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0x33, 32);
        uint64_t qty[20];
        for (int i = 0; i < 20; i++) qty[i] = 1;
        uint8_t buf[1024];
        size_t len = slp_build_send(buf, sizeof(buf), &token_id, qty, 20);
        bool ok = (len == 0); /* builder should reject >19 outputs */
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Large quantity (UINT64_MAX) ─────────────────────── */

    printf("slp_build_genesis max quantity (UINT64_MAX)... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "MAX", "Max Supply", "", NULL, 0, 0, UINT64_MAX);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.initial_quantity == UINT64_MAX;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("slp_build_mint max quantity (UINT64_MAX)... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0xAA, 32);
        uint8_t buf[256];
        size_t len = slp_build_mint(buf, sizeof(buf), &token_id, 0, UINT64_MAX);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.additional_quantity == UINT64_MAX;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Zero quantity ───────────────────────────────────── */

    printf("slp_build_genesis zero quantity... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "ZERO", "Zero Token", "", NULL, 0, 0, 0);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.initial_quantity == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Truncated GENESIS (cut off at decimals field) ───── */

    printf("slp_parse truncated GENESIS... ");
    {
        uint8_t buf[512];
        size_t len = slp_build_genesis(buf, sizeof(buf),
            "X", "X", "", NULL, 0, 0, 1);
        /* Truncate to remove the last field (initial_quantity) */
        struct slp_message msg;
        bool ok = !slp_parse(buf, len - 10, &msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── SEND with zero-value quantities ─────────────────── */

    printf("slp_build_send with zero quantities... ");
    {
        struct uint256 token_id;
        memset(token_id.data, 0x44, 32);
        uint64_t qty[] = { 0, 0, 0 };
        uint8_t buf[512];
        size_t len = slp_build_send(buf, sizeof(buf), &token_id, qty, 3);
        struct slp_message msg;
        bool ok = slp_parse(buf, len, &msg);
        ok = ok && msg.type == SLP_TX_SEND;
        ok = ok && msg.num_outputs == 3;
        ok = ok && msg.output_quantities[0] == 0;
        ok = ok && msg.output_quantities[1] == 0;
        ok = ok && msg.output_quantities[2] == 0;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
