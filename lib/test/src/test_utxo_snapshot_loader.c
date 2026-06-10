/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_utxo_snapshot_loader: round-trips the sidecar format.
 *
 *   1. Synthesize a tiny in-memory sidecar (3 records).
 *   2. uss_open() with verify_full_sha3=true → must succeed and
 *      report the same header.
 *   3. uss_iter() → must visit all 3 records with matching values.
 *   4. Corrupt one byte → re-open must fail with "body sha3 mismatch".
 */

#include "test/test_helpers.h"
#include "chain/utxo_snapshot_loader.h"
#include "crypto/sha3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cap_ctx {
    int n;
    int script_lens[3];
    int64_t values[3];
};

static bool cap_cb(const struct uss_record *r, void *vctx)
{
    struct cap_ctx *c = vctx;
    if (c->n >= 3) return false;
    c->script_lens[c->n] = (int)r->script_len;
    c->values[c->n] = r->value;
    c->n++;
    return true;
}

static void wle32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void wle64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

int test_utxo_snapshot_loader(void)
{
    int failures = 0;

    /* Build the body in a buffer. 3 records, ascending vout indices. */
    uint8_t body[4096] = {0};
    size_t off = 0;

    uint8_t txid[32];
    for (int i = 0; i < 32; i++) txid[i] = (uint8_t)i;
    uint8_t script_a[3] = { 0x76, 0xa9, 0x14 };
    uint8_t script_b[1] = { 0x6a };
    uint8_t script_c[5] = { 0x21, 0x02, 0x03, 0x04, 0x05 };

    struct rec_in { uint32_t vout; int64_t value; const uint8_t *s; uint32_t sl;
                    uint32_t height; uint8_t is_cb; };
    struct rec_in recs[3] = {
        { 0, 50000, script_a, 3, 100, 1 },
        { 1, 12345, script_b, 1, 200, 0 },
        { 2, 99999, script_c, 5, 300, 0 },
    };
    for (int i = 0; i < 3; i++) {
        memcpy(body + off, txid, 32); off += 32;
        wle32(body + off, recs[i].vout); off += 4;
        wle64(body + off, (uint64_t)recs[i].value); off += 8;
        wle32(body + off, recs[i].sl); off += 4;
        memcpy(body + off, recs[i].s, recs[i].sl); off += recs[i].sl;
        wle32(body + off, recs[i].height); off += 4;
        body[off++] = recs[i].is_cb;
    }
    size_t body_len = off;

    /* Hash the body. */
    uint8_t sha3[32];
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, body, body_len);
    sha3_256_finalize(&ctx, sha3);

    /* Build header. */
    uint8_t header[104] = {0};
    memcpy(header, "ZCLUTXO\x00", 8);
    wle32(header + 8, 1);
    wle32(header + 16, 12345);
    wle64(header + 24, 3);
    wle64(header + 32, (uint64_t)(50000 + 12345 + 99999));
    /* anchor block hash: leave zero */
    memcpy(header + 72, sha3, 32);

    /* Write to a tmp file. */
    const char *tmp = "/tmp/zcl_test_uss.dat";
    FILE *f = fopen(tmp, "wb");
    if (!f) { printf("uss: tmp open FAIL\n"); return 1; }
    fwrite(header, 1, 104, f);
    fwrite(body, 1, body_len, f);
    fclose(f);

    /* Open and verify. */
    printf("uss: open+verify... ");
    char err[128] = {0};
    struct uss_header hdr;
    struct uss_handle *h = uss_open(tmp, true, NULL, &hdr,
                                    err, sizeof(err));
    if (!h) { printf("FAIL (%s)\n", err); failures++; goto cleanup; }
    if (hdr.count != 3) { printf("FAIL (count=%llu)\n",
        (unsigned long long)hdr.count); failures++; }
    if (hdr.total_supply != 50000 + 12345 + 99999) {
        printf("FAIL (supply=%lld)\n", (long long)hdr.total_supply);
        failures++;
    }
    printf("OK\n");

    /* Iterate. */
    printf("uss: iter... ");
    struct cap_ctx cap = {0};
    int64_t n = uss_iter(h, cap_cb, &cap);
    if (n != 3 || cap.n != 3) { printf("FAIL (n=%lld)\n",
        (long long)n); failures++; }
    else if (cap.values[0] != 50000 ||
             cap.values[1] != 12345 ||
             cap.values[2] != 99999 ||
             cap.script_lens[0] != 3 ||
             cap.script_lens[1] != 1 ||
             cap.script_lens[2] != 5) {
        printf("FAIL (record content mismatch)\n");
        failures++;
    }
    else printf("OK\n");
    uss_close(h);

    /* Corruption detection. */
    printf("uss: corruption detection... ");
    FILE *f2 = fopen(tmp, "rb+");
    if (!f2) { printf("FAIL (reopen)\n"); failures++; goto cleanup; }
    fseek(f2, 200, SEEK_SET); /* somewhere in the body */
    uint8_t orig;
    fread(&orig, 1, 1, f2);
    fseek(f2, 200, SEEK_SET);
    uint8_t flipped = orig ^ 0xff;
    fwrite(&flipped, 1, 1, f2);
    fclose(f2);

    err[0] = '\0';
    h = uss_open(tmp, true, NULL, NULL, err, sizeof(err));
    if (h) {
        printf("FAIL (open succeeded on corrupted file)\n");
        failures++;
        uss_close(h);
    } else if (strcmp(err, "body sha3 mismatch") != 0) {
        printf("FAIL (wrong err: %s)\n", err);
        failures++;
    } else {
        printf("OK\n");
    }

cleanup:
    unlink(tmp);
    return failures;
}
