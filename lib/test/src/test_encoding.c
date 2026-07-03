/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Encoding and core utility tests: uint256, base58, bech32, arith_uint256,
 * random, time, consensus upgrades, money, string encoding, version,
 * chainparams, noui, deprecation, timedata, ConvertBits. */

#include "test/test_helpers.h"

int test_encoding(void)
{
    int failures = 0;

    printf("uint256 hex... ");
    struct uint256 u;
    uint256_set_hex(&u, "00000000000000000000000000000000000000000000000000000000deadbeef");
    char hexbuf[65];
    uint256_get_hex(&u, hexbuf);
    if (strcmp(hexbuf, "00000000000000000000000000000000000000000000000000000000deadbeef") == 0) {
        printf("OK\n");
    } else {
        printf("FAIL: %s\n", hexbuf);
        failures++;
    }

    printf("base58 encode... ");
    {
        const unsigned char data[] = { 0x00, 0x01, 0x02, 0x03 };
        char b58[64];
        size_t b58_len;
        domain_encoding_base58_encode(data, 4, b58, sizeof(b58), &b58_len);
        if (strcmp(b58, "1Ldp") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", b58);
            failures++;
        }
    }

    printf("base58 decode... ");
    {
        unsigned char out[64];
        size_t out_len;
        if (domain_encoding_base58_decode("1Ldp", out, sizeof(out), &out_len) &&
            out_len == 4 && out[0] == 0x00 && out[1] == 0x01 && out[2] == 0x02 && out[3] == 0x03)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("base58check roundtrip... ");
    {
        const unsigned char payload[] = { 0x00, 0x14, 0x01, 0x02, 0x03 };
        char encoded[128];
        size_t enc_len;
        domain_encoding_base58check_encode(payload, 5, encoded, sizeof(encoded), &enc_len);
        unsigned char decoded[128];
        size_t dec_len;
        if (domain_encoding_base58check_decode(encoded, decoded, sizeof(decoded), &dec_len) &&
            dec_len == 5 && memcmp(decoded, payload, 5) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("bech32 encode... ");
    {
        uint8_t values[] = { 0, 14, 20, 15, 7, 13, 26, 0, 25, 18, 6, 11, 13, 8, 21, 4, 20, 3, 17, 2, 29, 3, 12, 29, 3, 4, 15, 24, 20, 6, 14, 30, 22 };
        char out[128];
        if (domain_encoding_bech32_encode(out, sizeof(out), "bc", values, 33) && strlen(out) > 0)
            printf("OK (%s)\n", out);
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("bech32 decode... ");
    {
        char hrp[16];
        uint8_t data[128];
        size_t data_len;
        if (domain_encoding_bech32_decode(hrp, sizeof(hrp), data, sizeof(data), &data_len, "a12uel5l") &&
            strcmp(hrp, "a") == 0 && data_len == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("bech32 roundtrip... ");
    {
        uint8_t values[] = { 1, 2, 3, 4, 5 };
        char encoded[128];
        domain_encoding_bech32_encode(encoded, sizeof(encoded), "test", values, 5);
        char hrp[16];
        uint8_t decoded[128];
        size_t dec_len;
        if (domain_encoding_bech32_decode(hrp, sizeof(hrp), decoded, sizeof(decoded), &dec_len, encoded) &&
            strcmp(hrp, "test") == 0 && dec_len == 5 &&
            memcmp(decoded, values, 5) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("arith_uint256 compact roundtrip... ");
    {
        struct arith_uint256 target;
        bool neg, ovf;
        arith_uint256_set_compact(&target, 0x1d00ffff, &neg, &ovf);
        uint32_t compact = arith_uint256_get_compact(&target, false);
        if (compact == 0x1d00ffff && !neg && !ovf)
            printf("OK\n");
        else {
            printf("FAIL: compact=0x%08x neg=%d ovf=%d\n", compact, neg, ovf);
            failures++;
        }
    }

    printf("arith_uint256 arithmetic... ");
    {
        struct arith_uint256 a, b, r;
        arith_uint256_set_u64(&a, 0xFFFFFFFF);
        arith_uint256_set_u64(&b, 2);
        arith_uint256_mul_u32(&r, &a, 2);
        if (arith_uint256_get_low64(&r) == 0x1FFFFFFFE)
            printf("OK\n");
        else {
            printf("FAIL: got 0x%llx\n", (unsigned long long)arith_uint256_get_low64(&r));
            failures++;
        }
    }

    printf("arith_uint256 shift... ");
    {
        struct arith_uint256 a, r;
        arith_uint256_set_u64(&a, 1);
        arith_uint256_shl(&r, &a, 64);
        if (r.pn[2] == 1 && r.pn[0] == 0 && r.pn[1] == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("arith_uint256 division... ");
    {
        struct arith_uint256 a, b, r;
        arith_uint256_set_u64(&a, 100);
        arith_uint256_set_u64(&b, 7);
        arith_uint256_div(&r, &a, &b);
        if (arith_uint256_get_low64(&r) == 14)
            printf("OK\n");
        else {
            printf("FAIL: got %llu\n", (unsigned long long)arith_uint256_get_low64(&r));
            failures++;
        }
    }

    printf("arith_uint256 <-> uint256 conversion... ");
    {
        struct uint256 u2;
        uint256_set_hex(&u2, "00000000000000000000000000000000000000000000000000000000deadbeef");
        struct arith_uint256 a;
        uint256_to_arith(&a, &u2);
        struct uint256 u3;
        arith_to_uint256(&u3, &a);
        char hex[65];
        uint256_get_hex(&u3, hex);
        if (strcmp(hex, "00000000000000000000000000000000000000000000000000000000deadbeef") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", hex);
            failures++;
        }
    }

    printf("random bytes... ");
    {
        unsigned char buf[32];
        memset(buf, 0, 32);
        GetRandBytes(buf, 32);
        int nonzero = 0;
        for (int i = 0; i < 32; i++)
            if (buf[i] != 0) nonzero++;
        if (nonzero > 0)
            printf("OK (%d non-zero bytes)\n", nonzero);
        else {
            printf("FAIL: all zeros\n");
            failures++;
        }
    }

    printf("GetRand... ");
    {
        uint64_t r = GetRand(100);
        if (r < 100)
            printf("OK (%llu)\n", (unsigned long long)r);
        else {
            printf("FAIL: %llu >= 100\n", (unsigned long long)r);
            failures++;
        }
    }

    printf("GetTime... ");
    {
        int64_t t = GetTime();
        if (t > 1700000000)
            printf("OK (%lld)\n", (long long)t);
        else {
            printf("FAIL: %lld\n", (long long)t);
            failures++;
        }
    }

    printf("DateTimeStrFormat... ");
    {
        char buf[64];
        DateTimeStrFormat(buf, sizeof(buf), "%Y-%m-%d", 0);
        if (strcmp(buf, "1970-01-01") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", buf);
            failures++;
        }
    }

    printf("consensus upgrade state... ");
    {
        struct consensus_params params;
        memset(&params, 0, sizeof(params));
        params.vUpgrades[BASE_SPROUT].nActivationHeight = NETWORK_UPGRADE_ALWAYS_ACTIVE;
        params.vUpgrades[UPGRADE_OVERWINTER].nActivationHeight = 100;
        params.vUpgrades[UPGRADE_SAPLING].nActivationHeight = 200;
        params.vUpgrades[UPGRADE_TESTDUMMY].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
        params.vUpgrades[UPGRADE_BUBBLES].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
        params.vUpgrades[UPGRADE_DIFFADJ].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
        params.vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;

        if (consensus_upgrade_state(50, &params, UPGRADE_OVERWINTER) == UPGRADE_PENDING &&
            consensus_upgrade_state(100, &params, UPGRADE_OVERWINTER) == UPGRADE_ACTIVE &&
            consensus_upgrade_state(50, &params, UPGRADE_TESTDUMMY) == UPGRADE_DISABLED &&
            consensus_current_epoch(150, &params) == UPGRADE_OVERWINTER &&
            consensus_current_epoch(250, &params) == UPGRADE_SAPLING)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("FormatMoney... ");
    {
        char buf[64];
        FormatMoney(100000000, buf, sizeof(buf));
        if (strcmp(buf, "1.0") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", buf);
            failures++;
        }
    }

    printf("ParseMoney... ");
    {
        CAmount val = 0;
        if (ParseMoney("1.5", &val) && val == 150000000)
            printf("OK\n");
        else {
            printf("FAIL: %lld\n", (long long)val);
            failures++;
        }
    }

    printf("IsHex... ");
    {
        if (IsHex("deadbeef") && !IsHex("deadbee") && !IsHex("xyz"))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("ParseHex... ");
    {
        unsigned char out[32];
        size_t n = ParseHex("deadbeef", out, sizeof(out));
        if (n == 4 && out[0] == 0xde && out[1] == 0xad && out[2] == 0xbe && out[3] == 0xef)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("HexStr... ");
    {
        unsigned char data[] = { 0xde, 0xad, 0xbe, 0xef };
        char hexout[64];
        HexStr(data, 4, false, hexout, sizeof(hexout));
        if (strcmp(hexout, "deadbeef") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", hexout);
            failures++;
        }
    }

    printf("EncodeBase64... ");
    {
        char b64[64];
        EncodeBase64((const unsigned char *)"Hello", 5, b64, sizeof(b64));
        if (strcmp(b64, "SGVsbG8=") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", b64);
            failures++;
        }
    }

    printf("DecodeBase64... ");
    {
        unsigned char out[64];
        bool invalid = false;
        size_t n = DecodeBase64("SGVsbG8=", out, sizeof(out), &invalid);
        if (!invalid && n == 5 && memcmp(out, "Hello", 5) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("EncodeBase32... ");
    {
        char b32[64];
        EncodeBase32((const unsigned char *)"Hello", 5, b32, sizeof(b32));
        if (strcmp(b32, "jbswy3dp") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", b32);
            failures++;
        }
    }

    printf("ParseInt32... ");
    {
        int32_t val = 0;
        if (ParseInt32("12345", &val) && val == 12345 &&
            !ParseInt32("", &val) && !ParseInt32(" 1", &val))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("ParseFixedPoint... ");
    {
        int64_t amount = 0;
        if (ParseFixedPoint("1.5", 8, &amount) && amount == 150000000LL &&
            ParseFixedPoint("-0.5", 8, &amount) && amount == -50000000LL)
            printf("OK\n");
        else {
            printf("FAIL: %lld\n", (long long)amount);
            failures++;
        }
    }

    printf("SanitizeString... ");
    {
        char out[64];
        SanitizeString("hello<world>&test", SAFE_CHARS_DEFAULT, out, sizeof(out));
        if (strcmp(out, "helloworldtest") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", out);
            failures++;
        }
    }

    printf("FormatVersion... ");
    {
        char ver[64];
        FormatVersion(CLIENT_VERSION, ver, sizeof(ver));
        if (strstr(ver, "0.1.0") != NULL)
            printf("OK (%s)\n", ver);
        else {
            printf("FAIL: %s\n", ver);
            failures++;
        }
    }

    printf("CLIENT_NAME... ");
    {
        if (strcmp(CLIENT_NAME, "ZClassic23") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", CLIENT_NAME);
            failures++;
        }
    }

    printf("ParseParameters... ");
    {
        const char *argv[] = { "test", "-foo=bar", "-debug", "-baz=42" };
        ParseParameters(4, argv);
        if (strcmp(GetArg("-foo", ""), "bar") == 0 &&
            GetBoolArg("-debug", false) == true &&
            GetArgInt("-baz", 0) == 42 &&
            strcmp(GetArg("-noexist", "default"), "default") == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("GetDataDir cache invalidates on ParseParameters... ");
    {
        char before[1024];
        char after[1024];
        const char *empty_argv[] = { "test" };
        const char *datadir_argv[] = {
            "test",
            "-datadir=/tmp/zclassic23-test-datadir-cache"
        };

        ParseParameters(1, empty_argv);
        GetDataDir(false, before, sizeof(before));
        ParseParameters(2, datadir_argv);
        GetDataDir(false, after, sizeof(after));

        if (strcmp(after, "/tmp/zclassic23-test-datadir-cache") == 0 &&
            strcmp(before, after) != 0)
            printf("OK\n");
        else {
            printf("FAIL: before=%s after=%s\n", before, after);
            failures++;
        }
    }

    printf("SetDataDir overrides cached default... ");
    {
        char before[1024];
        char after[1024];
        const char *empty_argv[] = { "test" };

        ParseParameters(1, empty_argv);
        GetDataDir(false, before, sizeof(before));
        SetDataDir("/tmp/zclassic23-test-selected-datadir");
        GetDataDir(false, after, sizeof(after));

        if (strcmp(after, "/tmp/zclassic23-test-selected-datadir") == 0 &&
            strcmp(before, after) != 0)
            printf("OK\n");
        else {
            printf("FAIL: before=%s after=%s\n", before, after);
            failures++;
        }

        ParseParameters(1, empty_argv);
        ClearDataDirCache();
    }

    printf("GetNumCores... ");
    {
        int n = GetNumCores();
        if (n >= 1)
            printf("OK (%d)\n", n);
        else {
            printf("FAIL: %d\n", n);
            failures++;
        }
    }

    printf("chainparamsbase... ");
    {
        SelectBaseParams(CHAIN_MAIN);
        const struct base_chain_params *p = BaseParams();
        if (p->nRPCPort == 8023 && AreBaseParamsConfigured()) {
            SelectBaseParams(CHAIN_TESTNET);
            p = BaseParams();
            if (p->nRPCPort == 18023 && strcmp(p->strDataDir, "testnet3") == 0)
                printf("OK\n");
            else {
                printf("FAIL: testnet\n");
                failures++;
            }
        } else {
            printf("FAIL: main\n");
            failures++;
        }
    }

    printf("noui_connect... ");
    {
        noui_connect();
        if (uiInterface.ThreadSafeMessageBox != NULL &&
            uiInterface.InitMessage != NULL)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("GetAdjustedTime... ");
    {
        int64_t t = GetAdjustedTime();
        if (t > 1700000000)
            printf("OK (%lld)\n", (long long)t);
        else {
            printf("FAIL: %lld\n", (long long)t);
            failures++;
        }
    }

    printf("ConvertBits 8->5... ");
    {
        unsigned char in[] = { 0xff, 0x00 };
        unsigned char out[8];
        size_t out_len = 0;
        if (ConvertBits(8, 5, true, in, 2, out, sizeof(out), &out_len) &&
            out_len == 4 && out[0] == 0x1f && out[1] == 0x1c && out[2] == 0x00 && out[3] == 0x00)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    return failures;
}
